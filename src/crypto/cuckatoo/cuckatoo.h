// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Quicksilver Cuckatoo PoW — public, consensus-facing API.
// Wraps the vendored Tromp verifier/solver behind a runtime-EDGEBITS interface.
#ifndef QUICKSILVER_CRYPTO_CUCKATOO_CUCKATOO_H
#define QUICKSILVER_CRYPTO_CUCKATOO_CUCKATOO_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

#include <uint256.h>

namespace cuckatoo {

static constexpr int PROOFSIZE = 42;              // 42-cycle, fixed
using Cycle = std::array<uint32_t, PROOFSIZE>;    // edge indices, ascending
using SolverProgressCallback = std::function<void(uint32_t nonce)>;
//! Polled by every solve loop; returning true abandons the sweep and returns
//! false to the caller. A grind is the longest uninterruptible span in the node
//! — one GPU attempt can block for the full no-progress window and a CPU sweep
//! for its whole budget — so shutdown has to reach inside it, not merely between
//! calls. An empty callback never cancels.
using SolverCancelCallback = std::function<bool()>;

//! Why a GPU solve attempt ended. A bool says only "did we get a cycle", which
//! conflates a dead card with an unlucky nonce window — on a CUDA test node that made a
//! failed GPU look like a slow grind for hours. This layer has no logging (it
//! links Threads and nothing else), so the reason is reported out and the node
//! logs it. Declared here rather than in gpu_solver.h because dispatch passes it
//! through the public solve entry points, and gpu_solver.h includes this header.
enum class GpuSolveStatus {
    kSolved,        //!< a cycle was parsed from stdout; dispatch overwrites this if self-verify fails
    kNoSolver,      //!< CUCKATOO_GPU_SOLVER unset or empty, or the preimage is too short
    kNoCycle,       //!< the solver ran and exited 0 without producing a cycle
    kNoCudaDevice,  //!< the solver reports no usable CUDA device (exit 4)
    kDeviceFault,   //!< the device faulted mid-solve, e.g. a driver watchdog reset (exit 5)
    kSolverError,   //!< the solver exited non-zero, or a parsed cycle failed self-verify
    kTimedOut,      //!< killed by the no-progress watchdog
    kCancelled,     //!< the caller cancelled the attempt
};

//! Length of a block pre-pow: the CBlockHeader prefix through nNonce. Kept here so
//! the solver entry points do not depend on primitives/block.h; a static_assert in
//! node/block_solve.cpp fences it against CBlockHeader::PREPOW_SIZE.
inline constexpr size_t PREPOW_BYTES{84};

//! SipHash keys derived from a block's 84-byte pre-pow prefix.
struct Keys { uint64_t k0, k1, k2, k3; };

//! Derive keys from the serialized pre-pow header bytes (len is normally PREPOW_BYTES).
Keys CuckatooSetHeader(const unsigned char* header, uint32_t headerlen);

//! blake2b over the LE-serialized cycle. The value thresholded by nBits.
uint256 CuckatooProofHash(const Cycle& cycle);

//! Returns true iff `cycle` is a valid 42-cycle in the graph keyed by `keys`
//! at graph size `edgebits`. Consensus. ~1 us. edgebits must be in {19,28}.
bool CuckatooVerify(const Cycle& cycle, const Keys& keys, uint8_t edgebits);

//! Mining only — NEVER call from validation. Sweeps nonces start_nonce ..
//! start_nonce+max_attempts-1 (written into the pre-pow's nNonce slot), solving
//! the graph keyed by each, until a 42-cycle is found. On success fills `out`
//! with the cycle, sets `out_nonce` to the winning nonce, and returns true.
//! Reuses one multi-threaded solver context across the sweep. edgebits in {19,28}
//! (block PoW shares the per-tx E28 graph size; a single E28 solver serves both).
//! When cpu_fallback is false, a failed GPU attempt returns false WITHOUT running
//! the CPU solver (used where a CPU grind is infeasible; see SP1b design).
//! A GPU cycle that fails self-verify is reported as kSolverError, not kSolved.
bool CuckatooSolve(const std::array<unsigned char, PREPOW_BYTES>& prepow, uint8_t edgebits,
                   uint32_t start_nonce, uint32_t max_attempts,
                   Cycle& out, uint32_t& out_nonce, bool cpu_fallback = true,
                   const SolverProgressCallback& progress = {},
                   const SolverCancelCallback& cancel = {},
                   GpuSolveStatus* gpu_status = nullptr);

//! Mining only — variable-length pre-image grinder. Sweeps start_nonce ..
//! start_nonce+max_attempts-1, writing each nonce as the trailing 4 LE bytes of a
//! `len`-byte copy of `prepow` (no 4-byte-alignment requirement), solving the graph
//! keyed by each until a 42-cycle is found. On success fills `out`, sets `out_nonce`,
//! returns true. edgebits in {19,28}.
//! When cpu_fallback is false, a failed GPU attempt returns false WITHOUT running
//! the CPU solver (used where a CPU grind is infeasible; see SP1b design).
//! A GPU cycle that fails self-verify is reported as kSolverError, not kSolved.
bool CuckatooSolveBytes(const unsigned char* prepow, size_t len, uint8_t edgebits,
                        uint32_t start_nonce, uint32_t max_attempts,
                        Cycle& out, uint32_t& out_nonce, bool cpu_fallback = true,
                        const SolverProgressCallback& progress = {},
                        const SolverCancelCallback& cancel = {},
                        GpuSolveStatus* gpu_status = nullptr);

} // namespace cuckatoo

#endif // QUICKSILVER_CRYPTO_CUCKATOO_CUCKATOO_H
