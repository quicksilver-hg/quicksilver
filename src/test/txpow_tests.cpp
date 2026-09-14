// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <crypto/cuckatoo/cuckatoo.h>
#include <pow.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <util/chaintype.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(txpow_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(txpow_fields_roundtrip)
{
    CMutableTransaction mtx;
    mtx.vin.resize(1);
    mtx.vout.resize(1);
    mtx.nPowNonce = 0xA1B2C3D4;
    for (uint32_t i = 0; i < 42; ++i) mtx.nCycle[i] = i * 7 + 1;

    DataStream ss;
    ss << TX_WITH_WITNESS(CTransaction(mtx));
    CMutableTransaction back;
    ss >> TX_WITH_WITNESS(back);

    BOOST_CHECK_EQUAL(back.nPowNonce, 0xA1B2C3D4);
    for (uint32_t i = 0; i < 42; ++i) BOOST_CHECK_EQUAL(back.nCycle[i], i * 7 + 1);

    // The proof is part of the txid: mutating nCycle changes the hash.
    CMutableTransaction mtx2 = mtx;
    mtx2.nCycle[0] += 1;
    BOOST_CHECK(CTransaction(mtx).GetHash() != CTransaction(mtx2).GetHash());
}

BOOST_AUTO_TEST_CASE(txpow_preimage_excludes_sig_includes_nonce)
{
    CMutableTransaction mtx;
    mtx.vin.resize(1);
    mtx.vin[0].prevout = COutPoint(Txid::FromUint256(uint256::ONE), 0);
    mtx.vin[0].nSequence = 0xfffffffe;
    mtx.vout.resize(1);
    mtx.vout[0].nValue = 1000;
    mtx.nLockTime = 7;
    mtx.nPowNonce = 42;

    const uint256 anchor{}; // anchor hash is irrelevant to this structural test
    const std::vector<unsigned char> base = CTransaction(mtx).PowPreimage(anchor);

    // scriptSig is NOT in the pre-image: changing it leaves the pre-image equal.
    mtx.vin[0].scriptSig = CScript() << OP_1;
    BOOST_CHECK(CTransaction(mtx).PowPreimage(anchor) == base);

    // nPowNonce IS in the pre-image, as the trailing 4 LE bytes.
    mtx.vin[0].scriptSig.clear();
    mtx.nPowNonce = 43;
    const std::vector<unsigned char> bumped = CTransaction(mtx).PowPreimage(anchor);
    BOOST_CHECK(bumped != base);
    BOOST_CHECK_EQUAL(bumped.size(), base.size());
    BOOST_CHECK_EQUAL(bumped[bumped.size() - 4], 43);
    BOOST_CHECK_EQUAL(base[base.size() - 4], 42);
}

static CMutableTransaction MakeSpend()
{
    CMutableTransaction mtx;
    mtx.vin.resize(1);
    mtx.vin[0].prevout = COutPoint(Txid::FromUint256(uint256::ONE), 0);
    mtx.vout.resize(1);
    mtx.vout[0].nValue = 1000;
    return mtx;
}

// Grinds over PowPreimage(uint256{}); callers must verify with the same zero anchor.
static void GrindProof(CMutableTransaction& mtx, uint8_t edgebits)
{
    const auto pre = CTransaction(mtx).PowPreimage(uint256{});
    cuckatoo::Cycle cyc{};
    uint32_t won = 0;
    BOOST_REQUIRE(cuckatoo::CuckatooSolveBytes(pre.data(), pre.size(), edgebits, 0, 1u << 20, cyc, won));
    mtx.nCycle = cyc;
    mtx.nPowNonce = won;
}

//! One input, `nout` outputs, no witness. Small enough that the size factor is
//! far below one unless the caller pads it.
static CMutableTransaction MakeSizedTx(size_t nout)
{
    CMutableTransaction mtx;
    mtx.vin.resize(1);
    mtx.vout.resize(nout);
    for (auto& out : mtx.vout) {
        out.nValue = 1;
        out.scriptPubKey = CScript() << OP_TRUE;
    }
    return mtx;
}

//! Grow `mtx` by padding vin[0].scriptSig until it serializes to at least `target`
//! with-witness bytes. Returns the size achieved: compact-size and pushdata
//! boundaries mean not every target is exactly reachable, so callers that care
//! assert on the returned value rather than assuming.
static int64_t PadTxToAtLeast(CMutableTransaction& mtx, int64_t target)
{
    for (size_t n = 0;; ++n) {
        mtx.vin[0].scriptSig = CScript() << std::vector<unsigned char>(n, 0x00);
        const int64_t s{static_cast<int64_t>(::GetSerializeSize(TX_WITH_WITNESS(CTransaction(mtx))))};
        if (s >= target) return s;
    }
}

//! A linear chain of `nBits`-identical indices at the congestion floor, so
//! BaseTxWork is a known constant and the transaction-local factor is isolated.
static void BuildFlatChain(std::vector<CBlockIndex>& chain, uint32_t nBits)
{
    for (size_t h = 0; h < chain.size(); ++h) {
        chain[h].nHeight = int(h);
        chain[h].pprev = h ? &chain[h - 1] : nullptr;
        chain[h].nBits = nBits;
        chain[h].m_congestion = CONGESTION_ONE;
        chain[h].BuildSkip();
    }
}

//! The rule, restated independently of pow.cpp so the tests state the RULE rather
//! than echo the implementation: one ceiling division, delta clamped at zero.
static arith_uint256 ExpectedWork(uint64_t base, int64_t bytes, int64_t nout, int64_t nin,
                                  uint64_t r_b = 4739, uint64_t u = 50)
{
    const uint64_t delta = nout > nin ? uint64_t(nout - nin) : 0;
    const arith_uint256 denom{r_b * u};
    const arith_uint256 numerator = arith_uint256{uint64_t(bytes)} * arith_uint256{u}
                              + arith_uint256{delta} * arith_uint256{r_b};
    arith_uint256 out = (arith_uint256{base} * numerator + denom - arith_uint256{1}) / denom;
    return out == 0 ? arith_uint256{1} : out;
}

BOOST_AUTO_TEST_CASE(txpow_predicate_accept_reject)
{
    SelectParams(ChainType::SANDBOX);
    const Consensus::Params& cp = Params().GetConsensus();
    // A null anchor is permissive whatever the transaction, so any tx serves here.
    const uint256 target = GetTxPowTarget(cp, nullptr, CTransaction(MakeSpend()));
    const uint256 anchor{}; // GrindProof grinds over the zero anchor; verify with same

    // Missing proof (all-zero cycle) -> reject.
    CMutableTransaction missing = MakeSpend();
    BOOST_CHECK(!CheckTxProofOfWork(CTransaction(missing), target, anchor, cp));

    // Valid proof + permissive target -> accept.
    CMutableTransaction good = MakeSpend();
    GrindProof(good, cp.nTxEdgeBits);
    BOOST_CHECK(CheckTxProofOfWork(CTransaction(good), target, anchor, cp));

    // Mangled cycle -> reject.
    CMutableTransaction bad = good;
    bad.nCycle[0] ^= 1;
    BOOST_CHECK(!CheckTxProofOfWork(CTransaction(bad), target, anchor, cp));

    // Injected binding target just below the proof's hash -> reject (threshold enforced).
    arith_uint256 a = UintToArith256(cuckatoo::CuckatooProofHash(good.nCycle));
    BOOST_REQUIRE(a > arith_uint256(0));
    a -= 1;
    const uint256 tight = ArithToUint256(a);
    BOOST_CHECK(!CheckTxProofOfWork(CTransaction(good), tight, anchor, cp));

    // Coinbase is exempt even with a zero proof.
    CMutableTransaction cb;
    cb.vin.resize(1);
    cb.vin[0].prevout.SetNull();
    cb.vout.resize(1);
    BOOST_CHECK(CheckTxProofOfWork(CTransaction(cb), target, anchor, cp));
}

BOOST_FIXTURE_TEST_CASE(txpow_connectblock_rejects_unproven_tx, TestChain100Setup)
{
    // A fully-signed spend of a mature coinbase but with NO per-tx proof. The
    // signature is valid, so the ONLY reason the block can fail to connect is the
    // missing per-tx PoW — isolating the rule under test.
    CMutableTransaction spend = CreateValidRelayPoolTransaction(
        m_coinbase_txns[0], /*input_vout=*/0, /*input_height=*/0, coinbaseKey,
        m_coinbase_txns[0]->vout[0].scriptPubKey, /*output_amount=*/49 * COIN,
        /*submit=*/false);

    const CBlock block = CreateAndProcessBlock({spend}, m_coinbase_txns[0]->vout[0].scriptPubKey);
    LOCK(cs_main);
    BOOST_CHECK(m_node.chainman->ActiveChain().Tip()->GetBlockHash() != block.GetHash());
}

BOOST_AUTO_TEST_CASE(check_tx_anchor_window)
{
    // Build a tiny linear chain of CBlockIndex at heights 0..100.
    std::vector<CBlockIndex> chain(101);
    for (int h = 0; h <= 100; ++h) {
        chain[h].nHeight = h;
        chain[h].pprev = (h > 0) ? &chain[h - 1] : nullptr;
        chain[h].phashBlock = nullptr; // hash not needed for anchor-window logic
        chain[h].BuildSkip();
    }
    Consensus::Params cp{};
    cp.nMaxAnchorAge = 20;
    const CBlockIndex* tip = &chain[100];

    auto with_anchor = [](uint32_t a) { CMutableTransaction m; m.vin.resize(1); m.vout.resize(1); m.nAnchorHeight = a; return CTransaction(m); };

    // Spec §4.2: recency rule is inclusive, H - W <= A <= H. With H=100, W=20 the
    // valid window is [80, 100]; A=80 is the H-W edge (still valid), A=79 is too old.
    BOOST_CHECK(CheckTxAnchor(with_anchor(100), tip, cp) == &chain[100]); // tip itself ok
    BOOST_CHECK(CheckTxAnchor(with_anchor(80),  tip, cp) == &chain[80]);  // edge of window (H-W) ok
    BOOST_CHECK(CheckTxAnchor(with_anchor(79),  tip, cp) == nullptr);     // one too old
    BOOST_CHECK(CheckTxAnchor(with_anchor(101), tip, cp) == nullptr);     // future
}

BOOST_AUTO_TEST_CASE(proof_is_bound_to_anchor_hash)
{
    SelectParams(ChainType::SANDBOX); // edgebits=19, cheap to solve
    const Consensus::Params& cp = Params().GetConsensus();
    const uint256 anchor_a = uint256::ONE;
    const uint256 anchor_b{}; // different anchor

    // Build a NON-coinbase tx (real prevout, else IsCoinBase() exempts the PoW check)
    // and grind a valid cycle over the preimage that commits anchor_a.
    CMutableTransaction m; m.vin.resize(1); m.vout.resize(1); m.nAnchorHeight = 1;
    m.vin[0].prevout = COutPoint(Txid::FromUint256(uint256::ONE), 0);
    const std::vector<unsigned char> pre = CTransaction(m).PowPreimage(anchor_a);
    cuckatoo::Cycle cyc{};
    uint32_t nonce = 0;
    BOOST_REQUIRE(cuckatoo::CuckatooSolveBytes(pre.data(), pre.size(), cp.nTxEdgeBits, 0, 1u << 20, cyc, nonce));
    m.nPowNonce = nonce;
    m.nCycle = cyc;
    CTransaction good(m);

    const uint256 target = GetTxPowTarget(cp, nullptr, good); // permissive: no anchor context
    BOOST_CHECK(CheckTxProofOfWork(good, target, anchor_a, cp));   // correct anchor: valid
    BOOST_CHECK(!CheckTxProofOfWork(good, target, anchor_b, cp));  // wrong anchor: invalid
}

BOOST_AUTO_TEST_CASE(congestion_multiplier_recurrence)
{
    Consensus::Params cp{};
    cp.nCongestionTargetPermille = 500;   // T = 0.5
    cp.nCongestionStepDenom      = 8;      // s = 0.125
    cp.nCongestionMaxMultiplier  = 64;     // m_cap
    const int64_t full  = MAX_BLOCK_WEIGHT;          // 100% full
    const int64_t target= MAX_BLOCK_WEIGHT / 2;      // exactly T
    const int64_t empty = 0;

    const uint64_t ONE = CONGESTION_ONE;
    const uint64_t CAP = uint64_t(cp.nCongestionMaxMultiplier) * ONE;

    // At exactly target fullness, m is unchanged.
    BOOST_CHECK_EQUAL(NextCongestionMultiplier(ONE, target, cp), ONE);
    // Above target, m rises (full block: +0.125 from 1.0 -> 1.125).
    BOOST_CHECK_EQUAL(NextCongestionMultiplier(ONE, full, cp), ONE + ONE / 8);
    // Below target, m falls but never under the floor of 1.0.
    BOOST_CHECK_EQUAL(NextCongestionMultiplier(ONE, empty, cp), ONE);          // 1.0 - 0.125 clamps to 1.0
    BOOST_CHECK_EQUAL(NextCongestionMultiplier(ONE + ONE / 4, empty, cp),
                      (ONE + ONE / 4) - (ONE + ONE / 4) / 8);                  // 1.25 -> 1.25 - 0.15625
    // Ceiling clamp: even a full block cannot exceed m_cap.
    BOOST_CHECK_EQUAL(NextCongestionMultiplier(CAP, full, cp), CAP);
}

BOOST_AUTO_TEST_CASE(congestion_step_denom_zero_does_not_divide)
{
    // target_weight <= 0 is already guarded. nCongestionStepDenom == 0 was not:
    // delta = ... / step divides by zero. Shipped params are 4/4/8, so this is
    // unreachable without editing params, but it sits one line from a guard that
    // was thought worth writing. Same policy as a 0 target: no recurrence, clamp
    // prev_m. Restoring the unguarded `/ step` SIGFPEs this case.
    Consensus::Params cp{};
    cp.nCongestionTargetPermille = 500;
    cp.nCongestionStepDenom      = 0;
    cp.nCongestionMaxMultiplier  = 64;
    const uint64_t ONE = CONGESTION_ONE;
    const uint64_t CAP = uint64_t(cp.nCongestionMaxMultiplier) * ONE;
    const int64_t full   = MAX_BLOCK_WEIGHT;
    const int64_t empty  = 0;
    const int64_t target = MAX_BLOCK_WEIGHT / 2;
    const uint64_t mid   = ONE + ONE / 2;

    BOOST_CHECK_EQUAL(NextCongestionMultiplier(ONE, full, cp), ONE);
    BOOST_CHECK_EQUAL(NextCongestionMultiplier(ONE, empty, cp), ONE);
    BOOST_CHECK_EQUAL(NextCongestionMultiplier(ONE, target, cp), ONE);
    BOOST_CHECK_EQUAL(NextCongestionMultiplier(mid, full, cp), mid);
    BOOST_CHECK_EQUAL(NextCongestionMultiplier(CAP, full, cp), CAP);
    BOOST_CHECK_EQUAL(NextCongestionMultiplier(0, full, cp), ONE);     // below floor
    BOOST_CHECK_EQUAL(NextCongestionMultiplier(CAP + 1, empty, cp), CAP); // above cap

    // Negative denom becomes a huge uint64 after the cast and would silently
    // no-op (delta ~ 0). Treat it as the same fail-closed case as zero.
    cp.nCongestionStepDenom = -1;
    BOOST_CHECK_EQUAL(NextCongestionMultiplier(ONE, full, cp), ONE);
    BOOST_CHECK_EQUAL(NextCongestionMultiplier(mid, full, cp), mid);
}

BOOST_AUTO_TEST_CASE(base_tx_work_and_target)
{
    SelectParams(ChainType::SANDBOX);
    Consensus::Params cp = Params().GetConsensus(); // sandbox fixture
    cp.nBaseWorkMAWindow = 4;
    cp.nTxWorkCouplingK  = 1;                        // K=1 so base_coupled bites in this unit test

    // Stage 2 makes required work per-transaction, so a probe is needed -- and it must
    // be big enough for the size factor to clear the one-cycle floor, or congestion
    // has nothing to move. base is 2 here, and the factor is bytes/R_b, so exactly
    // R_b bytes gives 2 cycles at m = 1 and 4 at m = 2.
    CMutableTransaction probe_mtx = MakeSizedTx(1);
    BOOST_REQUIRE_EQUAL(PadTxToAtLeast(probe_mtx, 4739), 4739);
    const CTransaction probe{probe_mtx};

    // Linear chain of 10 indices, all with the same nBits (constant blockwork).
    std::vector<CBlockIndex> chain(10);
    for (int h = 0; h < 10; ++h) {
        chain[h].nHeight = h;
        chain[h].pprev = h ? &chain[h - 1] : nullptr;
        chain[h].nBits = 0x207fffff;                 // sandbox-style easy bits => small blockwork
        chain[h].m_congestion = CONGESTION_ONE;      // m = 1.0
        chain[h].BuildSkip();
    }
    CBlockIndex* anchor = &chain[9];

    // base_coupled = MA(blockwork)/K; at m=1 the base work equals base_coupled.
    const arith_uint256 base = BaseTxWork(anchor, cp);
    BOOST_CHECK(base > 0);
    const arith_uint256 target_m1 = UintToArith256(GetTxPowTarget(cp, anchor, probe));

    // Doubling m raises required work (target gets harder => strictly smaller). Mutate the
    // same anchor in place — CBlockIndex's copy ctor is protected.
    anchor->m_congestion = 2 * CONGESTION_ONE;       // m = 2.0
    const arith_uint256 work_m2 = BaseTxWork(anchor, cp);
    const arith_uint256 target_m2 = UintToArith256(GetTxPowTarget(cp, anchor, probe));
    BOOST_CHECK(work_m2 > base);                      // more congestion => more work
    BOOST_CHECK(target_m2 < target_m1);              // harder target
    anchor->m_congestion = CONGESTION_ONE;           // restore the floor

    // The floor never makes work EASIER than txPowLimit (congestion only pushes work up).
    BOOST_CHECK(target_m1 <= UintToArith256(cp.txPowLimit));

    // Permissive coupling (huge K) drives base_coupled to zero -> required clamps to 1 ->
    // the floor must be FULLY permissive: target == txPowLimit exactly (not 2^255). This is
    // the regression for the floor-undershoot bug that let ~half of proofs fail the floor.
    Consensus::Params perm = cp;
    perm.nTxWorkCouplingK = 0xFFFFFFFFu;             // huge K => tiny base_coupled => permissive
    BOOST_CHECK_EQUAL(GetTxPowTarget(perm, anchor, probe), perm.txPowLimit);

    // Stage 2 consequence, asserted rather than left implicit: congestion moves the
    // CHAIN scalar, but a transaction only feels it once its size factor lifts the
    // product past the one-cycle floor. A 237-byte payment at base 2 owes 237/4739 of
    // a cycle either way, so doubling m leaves its target alone. This is finding 3.1
    // working as intended -- a transaction is charged for its share of the block, not
    // a flat per-transaction price -- and it is why the probe above is padded.
    const CTransaction tiny{MakeSizedTx(1)};
    BOOST_CHECK_EQUAL(::GetSerializeSize(TX_WITH_WITNESS(tiny)), 237);
    BOOST_CHECK_EQUAL(RequiredTxWork(tiny, anchor, cp).GetHex(), arith_uint256(1).GetHex());
    anchor->m_congestion = 2 * CONGESTION_ONE;
    BOOST_CHECK_EQUAL(RequiredTxWork(tiny, anchor, cp).GetHex(), arith_uint256(1).GetHex());
    anchor->m_congestion = CONGESTION_ONE;
}

// #5c-1 Phase 3 — m-dynamics validation gate for the calibrated congestion params
// (s = 0.25 => nCongestionStepDenom 4, m_cap = 64). Confirms the floor ratchets UP
// under sustained over-target load at the intended rate (~double in 4 blocks), clamps
// at the cap, and DECAYS back to the floor under empty blocks without sticking.
BOOST_AUTO_TEST_CASE(congestion_m_dynamics_calibrated)
{
    Consensus::Params cp{};
    cp.nCongestionTargetPermille = 500;   // T = 0.5  (calibrated)
    cp.nCongestionStepDenom      = 4;      // s = 0.25 (calibrated)
    cp.nCongestionMaxMultiplier  = 64;     // m_cap     (calibrated)

    const uint64_t ONE = CONGESTION_ONE;
    const uint64_t CAP = uint64_t(cp.nCongestionMaxMultiplier) * ONE;
    const int64_t full  = MAX_BLOCK_WEIGHT;        // 100% full => above target
    const int64_t empty = 0;                       // 0% full   => below target

    // (1) RISE: a full block multiplies m by 1.25 (delta = prev/4). The floor doubles
    // within 4 full blocks (1.25^3 = 1.953 < 2 <= 1.25^4 = 2.441) and is strictly rising.
    uint64_t m = ONE, prev = m;
    for (int i = 0; i < 4; ++i) {
        m = NextCongestionMultiplier(m, full, cp);
        BOOST_CHECK(m > prev);                     // strictly rising under sustained load
        prev = m;
    }
    BOOST_CHECK(m >= 2 * ONE);                      // doubled within 4 blocks (~15 min @ 5-min blocks)
    BOOST_CHECK(m <  3 * ONE);                      // but no overshoot (1.25^4 = 2.44x)

    // (2) CAP CLAMP: sustained full blocks pin m at the cap and never exceed it.
    for (int i = 0; i < 100; ++i) m = NextCongestionMultiplier(m, full, cp);
    BOOST_CHECK_EQUAL(m, CAP);
    BOOST_CHECK_EQUAL(NextCongestionMultiplier(CAP, full, cp), CAP);

    // (3) DECAY: from the cap, empty blocks multiply m by 0.75 (delta = prev/4),
    // strictly decreasing, returning to the floor without sticking, then clamped at ONE.
    int steps = 0;
    while (m > ONE && steps < 1000) {
        uint64_t next = NextCongestionMultiplier(m, empty, cp);
        BOOST_CHECK(next < m);                      // strictly decaying, never stuck
        m = next;
        ++steps;
    }
    BOOST_CHECK_EQUAL(m, ONE);                      // returned exactly to the floor
    BOOST_CHECK(steps <= 20);                       // 64x -> 1x in ~15 empty blocks
    BOOST_CHECK_EQUAL(NextCongestionMultiplier(ONE, empty, cp), ONE); // clamped at the floor
}

// A committed constant cycle (NOT runtime-grinded). The runtime-grind cases above
// only prove each platform verifies its OWN grind; this vector proves that a set of
// STORED cycle bytes hashes and verifies to fixed values identically on GCC and MSVC
// — the compiler-determinism guarantee behind the live cross-platform tx-PoW test
// (Linux <-> Windows 10, 2026-07-25). The constants were grinded once on
// Linux at sandbox edgebits=19 over MakeSpend()'s preimage committing the zero anchor;
// regenerate by grinding MakeSpend() and printing nPowNonce/nCycle/CuckatooProofHash.
BOOST_AUTO_TEST_CASE(txpow_committed_xplat_vector)
{
    SelectParams(ChainType::SANDBOX);              // edgebits=19, matches the grind
    const Consensus::Params& cp = Params().GetConsensus();
    const uint256 anchor{};                        // zero anchor (matches GrindProof)

    // Grinded constants — reproduce the exact preimage: MakeSpend() + this nonce.
    static const uint32_t CYC[42] = {
        37392,65786,71970,80690,86634,119523,128148,135823,136745,137262,
        156089,167983,184921,186762,187197,189923,190226,195783,196960,197245,
        210196,219836,257676,274485,279435,312041,321711,334666,337118,346626,
        350956,382531,382573,445067,450825,460444,475022,476540,496696,498603,
        513301,516196};
    CMutableTransaction mtx = MakeSpend();
    mtx.nPowNonce = 228;
    for (int i = 0; i < 42; ++i) mtx.nCycle[i] = CYC[i];
    const CTransaction tx(mtx);

    // (1) exact-bytes determinism of the proof hash across compilers.
    const uint256 h = cuckatoo::CuckatooProofHash(tx.nCycle);
    BOOST_CHECK_EQUAL(h.GetHex(),
        "fb33f340729faf6be68e3330f6ce9a71441161ef9c3075fa12b93052e0d471ad");

    // (2) structural cycle verification of the stored bytes over the fixed preimage.
    const auto pre = tx.PowPreimage(anchor);
    const cuckatoo::Keys keys = cuckatoo::CuckatooSetHeader(pre.data(), pre.size());
    BOOST_CHECK(cuckatoo::CuckatooVerify(tx.nCycle, keys, cp.nTxEdgeBits));

    // (3) accept exactly at target == proof hash (boundary).
    BOOST_CHECK(CheckTxProofOfWork(tx, h, anchor, cp));

    // (4) an elevated (tighter) target one below the hash rejects.
    const arith_uint256 tighter = UintToArith256(h) - 1;
    BOOST_CHECK(!CheckTxProofOfWork(tx, ArithToUint256(tighter), anchor, cp));
}

// Stage 2 flag day: the size term, the UTXO term, and the mint cap are consensus
// parameters, and their values are the ones the calibration model was run against
// (tools/calibration/stage2-byte-pricing.md). A change here is a change to the
// chain, not a tuning knob. Uniform across networks so the sandbox exercises the
// shipped arithmetic rather than a special case that could hide a bug.
BOOST_AUTO_TEST_CASE(stage2_reference_parameters_are_the_calibrated_values)
{
    for (const ChainType chain : {ChainType::MAIN, ChainType::PUBLIC_TEST, ChainType::SANDBOX}) {
        SelectParams(chain);
        const Consensus::Params& cp = Params().GetConsensus();
        // R_b: serialized bytes that cost one unit of base work. R/4, where
        // R = 18,957 is the Stage 1 weight reference.
        BOOST_CHECK_EQUAL(cp.nTxWorkRefBytes, 4739u);
        // U: net new UTXOs that cost one unit of base work. Inside the 28-109
        // window the model reports.
        BOOST_CHECK_EQUAL(cp.nTxUtxoRefCount, 50u);
        // The per-block mint cap makes maximum issuance independent of tx size.
        BOOST_CHECK_EQUAL(cp.nMaxBlockMint, 2 * COIN);
    }
    SelectParams(ChainType::SANDBOX);
}

// --- Stage 2: the size and UTXO terms --------------------------------------

BOOST_AUTO_TEST_CASE(required_tx_work_prices_serialized_bytes)
{
    SelectParams(ChainType::SANDBOX);
    const Consensus::Params& cp = Params().GetConsensus();
    std::vector<CBlockIndex> chain(10);
    BuildFlatChain(chain, 0x207fffff);
    const CBlockIndex* anchor = &chain[9];

    // Sandbox's huge K drives base_coupled to zero and BaseTxWork's clamp raises it
    // to one, so required work here IS the transaction-local factor.
    BOOST_REQUIRE_EQUAL(BaseTxWork(anchor, cp).GetHex(), arith_uint256(1).GetHex());
    BOOST_REQUIRE_EQUAL(cp.nTxWorkRefBytes, 4739u);

    // A small transaction pays one cycle: the factor is far below one and the floor
    // catches it.
    CMutableTransaction small = MakeSizedTx(1);
    BOOST_CHECK_EQUAL(RequiredTxWork(CTransaction(small), anchor, cp).GetHex(),
                      arith_uint256(1).GetHex());

    // Exactly R_b bytes is exactly one unit; one byte more is two, because the
    // division rounds UP.
    CMutableTransaction at_ref = MakeSizedTx(1);
    BOOST_CHECK_EQUAL(PadTxToAtLeast(at_ref, 4739), 4739);
    BOOST_CHECK_EQUAL(RequiredTxWork(CTransaction(at_ref), anchor, cp).GetHex(),
                      arith_uint256(1).GetHex());

    CMutableTransaction over_ref = MakeSizedTx(1);
    BOOST_CHECK_EQUAL(PadTxToAtLeast(over_ref, 4740), 4740);
    BOOST_CHECK_EQUAL(RequiredTxWork(CTransaction(over_ref), anchor, cp).GetHex(),
                      arith_uint256(2).GetHex());

    // The attacker's best slice under FLOOR division is 2*R_b - 1 = 9,477 bytes,
    // where flooring charges one unit for nearly two and halves the cost of filling
    // a block (model: criterion 1 at 0.5024). Under ceiling division it costs two.
    // This assertion IS the fix.
    CMutableTransaction best_slice = MakeSizedTx(1);
    BOOST_CHECK_EQUAL(PadTxToAtLeast(best_slice, 9477), 9477);
    BOOST_CHECK_EQUAL(RequiredTxWork(CTransaction(best_slice), anchor, cp).GetHex(),
                      arith_uint256(2).GetHex());
}

BOOST_AUTO_TEST_CASE(required_tx_work_prices_utxo_creation)
{
    SelectParams(ChainType::SANDBOX);
    const Consensus::Params& cp = Params().GetConsensus();
    std::vector<CBlockIndex> chain(10);
    BuildFlatChain(chain, 0x207fffff);
    const CBlockIndex* anchor = &chain[9];
    BOOST_REQUIRE_EQUAL(cp.nTxUtxoRefCount, 50u);

    // 200 outputs from one input: delta 199, so the UTXO term alone is 199/50 = 3.98
    // and dominates a byte term of about 0.47.
    CMutableTransaction many = MakeSizedTx(200);
    const int64_t bytes{static_cast<int64_t>(::GetSerializeSize(TX_WITH_WITNESS(CTransaction(many))))};
    const arith_uint256 expected{ExpectedWork(1, bytes, 200, 1)};
    BOOST_CHECK(expected > 1); // else the term is not biting and the test proves nothing
    BOOST_CHECK_EQUAL(RequiredTxWork(CTransaction(many), anchor, cp).GetHex(),
                      expected.GetHex());

    // Bytes alone would have charged the floor. The difference is the UTXO term.
    BOOST_CHECK_EQUAL(ExpectedWork(1, bytes, 1, 1).GetHex(), arith_uint256(1).GetHex());
}

BOOST_AUTO_TEST_CASE(utxo_delta_clamps_at_zero_so_consolidation_is_never_rewarded)
{
    SelectParams(ChainType::SANDBOX);
    Consensus::Params cp = Params().GetConsensus();
    // A base that actually bites, so the clamp is observable rather than swallowed
    // by the one-cycle floor. K=1 over a one-block window makes base = block work.
    cp.nTxWorkCouplingK = 1;
    cp.nBaseWorkMAWindow = 1;
    std::vector<CBlockIndex> chain(10);
    BuildFlatChain(chain, 0x1d00ffffu); // GetBlockProof ~ 2^32
    const CBlockIndex* anchor = &chain[9];
    const arith_uint256 base{BaseTxWork(anchor, cp)};
    BOOST_REQUIRE(base > 1000); // the clamp must be visible, not floored away

    // 40 inputs, one output: raw delta is -39. Crediting it would refund 39/50 of
    // base against a byte term of ~0.39, so required work would collapse to the
    // floor and adding inputs would make a transaction CHEAPER.
    CMutableTransaction consolidating = MakeSizedTx(1);
    consolidating.vin.resize(40);
    const int64_t bytes{static_cast<int64_t>(::GetSerializeSize(TX_WITH_WITNESS(CTransaction(consolidating))))};
    const arith_uint256 byte_only{ExpectedWork(base.GetLow64(), bytes, 0, 0)};
    BOOST_REQUIRE(byte_only > 1);
    BOOST_CHECK_EQUAL(RequiredTxWork(CTransaction(consolidating), anchor, cp).GetHex(),
                      byte_only.GetHex());

    // A balanced transaction of the same byte count is charged the same: consolidation
    // is never penalised for consuming inputs and never rewarded for it either.
    CMutableTransaction balanced = MakeSizedTx(1);
    const int64_t achieved{PadTxToAtLeast(balanced, bytes)};
    BOOST_CHECK_EQUAL(achieved, bytes);
    BOOST_CHECK_EQUAL(RequiredTxWork(CTransaction(balanced), anchor, cp).GetHex(),
                      byte_only.GetHex());
}

BOOST_AUTO_TEST_CASE(required_tx_work_does_not_wrap_on_a_huge_base)
{
    SelectParams(ChainType::SANDBOX);
    Consensus::Params cp = Params().GetConsensus();
    cp.nTxWorkCouplingK = 1;
    cp.nBaseWorkMAWindow = 1;
    // Target 2^17, so block work is ~2^239 -- the largest base BaseTxWork itself can
    // carry through its own `base * m / CONGESTION_ONE` step without wrapping.
    std::vector<CBlockIndex> chain(10);
    BuildFlatChain(chain, 0x04000200u);
    const CBlockIndex* anchor = &chain[9];
    const arith_uint256 base{BaseTxWork(anchor, cp)};
    BOOST_REQUIRE(base > (arith_uint256(1) << 200));

    // arith_uint256 multiplication wraps SILENTLY, and a wrapped product is SMALLER,
    // which would be a free-transaction bug. Real base is nowhere near this, which is
    // exactly why the guard must be written and tested rather than assumed.
    CMutableTransaction big = MakeSizedTx(1);
    PadTxToAtLeast(big, 100000);
    const arith_uint256 required{RequiredTxWork(CTransaction(big), anchor, cp)};
    // A transaction larger than the reference can never owe LESS than base. That
    // inequality is the wrap signature, and it is what the guard stops.
    BOOST_CHECK(required >= base);
}

BOOST_AUTO_TEST_CASE(base_tx_work_does_not_wrap_when_congestion_exceeds_one)
{
    SelectParams(ChainType::SANDBOX);
    Consensus::Params cp = Params().GetConsensus();
    cp.nTxWorkCouplingK = 1;
    cp.nBaseWorkMAWindow = 1;
    // Same nBits as required_tx_work_does_not_wrap_on_a_huge_base: the largest
    // base that survives BaseTxWork's `base * m / CONGESTION_ONE` step at
    // multiplier 1.0. Doubling m still fits; tripling it wraps.
    std::vector<CBlockIndex> chain(10);
    BuildFlatChain(chain, 0x04000200u);
    CBlockIndex& anchor = chain[9];

    const arith_uint256 base_m1{BaseTxWork(&anchor, cp)};
    BOOST_REQUIRE(base_m1 > (arith_uint256(1) << 200));
    BOOST_REQUIRE_EQUAL(anchor.m_congestion, CONGESTION_ONE);

    // Control: multiplier 2.0 does not overflow this base. Saturating here
    // would change a result that still fits, which is not the defect.
    anchor.m_congestion = 2 * CONGESTION_ONE;
    const arith_uint256 at_m2{BaseTxWork(&anchor, cp)};
    BOOST_CHECK_EQUAL(at_m2.GetHex(), (base_m1 * arith_uint256(2)).GetHex());

    // Multiplier 3.0 wraps the product. A wrapped required is smaller than
    // base_m1 -- the free-transaction signature -- which is what the guard
    // stops. nCongestionMaxMultiplier (64) wraps too, but the wrapped quotient
    // there is still >= base_m1, so that case cannot prove the guard.
    anchor.m_congestion = 3 * CONGESTION_ONE;
    const arith_uint256 at_m3{BaseTxWork(&anchor, cp)};
    BOOST_CHECK(at_m3 >= base_m1);
    BOOST_CHECK_EQUAL(at_m3.GetHex(), (~arith_uint256(0)).GetHex());
}

BOOST_AUTO_TEST_SUITE_END()
