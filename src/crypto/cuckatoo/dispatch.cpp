// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Runtime dispatch over the supported EDGEBITS set (19/28). Block and per-tx PoW
// share EDGEBITS=28.

#include <crypto/cuckatoo/cuckatoo.h>
#include <crypto/cuckatoo/gpu_solver.h>

#include <cassert>
#include <vector>

namespace cuckatoo {

// Reconstruct keys from `prepow` with `nonce` placed at the tail, then verify the
// cycle. Mirrors the byte placement in solve_28.cpp / qsgpusolve.cu. Used to
// self-verify an untrusted GPU proof before the node accepts it (mining only).
static bool GpuProofValid(const unsigned char* prepow, size_t len, uint8_t edgebits,
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
    if (GpuProofValid(prepow, len, edgebits, cyc, nonce)) return true;
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
                   GpuSolveStatus* gpu_status)
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
    switch (edgebits) {
        case 19: return Solve19(prepow, start_nonce, max_attempts, out, out_nonce, progress, cancel);
        case 28: return Solve28(prepow, start_nonce, max_attempts, out, out_nonce, progress, cancel);
        default: return false;  // no solver built for this graph size
    }
}

// Variable-length pre-image solver (per-tx PoW). 19 = sandbox, 28 = publictest/main.
bool Solve19Bytes(const unsigned char*, size_t, uint32_t, uint32_t, Cycle&, uint32_t&, const SolverProgressCallback&, const SolverCancelCallback&);
bool Solve28Bytes(const unsigned char*, size_t, uint32_t, uint32_t, Cycle&, uint32_t&, const SolverProgressCallback&, const SolverCancelCallback&);

bool CuckatooSolveBytes(const unsigned char* prepow, size_t len, uint8_t edgebits,
                        uint32_t start_nonce, uint32_t max_attempts, Cycle& out, uint32_t& out_nonce,
                        bool cpu_fallback,
                        const SolverProgressCallback& progress,
                        const SolverCancelCallback& cancel,
                        GpuSolveStatus* gpu_status)
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
    switch (edgebits) {
        case 19: return Solve19Bytes(prepow, len, start_nonce, max_attempts, out, out_nonce, progress, cancel);
        case 28: return Solve28Bytes(prepow, len, start_nonce, max_attempts, out, out_nonce, progress, cancel);
        default:
            assert(false && "CuckatooSolveBytes: unsupported edgebits");
            return false;
    }
}

} // namespace cuckatoo
