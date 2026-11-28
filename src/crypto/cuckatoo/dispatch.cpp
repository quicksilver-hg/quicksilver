// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Runtime dispatch for block and transaction PoW. Sandbox uses 19 edge bits.
// The public networks use 28.

#include <crypto/cuckatoo/cuckatoo.h>
#include <crypto/cuckatoo/multi_device.h>

#include <atomic>
#include <cassert>
#include <limits>
#include <vector>

namespace cuckatoo {

// Reconstruct keys from `prepow` with `nonce` placed at the tail, then verify the
// cycle. Mirrors the byte placement in solve_28.cpp / qsgpusolve-opencl.cpp, and the vendored
// solver's own `setheadernonce`, which places the nonce in the same trailing
// four bytes for the fixed-header sweep.
//
// Self-verifies BOTH solvers now. It began as a guard on the untrusted GPU subprocess;
// F-253 showed the in-process CPU solver needed it just as much (see SweepVerified).
static bool ProofValidForNonce(const unsigned char* prepow, size_t len, uint8_t edgebits,
                               const Cycle& cyc, uint32_t nonce)
{
    if (len < 4 || len > std::numeric_limits<uint32_t>::max()) return false;
    std::vector<unsigned char> buf(prepow, prepow + len);
    buf[len - 4] = (unsigned char)(nonce & 0xff);
    buf[len - 3] = (unsigned char)((nonce >> 8) & 0xff);
    buf[len - 2] = (unsigned char)((nonce >> 16) & 0xff);
    buf[len - 1] = (unsigned char)((nonce >> 24) & 0xff);
    Keys k = CuckatooSetHeader(buf.data(), (uint32_t)len);
    return CuckatooVerify(cyc, k, edgebits);
}

// Per-EDGEBITS verify entry points (one translation unit each).
bool Verify19(const Cycle&, const Keys&);
bool Verify28(const Cycle&, const Keys&);

bool ConsensusVerifiesEdgeBits(uint8_t edgebits)
{
    return edgebits == 19 || edgebits == 28;
}

bool CuckatooVerify(const Cycle& cycle, const Keys& keys, uint8_t edgebits)
{
    switch (edgebits) {
        case 19: return Verify19(cycle, keys);
        case 28: return Verify28(cycle, keys);
        default: return false;  // unsupported graph size is invalid by consensus
    }
}

//! Verify each candidate with the same verifier as consensus.
//! Continue at the next nonce when verification or the caller's predicate rejects it.
template <typename SolveFn, typename VerifyFn>
static bool SweepVerified(const SolveFn& solve, const VerifyFn& verify_at,
                          uint32_t start_nonce, uint32_t max_attempts,
                          Cycle& out, uint32_t& out_nonce, uint32_t* discarded_cycles,
                          const SolverAcceptPredicate& accept)
{
    uint32_t nonce = start_nonce;
    uint32_t budget = max_attempts;
    while (budget > 0) {
        Cycle cyc{};
        uint32_t won = 0;
        if (!solve(nonce, budget, cyc, won)) return false;  // swept out, or cancelled
        const bool valid = verify_at(cyc, won);
        if (valid && (!accept || accept(cyc, won))) {
            out = cyc;
            out_nonce = won;
            return true;
        }
        // The attempt itself is already registered: `progress` fires per nonce at the top
        // of the sweep, so attempts_per_second stays honest whatever the outcome. What is
        // recorded here is the OUTCOME -- completed, then discarded -- because after this
        // fix the only other symptom of a solver emitting false cycles is "the grind got
        // slower", which reads identically to a run of hard graphs.
        if (!valid && discarded_cycles != nullptr) ++*discarded_cycles;
        if (won == std::numeric_limits<uint32_t>::max()) return false;  // nonce space exhausted
        const uint32_t consumed = won - nonce + 1;
        if (consumed >= budget) return false;  // the caller's attempt budget is spent
        budget -= consumed;
        nonce = won + 1;
    }
    return false;
}

//! The CPU sweep reports through *gpu_status too. Left alone, a sweep the caller
//! cancelled still reads as the GPU attempt's kNoSolver or kNoGpuDevice, and the
//! agent then tells the operator the grind failed, or that no card was found,
//! instead of that they stopped it (S0118-A-01).
static void MarkCpuSweepCancelled(bool solved, const SolverCancelCallback& cancel, GpuSolveStatus* gpu_status)
{
    if (!solved && gpu_status && cancel && cancel()) *gpu_status = GpuSolveStatus::kCancelled;
}

// Per-EDGEBITS fixed-header (PREPOW_BYTES block pre-pow) solve entry points. Block
// PoW shares the per-tx E28 graph size, so a single E28 solver serves both;
// Solve28Bytes (variable-length) remains the per-tx entry point.
bool Solve19(const std::array<unsigned char, PREPOW_BYTES>&, uint32_t, uint32_t, Cycle&, uint32_t&, const SolverProgressCallback&, const SolverCancelCallback&);
bool Solve28(const std::array<unsigned char, PREPOW_BYTES>&, uint32_t, uint32_t, Cycle&, uint32_t&, const SolverProgressCallback&, const SolverCancelCallback&);

bool CuckatooSolve(const std::array<unsigned char, PREPOW_BYTES>& prepow, uint8_t edgebits,
                   uint32_t start_nonce, uint32_t max_attempts, Cycle& out, uint32_t& out_nonce,
                   bool cpu_fallback,
                   const SolverProgressCallback& progress,
                   const SolverCancelCallback& cancel,
                   GpuSolveStatus* gpu_status, uint32_t* discarded_cycles,
                   const SolverAcceptPredicate& accept,
                   std::vector<GpuDeviceFault>* gpu_faults,
                   std::string* gpu_discovery)
{
    const MultiDeviceResult gpu = MultiDeviceSolveBytes(prepow.data(), prepow.size(), edgebits,
                                                         start_nonce, max_attempts, out, out_nonce,
                                                         accept, progress, cancel);
    if (gpu_status) *gpu_status = gpu.status;
    if (gpu_faults) *gpu_faults = gpu.faults;
    if (gpu_discovery) *gpu_discovery = gpu.discovery;
    if (gpu.solved) return true;
    if (!cpu_fallback || (gpu.had_devices && !gpu.all_benched)) return false;
    if (cancel && cancel()) {  // cancelled during the GPU attempt: do not start a CPU sweep
        if (gpu_status) *gpu_status = GpuSolveStatus::kCancelled;
        return false;
    }
    if (edgebits != 19 && edgebits != 28) return false;  // no solver built for this graph size
    // mutate_nonce=1 here: the vendored setheadernonce writes the nonce into the pre-pow's
    // trailing 4 bytes, the same slot ProofValidForNonce places it in.
    const bool solved = SweepVerified(
        [&](uint32_t sweep_from, uint32_t budget, Cycle& cyc, uint32_t& won) {
            return edgebits == 19 ? Solve19(prepow, sweep_from, budget, cyc, won, progress, cancel)
                                  : Solve28(prepow, sweep_from, budget, cyc, won, progress, cancel);
        },
        [&](const Cycle& cyc, uint32_t won) {
            return ProofValidForNonce(prepow.data(), prepow.size(), edgebits, cyc, won);
        },
        start_nonce, max_attempts, out, out_nonce, discarded_cycles, accept);
    MarkCpuSweepCancelled(solved, cancel, gpu_status);
    return solved;
}

// Variable-length pre-image solver (per-tx PoW). 19 = sandbox, 28 = publictest/main.
bool Solve19Bytes(const unsigned char*, size_t, uint32_t, uint32_t, Cycle&, uint32_t&, const SolverProgressCallback&, const SolverCancelCallback&);
bool Solve28Bytes(const unsigned char*, size_t, uint32_t, uint32_t, Cycle&, uint32_t&, const SolverProgressCallback&, const SolverCancelCallback&);

bool CuckatooSolveBytes(const unsigned char* prepow, size_t len, uint8_t edgebits,
                        uint32_t start_nonce, uint32_t max_attempts, Cycle& out, uint32_t& out_nonce,
                        bool cpu_fallback,
                        const SolverProgressCallback& progress,
                        const SolverCancelCallback& cancel,
                        GpuSolveStatus* gpu_status, uint32_t* discarded_cycles,
                        const SolverAcceptPredicate& accept,
                        std::vector<GpuDeviceFault>* gpu_faults,
                        std::string* gpu_discovery)
{
    // The CPU solvers write the nonce into prepow[len-4..len-1] and take the
    // length as uint32_t. Every caller passes a full pre-image, so this is a
    // guard against a future one: a short or oversized buffer is refused here,
    // before any solver runs, rather than written out of bounds or truncated.
    if (len < 4 || len > std::numeric_limits<uint32_t>::max()) {
        if (gpu_status) *gpu_status = GpuSolveStatus::kNoSolver;  // as GpuSolveBytes reports a short buffer
        return false;
    }
    const MultiDeviceResult gpu = MultiDeviceSolveBytes(prepow, len, edgebits, start_nonce,
                                                         max_attempts, out, out_nonce, accept,
                                                         progress, cancel);
    if (gpu_status) *gpu_status = gpu.status;
    if (gpu_faults) *gpu_faults = gpu.faults;
    if (gpu_discovery) *gpu_discovery = gpu.discovery;
    if (gpu.solved) return true;
    if (!cpu_fallback || (gpu.had_devices && !gpu.all_benched)) return false;
    if (cancel && cancel()) {  // cancelled during the GPU attempt: do not start a CPU sweep
        if (gpu_status) *gpu_status = GpuSolveStatus::kCancelled;
        return false;
    }
    if (edgebits != 19 && edgebits != 28) {
        assert(false && "CuckatooSolveBytes: unsupported edgebits");
        return false;
    }
    const bool solved = SweepVerified(
        [&](uint32_t sweep_from, uint32_t budget, Cycle& cyc, uint32_t& won) {
            return edgebits == 19 ? Solve19Bytes(prepow, len, sweep_from, budget, cyc, won, progress, cancel)
                                  : Solve28Bytes(prepow, len, sweep_from, budget, cyc, won, progress, cancel);
        },
        [&](const Cycle& cyc, uint32_t won) {
            return ProofValidForNonce(prepow, len, edgebits, cyc, won);
        },
        start_nonce, max_attempts, out, out_nonce, discarded_cycles, accept);
    MarkCpuSweepCancelled(solved, cancel, gpu_status);
    return solved;
}

namespace {
std::atomic<F265CaptureSink> g_f265_capture_sink{nullptr};
} // namespace

void SetF265CaptureSink(F265CaptureSink sink) { g_f265_capture_sink.store(sink); }
F265CaptureSink GetF265CaptureSink() { return g_f265_capture_sink.load(); }

} // namespace cuckatoo
