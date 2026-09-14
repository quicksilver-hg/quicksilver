// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Frame-B mint (#4): a valid per-tx PoW authorizes a capped mint C to the block
// miner. These tests cover the inflation arithmetic in isolation — the most
// dangerous code in the coin — plus the adversarial replay/transplant cases.
// #5c-1: the allowance now resolves each tx's anchor against a branch tip, so a
// tx mints only when its anchor is on-branch/in-window AND its proof verifies
// against that anchor's block hash.

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <consensus/amount.h>
#include <crypto/cuckatoo/cuckatoo.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(mint_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(sandbox_mint_param_is_one_coin_test_fixture)
{
    SelectParams(ChainType::SANDBOX);
    BOOST_CHECK_EQUAL(Params().GetConsensus().nTxPowMint, 1 * COIN);
}

// Build a minimal coinbase tx (null prevout) so IsCoinBase() is true.
static CTransactionRef MakeCoinbaseRef()
{
    CMutableTransaction cb;
    cb.vin.resize(1);
    cb.vin[0].prevout.SetNull();
    cb.vout.resize(1);
    cb.vout[0].nValue = 50 * COIN;
    return MakeTransactionRef(cb);
}

// A distinct, valid-looking non-coinbase tx body anchored at height 0. `seed`
// varies the prevout so each tx has a different pre-image (and so a different
// ground proof).
static CMutableTransaction MakeUnprovenTx(uint32_t seed)
{
    CMutableTransaction mtx;
    mtx.vin.resize(1);
    mtx.vin[0].prevout = COutPoint(Txid::FromUint256(ArithToUint256(arith_uint256(seed) + 1)), 0);
    mtx.vout.resize(1);
    mtx.vout[0].nValue = 1000;
    mtx.nAnchorHeight = 0; // anchored to the height-0 tip built by MakeAnchorTip()
    return mtx;
}

// Grind a valid per-tx Cuckatoo proof into `mtx`, bound to `anchor_hash`.
static void GrindProof(CMutableTransaction& mtx, uint8_t edgebits, const uint256& anchor_hash)
{
    const auto pre = CTransaction(mtx).PowPreimage(anchor_hash);
    cuckatoo::Cycle cyc{};
    uint32_t won = 0;
    BOOST_REQUIRE(cuckatoo::CuckatooSolveBytes(pre.data(), pre.size(), edgebits, 0, 1u << 20, cyc, won));
    mtx.nCycle = cyc;
    mtx.nPowNonce = won;
}

// Initialise a one-block "chain": a height-0 index that is both the branch tip and
// the anchor every test tx points at. `hash_store` must outlive `tip`. (CBlockIndex
// is non-movable/copyable, so the caller owns the object and we fill it in place.)
static void InitAnchorTip(CBlockIndex& tip, const uint256& hash_store)
{
    tip.nHeight = 0;
    tip.pprev = nullptr;
    tip.phashBlock = &hash_store;
}

BOOST_AUTO_TEST_CASE(allowance_zero_for_coinbase_only_block)
{
    SelectParams(ChainType::SANDBOX);
    const Consensus::Params& cp = Params().GetConsensus();
    const uint256 anchor_hash{uint256::ONE};
    CBlockIndex tip;
    InitAnchorTip(tip, anchor_hash);

    CBlock block;
    block.vtx.push_back(MakeCoinbaseRef());
    BOOST_CHECK_EQUAL(GetBlockMintAllowance(block, cp, &tip), 0);
}

BOOST_AUTO_TEST_CASE(allowance_counts_each_proven_tx)
{
    SelectParams(ChainType::SANDBOX);
    const Consensus::Params& cp = Params().GetConsensus();
    const uint256 anchor_hash{uint256::ONE};
    CBlockIndex tip;
    InitAnchorTip(tip, anchor_hash);

    // Two, not three: sandbox mints 1 COIN per transaction against a 2 COIN
    // nMaxBlockMint, so a third would be measuring the cap rather than the
    // per-transaction counting this case exists to measure. The cap has its own test.
    BOOST_REQUIRE_EQUAL(cp.nMaxBlockMint, 2 * cp.nTxPowMint);
    CBlock block;
    block.vtx.push_back(MakeCoinbaseRef());
    for (uint32_t i = 0; i < 2; ++i) {
        CMutableTransaction tx = MakeUnprovenTx(i);
        GrindProof(tx, cp.nTxEdgeBits, anchor_hash);
        block.vtx.push_back(MakeTransactionRef(tx));
    }
    BOOST_CHECK_EQUAL(GetBlockMintAllowance(block, cp, &tip), 2 * cp.nTxPowMint);
}

BOOST_AUTO_TEST_CASE(allowance_excludes_unproven_and_tampered_tx)
{
    SelectParams(ChainType::SANDBOX);
    const Consensus::Params& cp = Params().GetConsensus();
    const uint256 anchor_hash{uint256::ONE};
    CBlockIndex tip;
    InitAnchorTip(tip, anchor_hash);

    CBlock block;
    block.vtx.push_back(MakeCoinbaseRef());

    // (a) a genuinely proven tx -> counts
    CMutableTransaction good = MakeUnprovenTx(10);
    GrindProof(good, cp.nTxEdgeBits, anchor_hash);
    block.vtx.push_back(MakeTransactionRef(good));

    // (b) an unproven tx (all-zero cycle) -> does NOT count
    block.vtx.push_back(MakeTransactionRef(MakeUnprovenTx(20)));

    // (c) a tampered proof (valid then mangled) -> does NOT count
    CMutableTransaction tampered = MakeUnprovenTx(30);
    GrindProof(tampered, cp.nTxEdgeBits, anchor_hash);
    tampered.nCycle[0] ^= 1;
    block.vtx.push_back(MakeTransactionRef(tampered));

    // Only (a) is authorized: exactly one C.
    BOOST_CHECK_EQUAL(GetBlockMintAllowance(block, cp, &tip), 1 * cp.nTxPowMint);
}

BOOST_AUTO_TEST_CASE(allowance_excludes_bad_anchor_tx)
{
    // #5c-1: a tx whose proof is otherwise valid but whose anchor does NOT resolve
    // on-branch (here: future/out-of-window anchor height) mints nothing.
    SelectParams(ChainType::SANDBOX);
    const Consensus::Params& cp = Params().GetConsensus();
    const uint256 anchor_hash{uint256::ONE};
    CBlockIndex tip;
    InitAnchorTip(tip, anchor_hash);

    CBlock block;
    block.vtx.push_back(MakeCoinbaseRef());

    // Grind a valid proof, but point the anchor at a height that does not exist on
    // this one-block branch (height 5 > tip height 0 -> future -> CheckTxAnchor fails).
    CMutableTransaction bad_anchor = MakeUnprovenTx(40);
    bad_anchor.nAnchorHeight = 5;
    GrindProof(bad_anchor, cp.nTxEdgeBits, anchor_hash);
    block.vtx.push_back(MakeTransactionRef(bad_anchor));

    BOOST_CHECK_EQUAL(GetBlockMintAllowance(block, cp, &tip), 0);
}

BOOST_AUTO_TEST_CASE(allowance_guards_moneyrange_overflow)
{
    SelectParams(ChainType::SANDBOX);
    Consensus::Params cp = Params().GetConsensus();  // a mutable copy
    cp.nTxPowMint = MAX_MONEY;                        // each mint is the whole money supply
    const uint256 anchor_hash{uint256::ONE};
    CBlockIndex tip;
    InitAnchorTip(tip, anchor_hash);

    CBlock block;
    block.vtx.push_back(MakeCoinbaseRef());
    for (uint32_t i = 0; i < 2; ++i) {               // 2 * MAX_MONEY overflows MoneyRange
        CMutableTransaction tx = MakeUnprovenTx(100 + i);
        GrindProof(tx, cp.nTxEdgeBits, anchor_hash);
        block.vtx.push_back(MakeTransactionRef(tx));
    }
    BOOST_CHECK_EQUAL(GetBlockMintAllowance(block, cp, &tip), MINT_ALLOWANCE_INVALID);
}

BOOST_AUTO_TEST_CASE(allowance_rejects_transplanted_proof)
{
    SelectParams(ChainType::SANDBOX);
    const Consensus::Params& cp = Params().GetConsensus();
    const uint256 anchor_hash{uint256::ONE};
    CBlockIndex tip;
    InitAnchorTip(tip, anchor_hash);

    // Grind a valid proof for tx A.
    CMutableTransaction a = MakeUnprovenTx(200);
    GrindProof(a, cp.nTxEdgeBits, anchor_hash);

    // Transplant A's proof onto tx B (different prevout -> different pre-image).
    CMutableTransaction b = MakeUnprovenTx(201);
    b.nCycle = a.nCycle;
    b.nPowNonce = a.nPowNonce;

    // A counts; B's stolen proof does not verify against B's pre-image.
    CBlock block;
    block.vtx.push_back(MakeCoinbaseRef());
    block.vtx.push_back(MakeTransactionRef(a));
    block.vtx.push_back(MakeTransactionRef(b));
    BOOST_CHECK_EQUAL(GetBlockMintAllowance(block, cp, &tip), 1 * cp.nTxPowMint);
}

BOOST_AUTO_TEST_CASE(allowance_tolerates_null_coinbase_slot)
{
    // During block assembly (BlockAssembler::CreateNewBlock) vtx[0] is a NULL
    // CTransactionRef slot until the real coinbase is filled in. The miner
    // computes the mint over this half-built block, so the allowance must skip a
    // null entry (it mints nothing) rather than dereference it. Regression for the
    // null-deref crash found mining the first block.
    SelectParams(ChainType::SANDBOX);
    const Consensus::Params& cp = Params().GetConsensus();
    const uint256 anchor_hash{uint256::ONE};
    CBlockIndex tip;
    InitAnchorTip(tip, anchor_hash);

    CBlock block;
    block.vtx.push_back(CTransactionRef{});  // null coinbase slot, as in CreateNewBlock
    CMutableTransaction tx = MakeUnprovenTx(300);
    GrindProof(tx, cp.nTxEdgeBits, anchor_hash);
    block.vtx.push_back(MakeTransactionRef(tx));
    BOOST_CHECK_EQUAL(GetBlockMintAllowance(block, cp, &tip), 1 * cp.nTxPowMint);
}

// #5c-1 Phase 2 — MINT-SAFETY FLOOR BINDING (the catastrophic-bug gate, economics §5).
// A mint is authorized for a tx IFF that tx clears its OWN per-tx congestion target — the
// SAME GetTxPowTarget the validity predicate uses. So a proof that fails its congestion
// floor mints NOTHING: a miner cannot manufacture cheap self-txs to harvest free C once the
// floor rises. We prove both directions with ONE ground proof: it mints C at the permissive
// floor, and the SAME proof mints 0 once the floor is tightened above its hash.
BOOST_AUTO_TEST_CASE(mint_safety_subfloor_proof_mints_nothing)
{
    SelectParams(ChainType::SANDBOX);
    Consensus::Params cp = Params().GetConsensus();   // mutable copy
    const uint256 anchor_hash{uint256::ONE};
    CBlockIndex tip;
    InitAnchorTip(tip, anchor_hash);
    tip.m_congestion = CONGESTION_ONE;                // start at the 1.0 floor

    // A genuinely proven non-coinbase tx anchored at height 0.
    CMutableTransaction mtx = MakeUnprovenTx(500);
    GrindProof(mtx, cp.nTxEdgeBits, anchor_hash);
    const CTransaction tx{mtx};
    const uint256 proof_hash = cuckatoo::CuckatooProofHash(tx.nCycle);

    CBlock block;
    block.vtx.push_back(MakeCoinbaseRef());
    block.vtx.push_back(MakeTransactionRef(tx));

    // (1) At the permissive floor (sandbox's huge K -> base_coupled ~ 0 -> required==1 ->
    //     target == txPowLimit) the proof clears, so it mints exactly one C.
    BOOST_CHECK_EQUAL(GetBlockMintAllowance(block, cp, &tip), 1 * cp.nTxPowMint);
    BOOST_CHECK(CheckTxProofOfWork(tx, GetTxPowTarget(cp, &tip, tx), anchor_hash, cp));

    // (2) Tighten the floor above this proof's hash. K=1 + a real block difficulty give a
    //     biting base_coupled; m at the cap pushes it higher. (base*m stays < 2^256: base
    //     ~2^32 from nBits=0x1d00ffff, m ~2^22, product ~2^54 — no overflow.)
    cp.nTxWorkCouplingK  = 1;
    cp.nBaseWorkMAWindow = 1;
    tip.nBits = 0x1d00ffffu;                          // GetBlockProof(tip) ~ 2^32
    tip.m_congestion = uint64_t(cp.nCongestionMaxMultiplier) * CONGESTION_ONE; // m at cap

    const uint256 tight_target = GetTxPowTarget(cp, &tip, tx);
    // The floor must TRULY bite this specific proof, else the test proves nothing.
    BOOST_REQUIRE(UintToArith256(proof_hash) > UintToArith256(tight_target));

    // The same proof now fails its own congestion target...
    BOOST_CHECK(!CheckTxProofOfWork(tx, tight_target, anchor_hash, cp));
    // ...and therefore mints NOTHING. Mint-safety holds: C binds at the floor a valid tx
    // must clear, never below it.
    BOOST_CHECK_EQUAL(GetBlockMintAllowance(block, cp, &tip), 0);
}

// Decision criterion 4: maximum issuance must not depend on how small transactions
// are. GetBlockMintAllowance had no count cap -- only a MoneyRange guard -- so a
// block of minimum-size transactions minted proportionally more than a block of
// realistic ones, and nTxPowMint's "2 COIN at a full block" property rested on an
// ASSUMED 1,143-byte transaction rather than on a rule. M4 measured a 357x span in
// transactions per block across realistic shapes.
BOOST_AUTO_TEST_CASE(mint_allowance_is_capped_per_block)
{
    SelectParams(ChainType::SANDBOX);
    const Consensus::Params& cp = Params().GetConsensus();
    const uint256 anchor_hash{uint256::ONE};
    CBlockIndex tip;
    InitAnchorTip(tip, anchor_hash);
    BOOST_REQUIRE_EQUAL(cp.nTxPowMint, 1 * COIN);
    BOOST_REQUIRE_EQUAL(cp.nMaxBlockMint, 2 * COIN);

    // n anchor-valid, genuinely proven, distinct transactions in one block.
    const auto allowance_for = [&](uint32_t n) {
        CBlock block;
        block.vtx.push_back(MakeCoinbaseRef());
        for (uint32_t i = 0; i < n; ++i) {
            CMutableTransaction tx = MakeUnprovenTx(900 + i);
            GrindProof(tx, cp.nTxEdgeBits, anchor_hash);
            block.vtx.push_back(MakeTransactionRef(tx));
        }
        return GetBlockMintAllowance(block, cp, &tip);
    };

    BOOST_CHECK_EQUAL(allowance_for(1), 1 * COIN);  // below the cap: unchanged
    BOOST_CHECK_EQUAL(allowance_for(2), 2 * COIN);  // exactly at it
    BOOST_CHECK_EQUAL(allowance_for(3), 2 * COIN);  // just above: clamped
    BOOST_CHECK_EQUAL(allowance_for(5), 2 * COIN);  // far above: the same number
}

BOOST_AUTO_TEST_SUITE_END()
