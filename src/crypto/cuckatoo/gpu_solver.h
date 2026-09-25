// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Mining-only subprocess bridge to an external GPU Cuckatoo solver. NEVER on the
// validation path. The caller MUST verify any returned cycle with CuckatooVerify.
#ifndef QUICKSILVER_CRYPTO_CUCKATOO_GPU_SOLVER_H
#define QUICKSILVER_CRYPTO_CUCKATOO_GPU_SOLVER_H

#include <crypto/cuckatoo/cuckatoo.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cuckatoo {

/**
 * Map the solver's last outcome onto a reportable fault. Returns nullopt when
 * the outcome says nothing bad: a solved graph, an exhausted nonce window, a
 * cancelled attempt, or a missing GPU solver on a chain where the CPU solver is
 * the intended one. Otherwise returns the operator-facing sentence.
 *
 * A missing GPU solver is a fault only when cpu_fallback is false, because that
 * is the only configuration in which nothing at all is mining. A dead card or a
 * broken child process is always a fault -- reporting it while a CPU fallback
 * quietly carries on is the point, not a bug.
 */
std::optional<std::string> SolverFault(GpuSolveStatus status, bool cpu_fallback);

/**
 * True when the reportable fault is specifically "no solver is configured", as
 * opposed to a solver that ran and failed. SolverFault's sentence is written for
 * quicksilver-daemon and names -cuckatoosolver; a GUI has that setting as a control and
 * must say so instead, which means it needs the fault's identity and not its prose.
 *
 * Defined in terms of SolverFault so the two cannot disagree about which outcomes
 * are faults at all: a missing solver under a CPU fallback is not one.
 */
bool SolverMissing(GpuSolveStatus status, bool cpu_fallback);

/**
 * Return the solver path active in this process, or nullopt when the runtime
 * bridge has no solver. The GUI setting can change before restart, but the
 * bridge deliberately continues to use the environment installed during
 * startup; runtime status must use this same source or it can advertise a
 * solver that no solve attempt can actually start.
 */
std::optional<std::string> GpuSolverPath();

//! If CUCKATOO_GPU_SOLVER is set, runs it as
//!   <path> <edgebits> <hex(prepow)> <start_nonce> <max_attempts>
//! and parses "nonce="/"cycle=". The external binary grinds the nonce into
//! buf[len-4..len-1] (LE) itself, matching the CPU solver. Returns true and
//! fills out/out_nonce on a parsed cycle; false if the env is unset/empty, the
//! subprocess errors, or no cycle is produced. The caller MUST verify the
//! returned cycle with CuckatooVerify before trusting it.
//! `cancel` is polled while waiting on the child: when it returns true the child
//! process group is killed and the call returns false without waiting out the
//! no-progress window.
bool GpuSolveBytes(const unsigned char* prepow, size_t len, uint8_t edgebits,
                   uint32_t start_nonce, uint32_t max_attempts,
                   Cycle& out, uint32_t& out_nonce,
                   const SolverProgressCallback& progress = {},
                   const SolverCancelCallback& cancel = {},
                   GpuSolveStatus* status = nullptr);

} // namespace cuckatoo

#endif // QUICKSILVER_CRYPTO_CUCKATOO_GPU_SOLVER_H
