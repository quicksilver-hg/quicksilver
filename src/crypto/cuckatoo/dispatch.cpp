// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Runtime dispatch over the supported EDGEBITS set (19/28). Block and per-tx PoW
// share EDGEBITS=28.

#include <crypto/cuckatoo/cuckatoo.h>
#include <crypto/cuckatoo/gpu_solver.h>

#include <cassert>
#include <limits>
#include <vector>

namespace cuckatoo {

// Reconstruct keys from `prepow` with `nonce` placed at the tail, then verify the
// cycle. Mirrors the byte placement in solve_28.cpp / qsgpusolve.cu, and the vendored
// solver's own `setheadernonce`, which writes the nonce into the same trailing 4 bytes
// for the fixed-header sweep -- so one helper serves every solve path.
//
// Self-verifies BOTH solvers now. It began as a guard on the untrusted GPU subprocess;
// F-253 showed the in-process CPU solver needed it just as much (see SweepVerified).
static bool ProofValidForNonce(const unsigned char* prepow, size_t len, uint8_t edgebits,
                               const Cycle& cyc, uint32_t nonce)
{
    if (len < 4) return false;
    std::vector<unsigned char> buf(prepow, prepow + len);
    buf[len - 4] = (unsigned char)(nonce & 0xff);
    buf[len - 3] = (unsigned char)((nonce >> 8) & 0xff);
    buf[len - 2] = (unsigned char)((nonce >> 16) & 0xff);
    buf[len - 1] = (unsigned char)((nonce >> 24) & 0xff);
    Keys k = CuckatooSetHeader(buf.data(), (uint32_t)len);
    return CuckatooVerify(cyc, k, edgebits);
}

// GpuSolveBytes reports kSolved as soon as it parses a cycle. The GPU is
// untrusted, so a cycle that fails CuckatooVerify must not leave that status
// in place: SolverFault treats kSolved as healthy, and getminingstatus.solver_ok
// would stay true while the card emits proofs that do not verify.
static bool GpuProofAccepted(const unsigned char* prepow, size_t len, uint8_t edgebits,
                             const Cycle& cyc, uint32_t nonce, GpuSolveStatus* gpu_status)
{
    if (ProofValidForNonce(prepow, len, edgebits, cyc, nonce)) return true;
    if (gpu_status) *gpu_status = GpuSolveStatus::kSolverError;
    return false;
}

// Per-EDGEBITS verify entry points (one translation unit each).
bool Verify19(const Cycle&, const Keys&);
bool Verify28(const Cycle&, const Keys&);

bool CuckatooVerify(const Cycle& cycle, const Keys& keys, uint8_t edgebits)
{
    switch (edgebits) {
        case 19: return Verify19(cycle, keys);
        case 28: return Verify28(cycle, keys);
        default: return false;  // unsupported graph size is invalid by consensus
    }
}

//! Run a CPU sweep and hand back only a cycle that verifies.
//!
//! F-253: the vendored lean solver runs the full cuckatoo check on every cycle it finds
//! and then discards the verdict -- its only consumer is `print_log`, and both
//! solve_19.cpp and solve_28.cpp define SQUASH_OUTPUT to 1, so the call compiles to
//! nothing. A false cycle therefore left the solver indistinguishable from a sound one,
//! and consensus rejected it later, after the sender had already paid for the work. At
//! mainnet edge bits that is minutes of GPU time for a transfer that then fails.
//!
//! The verdict is deliberately recomputed here with CuckatooVerify rather than read from
//! the vendored `verify()`: the defect class is "the solver believed a cycle consensus
//! rejects", so the check has to be the one consensus performs. Two copies of the same
//! idea agreeing would prove consistency, not correctness.
//!
//! A rejection RESUMES the sweep rather than ending it. Returning false here would turn
//! one false cycle into "Could not produce per-tx proof-of-work" and kill the user's
//! transfer -- the CPU path is the last resort, so it has nowhere to fall back to.
template <typename SolveFn, typename VerifyFn>
static bool SweepVerified(const SolveFn& solve, const VerifyFn& verify_at,
                          uint32_t start_nonce, uint32_t max_attempts,
                          Cycle& out, uint32_t& out_nonce, uint32_t* discarded_cycles)
{
    uint32_t nonce = start_nonce;
    uint32_t budget = max_attempts;
    while (budget > 0) {
        Cycle cyc{};
        uint32_t won = 0;
        if (!solve(nonce, budget, cyc, won)) return false;  // swept out, or cancelled
        if (verify_at(cyc, won)) {
            out = cyc;
            out_nonce = won;
            return true;
        }
        // The attempt itself is already registered: `progress` fires per nonce at the top
        // of the sweep, so attempts_per_second stays honest whatever the outcome. What is
        // recorded here is the OUTCOME -- completed, then discarded -- because after this
        // fix the only other symptom of a solver emitting false cycles is "the grind got
        // slower", which reads identically to a run of hard graphs.
        if (discarded_cycles != nullptr) ++*discarded_cycles;
        if (won == std::numeric_limits<uint32_t>::max()) return false;  // nonce space exhausted
        const uint32_t consumed = won - nonce + 1;
        if (consumed >= budget) return false;  // the caller's attempt budget is spent
        budget -= consumed;
        nonce = won + 1;
    }
    return false;
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
                   GpuSolveStatus* gpu_status, uint32_t* discarded_cycles)
{
    // Mining-only GPU acceleration: try the external solver, self-verify, else CPU.
    {
        Cycle gpu_cyc{}; uint32_t gpu_nonce = 0;
        if (GpuSolveBytes(prepow.data(), prepow.size(), edgebits, start_nonce, max_attempts, gpu_cyc, gpu_nonce, progress, cancel, gpu_status)
            && GpuProofAccepted(prepow.data(), prepow.size(), edgebits, gpu_cyc, gpu_nonce, gpu_status)) {
            out = gpu_cyc; out_nonce = gpu_nonce; return true;
        }
    }
    if (!cpu_fallback) return false;  // CPU grind infeasible at this graph size per caller policy
    if (cancel && cancel()) return false;  // cancelled during the GPU attempt: do not start a CPU sweep
    if (edgebits != 19 && edgebits != 28) return false;  // no solver built for this graph size
    // mutate_nonce=1 here: the vendored setheadernonce writes the nonce into the pre-pow's
    // trailing 4 bytes, the same slot ProofValidForNonce places it in.
    return SweepVerified(
        [&](uint32_t sweep_from, uint32_t budget, Cycle& cyc, uint32_t& won) {
            return edgebits == 19 ? Solve19(prepow, sweep_from, budget, cyc, won, progress, cancel)
                                  : Solve28(prepow, sweep_from, budget, cyc, won, progress, cancel);
        },
        [&](const Cycle& cyc, uint32_t won) {
            return ProofValidForNonce(prepow.data(), prepow.size(), edgebits, cyc, won);
        },
        start_nonce, max_attempts, out, out_nonce, discarded_cycles);
}

// Variable-length pre-image solver (per-tx PoW). 19 = sandbox, 28 = publictest/main.
bool Solve19Bytes(const unsigned char*, size_t, uint32_t, uint32_t, Cycle&, uint32_t&, const SolverProgressCallback&, const SolverCancelCallback&);
bool Solve28Bytes(const unsigned char*, size_t, uint32_t, uint32_t, Cycle&, uint32_t&, const SolverProgressCallback&, const SolverCancelCallback&);

bool CuckatooSolveBytes(const unsigned char* prepow, size_t len, uint8_t edgebits,
                        uint32_t start_nonce, uint32_t max_attempts, Cycle& out, uint32_t& out_nonce,
                        bool cpu_fallback,
                        const SolverProgressCallback& progress,
                        const SolverCancelCallback& cancel,
                        GpuSolveStatus* gpu_status, uint32_t* discarded_cycles)
{
    // Mining-only GPU acceleration: try the external solver, self-verify, else CPU.
    {
        Cycle gpu_cyc{}; uint32_t gpu_nonce = 0;
        if (GpuSolveBytes(prepow, len, edgebits, start_nonce, max_attempts, gpu_cyc, gpu_nonce, progress, cancel, gpu_status)
            && GpuProofAccepted(prepow, len, edgebits, gpu_cyc, gpu_nonce, gpu_status)) {
            out = gpu_cyc; out_nonce = gpu_nonce; return true;
        }
    }
    if (!cpu_fallback) return false;  // CPU grind infeasible at this graph size per caller policy
    if (cancel && cancel()) return false;  // cancelled during the GPU attempt: do not start a CPU sweep
    if (edgebits != 19 && edgebits != 28) {
        assert(false && "CuckatooSolveBytes: unsupported edgebits");
        return false;
    }
    return SweepVerified(
        [&](uint32_t sweep_from, uint32_t budget, Cycle& cyc, uint32_t& won) {
            return edgebits == 19 ? Solve19Bytes(prepow, len, sweep_from, budget, cyc, won, progress, cancel)
                                  : Solve28Bytes(prepow, len, sweep_from, budget, cyc, won, progress, cancel);
        },
        [&](const Cycle& cyc, uint32_t won) {
            return ProofValidForNonce(prepow, len, edgebits, cyc, won);
        },
        start_nonce, max_attempts, out, out_nonce, discarded_cycles);
}

} // namespace cuckatoo
