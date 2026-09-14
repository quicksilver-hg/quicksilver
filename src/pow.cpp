// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pow.h>

#include <arith_uint256.h>
#include <chain.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <crypto/cuckatoo/cuckatoo.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <uint256.h>
#include <util/check.h>

#include <algorithm>
#include <cstdlib>
#include <limits>

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    assert(pindexLast != nullptr);
    unsigned int nProofOfWorkLimit = UintToArith256(params.powLimit).GetCompact();

    // Only change once per difficulty adjustment interval
    if ((pindexLast->nHeight+1) % params.DifficultyAdjustmentInterval() != 0)
    {
        if (params.fPowAllowMinDifficultyBlocks)
        {
            // Special difficulty rule for networks that allow min-difficulty blocks:
            // If the new block's timestamp is more than 2* 10 minutes
            // then allow mining of a min-difficulty block.
            if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing*2)
                return nProofOfWorkLimit;
            else
            {
                // Return the last non-special-min-difficulty-rules-block
                const CBlockIndex* pindex = pindexLast;
                while (pindex->pprev && pindex->nHeight % params.DifficultyAdjustmentInterval() != 0 && pindex->nBits == nProofOfWorkLimit)
                    pindex = pindex->pprev;
                return pindex->nBits;
            }
        }
        return pindexLast->nBits;
    }

    // Go back by exactly one retarget interval. Anchoring on the PREVIOUS period's
    // last block (rather than this period's first) makes the measured span cover
    // `interval` spacings, not `interval - 1`. Two consequences, both wanted:
    // it removes a permanent 0.70% slow bias in the honest case, and it makes every
    // timestamp appear in two consecutive measurements with opposite signs, so a
    // boundary timestamp dragged backwards deflates the earlier period by exactly
    // what it inflates the later one -- which is what closes the timewarp hole and
    // is why this chain carries no MAX_TIMEWARP guard. See F-147.
    int nHeightFirst = pindexLast->nHeight - params.DifficultyAdjustmentInterval();
    // The first retarget has no earlier period; genesis is the earliest anchor that
    // exists. That one period still measures `interval - 1` spacings, once, at the
    // very start of a chain. It self-corrects at the second retarget.
    if (nHeightFirst < 0) nHeightFirst = 0;
    const CBlockIndex* pindexFirst = pindexLast->GetAncestor(nHeightFirst);
    assert(pindexFirst);

    return CalculateNextWorkRequired(pindexLast, pindexFirst->GetBlockTime(), params);
}

arith_uint256 ScaleTarget(arith_uint256 target, int64_t numerator, int64_t denominator)
{
    assert(numerator > 0);
    assert(denominator > 0);
    assert(numerator <= std::numeric_limits<uint32_t>::max());

    const arith_uint256 maximum{~arith_uint256()};
    const arith_uint256 num{static_cast<uint64_t>(numerator)};
    const arith_uint256 den{static_cast<uint64_t>(denominator)};

    if (target <= maximum / num) {
        // Multiplying first is safe here and keeps full precision.
        target *= static_cast<uint32_t>(numerator);
        target /= den;
        return target;
    }

    // Multiplying first would wrap, so divide first. Dividing first truncates,
    // though, and the dropped remainder is NOT harmless: GetCompact() floors the
    // mantissa, so when the target sits exactly on a mantissa boundary — which is
    // where a chain resting at powLimit sits — an absolute loss far below one
    // mantissa unit still costs a whole unit. Left uncarried that ratchets
    // difficulty upward by one unit at every retarget, forever, and the powLimit
    // clamp cannot catch it because that clamp only bounds from above. Carry the
    // remainder through at full precision so ScaleTarget(x, n, n) == x exactly.
    const arith_uint256 quotient{target / den};
    const arith_uint256 remainder{target - quotient * den};

    // Above ~2^254 even dividing first is not enough: a 4x step off a target that
    // large has no 256-bit representation, and wrapping would turn "blocks are
    // slow, ease off" into a huge difficulty INCREASE. Saturate instead — both
    // callers clamp the result to powLimit, which is the answer saturation stands
    // in for.
    if (quotient > maximum / num) return maximum;

    const arith_uint256 scaled{quotient * static_cast<uint32_t>(numerator)};
    // remainder < den, so remainder * num stays far inside 256 bits.
    const arith_uint256 carry{remainder * static_cast<uint32_t>(numerator) / den};
    if (scaled > maximum - carry) return maximum;
    return scaled + carry;
}

unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params& params)
{
    if (params.fPowNoRetargeting)
        return pindexLast->nBits;

    // Limit adjustment step
    int64_t nActualTimespan = pindexLast->GetBlockTime() - nFirstBlockTime;
    if (nActualTimespan < params.nPowTargetTimespan/4)
        nActualTimespan = params.nPowTargetTimespan/4;
    if (nActualTimespan > params.nPowTargetTimespan*4)
        nActualTimespan = params.nPowTargetTimespan*4;

    // Retarget
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    arith_uint256 bnNew;

    bnNew.SetCompact(pindexLast->nBits);

    bnNew = ScaleTarget(bnNew, nActualTimespan, params.nPowTargetTimespan);

    if (bnNew > bnPowLimit)
        bnNew = bnPowLimit;

    return bnNew.GetCompact();
}

// Check that on difficulty adjustments, the new difficulty does not increase
// or decrease beyond the permitted limits.
bool PermittedDifficultyTransition(const Consensus::Params& params, int64_t height, uint32_t old_nbits, uint32_t new_nbits)
{
    if (params.fPowAllowMinDifficultyBlocks) return true;

    if (height % params.DifficultyAdjustmentInterval() == 0) {
        int64_t smallest_timespan = params.nPowTargetTimespan/4;
        int64_t largest_timespan = params.nPowTargetTimespan*4;

        const arith_uint256 pow_limit = UintToArith256(params.powLimit);
        arith_uint256 observed_new_target;
        observed_new_target.SetCompact(new_nbits);

        // Calculate the largest difficulty value possible:
        arith_uint256 largest_difficulty_target;
        largest_difficulty_target.SetCompact(old_nbits);
        largest_difficulty_target = ScaleTarget(largest_difficulty_target, largest_timespan, params.nPowTargetTimespan);

        if (largest_difficulty_target > pow_limit) {
            largest_difficulty_target = pow_limit;
        }

        // Round and then compare this new calculated value to what is
        // observed.
        arith_uint256 maximum_new_target;
        maximum_new_target.SetCompact(largest_difficulty_target.GetCompact());
        if (maximum_new_target < observed_new_target) return false;

        // Calculate the smallest difficulty value possible:
        arith_uint256 smallest_difficulty_target;
        smallest_difficulty_target.SetCompact(old_nbits);
        smallest_difficulty_target = ScaleTarget(smallest_difficulty_target, smallest_timespan, params.nPowTargetTimespan);

        if (smallest_difficulty_target > pow_limit) {
            smallest_difficulty_target = pow_limit;
        }

        // Round and then compare this new calculated value to what is
        // observed.
        arith_uint256 minimum_new_target;
        minimum_new_target.SetCompact(smallest_difficulty_target.GetCompact());
        if (minimum_new_target > observed_new_target) return false;
    } else if (old_nbits != new_nbits) {
        return false;
    }
    return true;
}

// Bypasses the actual proof of work check during fuzz testing with a simplified validation checking whether
// the most significant bit of the last byte of the hash is set.
bool CheckProofOfWork(const CBlockHeader& header, const Consensus::Params& params)
{
    if constexpr (G_FUZZING) return (header.nNonce & 0x80u) == 0;
    return CheckProofOfWorkImpl(header, params);
}

std::optional<arith_uint256> DeriveTarget(unsigned int nBits, const uint256 pow_limit)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(pow_limit))
        return {};

    return bnTarget;
}

bool CheckProofOfWorkImpl(const CBlockHeader& header, const Consensus::Params& params)
{
    // Quicksilver: the genesis block is a hardcoded trust anchor — its hash is
    // asserted at compile time in chainparams, a stronger guarantee than PoW —
    // and is exempt from the PoW check, consistent with AcceptBlockHeader's
    // existing genesis special-case. Only the genesis can have this hash, so this
    // is collision-safe. This lets a fresh fork carry a deterministic zero-cycle
    // genesis while normal Cuckatoo PoW begins at height 1.
    if (header.GetHash() == params.hashGenesisBlock) return true;

    // (1) Difficulty threshold on the found cycle's hash.
    const auto bnTarget{DeriveTarget(header.nBits, params.powLimit)};
    if (!bnTarget) return false;
    if (UintToArith256(cuckatoo::CuckatooProofHash(header.nCycle)) > *bnTarget)
        return false;

    // Quicksilver: sandbox (and only sandbox) keeps upstream's trivial-but-real PoW —
    // the target check above stands, but the expensive real-cycle requirement below is
    // skipped. Block-cycle solve/verify is still covered by cuckatoo_tests; main/publictest
    // keep fBlockPowNoCycle false and run the full check. See consensus/params.h.
    if (params.fBlockPowNoCycle) return true;

    // (2) The cycle must be a real 42-cycle in the graph keyed by the pre-pow.
    const auto pre = header.PrePowBytes();
    const cuckatoo::Keys keys = cuckatoo::CuckatooSetHeader(pre.data(), pre.size());
    return cuckatoo::CuckatooVerify(header.nCycle, keys, params.nEdgeBits);
}

arith_uint256 BaseTxWork(const CBlockIndex* anchor, const Consensus::Params& params)
{
    if (anchor == nullptr) return arith_uint256(1);

    // base_coupled = average blockwork over the MA window back from the anchor, / K.
    arith_uint256 sum{0};
    int n = 0;
    for (const CBlockIndex* p = anchor; p != nullptr && n < params.nBaseWorkMAWindow;
         p = p->pprev, ++n) {
        sum += GetBlockProof(*p);
    }
    if (n == 0) return arith_uint256(1);
    arith_uint256 base = (sum / arith_uint256(n)) / arith_uint256(params.nTxWorkCouplingK);

    // required = base * m / CONGESTION_ONE   (m >= CONGESTION_ONE, so required >= base).
    uint64_t m = anchor->m_congestion;
    if (m < CONGESTION_ONE) m = CONGESTION_ONE;     // defensive: never below the floor
    // arith_uint256 multiplication wraps SILENTLY, and a wrapped product is SMALLER,
    // which would be a free-transaction bug. Guard the base * m product before the
    // divide. m is at least CONGESTION_ONE after the floor above, so the divisor
    // is never zero. Real bases are nowhere near this bound; the guard exists
    // because "cannot happen" is not something consensus code may assume.
    if (base > (~arith_uint256(0)) / arith_uint256(m)) return ~arith_uint256(0);
    arith_uint256 required = base * arith_uint256(m) / arith_uint256(CONGESTION_ONE);
    if (required == 0) required = arith_uint256(1); // never divide by zero in the target step
    return required;
}

arith_uint256 RequiredTxWorkForBytes(uint64_t bytes, size_t nout, size_t nin,
                                     const CBlockIndex* anchor,
                                     const Consensus::Params& params)
{
    const arith_uint256 base = BaseTxWork(anchor, params);

    // Net UTXO creation, clamped at zero. A negative delta is not credited: an input
    // costs ~148 bytes, which is 0.031 of base on the byte term, while a refund would
    // be 1/U -- so at the low end of the admissible U window adding inputs would
    // LOWER required work until the one-cycle floor caught it, and the window's own
    // lower bound would then depend on an input-size estimate rather than on anything
    // measured. vout.size() is counted plainly, without excluding provably-unspendable
    // outputs: script parsing does not belong in consensus arithmetic, and the error
    // runs in the safe direction -- an OP_RETURN spammer is over-charged, never under.
    const uint64_t utxo_delta = nout > nin ? uint64_t(nout - nin) : 0;

    const arith_uint256 r_b{uint64_t(params.nTxWorkRefBytes)};
    const arith_uint256 u{uint64_t(params.nTxUtxoRefCount)};
    const arith_uint256 denom = r_b * u;
    if (denom == 0) return base; // defensive: a zero reference prices nothing

    // ONE division. Summing two separately-floored quotients loses precision twice --
    // the class of defect behind finding 3.2 and the W = 2K-1 correction, which are
    // this same integer division in two other places.
    const arith_uint256 factor = arith_uint256(bytes) * u + arith_uint256(utxo_delta) * r_b;

    // arith_uint256 multiplication wraps SILENTLY, and a wrapped product is SMALLER,
    // which would be a free-transaction bug. The factor reaches ~1e9, so only an
    // absurd base could reach the bound; the guard exists because "cannot happen" is
    // not something consensus code may assume.
    if (factor != 0) {
        const arith_uint256 headroom = ((~arith_uint256(0)) - (denom - arith_uint256(1))) / factor;
        if (base > headroom) return ~arith_uint256(0);
    }

    // Round UP. Flooring would let an attacker slice their bytes into 2*R_b - 1-byte
    // transactions, each charged one unit while carrying nearly two, and fill a block
    // for half the honest price -- the W = 2K-1 pathology one level down, measured at
    // 0.5024 in tools/calibration/stage2-byte-pricing.md. Rounding up charges at least
    // bytes/R_b for every slicing, because a sum of ceilings is never below the
    // ceiling of the sum, so the fill >= mine guarantee does not depend on how the
    // attacker cuts their bytes.
    arith_uint256 required = (base * factor + denom - arith_uint256(1)) / denom;
    if (required == 0) required = arith_uint256(1);
    return required;
}

arith_uint256 RequiredTxWork(const CTransaction& tx, const CBlockIndex* anchor,
                             const Consensus::Params& params)
{
    // WITH-witness serialization: the segwit marker and flag, the witness data, and
    // the 176-byte transaction PoW tail are all bytes an operator stores forever, so
    // they are all bytes the sender pays for. The non-witness serialization is not
    // used anywhere in this rule; that ambiguity is what the weight term concealed.
    return RequiredTxWorkForBytes(uint64_t(::GetSerializeSize(TX_WITH_WITNESS(tx))),
                                  tx.vout.size(), tx.vin.size(), anchor, params);
}

arith_uint256 GetTxActualWork(const CTransaction& tx)
{
    const arith_uint256 h = UintToArith256(cuckatoo::CuckatooProofHash(tx.nCycle));
    // Mirror GetBlockProof: compute 2^256/(h+1) as ~h/(h+1) + 1. Guard the degenerate
    // maximum where h+1 overflows to 0 (h == 2^256-1), as GetBlockProof guards bnTarget==0.
    if (h + 1 == arith_uint256(0)) return arith_uint256(1);
    return (~h / (h + 1)) + 1;
}

arith_uint256 GetTxWorkSurplus(const CTransaction& tx, const CBlockIndex* anchor,
                               const Consensus::Params& params)
{
    const arith_uint256 actual = GetTxActualWork(tx);
    // Stage 2: the floor is the TRANSACTION's own requirement, not a shared scalar.
    // That is what keeps the relay ranking honest once transactions of different
    // sizes owe different work — otherwise a bloat transaction would out-rank a
    // payment simply by being charged the same floor.
    const arith_uint256 floor = RequiredTxWork(tx, anchor, params);
    return (actual > floor) ? (actual - floor) : arith_uint256(0);
}

uint256 GetTxPowTarget(const Consensus::Params& params, const CBlockIndex* anchor,
                       const CTransaction& tx)
{
    if (anchor == nullptr) return params.txPowLimit; // no anchor context: permissive

    const arith_uint256 required = RequiredTxWork(tx, anchor, params);

    // target = floor((2^256 - 1) / required): the easiest proof hash that still represents
    // `required` expected attempts. Use UINT256_MAX/required directly — NOT the GetBlockProof
    // 2^256/(x+1) idiom — because that idiom maps required==1 to 2^255 (half the space), which
    // makes a ~zero base_coupled (permissive K) wrongly bite ~half of all proofs. With this
    // form required==1 maps to UINT256_MAX, so the permissive floor is genuinely permissive.
    const arith_uint256 target = (~arith_uint256(0)) / required;

    // txPowLimit is the absolute easiest target: congestion only makes work HARDER.
    const arith_uint256 limit = UintToArith256(params.txPowLimit);
    return ArithToUint256(target < limit ? target : limit);
}

uint64_t NextCongestionMultiplier(uint64_t prev_m, int64_t block_weight,
                                  const Consensus::Params& params)
{
    const uint64_t floor_m = CONGESTION_ONE;
    const uint64_t cap_m   = uint64_t(params.nCongestionMaxMultiplier) * CONGESTION_ONE;

    // target_weight = MAX_BLOCK_WEIGHT * T (permille). Guard a 0 target (would
    // divide by zero) and a 0/negative step denom (0 divides; negative becomes
    // a huge uint64 and silently no-ops). Same policy: no recurrence, clamp
    // prev_m. Shipped denoms are 4/4/8.
    const int64_t target_weight =
        int64_t(MAX_BLOCK_WEIGHT) * params.nCongestionTargetPermille / 1000;
    if (target_weight <= 0 || params.nCongestionStepDenom <= 0) {
        return std::clamp(prev_m, floor_m, cap_m);
    }

    // delta = prev_m * |weight - target| / target / step_denom   (all uint64).
    // Bound: prev_m <= cap (~4.2e6). |weight-target| <= MAX_BLOCK_WEIGHT (4e6)
    // for any block that passed ContextualCheckBlock, so the product is
    // < 1.7e13 << 2^64. ConnectBlock does not re-run that check
    // (-reindex-chainstate skips ContextualCheckBlock); the bound still holds
    // on replay of our own datadir because AcceptBlock already enforced the
    // weight cap when the block was stored, and LoadExternalBlockFile
    // (-reindex) goes through AcceptBlock. This function is not itself a
    // weight cap.
    const uint64_t diff = uint64_t(std::llabs(block_weight - target_weight));
    const uint64_t step = uint64_t(params.nCongestionStepDenom);
    const uint64_t delta = prev_m * diff / uint64_t(target_weight) / step;

    uint64_t next_m = (block_weight > target_weight) ? prev_m + delta
                                                     : prev_m - std::min(prev_m, delta);
    return std::clamp(next_m, floor_m, cap_m);
}

const CBlockIndex* CheckTxAnchor(const CTransaction& tx, const CBlockIndex* branch_tip,
                                 const Consensus::Params& params)
{
    if (branch_tip == nullptr) return nullptr;
    const int64_t a = tx.nAnchorHeight;
    const int tip_h = branch_tip->nHeight;
    if (a > tip_h) return nullptr;                                 // future anchor
    if (a < int64_t(tip_h) - params.nMaxAnchorAge) return nullptr; // too old (outside W)
    const CBlockIndex* anchor = branch_tip->GetAncestor(int(a));
    if (anchor == nullptr || anchor->nHeight != int(a)) return nullptr; // off-branch
    return anchor;
}

bool CheckTxProofOfWork(const CTransaction& tx, const uint256& target,
                        const uint256& anchor_hash, const Consensus::Params& params)
{
    if (tx.IsCoinBase()) return true; // exempt — the coinbase is the miner's, not a per-tx proof

    if (UintToArith256(cuckatoo::CuckatooProofHash(tx.nCycle)) > UintToArith256(target))
        return false;

    // Quicksilver (test-only, opt-in): skip the expensive cycle verification, keeping
    // the target threshold above. Mirrors fBlockPowNoCycle for the per-tx proof so
    // block-assembly tests can inject many txs without an infeasible per-tx grind.
    if (params.fTxPowNoCycle) return true;

    const auto pre = tx.PowPreimage(anchor_hash);
    const cuckatoo::Keys keys = cuckatoo::CuckatooSetHeader(pre.data(), pre.size());
    return cuckatoo::CuckatooVerify(tx.nCycle, keys, params.nTxEdgeBits);
}

CAmount GetBlockMintAllowance(const CBlock& block, const Consensus::Params& params,
                              const CBlockIndex* pprev)
{
    CAmount total{0};
    for (const auto& txref : block.vtx) {
        if (!txref) continue;            // null placeholder (block mid-assembly) mints nothing
        const CTransaction& tx = *txref;
        if (tx.IsCoinBase()) continue;                          // coinbase mints nothing
        const CBlockIndex* anchor = CheckTxAnchor(tx, pprev, params);
        if (anchor == nullptr) continue;                        // bad/stale anchor mints nothing
        const uint256 target = GetTxPowTarget(params, anchor, tx);
        if (!CheckTxProofOfWork(tx, target, anchor->GetBlockHash(), params)) continue; // only valid proofs mint
        total += params.nTxPowMint;
        if (!MoneyRange(total)) return MINT_ALLOWANCE_INVALID;
    }
    // Decision criterion 4: maximum issuance must not depend on transaction shape.
    // Without this, a block of minimum-size transactions mints proportionally more
    // than a block of realistic ones -- M4 measured a 357x span -- and nTxPowMint's
    // "2 COIN at a full block" design property rests on an ASSUMED 1,143-byte
    // transaction rather than on a rule. The MoneyRange guard stays ahead of the
    // clamp: an out-of-range sum is a malformed block, not a capped one.
    return std::min(total, params.nMaxBlockMint);
}
