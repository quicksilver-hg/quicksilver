// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_NODE_BLOCK_SOLVE_H
#define QUICKSILVER_NODE_BLOCK_SOLVE_H

#include <crypto/cuckatoo/cuckatoo.h>

#include <cstdint>
#include <functional>

class CBlock;
class ChainstateManager;

namespace node {

/**
 * Solve a block's proof-of-work in place. Sets hashMerkleRoot, then sweeps up to
 * `max_tries` nonces (decrementing it by the amount consumed on the real path).
 * On sandbox fBlockPowNoCycle, grinds the trivial proof-hash instead. Returns true
 * iff a proof meeting target was found (nNonce/nCycle set). Does NOT process the
 * block; the caller submits via ProcessNewBlock.
 *
 * `abandon` reports that the work in flight is pointless -- for the miner, that a
 * block arrived on the template's parent or the relay pool changed. It is polled
 * from inside the solver, so a stale sweep dies mid-cycle instead of running to
 * completion; see the polling note in the implementation. `on_attempt` fires once
 * per graph attempted.
 *
 * `out_status`, when given, receives the last solver outcome so the caller can
 * tell a healthy miner from a broken one. Every early return sets it, so it is
 * always defined on return; pass it to cuckatoo::SolverFault to interpret it.
 */
bool SolveBlockPoW(ChainstateManager& chainman, CBlock& block, uint64_t& max_tries,
                   bool cpu_fallback = true,
                   const std::function<bool()>& abandon = {},
                   const std::function<void()>& on_attempt = {},
                   cuckatoo::GpuSolveStatus* out_status = nullptr);

//! Policy: may the block miner fall back to the CPU solver at this graph size?
//!
//! True for the sandbox graph (edgebits 19), or when the operator opts in.
//! Mainnet and publictest use the same E28 graph a transfer uses.
//! vault::AllowsTxPowCpuFallback allows the CPU there: a transfer is one
//! bounded solve the sender waits out. agent::AllowsAllotmentCpuFallback is
//! the other per-transaction rule, and it does not: an agent spend starts on
//! the agent's schedule, so that graph is refused unless the operator opts in.
//! Block mining is a continuous race against GPU cards, so this refuses that
//! same graph unless `allow_cpu_mining` is set (`-allowcpumining`, or the
//! desktop checkbox that writes it). The refusal is economic. The CPU can
//! solve the graph; an unbounded grind at near-zero odds would read as a
//! broken miner.
bool CpuBlockMiningAllowed(uint8_t edgebits, bool allow_cpu_mining);

} // namespace node

#endif // QUICKSILVER_NODE_BLOCK_SOLVE_H
