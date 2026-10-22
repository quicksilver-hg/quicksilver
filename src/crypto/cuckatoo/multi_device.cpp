// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/cuckatoo/multi_device.h>

#include <crypto/cuckatoo/gpu_solver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <thread>

namespace cuckatoo {
namespace {
constexpr uint32_t BLOCK_ATTEMPTS{64};
// One coordinator block smooths short jitter without allowing an old device's
// rate to dominate after work moves elsewhere.
constexpr size_t RATE_WINDOW_GRAPHS{64};

struct MutableStats {
    GpuDeviceStats public_stats;
    std::deque<double> seconds;
    std::chrono::steady_clock::time_point last_progress;
    bool have_progress_time{false};
};

std::mutex g_stats_mutex;
std::map<std::string, MutableStats> g_stats;

bool ProofValidForNonce(const unsigned char* prepow, size_t len, uint8_t edgebits,
                        const Cycle& cycle, uint32_t nonce)
{
    if (len < 4) return false;
    std::vector<unsigned char> bytes(prepow, prepow + len);
    bytes[len - 4] = static_cast<unsigned char>(nonce);
    bytes[len - 3] = static_cast<unsigned char>(nonce >> 8);
    bytes[len - 2] = static_cast<unsigned char>(nonce >> 16);
    bytes[len - 1] = static_cast<unsigned char>(nonce >> 24);
    return CuckatooVerify(cycle, CuckatooSetHeader(bytes.data(), static_cast<uint32_t>(len)), edgebits);
}

void SetState(const std::string& device, GpuDeviceState state, GpuSolveStatus status)
{
    std::lock_guard<std::mutex> lock{g_stats_mutex};
    auto& entry = g_stats[device];
    entry.public_stats.device = device;
    entry.public_stats.state = state;
    entry.public_stats.last_status = status;
    if (state == GpuDeviceState::kSolving) {
        entry.last_progress = std::chrono::steady_clock::now();
        entry.have_progress_time = true;
    }
}

void RecordProgress(const std::string& device)
{
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock{g_stats_mutex};
    auto& entry = g_stats[device];
    entry.public_stats.device = device;
    entry.public_stats.state = GpuDeviceState::kSolving;
    if (entry.have_progress_time) {
        entry.seconds.push_back(std::chrono::duration<double>(now - entry.last_progress).count());
        if (entry.seconds.size() > RATE_WINDOW_GRAPHS) entry.seconds.pop_front();
    }
    entry.last_progress = now;
    entry.have_progress_time = true;
    entry.public_stats.attempts = static_cast<uint32_t>(entry.seconds.size());
    double total{0.0};
    for (const double value : entry.seconds) total += value;
    entry.public_stats.mean_seconds_per_graph = entry.seconds.empty() ? 0.0 : total / entry.seconds.size();
}

bool FaultStatus(GpuSolveStatus status)
{
    return status == GpuSolveStatus::kNoGpuDevice || status == GpuSolveStatus::kDeviceFault ||
           status == GpuSolveStatus::kSolverError || status == GpuSolveStatus::kTimedOut;
}
} // namespace

std::vector<GpuDeviceStats> GetGpuDeviceStats()
{
    std::lock_guard<std::mutex> lock{g_stats_mutex};
    std::vector<GpuDeviceStats> out;
    out.reserve(g_stats.size());
    for (const auto& [device, entry] : g_stats) out.push_back(entry.public_stats);
    return out;
}

void ResetGpuDeviceStatsForTesting()
{
    std::lock_guard<std::mutex> lock{g_stats_mutex};
    g_stats.clear();
}

void SetGpuDeviceStatsForTesting(std::vector<GpuDeviceStats> rows)
{
    std::lock_guard<std::mutex> lock{g_stats_mutex};
    g_stats.clear();
    for (auto& row : rows) {
        const std::string id = row.device;
        g_stats[id].public_stats = std::move(row);
    }
}

MultiDeviceResult MultiDeviceSolveBytes(const unsigned char* prepow, size_t len,
                                        uint8_t edgebits, uint32_t start_nonce,
                                        uint32_t max_attempts, Cycle& out,
                                        uint32_t& out_nonce,
                                        const SolverAcceptPredicate& accept,
                                        const SolverProgressCallback& progress,
                                        const SolverCancelCallback& cancel)
{
    MultiDeviceResult result;
    const GpuDiscovery discovery = DiscoverGpus(edgebits);
    const auto& devices = discovery.selection.use;
    result.had_devices = !devices.empty();
    if (devices.empty()) {
        result.status = discovery.helper ? GpuSolveStatus::kNoGpuDevice : GpuSolveStatus::kNoSolver;
        return result;
    }

    const uint64_t nonce_capacity = uint64_t{std::numeric_limits<uint32_t>::max()} - start_nonce + 1;
    const uint64_t budget = std::min<uint64_t>(max_attempts, nonce_capacity);
    std::atomic<uint64_t> next{0};
    std::atomic<bool> accepted{false};
    std::atomic<bool> external_cancelled{false};
    std::atomic<size_t> benched{0};
    std::mutex callbacks_mutex;
    std::mutex result_mutex;

    const auto coordinated_cancel = [&]() {
        if (accepted.load(std::memory_order_acquire)) return true;
        std::lock_guard<std::mutex> lock{callbacks_mutex};
        const bool stopped = cancel && cancel();
        if (stopped) external_cancelled.store(true, std::memory_order_release);
        return stopped;
    };

    std::vector<std::thread> workers;
    workers.reserve(devices.size());
    for (const auto& selected : devices) {
        workers.emplace_back([&, device = selected.id] {
            SetState(device, GpuDeviceState::kSolving, GpuSolveStatus::kNoCycle);
            bool faulted{false};
            GpuSolveStatus last{GpuSolveStatus::kNoCycle};
            while (!accepted.load(std::memory_order_acquire)) {
                if (coordinated_cancel()) { last = GpuSolveStatus::kCancelled; break; }
                const uint64_t offset = next.fetch_add(BLOCK_ATTEMPTS, std::memory_order_relaxed);
                if (offset >= budget) break;
                const uint32_t block_start = static_cast<uint32_t>(uint64_t{start_nonce} + offset);
                uint32_t block_left = static_cast<uint32_t>(std::min<uint64_t>(BLOCK_ATTEMPTS, budget - offset));
                uint32_t from = block_start;
                while (block_left > 0 && !accepted.load(std::memory_order_acquire)) {
                    Cycle cycle{};
                    uint32_t won{0};
                    const auto device_progress = [&](uint32_t nonce) {
                        RecordProgress(device);
                        std::lock_guard<std::mutex> lock{callbacks_mutex};
                        if (progress) progress(nonce);
                    };
                    const bool solved = GpuSolveBytes(prepow, len, edgebits, device, from,
                                                      block_left, cycle, won, device_progress,
                                                      coordinated_cancel, &last);
                    if (!solved) {
                        if (FaultStatus(last)) faulted = true;
                        break;
                    }
                    if (!ProofValidForNonce(prepow, len, edgebits, cycle, won)) {
                        last = GpuSolveStatus::kSolverError;
                        faulted = true;
                        break;
                    }
                    bool accepted_by_caller{true};
                    {
                        std::lock_guard<std::mutex> lock{callbacks_mutex};
                        if (accept) accepted_by_caller = accept(cycle, won);
                    }
                    if (accepted_by_caller) {
                        bool expected{false};
                        if (accepted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                            std::lock_guard<std::mutex> lock{result_mutex};
                            out = cycle;
                            out_nonce = won;
                            result.solved = true;
                        }
                        last = GpuSolveStatus::kSolved;
                        break;
                    }
                    const uint64_t consumed = uint64_t{won} - from + 1;
                    if (won == std::numeric_limits<uint32_t>::max() || consumed >= block_left) {
                        block_left = 0;
                    } else {
                        block_left -= static_cast<uint32_t>(consumed);
                        from = won + 1;
                    }
                }
                if (faulted || last == GpuSolveStatus::kCancelled) break;
            }
            if (faulted) {
                {
                    std::lock_guard<std::mutex> lock{result_mutex};
                    result.faults.push_back({device, last});
                }
                benched.fetch_add(1, std::memory_order_relaxed);
                SetState(device, GpuDeviceState::kFaulted, last);
            } else {
                SetState(device, GpuDeviceState::kIdle, last);
            }
        });
    }
    for (auto& worker : workers) worker.join();

    result.all_benched = benched.load(std::memory_order_relaxed) == devices.size();
    if (result.solved) result.status = GpuSolveStatus::kSolved;
    else if (result.all_benched && !result.faults.empty()) result.status = result.faults.front().status;
    else if (external_cancelled.load(std::memory_order_acquire)) result.status = GpuSolveStatus::kCancelled;
    else result.status = GpuSolveStatus::kNoCycle;
    return result;
}

} // namespace cuckatoo
