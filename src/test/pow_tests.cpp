// Copyright (c) 2015-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <common/args.h>
#include <crypto/cuckatoo/cuckatoo.h>
#include <pow.h>
#include <primitives/block.h>
#include <script/script.h>
#include <streams.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <util/chaintype.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <string>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(pow_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(scale_target_matches_naive_math_below_the_overflow_ceiling)
{
    // Below the overflow ceiling the helper must be bit-identical to the
    // original `target *= n; target /= d;` arithmetic it replaces.
    arith_uint256 target;
    target.SetCompact(0x1d00ffff);

    arith_uint256 naive{target};
    naive *= 302400u;                 // half of a 7-day timespan, in seconds
    naive /= arith_uint256(604800u);

    BOOST_CHECK(ScaleTarget(target, 302400, 604800) == naive);
}

BOOST_AUTO_TEST_CASE(scale_target_does_not_overflow_in_quicksilver_regime)
{
    // A target near 2^255 is the regime Quicksilver mainnet occupies. The
    // naive multiply wraps here; the helper must not. Scaling by 4x must
    // produce a value strictly greater than the input, never a wrapped
    // smaller one.
    const arith_uint256 target{UintToArith256(uint256{"7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"})};

    const arith_uint256 scaled_up{ScaleTarget(target, 2419200, 604800)}; // 4x
    BOOST_CHECK(scaled_up > target);

    const arith_uint256 scaled_down{ScaleTarget(target, 151200, 604800)}; // 0.25x
    BOOST_CHECK(scaled_down < target);
    BOOST_CHECK(scaled_down > arith_uint256(0));
}

BOOST_AUTO_TEST_CASE(scale_target_saturates_rather_than_wrapping_when_scaling_up)
{
    // Scaling a target UP must never yield a smaller value. Above ~2^254 a 4x
    // step is not representable in 256 bits at all, so the only safe answer is
    // saturation at the 256-bit maximum — which both callers then clamp to
    // powLimit. If it wraps instead, "blocks are too slow, make it easier"
    // silently becomes a massive difficulty INCREASE: a death spiral on a
    // low-hashrate chain, and unfixable after launch.
    const int64_t timespan{144 * 5 * 60};
    const arith_uint256 maximum{~arith_uint256()};

    for (const arith_uint256& target : {
             UintToArith256(uint256{"2000000000000000000000000000000000000000000000000000000000000000"}), // 2^253
             UintToArith256(uint256{"4000000000000000000000000000000000000000000000000000000000000000"}), // 2^254
             UintToArith256(uint256{"4000000000000100000000000000000000000000000000000000000000000000"}), // 2^254+2^200
             UintToArith256(uint256{"7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"}), // 2^255-1
             UintToArith256(uint256{"8000000000000000000000000000000100000000000000000000000000000000"}), // 2^255+2^128
         }) {
        const arith_uint256 scaled{ScaleTarget(target, timespan * 4, timespan)};
        BOOST_CHECK_MESSAGE(scaled >= target,
                            "scaling " << target.ToString() << " up by 4x produced the SMALLER value "
                                       << scaled.ToString());
        BOOST_CHECK(scaled <= maximum);
    }
}

BOOST_AUTO_TEST_CASE(scale_target_is_monotonic_across_the_overflow_ceiling)
{
    // Walking the numerator up must never make the result go down. A wrapping
    // multiply shows up here as a discontinuity.
    const arith_uint256 target{UintToArith256(uint256{"0fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"})};
    arith_uint256 previous{ScaleTarget(target, 151200, 604800)};

    for (int64_t numerator = 302400; numerator <= 2419200; numerator += 302400) {
        const arith_uint256 current{ScaleTarget(target, numerator, 604800)};
        BOOST_CHECK(current >= previous);
        previous = current;
    }
}

BOOST_AUTO_TEST_CASE(scale_target_is_exact_when_numerator_equals_denominator)
{
    // Scaling by 1 must be the identity, on BOTH sides of the overflow ceiling.
    // Above it ScaleTarget has to divide before it multiplies, and dropping the
    // division remainder there is not a harmless rounding: GetCompact() floors the
    // mantissa, so a target sitting exactly on a mantissa boundary loses a WHOLE
    // mantissa unit to an absolute loss of a few thousand. A chain resting at
    // powLimit sits on exactly such a boundary, so an uncarried remainder ratchets
    // difficulty upward one unit per retarget, forever -- and the powLimit clamp
    // cannot catch it, because that clamp only bounds from above.
    const int64_t timespan{144 * 5 * 60};

    // Below the ceiling: multiply-first, always was exact.
    const arith_uint256 small{UintToArith256(uint256{"00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff"})};
    BOOST_CHECK(ScaleTarget(small, timespan, timespan) == small);

    // Above the ceiling: divide-first. This is the regression.
    const arith_uint256 pow_limit{UintToArith256(uint256{"3fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"})};
    BOOST_CHECK(pow_limit > (~arith_uint256()) / arith_uint256{static_cast<uint64_t>(timespan)});
    BOOST_CHECK(ScaleTarget(pow_limit, timespan, timespan) == pow_limit);

    // And on the mantissa boundary a chain at powLimit actually occupies: the
    // compact round-trip of powLimit, whose low 232 bits are all zero.
    arith_uint256 on_boundary;
    on_boundary.SetCompact(pow_limit.GetCompact());
    BOOST_CHECK_EQUAL(ScaleTarget(on_boundary, timespan, timespan).GetCompact(), on_boundary.GetCompact());
}

// NOTE each vector's elapsed time (pindexLast.nTime - nLastRetargetTime) is halved,
// preserving the actual/target ratio the result depends on. nLastRetargetTime is
// therefore synthetic (no longer a real block time); pindexLast.nTime is kept as-is.
// That keeps upstream's authoritative expected_nbits as independent ground truth for
// the retarget formula (new = old * actualTimespan / nPowTargetTimespan, clamped
// to [T/4, T*4] then powLimit) rather than re-snapshotting our own output.
//
// They therefore run against an explicit 604800s consensus, NOT mainnet's. They
// used to read CreateChainParams(MAIN) because mainnet happened to use that same
// timespan; that coincidence ended when P0a moved mainnet to a 144-block (43200s)
// retarget window. The 604800s figure is load-bearing for these expected values —
// under a 43200s timespan every vector's elapsed time exceeds the 4x clamp and
// the ratio the vector encodes is destroyed — so it is now stated here instead of
// being inherited from a chain policy value that is free to move. The vectors
// keep testing the arithmetic; mainnet's chosen window is covered separately by
// publictest_matches_mainnet_retarget_and_pow_params and the high-target regime tests.
static Consensus::Params UpstreamRegimeRetargetParams()
{
    Consensus::Params params{};
    params.powLimit = uint256{"00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
    params.nPowTargetSpacing = 5 * 60;
    params.nPowTargetTimespan = 2016 * 5 * 60; // 604800s — see the note above
    params.fPowAllowMinDifficultyBlocks = false;
    params.fPowNoRetargeting = false;
    return params;
}

/* Test calculation of next difficulty target with no constraints applying */
BOOST_AUTO_TEST_CASE(get_next_work)
{
    const Consensus::Params params{UpstreamRegimeRetargetParams()};
    int64_t nLastRetargetTime = 1261641450; // pindexLast.nTime - 511289 (= 1022578/2)
    CBlockIndex pindexLast;
    pindexLast.nHeight = 32255;
    pindexLast.nTime = 1262152739;  // Block #32255
    pindexLast.nBits = 0x1d00ffff;

    // Here (and below): expected_nbits is calculated in
    // CalculateNextWorkRequired(); redoing the calculation here would be just
    // reimplementing the same code that is written in pow.cpp. Rather than
    // copy that code, we just hardcode the expected result.
    unsigned int expected_nbits = 0x1d00d86aU;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, params), expected_nbits);
    BOOST_CHECK(PermittedDifficultyTransition(params, pindexLast.nHeight+1, pindexLast.nBits, expected_nbits));
}

/* Test the constraint on the upper bound for next work */
BOOST_AUTO_TEST_CASE(get_next_work_pow_limit)
{
    const Consensus::Params params{UpstreamRegimeRetargetParams()};
    int64_t nLastRetargetTime = 1232034251; // pindexLast.nTime - 1027745 (= 2055491/2)
    CBlockIndex pindexLast;
    pindexLast.nHeight = 2015;
    pindexLast.nTime = 1233061996;  // Block #2015
    pindexLast.nBits = 0x1d00ffff;
    unsigned int expected_nbits = 0x1d00ffffU;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, params), expected_nbits);
    BOOST_CHECK(PermittedDifficultyTransition(params, pindexLast.nHeight+1, pindexLast.nBits, expected_nbits));
}

/* Test the constraint on the lower bound for actual time taken */
BOOST_AUTO_TEST_CASE(get_next_work_lower_limit_actual)
{
    const Consensus::Params params{UpstreamRegimeRetargetParams()};
    int64_t nLastRetargetTime = 1279152954; // pindexLast.nTime - 144717 (= 289434/2)
    CBlockIndex pindexLast;
    pindexLast.nHeight = 68543;
    pindexLast.nTime = 1279297671;  // Block #68543
    pindexLast.nBits = 0x1c05a3f4;
    unsigned int expected_nbits = 0x1c0168fdU;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, params), expected_nbits);
    BOOST_CHECK(PermittedDifficultyTransition(params, pindexLast.nHeight+1, pindexLast.nBits, expected_nbits));
    // Test that reducing nbits further would not be a PermittedDifficultyTransition.
    unsigned int invalid_nbits = expected_nbits-1;
    BOOST_CHECK(!PermittedDifficultyTransition(params, pindexLast.nHeight+1, pindexLast.nBits, invalid_nbits));
}

/* Test the constraint on the upper bound for actual time taken */
BOOST_AUTO_TEST_CASE(get_next_work_upper_limit_actual)
{
    const Consensus::Params params{UpstreamRegimeRetargetParams()};
    int64_t nLastRetargetTime = 1266187443; // pindexLast.nTime - 3024000 (= 6048000/2)
    CBlockIndex pindexLast;
    pindexLast.nHeight = 46367;
    pindexLast.nTime = 1269211443;  // Block #46367
    pindexLast.nBits = 0x1c387f6f;
    unsigned int expected_nbits = 0x1d00e1fdU;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, params), expected_nbits);
    BOOST_CHECK(PermittedDifficultyTransition(params, pindexLast.nHeight+1, pindexLast.nBits, expected_nbits));
    // Test that increasing nbits further would not be a PermittedDifficultyTransition.
    unsigned int invalid_nbits = expected_nbits+1;
    BOOST_CHECK(!PermittedDifficultyTransition(params, pindexLast.nHeight+1, pindexLast.nBits, invalid_nbits));
}

//! A synthetic mainnet-shaped consensus for retarget tests in Quicksilver's own
//! high-target regime. Deliberately NOT CreateChainParams: these must keep
//! testing the arithmetic even after the launch floor is chosen in P0b.
static Consensus::Params HighTargetRetargetParams(int64_t timespan)
{
    Consensus::Params params{};
    params.powLimit = uint256{"7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
    params.nPowTargetSpacing = 5 * 60;
    params.nPowTargetTimespan = timespan;
    params.fPowAllowMinDifficultyBlocks = false;
    params.fPowNoRetargeting = false;
    return params;
}

BOOST_AUTO_TEST_CASE(retarget_in_high_target_regime_raises_difficulty_when_blocks_are_fast)
{
    // Blocks arrived in half the intended time => difficulty must roughly
    // double => the target must roughly halve. In the pre-fix code the
    // multiply wrapped here and produced a nonsense target.
    const int64_t timespan{144 * 5 * 60};
    const Consensus::Params params{HighTargetRetargetParams(timespan)};

    arith_uint256 start;
    start.SetCompact(0x1f00ffff);

    CBlockIndex pindexLast;
    pindexLast.nHeight = 143;
    pindexLast.nTime = 1783123200 + timespan / 2;
    pindexLast.nBits = start.GetCompact();

    arith_uint256 result;
    result.SetCompact(CalculateNextWorkRequired(&pindexLast, 1783123200, params));

    BOOST_CHECK(result < start);
    BOOST_CHECK(result > start / arith_uint256(4));
}

BOOST_AUTO_TEST_CASE(retarget_in_high_target_regime_lowers_difficulty_when_blocks_are_slow)
{
    const int64_t timespan{144 * 5 * 60};
    const Consensus::Params params{HighTargetRetargetParams(timespan)};

    arith_uint256 start;
    start.SetCompact(0x1f00ffff);

    CBlockIndex pindexLast;
    pindexLast.nHeight = 143;
    pindexLast.nTime = 1783123200 + timespan * 2;
    pindexLast.nBits = start.GetCompact();

    arith_uint256 result;
    result.SetCompact(CalculateNextWorkRequired(&pindexLast, 1783123200, params));

    BOOST_CHECK(result > start);
}

BOOST_AUTO_TEST_CASE(retarget_is_clamped_to_powlimit_in_high_target_regime)
{
    // An absurdly slow period must clamp at powLimit, not wrap past it.
    const int64_t timespan{144 * 5 * 60};
    const Consensus::Params params{HighTargetRetargetParams(timespan)};

    CBlockIndex pindexLast;
    pindexLast.nHeight = 143;
    pindexLast.nTime = 1783123200 + timespan * 1000;
    pindexLast.nBits = UintToArith256(params.powLimit).GetCompact();

    arith_uint256 result;
    result.SetCompact(CalculateNextWorkRequired(&pindexLast, 1783123200, params));

    BOOST_CHECK(result <= UintToArith256(params.powLimit));
}

BOOST_AUTO_TEST_CASE(calculated_retarget_is_always_a_permitted_transition)
{
    // THE agreement property: whatever the miner-side calculation produces must
    // always be inside the validator-side permitted range. If these two ever
    // disagree, honest miners produce headers the network rejects. Swept across
    // the full clamp range in the high-target regime.
    const int64_t timespan{144 * 5 * 60};
    const Consensus::Params params{HighTargetRetargetParams(timespan)};
    const int64_t height{params.DifficultyAdjustmentInterval()};

    for (const uint32_t start_nbits : {0x1f00ffffu, 0x1e00ffffu, 0x1d00ffffu, 0x2000ffffu}) {
        for (const int64_t elapsed : {timespan / 8, timespan / 2, timespan, timespan * 2, timespan * 8}) {
            CBlockIndex pindexLast;
            pindexLast.nHeight = static_cast<int>(height - 1);
            pindexLast.nTime = 1783123200 + elapsed;
            pindexLast.nBits = start_nbits;

            const uint32_t next_nbits{CalculateNextWorkRequired(&pindexLast, 1783123200, params)};
            BOOST_CHECK_MESSAGE(
                PermittedDifficultyTransition(params, height, start_nbits, next_nbits),
                "calculated nBits " << next_nbits << " rejected as a permitted transition from "
                                    << start_nbits << " after " << elapsed << "s");
        }
    }
}

//! Build `count` linked, synthetic block indices starting at `start_time`, each
//! `spacing` seconds after the last, all carrying `nbits`. Heights are 0..count-1.
//! Only nHeight/nTime/nBits/pprev are read by the retarget path.
static std::vector<CBlockIndex> LinkedChain(size_t count, int64_t start_time, int64_t spacing, uint32_t nbits)
{
    std::vector<CBlockIndex> chain(count);
    for (size_t i = 0; i < count; ++i) {
        chain[i].nHeight = static_cast<int>(i);
        chain[i].nTime = static_cast<unsigned int>(start_time + static_cast<int64_t>(i) * spacing);
        chain[i].nBits = nbits;
        chain[i].pprev = (i == 0) ? nullptr : &chain[i - 1];
    }
    return chain;
}

BOOST_AUTO_TEST_CASE(retarget_span_covers_exactly_one_full_interval)
{
    // A chain arriving at EXACTLY the target spacing must retarget to exactly the
    // same nBits. Before F-147 the anchor was this period's FIRST block, so the
    // span covered interval-1 spacings (143 * 300 = 42900) while the divisor was
    // interval spacings (43200) -- a 0.70% permanent bias with no attacker.
    //
    // This uses the SECOND retarget (pindexLast->nHeight == 287, anchor 143), not
    // the first: at the first retarget the new anchor clamps to genesis and that
    // period legitimately still measures interval-1 spacings.
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    const int64_t interval{consensus.DifficultyAdjustmentInterval()};
    BOOST_CHECK_EQUAL(interval, 144);

    const uint32_t start_nbits{UintToArith256(consensus.powLimit).GetCompact()};
    auto chain{LinkedChain(2 * static_cast<size_t>(interval), 1788566400, consensus.nPowTargetSpacing, start_nbits)};

    const CBlockIndex& last{chain[2 * static_cast<size_t>(interval) - 1]};
    BOOST_CHECK_EQUAL(last.nHeight, 287);
    BOOST_CHECK_EQUAL((last.nHeight + 1) % interval, 0); // this really is a retarget height

    CBlockHeader dummy;
    BOOST_CHECK_EQUAL(GetNextWorkRequired(&last, &dummy, consensus), start_nbits);
}

BOOST_AUTO_TEST_CASE(lowering_a_boundary_timestamp_cannot_suppress_difficulty)
{
    // THE property that replaces MAX_TIMEWARP: no timestamp an attacker controls
    // can leave the chain EASIER than an honest one.
    //
    // Before F-147 each period was measured from its own FIRST block, so the block
    // that served as a period's anchor (heights 144, 288, ...) was an endpoint of
    // exactly ONE measurement. Dragging it backwards inflated that span with
    // nothing anywhere to deflate -- a free difficulty cut, available every period.
    // Bounding that is the whole job MAX_TIMEWARP was doing.
    //
    // With the anchor moved back one block, consecutive spans SHARE an endpoint. A
    // shared boundary dragged back by D shortens the earlier span by D and
    // lengthens the later one by D, and because the two spans sum to a constant the
    // product of the two scalings is maximised at D == 0:
    //     (S1-D)(S2+D) = S1*S2 - D*(S1-S2) - D^2  <=  S1*S2   when S1 == S2.
    // Every other block is now an endpoint of NO measurement, so moving it does
    // nothing at all.
    //
    // Hence the sweep rather than one hand-picked block: the claim is universal,
    // and picking a single block is how this test goes wrong. Dragging height 287
    // -- the obvious choice -- passes against the pre-F-147 code too, because 287
    // was that scheme's period END, which it also protected. The blocks that
    // separate the two schemes are the ANCHORS, 144 and 288, and a test that does
    // not touch them proves nothing.
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    const int interval{static_cast<int>(consensus.DifficultyAdjustmentInterval())};
    const int count{3 * interval}; // heights 0..431: three retargets, two shared boundaries

    // Start 256x harder than powLimit. Resting at powLimit leaves no headroom in
    // the "easier" direction, so the clamp in CalculateNextWorkRequired would be
    // doing part of the work this test means to attribute to the anchor. powLimit's
    // mantissa survives the shift exactly, so the start is still on a compact
    // boundary and ScaleTarget's divide-first path still runs.
    arith_uint256 start_target{UintToArith256(consensus.powLimit)};
    start_target >>= 8;
    const uint32_t start_nbits{start_target.GetCompact()};

    // 10% of the 43200s timespan: far inside the 4x clamp, and far above compact
    // mantissa rounding, so a real effect cannot round away into a false pass.
    const int64_t drag{consensus.nPowTargetTimespan / 10};

    CBlockHeader dummy;
    // Drag exactly one timestamp back by `drag` (none when `dragged` is negative),
    // then run the chain through all three retargets, carrying each result forward
    // as the next period's base exactly as a real chain does. Returns the target the
    // chain ends on -- a LARGER target is an EASIER chain.
    auto final_target = [&](int dragged) {
        auto chain{LinkedChain(static_cast<size_t>(count), 1788566400, consensus.nPowTargetSpacing, start_nbits)};
        if (dragged >= 0) chain[static_cast<size_t>(dragged)].nTime -= static_cast<unsigned int>(drag);
        uint32_t nbits{start_nbits};
        for (int h{interval - 1}; h < count; h += interval) {
            nbits = GetNextWorkRequired(&chain[static_cast<size_t>(h)], &dummy, consensus);
            for (int i{h + 1}; i < count; ++i) chain[static_cast<size_t>(i)].nBits = nbits;
        }
        arith_uint256 target;
        target.SetCompact(nbits);
        return target;
    };

    const arith_uint256 honest{final_target(-1)};
    // The honest chain must still have room to get easier, or the powLimit clamp
    // rather than the anchor would be what makes the sweep below pass.
    BOOST_CHECK(honest < UintToArith256(consensus.powLimit));

    // Every block except genesis -- whose timestamp is fixed by the chainparams --
    // and the tip, which is the attacker's own newest block and becomes a shared
    // boundary at the very next retarget.
    int strictly_harder{0};
    for (int i{1}; i < count - 1; ++i) {
        const arith_uint256 attacked{final_target(i)};
        BOOST_CHECK_MESSAGE(attacked <= honest,
                            "dragging height " << i << " back by " << drag << "s left the chain easier than honest");
        if (attacked < honest) ++strictly_harder;
    }

    // Exactly the two shared boundaries in range -- heights 143 and 287 -- may move
    // the result at all, and each must cost the attacker something. Without this the
    // sweep would pass vacuously on a chain where no timestamp mattered.
    BOOST_CHECK_EQUAL(strictly_harder, 2);
}

BOOST_AUTO_TEST_CASE(first_retarget_anchors_on_genesis)
{
    // The only period with no earlier boundary. The anchor must clamp to height 0
    // rather than reach for a negative height.
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    const int64_t interval{consensus.DifficultyAdjustmentInterval()};
    const uint32_t start_nbits{UintToArith256(consensus.powLimit).GetCompact()};

    auto chain{LinkedChain(static_cast<size_t>(interval), 1788566400, consensus.nPowTargetSpacing, start_nbits)};
    const CBlockIndex& last{chain[static_cast<size_t>(interval) - 1]};
    BOOST_CHECK_EQUAL(last.nHeight, 143);

    CBlockHeader dummy;
    // Must not abort. The result is the acknowledged first-period artifact: this one
    // period measures interval-1 spacings, because nothing precedes genesis. It
    // self-corrects at the second retarget, which anchors on height 143.
    const uint32_t next{GetNextWorkRequired(&last, &dummy, consensus)};
    arith_uint256 before, after;
    before.SetCompact(start_nbits);
    after.SetCompact(next);
    BOOST_CHECK(after < before); // 143 spacings inside a 144-spacing window reads as "fast"
    BOOST_CHECK(PermittedDifficultyTransition(consensus, interval, start_nbits, next));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_negative_target)
{
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    nBits = UintToArith256(consensus.powLimit).GetCompact(true);
    hash = uint256{1};
    CBlockHeader pow_hdr; pow_hdr.SetNull(); pow_hdr.nVersion = 1; pow_hdr.nNonce = 1; pow_hdr.nBits = nBits;
    (void)hash;
    BOOST_CHECK(!CheckProofOfWork(pow_hdr, consensus));  // bad target (or invalid cycle) rejected
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_overflow_target)
{
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits{~0x00800000U};
    hash = uint256{1};
    CBlockHeader pow_hdr; pow_hdr.SetNull(); pow_hdr.nVersion = 1; pow_hdr.nNonce = 1; pow_hdr.nBits = nBits;
    (void)hash;
    BOOST_CHECK(!CheckProofOfWork(pow_hdr, consensus));  // bad target (or invalid cycle) rejected
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_too_easy_target)
{
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    arith_uint256 nBits_arith = UintToArith256(consensus.powLimit);
    nBits_arith *= 2;
    nBits = nBits_arith.GetCompact();
    hash = uint256{1};
    CBlockHeader pow_hdr; pow_hdr.SetNull(); pow_hdr.nVersion = 1; pow_hdr.nNonce = 1; pow_hdr.nBits = nBits;
    (void)hash;
    BOOST_CHECK(!CheckProofOfWork(pow_hdr, consensus));  // bad target (or invalid cycle) rejected
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_biger_hash_than_target)
{
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    arith_uint256 hash_arith = UintToArith256(consensus.powLimit);
    nBits = hash_arith.GetCompact();
    hash_arith *= 2; // hash > nBits
    hash = ArithToUint256(hash_arith);
    CBlockHeader pow_hdr; pow_hdr.SetNull(); pow_hdr.nVersion = 1; pow_hdr.nNonce = 1; pow_hdr.nBits = nBits;
    (void)hash;
    BOOST_CHECK(!CheckProofOfWork(pow_hdr, consensus));  // bad target (or invalid cycle) rejected
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_zero_target)
{
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    arith_uint256 hash_arith{0};
    nBits = hash_arith.GetCompact();
    hash = ArithToUint256(hash_arith);
    CBlockHeader pow_hdr; pow_hdr.SetNull(); pow_hdr.nVersion = 1; pow_hdr.nNonce = 1; pow_hdr.nBits = nBits;
    (void)hash;
    BOOST_CHECK(!CheckProofOfWork(pow_hdr, consensus));  // bad target (or invalid cycle) rejected
}

BOOST_AUTO_TEST_CASE(GetBlockProofEquivalentTime_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    std::vector<CBlockIndex> blocks(10000);
    for (int i = 0; i < 10000; i++) {
        blocks[i].pprev = i ? &blocks[i - 1] : nullptr;
        blocks[i].nHeight = i;
        blocks[i].nTime = 1269211443 + i * chainParams->GetConsensus().nPowTargetSpacing;
        blocks[i].nBits = 0x207fffff; /* target 0x7fffff000... */
        blocks[i].nChainWork = i ? blocks[i - 1].nChainWork + GetBlockProof(blocks[i - 1]) : arith_uint256(0);
    }

    for (int j = 0; j < 1000; j++) {
        CBlockIndex *p1 = &blocks[m_rng.randrange(10000)];
        CBlockIndex *p2 = &blocks[m_rng.randrange(10000)];
        CBlockIndex *p3 = &blocks[m_rng.randrange(10000)];

        int64_t tdiff = GetBlockProofEquivalentTime(*p1, *p2, *p3, chainParams->GetConsensus());
        BOOST_CHECK_EQUAL(tdiff, p1->GetBlockTime() - p2->GetBlockTime());
    }
}

void sanity_check_chainparams(const ArgsManager& args, ChainType chain_type)
{
    const auto chainParams = CreateChainParams(args, chain_type);
    const auto consensus = chainParams->GetConsensus();

    // hash genesis is correct
    BOOST_CHECK_EQUAL(consensus.hashGenesisBlock, chainParams->GenesisBlock().GetHash());

    // target timespan is an even multiple of spacing
    BOOST_CHECK_EQUAL(consensus.nPowTargetTimespan % consensus.nPowTargetSpacing, 0);

    // genesis nBits is positive, doesn't overflow and is lower than powLimit
    arith_uint256 pow_compact;
    bool neg, over;
    pow_compact.SetCompact(chainParams->GenesisBlock().nBits, &neg, &over);
    BOOST_CHECK(!neg && pow_compact != 0);
    BOOST_CHECK(!over);
    BOOST_CHECK(UintToArith256(consensus.powLimit) >= pow_compact);

    // NOTE: the inherited "powLimit * 4*nPowTargetTimespan must not overflow"
    // ceiling used to live here. It is unsatisfiable at Quicksilver's one-GPU
    // floor (powLimit ~2^255 would require 4*timespan < 2) and it only ever
    // approximated the property that matters — that a 4x retarget step off
    // powLimit is safe. ScaleTarget saturates instead of wrapping (P0a) and both
    // callers clamp to powLimit, and that invariant is asserted directly by
    // mainnet_retarget_step_off_powlimit_is_safe_and_agreed below.
}

bool HasAllZeroCycle(const CBlockHeader& header)
{
    return std::all_of(header.nCycle.begin(), header.nCycle.end(), [](uint32_t edge) { return edge == 0; });
}

void check_public_chain_trust_anchors_reset(const CChainParams& params)
{
    const auto& consensus = params.GetConsensus();
    BOOST_CHECK(consensus.nMinimumChainWork.IsNull());
    BOOST_CHECK(consensus.defaultAssumeValid.IsNull());
    BOOST_CHECK_EQUAL(params.TxData().nTime, 0);
    BOOST_CHECK_EQUAL(params.TxData().tx_count, 0U);
    BOOST_CHECK_EQUAL(params.TxData().dTxRate, 0);
}

BOOST_AUTO_TEST_CASE(quicksilver_public_genesis_is_fresh_zero_cycle_trust_anchor)
{
    const auto main = CreateChainParams(*m_node.args, ChainType::MAIN);
    const auto publictest = CreateChainParams(*m_node.args, ChainType::PUBLIC_TEST);

    BOOST_CHECK_EQUAL(main->GenesisBlock().nTime, 1788566400U); // 2026-09-05 00:00 UTC, the F-147 re-mint
    BOOST_CHECK_EQUAL(main->GenesisBlock().nNonce, 0U);
    BOOST_CHECK(HasAllZeroCycle(main->GenesisBlock()));
    BOOST_CHECK_EQUAL(publictest->GenesisBlock().nTime, 1788566400U); // 2026-09-05 00:00 UTC, reminted with main
    BOOST_CHECK_EQUAL(publictest->GenesisBlock().nNonce, 0U);
    BOOST_CHECK(HasAllZeroCycle(publictest->GenesisBlock()));

    // Re-minted 2026-09-05 for F-147 (the retarget fix invalidates history from
    // height 144 on). Headline and nTime moved, so merkle roots and block hashes
    // both moved. Exact equality is the spec.
    BOOST_CHECK_EQUAL(main->GenesisBlock().hashMerkleRoot.ToString(), "da9499c1476c92b69b7214ddb1a365548fff51159e523ca01c77686e022de966");
    BOOST_CHECK_EQUAL(main->GenesisBlock().GetHash().ToString(), "c9144acae20212e57e9e59f34a681bd25f55d81438001c424ebb95cd4869bcf3");
    BOOST_CHECK_EQUAL(publictest->GenesisBlock().hashMerkleRoot.ToString(), "bdf0a510a4f1094ae464987ef01a0fe8409d7c61fee2ef4102d4d84159e78ad6");
    BOOST_CHECK_EQUAL(publictest->GenesisBlock().GetHash().ToString(), "917dde1f04c7470969bbdc344d39559e1af32b3b0e6caaed89db54637d6e46da");

    BOOST_CHECK_NE(main->GenesisBlock().GetHash().ToString(), "b1b46846abab5e84f8b2de4821f99cc4a861446c899122fc6675360d97f3e560");
    BOOST_CHECK_NE(main->GenesisBlock().GetHash().ToString(), "472a2f9c7a8130a23de225d68c8c0aa1febba140ba9e30fcbc2677e413799999");
    BOOST_CHECK_NE(publictest->GenesisBlock().GetHash().ToString(), "289228e56502b637b4720fc20a4ab456d29a013f146603d72d6d1b02f3b82de1");
    // The pre-P1 publictest genesis: minted while publictest still ran a 2016-block
    // window with retargeting disabled, so a node carrying it rehearsed none of
    // the retarget code mainnet runs. Never come back.
    BOOST_CHECK_NE(publictest->GenesisBlock().GetHash().ToString(), "90328f83a1f182bd7962562f2b99385cf5e82c9f20f09a490eb8397f06aa6f89");

    // The pre-E28 genesis pair, minted at the 2-cycle floor (nBits 0x207fffff)
    // when both graph sizes were 29. A node carrying either one is running the
    // old floor, where the cheapest block costs half the work it must. Never
    // come back.
    BOOST_CHECK_NE(main->GenesisBlock().GetHash().ToString(), "46ca06e23004fd503603ff4c08b017d534aaf153850bf55a06a15c3458a69b23");
    BOOST_CHECK_NE(publictest->GenesisBlock().GetHash().ToString(), "dc995052cdb9942ab1354a61743853ccf04de27cde509247d7d3209b7f1b25d0");

    // The pre-launch 2026-08-16 remint pair (E28 floor, 2026-08-01 / 2026-07-21
    // headlines). A node carrying either one is on a chain nobody else is on.
    BOOST_CHECK_NE(main->GenesisBlock().GetHash().ToString(), "319aa1b4a8a7a6d7ea636cef44b9a38e92e7c3a1b1a3b72a3b1c40386936662e");
    BOOST_CHECK_NE(publictest->GenesisBlock().GetHash().ToString(), "06fb6c59c9aa03d0d0aa809a6a379bea1242a2aefd9c7dfbce7ec47cba191aea");

    // The 2026-08-16 launch remint pair, superseded by the 2026-08-24 remint that
    // added the authors to the genesis mark. Nothing was ever published on them,
    // but a node carrying either one is on a chain nobody else is on.
    BOOST_CHECK_NE(main->GenesisBlock().GetHash().ToString(), "37bf0859c1f99a2a22c827ae6f0d7371f9b4d71575e3b9c0c8819cc77f6be1ca");
    BOOST_CHECK_NE(publictest->GenesisBlock().GetHash().ToString(), "736a88327bca8f58b3b85ac7f7edb03066457f682215e1dd1f8fb0a7ddb28af3");

    // The pre-F-147 chain (re-minted 2026-09-05): its history from height 144 on
    // was invalidated by the retarget anchor fix.
    BOOST_CHECK_NE(main->GenesisBlock().GetHash().ToString(), "e87427f26217fa34d390632aaf5d0d91f1c1fe3e6db4db9e4b643514772ff959");
    BOOST_CHECK_NE(publictest->GenesisBlock().GetHash().ToString(), "380519f5da3e0a735061c068eb3d3c507053697da8256fbc89d741d179dadafb");

    BOOST_CHECK(CheckProofOfWork(main->GenesisBlock(), main->GetConsensus()));
    BOOST_CHECK(CheckProofOfWork(publictest->GenesisBlock(), publictest->GetConsensus()));

    // The genesis coinbase output is the Quicksilver mark: a provably-unspendable
    // OP_RETURN carrying "Quicksilver Genesis"
    const std::string mark{"Quicksilver Genesis"};
    const CScript& main_spk = main->GenesisBlock().vtx[0]->vout[0].scriptPubKey;
    BOOST_CHECK(!main_spk.empty() && main_spk[0] == OP_RETURN);
    BOOST_CHECK(std::search(main_spk.begin(), main_spk.end(), mark.begin(), mark.end()) != main_spk.end());
    const CScript& publictest_spk = publictest->GenesisBlock().vtx[0]->vout[0].scriptPubKey;
    BOOST_CHECK(!publictest_spk.empty() && publictest_spk[0] == OP_RETURN);
    BOOST_CHECK(std::search(publictest_spk.begin(), publictest_spk.end(), mark.begin(), mark.end()) != publictest_spk.end());
}

BOOST_AUTO_TEST_CASE(genesis_coinbase_carries_no_bitcoin_magic)
{
    // 486604799 == 0x1D00FFFF is Bitcoin's genesis nBits, historically embedded
    // in Bitcoin's genesis coinbase scriptSig. Quicksilver has its own chain
    // root; that Bitcoin-linked value must not appear in the genesis coinbase
    // scriptSig on any network. The value pushed by `CScript() << 486604799`
    // serializes as the little-endian bytes {0xff, 0x00, 0x1d}.
    const std::vector<unsigned char> bitcoin_magic{0xff, 0x00, 0x1d};
    for (const ChainType ct : {ChainType::MAIN, ChainType::PUBLIC_TEST, ChainType::SANDBOX}) {
        const auto params = CreateChainParams(*m_node.args, ct);
        const CScript& ss = params->GenesisBlock().vtx[0]->vin[0].scriptSig;
        const bool found = std::search(ss.begin(), ss.end(),
                                       bitcoin_magic.begin(), bitcoin_magic.end()) != ss.end();
        BOOST_CHECK_MESSAGE(!found, "Bitcoin genesis nBits magic found in coinbase scriptSig for chain " << int(ct));
    }
}

BOOST_AUTO_TEST_CASE(genesis_nbits_equals_chain_powlimit)
{
    // Every chain must open at its OWN difficulty floor: genesis nBits has to be
    // exactly the compact form of that chain's powLimit.
    //
    // A genesis that is HARDER than powLimit is not merely cosmetic. On a chain
    // with fPowAllowMinDifficultyBlocks, GetNextWorkRequired (pow.cpp) walks back
    // past every min-difficulty block to "the last non-special-min-difficulty
    // block" and reuses its nBits. On a young chain every block is min-difficulty,
    // so that walk reaches genesis -- resurrecting genesis's harder nBits as the
    // required target for any block mined within 2*nPowTargetSpacing of its
    // parent. publictest shipped Bitcoin's 0x1d00ffff while its powLimit is 2^255,
    // which demanded Bitcoin difficulty-1 and stalled block production.
    //
    // The neighbouring sanity check (powLimit >= genesis target) is one-sided and
    // passes a harder genesis, so it cannot catch this; assert equality here.
    for (const ChainType ct : {ChainType::MAIN, ChainType::PUBLIC_TEST, ChainType::SANDBOX}) {
        const auto params = CreateChainParams(*m_node.args, ct);
        const uint32_t genesis_nbits = params->GenesisBlock().nBits;
        const uint32_t powlimit_compact = UintToArith256(params->GetConsensus().powLimit).GetCompact();
        BOOST_CHECK_MESSAGE(genesis_nbits == powlimit_compact,
                            "chain " << int(ct) << ": genesis nBits 0x" << std::hex << genesis_nbits
                                     << " != powLimit compact 0x" << powlimit_compact << std::dec);
    }
}

BOOST_AUTO_TEST_CASE(mainnet_has_no_inherited_activation_heights)
{
    // Quicksilver mainnet starts at height 0. The inherited Bitcoin
    // buried-deployment heights (BIP34 227931, BIP66 363725, BIP65 388381,
    // CSV 419328, segwit
    // 481824) are meaningless here and actively harmful: with SegwitHeight
    // 481824 the vault would hand out bech32 hg1... addresses for years of
    // blocks before segwit activated. Everything must be active from block 1,
    // as it already is on publictest and sandbox.
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();

    BOOST_CHECK_EQUAL(consensus.BIP34Height, 1);
    BOOST_CHECK(consensus.BIP34Hash.IsNull());
    BOOST_CHECK_EQUAL(consensus.BIP65Height, 1);
    BOOST_CHECK_EQUAL(consensus.BIP66Height, 1);
    BOOST_CHECK_EQUAL(consensus.CSVHeight, 1);
    BOOST_CHECK_EQUAL(consensus.SegwitHeight, 1);
    BOOST_CHECK_EQUAL(consensus.MinBIP9WarningHeight, 0);

    // The two exceptions are keyed to upstream Bitcoin block hashes and can
    // never be reached on this chain.
    BOOST_CHECK(consensus.script_flag_exceptions.empty());

    const auto& taproot = consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT];
    BOOST_CHECK_EQUAL(taproot.nStartTime, Consensus::BIP9Deployment::ALWAYS_ACTIVE);
    BOOST_CHECK_EQUAL(taproot.nTimeout, Consensus::BIP9Deployment::NO_TIMEOUT);
    BOOST_CHECK_EQUAL(taproot.min_activation_height, 0);
}

BOOST_AUTO_TEST_CASE(quicksilver_private_genesis_outputs_use_agent_mark)
{
    const auto sandbox = CreateChainParams(*m_node.args, ChainType::SANDBOX);

    const std::string mark{"Quicksilver Genesis"};
    const CScript& spk = sandbox->GenesisBlock().vtx[0]->vout[0].scriptPubKey;
    BOOST_CHECK(!spk.empty() && spk[0] == OP_RETURN);
    BOOST_CHECK(std::search(spk.begin(), spk.end(), mark.begin(), mark.end()) != spk.end());
}

BOOST_AUTO_TEST_CASE(quicksilver_private_genesis_is_zero_cycle_trust_anchor)
{
    const auto sandbox = CreateChainParams(*m_node.args, ChainType::SANDBOX);

    BOOST_CHECK(HasAllZeroCycle(sandbox->GenesisBlock()));
    BOOST_CHECK_EQUAL(sandbox->GenesisBlock().nTime, 1750000000U); // test-chain clock; headline reminted 2026-08-24
    BOOST_CHECK_EQUAL(sandbox->GenesisBlock().hashMerkleRoot.ToString(), "feacef8e2fca6169ea9726bbf4db05c5efa7c006c1f0d1f98c2cb056d7a06469");
    BOOST_CHECK_EQUAL(sandbox->GenesisBlock().GetHash().ToString(), "bd806e80eec28f4b16ab48db377ad6af369fa3b1197cb6d9d77d07db6d5e91e7");
    BOOST_CHECK_EQUAL(sandbox->GetConsensus().hashGenesisBlock, sandbox->GenesisBlock().GetHash());
    BOOST_CHECK(CheckProofOfWork(sandbox->GenesisBlock(), sandbox->GetConsensus()));
    BOOST_CHECK_NE(sandbox->GenesisBlock().GetHash().ToString(), "468da7aec51dce15fcc264caec67097215bc635fd2be679565df1ef52a74e308");
    // Superseded by the 2026-08-24 remint (genesis mark gained the authors).
    BOOST_CHECK_NE(sandbox->GenesisBlock().GetHash().ToString(), "5199caf02a08c6e3a6817a8871d1f97e9f9fbbfab0ddd9de4f634d2095382028");
}

BOOST_AUTO_TEST_CASE(quicksilver_public_trust_anchors_are_reset)
{
    const auto main = CreateChainParams(*m_node.args, ChainType::MAIN);
    const auto publictest = CreateChainParams(*m_node.args, ChainType::PUBLIC_TEST);

    check_public_chain_trust_anchors_reset(*main);
    check_public_chain_trust_anchors_reset(*publictest);
}

BOOST_AUTO_TEST_CASE(ChainParams_MAIN_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::MAIN);
}

BOOST_AUTO_TEST_CASE(ChainParams_SANDBOX_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::SANDBOX);
}

BOOST_AUTO_TEST_CASE(ChainParams_PUBLIC_TEST_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::PUBLIC_TEST);
}

BOOST_AUTO_TEST_CASE(txpow_no_cycle_arg_is_sandbox_only)
{
    ArgsManager args;
    args.ForceSetArg("-txpownocycle", "1");

    BOOST_CHECK(!CreateChainParams(args, ChainType::MAIN)->GetConsensus().fTxPowNoCycle);
    BOOST_CHECK(!CreateChainParams(args, ChainType::PUBLIC_TEST)->GetConsensus().fTxPowNoCycle);
    BOOST_CHECK(CreateChainParams(args, ChainType::SANDBOX)->GetConsensus().fTxPowNoCycle);

    ArgsManager default_args;
    BOOST_CHECK(!CreateChainParams(default_args, ChainType::SANDBOX)->GetConsensus().fTxPowNoCycle);
}

BOOST_AUTO_TEST_CASE(publictest_matches_mainnet_retarget_and_pow_params)
{
    // P1: publictest rehearses mainnet, so it must run mainnet's consensus.
    // Before P1 it used a 2016-block window with retargeting disabled, which
    // meant it exercised none of the retarget code mainnet depends on — the
    // soak proved liveness and nothing about difficulty adjustment. Any drift
    // reintroduced here silently downgrades the rehearsal to a smoke test.
    //
    // The chains still differ where they must: genesis, netmagic, ports,
    // address prefixes and hrp. Only the consensus surface is shared.
    const auto main = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    const auto publictest = CreateChainParams(*m_node.args, ChainType::PUBLIC_TEST)->GetConsensus();

    BOOST_CHECK_EQUAL(publictest.nPowTargetTimespan, main.nPowTargetTimespan);
    BOOST_CHECK_EQUAL(publictest.nBaseWorkMAWindow, main.nBaseWorkMAWindow);
    BOOST_CHECK_EQUAL(publictest.fPowNoRetargeting, main.fPowNoRetargeting);
    BOOST_CHECK_EQUAL(publictest.fPowAllowMinDifficultyBlocks,
                      main.fPowAllowMinDifficultyBlocks);
    BOOST_CHECK_EQUAL(publictest.nRuleChangeActivationThreshold,
                      main.nRuleChangeActivationThreshold);

    // The surface that was already shared, asserted so it cannot drift either.
    BOOST_CHECK(publictest.powLimit == main.powLimit);
    BOOST_CHECK_EQUAL(publictest.nPowTargetSpacing, main.nPowTargetSpacing);
    BOOST_CHECK_EQUAL(publictest.nTxWorkCouplingK, main.nTxWorkCouplingK);
    BOOST_CHECK_EQUAL(publictest.nEdgeBits, main.nEdgeBits);
    BOOST_CHECK_EQUAL(publictest.nMinerConfirmationWindow, main.nMinerConfirmationWindow);

    // Absolute values, so a change to BOTH chains cannot pass this test silently.
    BOOST_CHECK_EQUAL(main.nPowTargetSpacing, 5 * 60);
    BOOST_CHECK_EQUAL(main.DifficultyAdjustmentInterval(), 144);
    BOOST_CHECK_EQUAL(publictest.DifficultyAdjustmentInterval(), 144);
    BOOST_CHECK_EQUAL(main.nMinerConfirmationWindow, 2016);
    BOOST_CHECK(!publictest.fPowNoRetargeting);
    BOOST_CHECK(!publictest.fPowAllowMinDifficultyBlocks);
}

BOOST_AUTO_TEST_CASE(mainnet_retarget_moves_and_stays_a_permitted_transition)
{
    // Over a real linked period whose blocks arrive twice as fast as intended,
    // difficulty must rise, and the result must be a transition the validator
    // accepts. This case once also compared the two retarget base branches
    // against each other; that comparison died with the branch itself (F-147).
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    BOOST_CHECK(!consensus.fPowAllowMinDifficultyBlocks);

    const int64_t interval{consensus.DifficultyAdjustmentInterval()};
    const int64_t start_time{1788566400}; // mainnet genesis time
    const uint32_t start_nbits{UintToArith256(consensus.powLimit).GetCompact()};

    auto period{LinkedChain(static_cast<size_t>(interval), start_time, consensus.nPowTargetSpacing / 2, start_nbits)};
    const CBlockIndex& last{period.back()};

    const uint32_t next_nbits{CalculateNextWorkRequired(&last, start_time, consensus)};

    // Blocks came twice as fast, so difficulty must rise (target must fall)...
    arith_uint256 before, after;
    before.SetCompact(start_nbits);
    after.SetCompact(next_nbits);
    BOOST_CHECK(after < before);

    // ...and the result must be a transition the validator accepts.
    BOOST_CHECK(PermittedDifficultyTransition(consensus, interval, start_nbits, next_nbits));
}

BOOST_AUTO_TEST_CASE(mainnet_retarget_step_off_powlimit_is_safe_and_agreed)
{
    // Replaces an earlier guard that required powLimit < 2^256/(4*timespan).
    // That ceiling is unsatisfiable at Quicksilver's one-GPU floor (powLimit
    // ~2^255 would need 4*timespan < 2), and it was only ever a proxy for the
    // property that actually matters: a 4x retarget step off powLimit must be
    // safe. ScaleTarget now saturates rather than wrapping, and both callers
    // clamp to powLimit — so assert that directly, which stays true after P0b
    // raises the floor. See doc/audit/mainnet-difficulty-floor-model.md.
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    BOOST_CHECK(!consensus.fPowNoRetargeting);

    const arith_uint256 pow_limit{UintToArith256(consensus.powLimit)};
    const int64_t timespan{consensus.nPowTargetTimespan};

    // Easing off the easiest permitted target must never come back harder.
    BOOST_CHECK(ScaleTarget(pow_limit, timespan * 4, timespan) >= pow_limit);

    // An arbitrarily slow period at powLimit stays clamped at powLimit, and the
    // validator accepts what the miner-side calculation produced. The retarget
    // reads only the two endpoint timestamps, but a real linked period is kept
    // here so the test exercises the same shape a chain does.
    const int64_t start_time{1788566400}; // mainnet genesis time
    std::vector<CBlockIndex> period(static_cast<size_t>(consensus.DifficultyAdjustmentInterval()));
    for (size_t i = 0; i < period.size(); ++i) {
        period[i].nHeight = static_cast<int>(i);
        period[i].nTime = static_cast<unsigned int>(start_time + static_cast<int64_t>(i) * consensus.nPowTargetSpacing * 1000);
        period[i].nBits = pow_limit.GetCompact();
        period[i].pprev = (i == 0) ? nullptr : &period[i - 1];
    }
    const CBlockIndex& pindexLast{period.back()};

    const uint32_t next_nbits{CalculateNextWorkRequired(&pindexLast, start_time, consensus)};
    arith_uint256 next;
    next.SetCompact(next_nbits);
    BOOST_CHECK(next <= pow_limit);
    BOOST_CHECK(PermittedDifficultyTransition(consensus, consensus.DifficultyAdjustmentInterval(),
                                              pindexLast.nBits, next_nbits));
}

// Quicksilver (#5c-1 Phase 3): sandbox block PoW is trivial-but-real, mirroring
// upstream's easy-target sandbox. The Cuckatoo migration accidentally made block
// PoW require a real 42-cycle regardless of target (making the 100-block maturity
// fixtures take ~6 min). The fBlockPowNoCycle flag restores the upstream invariant
// on sandbox ONLY: the target threshold on CuckatooProofHash still applies, but a
// real graph cycle is no longer required. Per-tx PoW is unaffected.
BOOST_AUTO_TEST_CASE(sandbox_block_pow_skips_cycle_but_keeps_target)
{
    const auto sandbox = CreateChainParams(*m_node.args, ChainType::SANDBOX);
    Consensus::Params consensus = sandbox->GetConsensus();

    // A degenerate "cycle": 42 distinct ascending edges that do NOT form a real
    // 42-cycle in the graph. We then grind ONLY the cheap proof-hash threshold
    // (no cycle solving) so the header clears the trivial sandbox target.
    CBlockHeader hdr;
    hdr.SetNull();
    hdr.nVersion = 1;
    hdr.nBits = sandbox->GenesisBlock().nBits;
    hdr.hashPrevBlock = sandbox->GenesisBlock().GetHash();
    hdr.nTime = sandbox->GenesisBlock().nTime + 1;
    for (uint32_t i = 0; i < hdr.nCycle.size(); ++i) hdr.nCycle[i] = i + 1;

    const auto target = DeriveTarget(hdr.nBits, consensus.powLimit);
    BOOST_REQUIRE(target);
    while (UintToArith256(cuckatoo::CuckatooProofHash(hdr.nCycle)) > *target) {
        hdr.nCycle[0] += hdr.nCycle.size(); // stay clear of the other edges; cheap hashing only
    }

    // Confirm the premise: this is genuinely NOT a real 42-cycle.
    const auto pre = hdr.PrePowBytes();
    const cuckatoo::Keys keys = cuckatoo::CuckatooSetHeader(pre.data(), pre.size());
    BOOST_REQUIRE(!cuckatoo::CuckatooVerify(hdr.nCycle, keys, consensus.nEdgeBits));

    // Flag SET (sandbox default): block PoW accepts the trivial cycle (target met).
    BOOST_CHECK(CheckProofOfWork(hdr, consensus));

    // Flag CLEAR: the SAME header is rejected — a real 42-cycle is still required.
    // This is the production-safety property: only sandbox flips the flag.
    Consensus::Params strict = consensus;
    strict.fBlockPowNoCycle = false;
    BOOST_CHECK(!CheckProofOfWork(hdr, strict));
}

// The E28 flag day. Both graph sizes move together, which is the whole point: r,
// the transaction-solver rate relative to the block-solver rate, is a ratio of two
// solvers and stays 1.00 only while the two sizes are equal. Split them and the
// mint-safety derivation in doc/audit/mainnet-difficulty-floor-model.md no longer
// holds.
BOOST_AUTO_TEST_CASE(e28_flag_day_sizes_and_floor)
{
    for (const ChainType ct : {ChainType::MAIN, ChainType::PUBLIC_TEST}) {
        const auto cp = CreateChainParams(*m_node.args, ct)->GetConsensus();
        BOOST_CHECK_EQUAL(int(cp.nEdgeBits), 28);
        BOOST_CHECK_EQUAL(int(cp.nTxEdgeBits), 28);
        // Unified: the moment these differ, r != 1 and criterion 3 must be redone.
        BOOST_CHECK_EQUAL(int(cp.nEdgeBits), int(cp.nTxEdgeBits));
        // The floor is 4 cycles of work, i.e. powLimit = 2^254 - 1. GetBlockProof
        // is 2^256/(target+1), so this is the arithmetic the floor model derives.
        BOOST_CHECK_EQUAL(cp.powLimit,
                          uint256{"3fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"});
    }
    // Sandbox is not part of the flag day and keeps both its size and its floor.
    const auto sandbox = CreateChainParams(*m_node.args, ChainType::SANDBOX)->GetConsensus();
    BOOST_CHECK_EQUAL(int(sandbox.nEdgeBits), 19);
    BOOST_CHECK_EQUAL(int(sandbox.nTxEdgeBits), 19);
    BOOST_CHECK_EQUAL(sandbox.powLimit,
                      uint256{"7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"});
}

// ---------------------------------------------------------------------------
// Quicksilver (#5c-1 Phase 2): the congestion multiplier is a HEADER field.
// These cases fence the format properties that make that safe. They are not
// arithmetic checks — they are the load-bearing invariants of the byte layout.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(congestion_header_prepow_layout_keeps_nonce_at_the_tail)
{
    // THE hazard of #5c-1 Phase 2. Every Cuckatoo solver entry point grinds the last
    // four bytes of the pre-pow in place: solve_19/solve_28 via mutate_nonce,
    // Solve28Bytes explicitly, dispatch.cpp's key reconstruction, the bench
    // KeyedPrepow helpers, and the external CUDA solver gpu/qsgpusolve.cu. Appending
    // nCongestion AFTER nNonce would leave every one of them grinding the congestion
    // field while the nonce sat frozen — and it would not fail loudly. This assertion
    // is the permanent fence against that, so it must stay exact.
    BOOST_CHECK_EQUAL(CBlockHeader::PREPOW_SIZE, 84U);
    BOOST_CHECK_EQUAL(cuckatoo::PREPOW_BYTES, CBlockHeader::PREPOW_SIZE);

    CBlockHeader hdr;
    hdr.SetNull();
    hdr.nVersion = 1;
    hdr.nTime = 111;
    hdr.nBits = 0x207fffff;
    hdr.nCongestion = 0xAABBCCDDu;
    hdr.nNonce = 0x11223344u;

    const auto pre = hdr.PrePowBytes();
    BOOST_REQUIRE_EQUAL(pre.size(), 84U);

    // nNonce occupies bytes [80,84) little-endian...
    BOOST_CHECK_EQUAL(pre[80], 0x44);
    BOOST_CHECK_EQUAL(pre[81], 0x33);
    BOOST_CHECK_EQUAL(pre[82], 0x22);
    BOOST_CHECK_EQUAL(pre[83], 0x11);
    // ...and nCongestion sits immediately BEFORE it, at [76,80).
    BOOST_CHECK_EQUAL(pre[76], 0xDD);
    BOOST_CHECK_EQUAL(pre[77], 0xCC);
    BOOST_CHECK_EQUAL(pre[78], 0xBB);
    BOOST_CHECK_EQUAL(pre[79], 0xAA);
}

BOOST_AUTO_TEST_CASE(congestion_is_inside_the_prepow_so_it_rebinds_the_cycle)
{
    // The reason nCongestion sits in the pre-pow rather than after the cycle: a miner
    // must not be able to restate the congestion multiplier on an already-solved block.
    // Changing it must change the siphash keys, which invalidates any existing cycle.
    CBlockHeader a;
    a.SetNull();
    a.nVersion = 1;
    a.nTime = 222;
    a.nBits = 0x207fffff;
    a.nCongestion = 65536;
    a.nNonce = 7;

    CBlockHeader b{a};
    b.nCongestion = 65536 * 2;

    const auto pre_a = a.PrePowBytes();
    const auto pre_b = b.PrePowBytes();
    BOOST_CHECK(pre_a != pre_b);

    const cuckatoo::Keys ka = cuckatoo::CuckatooSetHeader(pre_a.data(), pre_a.size());
    const cuckatoo::Keys kb = cuckatoo::CuckatooSetHeader(pre_b.data(), pre_b.size());
    BOOST_CHECK(ka.k0 != kb.k0 || ka.k1 != kb.k1 || ka.k2 != kb.k2 || ka.k3 != kb.k3);

    // ...and it is part of block identity, not just of the grind.
    BOOST_CHECK(a.GetHash() != b.GetHash());
}

BOOST_AUTO_TEST_CASE(congestion_header_round_trips_through_serialization_and_the_index)
{
    CBlockHeader hdr;
    hdr.SetNull();
    hdr.nVersion = 1;
    hdr.nTime = 333;
    hdr.nBits = 0x207fffff;
    hdr.nCongestion = 65536 * 5;
    hdr.nNonce = 99;
    for (uint32_t i = 0; i < hdr.nCycle.size(); ++i) hdr.nCycle[i] = i + 1;

    // Wire round-trip, and the serialized size grew by exactly the new field.
    DataStream ds;
    ds << hdr;
    BOOST_CHECK_EQUAL(ds.size(), 84U + 42U * 4U);
    BOOST_CHECK_EQUAL(ds.size(), 252U);
    CBlockHeader back;
    ds >> back;
    BOOST_CHECK_EQUAL(back.nCongestion, hdr.nCongestion);
    BOOST_CHECK_EQUAL(back.nNonce, hdr.nNonce);
    BOOST_CHECK_EQUAL(back.GetHash().ToString(), hdr.GetHash().ToString());

    // Index round-trip: the header populates m_congestion, and GetBlockHeader gives it
    // back. This is the path CDiskBlockIndex::ConstructBlockHash depends on — miss it
    // and every block hash rebuilt from the disk index is wrong.
    CBlockIndex index{hdr};
    BOOST_CHECK_EQUAL(index.m_congestion, hdr.nCongestion);
    BOOST_CHECK_EQUAL(index.GetBlockHeader().nCongestion, hdr.nCongestion);
    BOOST_CHECK_EQUAL(index.GetBlockHeader().GetHash().ToString(), hdr.GetHash().ToString());
}

BOOST_AUTO_TEST_CASE(genesis_congestion_is_the_floor_on_every_network)
{
    // Genesis never reaches ConnectBlock, so its header field IS the seed of the whole
    // recurrence: every descendant's multiplier is derived from this value.
    const auto main{CreateChainParams(*m_node.args, ChainType::MAIN)};
    const auto publictest{CreateChainParams(*m_node.args, ChainType::PUBLIC_TEST)};
    const auto sandbox{CreateChainParams(*m_node.args, ChainType::SANDBOX)};
    BOOST_CHECK_EQUAL(main->GenesisBlock().nCongestion, CONGESTION_ONE);
    BOOST_CHECK_EQUAL(publictest->GenesisBlock().nCongestion, CONGESTION_ONE);
    BOOST_CHECK_EQUAL(sandbox->GenesisBlock().nCongestion, CONGESTION_ONE);
}

BOOST_AUTO_TEST_SUITE_END()
