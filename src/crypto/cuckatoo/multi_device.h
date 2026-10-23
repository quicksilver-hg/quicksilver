// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef QUICKSILVER_CRYPTO_CUCKATOO_MULTI_DEVICE_H
#define QUICKSILVER_CRYPTO_CUCKATOO_MULTI_DEVICE_H

#include <crypto/cuckatoo/cuckatoo.h>
#include <crypto/cuckatoo/gpu_devices.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cuckatoo {

enum class GpuDeviceState { kIdle, kSolving, kFaulted };

struct GpuDeviceStats {
    std::string device;
    GpuDeviceState state{GpuDeviceState::kIdle};
    GpuSolveStatus last_status{GpuSolveStatus::kNoCycle};
    uint32_t attempts{0};
    double mean_seconds_per_graph{0.0};
};

//! Thread-safe snapshot used by phase 2 reporting consumers.
std::vector<GpuDeviceStats> GetGpuDeviceStats();
void ResetGpuDeviceStatsForTesting();
//! Replace the process-wide stats table with deterministic rows in tests only.
void SetGpuDeviceStatsForTesting(std::vector<GpuDeviceStats> rows);

//! Test-only clock seam for the stats table. mean_seconds_per_graph is the gap
//! between progress heartbeats, so with the real steady_clock a case can only
//! bound it -- and a bound on how much wall-clock time passed is what R3 forbids
//! and F-532 was raised for. An injected clock makes the mean arithmetic the
//! case chose, and the assertion exact. Pass {} to restore steady_clock.
void SetGpuStatsClockForTesting(std::function<std::chrono::steady_clock::time_point()> clock);

struct MultiDeviceResult {
    bool solved{false};
    bool all_benched{false};
    bool had_devices{false};
    GpuSolveStatus status{GpuSolveStatus::kNoCycle};
    std::vector<GpuDeviceFault> faults;
};

//! Coordinate one helper process per selected card over disjoint 64-graph blocks.
MultiDeviceResult MultiDeviceSolveBytes(const unsigned char* prepow, size_t len,
                                        uint8_t edgebits, uint32_t start_nonce,
                                        uint32_t max_attempts, Cycle& out,
                                        uint32_t& out_nonce,
                                        const SolverAcceptPredicate& accept,
                                        const SolverProgressCallback& progress,
                                        const SolverCancelCallback& cancel);

} // namespace cuckatoo
#endif // QUICKSILVER_CRYPTO_CUCKATOO_MULTI_DEVICE_H
