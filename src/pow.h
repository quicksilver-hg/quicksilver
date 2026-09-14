// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_POW_H
#define QUICKSILVER_POW_H

#include <consensus/params.h>

#include <stdint.h>

class CBlock;
class CBlockHeader;
class CBlockIndex;
class CTransaction;
class uint256;
class arith_uint256;

/**
 * Convert nBits value to target.
 *
 * @param[in] nBits     compact representation of the target
 * @param[in] pow_limit PoW limit (consensus parameter)
 *
 * @return              the proof-of-work target or nullopt if the nBits value
 *                      is invalid (due to overflow or exceeding pow_limit)
 */
std::optional<arith_uint256> DeriveTarget(unsigned int nBits, const uint256 pow_limit);

/**
 * Scale a PoW target by numerator/denominator without overflowing 256 bits.
 *
 * Quicksilver's targets sit near 2^255, far above the ~2^234.8 point where
 * `target * (4 * nPowTargetTimespan)` wraps. Multiplying first preserves
 * precision and is used whenever it is safe; above that point the division is
 * applied first instead, which is exact enough because a target that large has
 * hundreds of significant bits to spare. Above ~2^254 a 4x step has no 256-bit
 * representation at all, so the result saturates at the 256-bit maximum; both
 * callers clamp to powLimit, which is what saturation stands in for.
 *
 * Both the retarget calculation and the permitted-transition check must use
 * this, or miners and validators can disagree on the next target.
 */
arith_uint256 ScaleTarget(arith_uint256 target, int64_t numerator, int64_t denominator);

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params&);
unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params&);

/**
 * Check whether a block header satisfies the Cuckatoo proof-of-work: the header's
 * 42-cycle (nCycle) must be a valid cycle in the graph keyed by the pre-pow prefix,
 * AND blake2b(nCycle) must meet the difficulty target (nBits). Takes the whole
 * header (not just a hash) because the cycle and the siphash keys both live there.
 */
bool CheckProofOfWork(const CBlockHeader& header, const Consensus::Params&);
bool CheckProofOfWorkImpl(const CBlockHeader& header, const Consensus::Params&);

/**
 * Quicksilver per-tx PoW target: floor((2^256 - 1) / RequiredTxWork(tx, anchor)),
 * never easier than params.txPowLimit. Takes the transaction because required work
 * is per-transaction after the Stage 2 flag day — see RequiredTxWork.
 */
uint256 GetTxPowTarget(const Consensus::Params& params, const CBlockIndex* anchor,
                       const CTransaction& tx);

/** Quicksilver (#5c-1 Phase 2): the CHAIN-dependent half of the per-tx work floor:
 *  base_coupled(anchor) * m(anchor) / CONGESTION_ONE, clamped >= 1.
 *  base_coupled = average GetBlockProof over nBaseWorkMAWindow blocks back from the
 *  anchor, divided by nTxWorkCouplingK. Pure function of chain state at the anchor —
 *  no transaction is involved, which is why mining status can publish it without
 *  inventing a reference transaction.
 *
 *  The clamp to 1 belongs HERE, not in RequiredTxWork. tools/calibration/model.py
 *  floors max(1, W//K) BEFORE applying the size factor, so decision criteria 1 and 2
 *  were verified against a design where the size term is live from block 1. Moving
 *  the clamp outward would make both Stage 2 terms inert until mean block work
 *  reaches K = 106 cycles (~57 GPUs) — spec finding 3.2 reappearing inside its own
 *  fix. */
arith_uint256 BaseTxWork(const CBlockIndex* anchor, const Consensus::Params& params);

/** Quicksilver Stage 2 flag day: the per-tx work a transaction must carry —
 *  BaseTxWork(anchor) scaled by the transaction-local size and UTXO terms. This is
 *  what consensus checks and what a sender must grind against. */
arith_uint256 RequiredTxWork(const CTransaction& tx, const CBlockIndex* anchor,
                             const Consensus::Params& params);

/** Quicksilver Stage 2 flag day: RequiredTxWork evaluated against a byte count and an
 *  output/input count rather than a finished transaction. A sender grinds BEFORE the
 *  signature bytes exist (PowPreimage covers neither scriptSig nor the witness), so it
 *  must price an upper bound on its final size — and a second copy of the formula in
 *  the vault is exactly the divergence this avoids. Consensus always goes through
 *  RequiredTxWork, which is a thin wrapper over this. */
arith_uint256 RequiredTxWorkForBytes(uint64_t bytes, size_t nout, size_t nin,
                                     const CBlockIndex* anchor,
                                     const Consensus::Params& params);

//! Quicksilver (#5c-1 Phase 2): fixed-point scale for the congestion multiplier m.
//! m == CONGESTION_ONE means multiplier 1.0 (the hardware floor). Integer math only.
static constexpr uint64_t CONGESTION_ONE = 65536; // 2^16

/** Quicksilver (#5c-1 Phase 2): one step of the EIP-1559 congestion recurrence.
 *  Given the parent multiplier prev_m (fixed-point, CONGESTION_ONE == 1.0) and the
 *  connecting block's weight, returns the next multiplier, clamped to
 *  [CONGESTION_ONE, params.nCongestionMaxMultiplier * CONGESTION_ONE].
 *  A 0 target weight or a 0/negative step denom returns the clamped prev_m
 *  (no recurrence) rather than dividing by zero.
 *  Deterministic integer math (consensus-critical: no floating point). */
uint64_t NextCongestionMultiplier(uint64_t prev_m, int64_t block_weight,
                                  const Consensus::Params& params);

/** Quicksilver (#5c-2): actual per-tx work represented by a tx's Cuckatoo proof,
 *  2^256/(proofhash+1) — the GetBlockProof idiom applied to CuckatooProofHash(nCycle).
 *  Pure function of tx.nCycle; does not require the proof to be valid. */
arith_uint256 GetTxActualWork(const CTransaction& tx);

/** Quicksilver (#5c-2): surplus per-tx work above the #5c-1 floor at the tx's anchor,
 *  max(0, GetTxActualWork(tx) - RequiredTxWork(tx, anchor)). The inclusion-ranking key
 *  (the EIP-1559 "tip" analog). Pure function of tx.nCycle + chain state at the anchor. */
arith_uint256 GetTxWorkSurplus(const CTransaction& tx, const CBlockIndex* anchor,
                               const Consensus::Params& params);

/** Quicksilver (#5c-1): resolve and validate a tx's PoW anchor against a branch.
 *  Returns the anchor CBlockIndex (an ancestor of branch_tip at tx.nAnchorHeight)
 *  iff it is on-branch and within [height(branch_tip) - nMaxAnchorAge, height(branch_tip)].
 *  Returns nullptr otherwise. Coinbase txs are not anchored and must not be passed here. */
const CBlockIndex* CheckTxAnchor(const CTransaction& tx, const CBlockIndex* branch_tip,
                                 const Consensus::Params& params);

/**
 * Quicksilver per-tx PoW predicate. Coinbase is exempt. Otherwise requires the
 * tx's nCycle to be a valid 42-cycle in the graph keyed by its PowPreimage(anchor_hash)
 * AND blake2b(nCycle) <= target. anchor_hash is the hash of the on-chain block at
 * tx.nAnchorHeight (resolved by the caller via CheckTxAnchor); a proof ground against
 * a different anchor fails here (precompute resistance). Consensus; ~1 us.
 */
bool CheckTxProofOfWork(const CTransaction& tx, const uint256& target,
                        const uint256& anchor_hash, const Consensus::Params& params);

/** Sentinel returned by GetBlockMintAllowance when the summed allowance leaves
 *  MoneyRange. Negative, so it is itself outside MoneyRange and forces the caller
 *  to reject the block. */
static constexpr CAmount MINT_ALLOWANCE_INVALID{-1};

/**
 * Frame-B mint accounting (#4 / #5c-1). Returns the total per-tx mint a block
 * authorizes: params.nTxPowMint for every NON-coinbase transaction whose anchor
 * resolves against `pprev` (CheckTxAnchor) AND whose per-tx PoW is valid against
 * its anchor-derived target (re-verified via CheckTxProofOfWork — the SAME predicate
 * that gates inclusion, so the mint cannot pay for an unproven or stale-anchored tx
 * regardless of caller ordering). A bad/stale anchor mints nothing. Coinbase is
 * exempt. The running total is MoneyRange-guarded; an out-of-range sum returns
 * MINT_ALLOWANCE_INVALID. Stage 2: the total is then clamped to params.nMaxBlockMint.
 * Both the assembler (node/miner.cpp) and the validator (validation.cpp) reach the cap
 * through THIS function, so there is no second implementation to keep in sync and they
 * cannot disagree about it. This is the sole inflation arithmetic of the coin — keep
 * it pure and independently tested.
 */
CAmount GetBlockMintAllowance(const CBlock& block, const Consensus::Params& params,
                              const CBlockIndex* pprev);

/**
 * Return false if the proof-of-work requirement specified by new_nbits at a
 * given height is not possible, given the proof-of-work on the prior block as
 * specified by old_nbits.
 *
 * This function only checks that the new value is within a factor of 4 of the
 * old value for blocks at the difficulty adjustment interval, and otherwise
 * requires the values to be the same.
 *
 * Always returns true on networks where min difficulty blocks are allowed,
 * such as sandbox/publictest.
 */
bool PermittedDifficultyTransition(const Consensus::Params& params, int64_t height, uint32_t old_nbits, uint32_t new_nbits);

#endif // QUICKSILVER_POW_H
