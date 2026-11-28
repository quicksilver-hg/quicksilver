// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Mining-only subprocess bridge to an external GPU Cuckatoo solver. NEVER on the
// validation path. The caller MUST verify any returned cycle with CuckatooVerify.
#ifndef QUICKSILVER_CRYPTO_CUCKATOO_GPU_SOLVER_H
#define QUICKSILVER_CRYPTO_CUCKATOO_GPU_SOLVER_H

#include <crypto/cuckatoo/cuckatoo.h>
#include <crypto/cuckatoo/gpu_devices.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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
//! The node's option that permits processor proofs. The agent's is -allowcputxpow.
inline constexpr std::string_view kNodeCpuOption{"-allowcpumining"};
//! `cpu_option` is the caller's own option that permits processor proofs: the
//! no-solver sentence names it, so a program that does not read -allowcpumining
//! does not tell its operator to set it (S0322-A-01).
std::optional<std::string> SolverFault(GpuSolveStatus status, bool cpu_fallback,
                                       std::string_view cpu_option = kNodeCpuOption);

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
const char* GpuSolveStatusName(GpuSolveStatus status);

/**
 * Check whether the runtime bridge can select a helper for this graph size.
 * It reads the environment set during startup, including the configured
 * override and the bundled helper for supported graph sizes.
 */
bool GpuHelperConfigured(uint8_t edgebits);
//! -cuckatoosolvertimeout: the no-progress watchdog window, in seconds. The help
//! text, the option's refusal and the solver's clamp all read these, so the
//! documented default and range are the ones the solver applies (F-695).
inline constexpr int kSolverTimeoutDefaultSec{30};
inline constexpr int kSolverTimeoutMinSec{1};
inline constexpr int kSolverTimeoutMaxSec{86400};
int NoProgressTimeoutMs();
std::string DescribeDiscovery(const GpuDiscovery& discovery);

//! If CUCKATOO_GPU_SOLVER is set, runs the helper for `device` as
//!   <path> --device <id> <edgebits> <hex(prepow)> <start_nonce> <max_attempts>
//! and parses "nonce="/"cycle=". The external binary grinds the nonce into
//! buf[len-4..len-1] (LE) itself, matching the CPU solver. Returns true and
//! fills out/out_nonce on a parsed cycle; false if the env is unset/empty, the
//! subprocess errors, or no cycle is produced. The caller MUST verify the
//! returned cycle with CuckatooVerify before trusting it.
//! `cancel` is polled while waiting on the child: when it returns true the child
//! process group is killed and the call returns false without waiting out the
//! no-progress window.
bool GpuSolveBytes(const unsigned char* prepow, size_t len, uint8_t edgebits,
                   const std::string& device,
                   uint32_t start_nonce, uint32_t max_attempts,
                   Cycle& out, uint32_t& out_nonce,
                   const SolverProgressCallback& progress = {},
                   const SolverCancelCallback& cancel = {},
                   GpuSolveStatus* status = nullptr);

} // namespace cuckatoo

#endif // QUICKSILVER_CRYPTO_CUCKATOO_GPU_SOLVER_H
