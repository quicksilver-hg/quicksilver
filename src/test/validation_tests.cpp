// Copyright (c) 2014-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <core_io.h>
#include <crypto/cuckatoo/cuckatoo.h>
#include <hash.h>
#include <net.h>
#include <node/miner.h>
#include <pow.h>
#include <primitives/transaction.h>
#include <test/util/mining.h>
#include <uint256.h>
#include <util/chaintype.h>
#include <validation.h>

#include <algorithm>
#include <string>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(validation_tests, TestingSetup)

// Quicksilver tail emission (#5a): GetBlockSubsidy is a near-linear, front-loaded ramp from
// the genesis subsidy S0 down to a perpetual tail, reaching the tail at N = nBootstrapBlocks.
// Mainnet/publictest magnitudes are calibrated in the tail-emission spec. These tests use
// sandbox's deliberately small N so the sums/sweeps are cheap, and pin curve shape.

BOOST_AUTO_TEST_CASE(block_subsidy_ramp_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::SANDBOX);
    const Consensus::Params& c = chainParams->GetConsensus();
    const CAmount S0 = c.nInitialSubsidy;
    const CAmount tail = c.nTailSubsidy;
    const int N = c.nBootstrapBlocks;

    BOOST_CHECK_EQUAL(GetBlockSubsidy(0, c), S0);               // genesis height = top of ramp
    BOOST_CHECK_EQUAL(GetBlockSubsidy(N, c), tail);             // tail reached exactly at N
    BOOST_CHECK_EQUAL(GetBlockSubsidy(N + 1, c), tail);         // perpetual tail
    BOOST_CHECK_EQUAL(GetBlockSubsidy(N + 1'000'000, c), tail);

    // Last bootstrap block: one step above the tail, never above S0.
    const CAmount last = GetBlockSubsidy(N - 1, c);
    BOOST_CHECK(last > tail);
    BOOST_CHECK(last <= S0);
}

BOOST_AUTO_TEST_CASE(block_subsidy_monotonic_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::SANDBOX);
    const Consensus::Params& c = chainParams->GetConsensus();
    const int N = c.nBootstrapBlocks;

    CAmount prev = GetBlockSubsidy(0, c);
    for (int h = 1; h <= N + 50; ++h) {
        const CAmount s = GetBlockSubsidy(h, c);
        BOOST_CHECK(s <= prev);          // monotonic non-increasing
        BOOST_CHECK(MoneyRange(s));      // always a valid money amount
        prev = s;
    }
}

BOOST_AUTO_TEST_CASE(block_subsidy_overflow_test)
{
    // Inflated S0 near MAX_MONEY stresses the ramp where the naive product drop*nHeight far
    // exceeds int64: values must stay in range and monotonic with no wraparound. GetBlockSubsidy
    // computes floor(drop*h/N) with a split-integer identity (no 128-bit intermediate, so the
    // Windows/MSVC build compiles); block_subsidy_int128_equivalence_test below proves that split
    // is bit-for-bit identical to the 128-bit reference. (S0 here is a stress value, not a placeholder.)
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::SANDBOX);
    Consensus::Params c = chainParams->GetConsensus();   // copy we can mutate
    c.nInitialSubsidy = MAX_MONEY;
    c.nTailSubsidy = 1 * COIN;
    c.nBootstrapBlocks = 1'051'920;

    BOOST_CHECK_EQUAL(GetBlockSubsidy(0, c), MAX_MONEY);
    BOOST_CHECK_EQUAL(GetBlockSubsidy(c.nBootstrapBlocks, c), 1 * COIN);

    CAmount prev = GetBlockSubsidy(0, c);
    for (int h = 1; h <= c.nBootstrapBlocks; h += 997) {
        const CAmount s = GetBlockSubsidy(h, c);
        BOOST_CHECK(s <= prev);
        BOOST_CHECK(MoneyRange(s));
        prev = s;
    }
}

#ifdef __SIZEOF_INT128__
// Prove the shipping split-integer subsidy math is bit-for-bit identical to a straightforward
// 128-bit floor(drop*h/N) reference, across a grid that includes drop = MAX_MONEY and heights
// packed right up against N. GetBlockSubsidy carries no 128-bit intermediate (MSVC lacks __int128),
// so this oracle-guarded sweep is the regression gate that the split stays faithful. The oracle
// only compiles where __int128 exists (the Linux CI gate); the production path never uses it.
BOOST_AUTO_TEST_CASE(block_subsidy_int128_equivalence_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::SANDBOX);
    Consensus::Params c = chainParams->GetConsensus();   // copy we can mutate

    // (S0, tail, N) grid: sandbox-scale, shipping mainnet magnitudes, and MAX_MONEY stress.
    const struct { CAmount S0; CAmount tail; int N; } cases[] = {
        {50 * COIN,   1 * COIN, 150},
        {50 * COIN,   1 * COIN, 1'051'920},
        {MAX_MONEY,   1 * COIN, 1'051'920},
        {MAX_MONEY,   0,        1'051'920},
        {MAX_MONEY,   1,        1'000'003},   // prime-ish N exercises the remainder term
    };

    for (const auto& tc : cases) {
        c.nInitialSubsidy = tc.S0;
        c.nTailSubsidy = tc.tail;
        c.nBootstrapBlocks = tc.N;
        const __int128 drop = static_cast<__int128>(tc.S0) - tc.tail;

        // Sample heights: the first/last few blocks, a stride sweep, and every h in [N-4, N+2]
        // so the ramp-to-tail boundary is covered exactly.
        auto check_height = [&](int h) {
            CAmount expected;
            if (h >= tc.N) {
                expected = tc.tail;
            } else {
                const CAmount decrement = static_cast<CAmount>(drop * h / tc.N);
                expected = tc.S0 - decrement;
            }
            BOOST_CHECK_EQUAL(GetBlockSubsidy(h, c), expected);
        };

        for (int h = 0; h < 5; ++h) check_height(h);
        for (int h = 0; h <= tc.N + 2; h += 4999) check_height(h);
        for (int h = std::max(0, tc.N - 4); h <= tc.N + 2; ++h) check_height(h);
    }
}
#endif // __SIZEOF_INT128__

BOOST_AUTO_TEST_CASE(block_subsidy_total_supply_test)
{
    // Pin the AREA under the ramp (the quantity calibration tunes). The discrete left-sum of a
    // decreasing ramp equals S0*N - drop*(N-1)/2 to within the per-block floor error (< 1 cinnabar
    // each, < N total). NOTE: the continuous trapezoid ((S0+tail)/2)*N is the WRONG reference
    // for a per-block sum — it sits ~drop/2 lower.
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::SANDBOX);
    const Consensus::Params& c = chainParams->GetConsensus();
    const CAmount S0 = c.nInitialSubsidy;
    const CAmount tail = c.nTailSubsidy;
    const int N = c.nBootstrapBlocks;
    const CAmount drop = S0 - tail;

    CAmount total = 0;
    for (int h = 0; h < N; ++h) total += GetBlockSubsidy(h, c);

    // int64 is exact here: sandbox S0*N = 50*COIN*150 ≈ 7.5e11, ~1e7x under INT64_MAX,
    // so no 128-bit intermediate is needed (MSVC lacks __int128; shipping GetBlockSubsidy
    // is int64-only too). Value is identical to the former __int128 computation.
    const CAmount closed_form = S0 * N - (drop * (N - 1)) / 2;
    BOOST_CHECK(total <= closed_form + N);
    BOOST_CHECK(total >= closed_form - N);
}

// Quicksilver tail-emission CALIBRATION lock (2026-07-09). The parametric curve tests
// above prove GetBlockSubsidy has the right *shape* for whatever params it is given.
// This test locks the finalized *magnitudes* on mainnet so accidental drift of S0/tail/N
// fails loudly. Values finalized in 2026-07-09-tail-emission-calibration-design.md.
BOOST_AUTO_TEST_CASE(calibrated_emission_lock_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    const Consensus::Params& c = chainParams->GetConsensus();

    // 1) Scalar magnitude locks.
    BOOST_CHECK_EQUAL(c.nInitialSubsidy,  50 * COIN);   // S0
    BOOST_CHECK_EQUAL(c.nTailSubsidy,      1 * COIN);   // tail
    BOOST_CHECK_EQUAL(c.nBootstrapBlocks, 1'051'920);   // N = 10yr @ 5-min

    // 2) Total-bootstrap-supply lock, exact ==, no magic literal.
    // `expected` is recomputed from the CALIBRATED LITERALS (not c.*), mirroring
    // GetBlockSubsidy's integer floor arithmetic bit-for-bit, so any drift in
    // chainparams' S0/tail/N breaks this ==. Analytical cross-check for the floored
    // sum is Gauss's identity  Σ_{h=1}^{N-1} floor(m*h/N) = ((m-1)(N-1)+gcd(m,N)-1)/2;
    // the loop below is what we assert (readable, no obscure identity to trust).
    const CAmount kS0 = 50 * COIN, kTail = 1 * COIN;
    const int     kN  = 1'051'920;
    const CAmount kDrop = kS0 - kTail;
    // int64 is exact here: max kDrop*h = 49*COIN*(kN-1) ≈ 5.15e15, ~1790x under INT64_MAX,
    // mirroring GetBlockSubsidy's int64 floor(drop*h/N) bit-for-bit (MSVC lacks __int128).
    CAmount expected = 0;
    for (int h = 0; h < kN; ++h)
        expected += kS0 - kDrop * h / kN;

    CAmount actual = 0;
    for (int h = 0; h < c.nBootstrapBlocks; ++h)
        actual += GetBlockSubsidy(h, c);

    BOOST_CHECK_EQUAL(actual, expected);
}

BOOST_AUTO_TEST_CASE(block_malleation)
{
    // Test utilities that calls `IsBlockMutated` and then clears the validity
    // cache flags on `CBlock`.
    auto is_mutated = [](CBlock& block, bool check_witness_root) {
        bool mutated{IsBlockMutated(block, check_witness_root)};
        block.fChecked = false;
        block.m_checked_witness_commitment = false;
        block.m_checked_merkle_root = false;
        return mutated;
    };
    auto is_not_mutated = [&is_mutated](CBlock& block, bool check_witness_root) {
        return !is_mutated(block, check_witness_root);
    };

    // Test utilities to create coinbase transactions and insert witness
    // commitments.
    //
    // Note: this will not include the witness stack by default to avoid
    // triggering the "no witnesses allowed for blocks that don't commit to
    // witnesses" rule when testing other malleation vectors.
    auto create_coinbase_tx = [](bool include_witness = false) {
        CMutableTransaction coinbase;
        coinbase.vin.resize(1);
        if (include_witness) {
            coinbase.vin[0].scriptWitness.stack.resize(1);
            coinbase.vin[0].scriptWitness.stack[0] = std::vector<unsigned char>(32, 0x00);
        }

        coinbase.vout.resize(1);
        coinbase.vout[0].scriptPubKey.resize(MINIMUM_WITNESS_COMMITMENT);
        coinbase.vout[0].scriptPubKey[0] = OP_RETURN;
        coinbase.vout[0].scriptPubKey[1] = 0x24;
        coinbase.vout[0].scriptPubKey[2] = 0xaa;
        coinbase.vout[0].scriptPubKey[3] = 0x21;
        coinbase.vout[0].scriptPubKey[4] = 0xa9;
        coinbase.vout[0].scriptPubKey[5] = 0xed;

        auto tx = MakeTransactionRef(coinbase);
        assert(tx->IsCoinBase());
        return tx;
    };
    auto insert_witness_commitment = [](CBlock& block, uint256 commitment) {
        assert(!block.vtx.empty() && block.vtx[0]->IsCoinBase() && !block.vtx[0]->vout.empty());

        CMutableTransaction mtx{*block.vtx[0]};
        CHash256().Write(commitment).Write(std::vector<unsigned char>(32, 0x00)).Finalize(commitment);
        memcpy(&mtx.vout[0].scriptPubKey[6], commitment.begin(), 32);
        block.vtx[0] = MakeTransactionRef(mtx);
    };

    {
        CBlock block;

        // Empty block is expected to have merkle root of 0x0.
        BOOST_CHECK(block.vtx.empty());
        block.hashMerkleRoot = uint256{1};
        BOOST_CHECK(is_mutated(block, /*check_witness_root=*/false));
        block.hashMerkleRoot = uint256{};
        BOOST_CHECK(is_not_mutated(block, /*check_witness_root=*/false));

        // Block with a single coinbase tx is mutated if the merkle root is not
        // equal to the coinbase tx's hash.
        block.vtx.push_back(create_coinbase_tx());
        BOOST_CHECK(block.vtx[0]->GetHash() != block.hashMerkleRoot);
        BOOST_CHECK(is_mutated(block, /*check_witness_root=*/false));
        block.hashMerkleRoot = block.vtx[0]->GetHash();
        BOOST_CHECK(is_not_mutated(block, /*check_witness_root=*/false));

        // Block with two transactions is mutated if the merkle root does not
        // match the double sha256 of the concatenation of the two transaction
        // hashes.
        block.vtx.push_back(MakeTransactionRef(CMutableTransaction{}));
        BOOST_CHECK(is_mutated(block, /*check_witness_root=*/false));
        HashWriter hasher;
        hasher.write(block.vtx[0]->GetHash());
        hasher.write(block.vtx[1]->GetHash());
        block.hashMerkleRoot = hasher.GetHash();
        BOOST_CHECK(is_not_mutated(block, /*check_witness_root=*/false));

        // Block with two transactions is mutated if any node is duplicate.
        {
            block.vtx[1] = block.vtx[0];
            HashWriter hasher;
            hasher.write(block.vtx[0]->GetHash());
            hasher.write(block.vtx[1]->GetHash());
            block.hashMerkleRoot = hasher.GetHash();
            BOOST_CHECK(is_mutated(block, /*check_witness_root=*/false));
        }

        // CVE-2012-2459 / 64-byte-transaction merkle-node masquerade is
        // STRUCTURALLY CLOSED in this fork. Every transaction's non-witness
        // serialization carries an unconditional 176-byte per-tx PoW tail
        // (nPowNonce[4] + nCycle[42*4]), appended after nLockTime for ALL
        // transactions including the coinbase (see (Un)SerializeTransaction in
        // primitives/transaction.h). The smallest possible transaction is
        // therefore 64 + 172 = 236 bytes, so no transaction can ever be
        // byte-indistinguishable from a 64-byte internal merkle node.
        //
        // Upstream's "a 64-byte coinbase tx is not falsely flagged mutated"
        // sub-case is unconstructible here. Rather than delete it (which would
        // leave the size invariant untested and let the vector silently reopen
        // if the PoW tail is ever shrunk or made conditional), we repurpose it
        // into a guard that locks in the structural property.
        block.vtx.clear();
        {
            CMutableTransaction mtx;
            mtx.vin.resize(1);
            mtx.vout.resize(1);
            mtx.vout[0].scriptPubKey.resize(4);  // upstream: exactly 64 bytes
            block.vtx.push_back(MakeTransactionRef(mtx));
            block.hashMerkleRoot = block.vtx.back()->GetHash();
            assert(block.vtx.back()->IsCoinBase());

            // Structural invariant that closes the 64-byte malleation vector:
            // the smallest constructible transaction still exceeds 64 bytes, so
            // no transaction can ever be byte-indistinguishable from an internal
            // merkle node. If a future change to the per-tx PoW tail drops the
            // floor to 64, this assertion fails and forces a re-review of the
            // CVE-2012-2459 masquerade vector.
            assert(GetSerializeSize(TX_NO_WITNESS(block.vtx.back())) > 64);
        }
        BOOST_CHECK(is_not_mutated(block, /*check_witness_root=*/false));
    }

    {
        // Merkle-root malleation via a forged 64-byte transaction.
        //
        // Upstream mines a 64-byte tx3 whose raw non-witness serialization
        // equals txid1 || txid2, so that a 1-tx block {tx3} forges the merkle
        // root of a 2-tx block {tx1, tx2} (txid3 == H(txid1 || txid2)). That
        // exploit is unconstructible in this fork: the malleation primitive is
        // a transaction that serializes to EXACTLY 64 bytes, and our
        // UnserializeTransaction requires a trailing 176-byte PoW tail after
        // nLockTime — so a 64-byte blob can never round-trip into a CTransaction
        // at all.
        //
        // We assert that closure at the decode layer directly. The hex below is
        // upstream's forged 64-byte tx3; it MUST fail to decode.
        CMutableTransaction forged64;
        BOOST_CHECK(!DecodeHexTx(forged64, "cdaf22d00002c6a7f848f8ae4d30054e61dcf3303d6fe01d282163341f06feecc10032b3160fcab87bdfe3ecfb769206ef2d991b92f8a268e423a6ef4d485f06", /*try_no_witness=*/true, /*try_witness=*/false));
    }

    {
        CBlock block;
        block.vtx.push_back(create_coinbase_tx(/*include_witness=*/true));
        {
            CMutableTransaction mtx;
            mtx.vin.resize(1);
            mtx.vin[0].scriptWitness.stack.resize(1);
            mtx.vin[0].scriptWitness.stack[0] = {0};
            block.vtx.push_back(MakeTransactionRef(mtx));
        }
        block.hashMerkleRoot = BlockMerkleRoot(block);
        // Block with witnesses is considered mutated if the witness commitment
        // is not validated.
        BOOST_CHECK(is_mutated(block, /*check_witness_root=*/false));
        // Block with invalid witness commitment is considered mutated.
        BOOST_CHECK(is_mutated(block, /*check_witness_root=*/true));

        // Block with valid commitment is not mutated
        {
            auto commitment{BlockWitnessMerkleRoot(block)};
            insert_witness_commitment(block, commitment);
            block.hashMerkleRoot = BlockMerkleRoot(block);
        }
        BOOST_CHECK(is_not_mutated(block, /*check_witness_root=*/true));

        // Malleating witnesses should be caught by `IsBlockMutated`.
        {
            CMutableTransaction mtx{*block.vtx[1]};
            assert(!mtx.vin[0].scriptWitness.stack[0].empty());
            ++mtx.vin[0].scriptWitness.stack[0][0];
            block.vtx[1] = MakeTransactionRef(mtx);
        }
        // Without also updating the witness commitment, the merkle root should
        // not change when changing one of the witnesses.
        BOOST_CHECK(block.hashMerkleRoot == BlockMerkleRoot(block));
        BOOST_CHECK(is_mutated(block, /*check_witness_root=*/true));
        {
            auto commitment{BlockWitnessMerkleRoot(block)};
            insert_witness_commitment(block, commitment);
            block.hashMerkleRoot = BlockMerkleRoot(block);
        }
        BOOST_CHECK(is_not_mutated(block, /*check_witness_root=*/true));

        // Test malleating the coinbase witness reserved value
        {
            CMutableTransaction mtx{*block.vtx[0]};
            mtx.vin[0].scriptWitness.stack.resize(0);
            block.vtx[0] = MakeTransactionRef(mtx);
            block.hashMerkleRoot = BlockMerkleRoot(block);
        }
        BOOST_CHECK(is_mutated(block, /*check_witness_root=*/true));
    }
}

BOOST_AUTO_TEST_CASE(publictest_has_fresh_minimum_chain_work)
{
    // Quicksilver launches every network from zero accumulated work;
    const auto params = CreateChainParams(*m_node.args, ChainType::PUBLIC_TEST);
    BOOST_CHECK(params->GetConsensus().nMinimumChainWork.IsNull());
}

// ---------------------------------------------------------------------------
// Frame-B mint (#4): full-block consensus behavior of the raised coinbase
// ceiling, and the inherited replay protection (transplant / double-include).
// ---------------------------------------------------------------------------

// Sign a value-preserving (in==out, feeless #5b-1) spend of a mature coinbase, then grind
// its per-tx PoW. The per-tx pre-image excludes scriptSig, so grinding AFTER signing is sound.
static CMutableTransaction MakeProvenSpend(TestChain100Setup& setup, const CTransactionRef& coinbase,
                                           const Consensus::Params& cp)
{
    CMutableTransaction spend = setup.CreateValidRelayPoolTransaction(
        coinbase, /*input_vout=*/0, /*input_height=*/0, setup.coinbaseKey,
        coinbase->vout[0].scriptPubKey, /*output_amount=*/coinbase->vout[0].nValue, /*submit=*/false);
    // #5c-1: anchor the proof to the current active tip (in-window) and grind over its hash.
    const CBlockIndex* tip = WITH_LOCK(::cs_main, return setup.m_node.chainman->ActiveChain().Tip());
    spend.nAnchorHeight = static_cast<uint32_t>(tip->nHeight);
    const auto pre = CTransaction(spend).PowPreimage(tip->GetBlockHash());
    cuckatoo::Cycle cyc{};
    uint32_t won = 0;
    BOOST_REQUIRE(cuckatoo::CuckatooSolveBytes(pre.data(), pre.size(), cp.nTxEdgeBits, 0, 1u << 20, cyc, won));
    spend.nCycle = cyc;
    spend.nPowNonce = won;
    return spend;
}

// Like MakeProvenSpend but deliberately leaves a 1-COIN fee (in > out) to exercise the
// feeless rejection. Otherwise fully valid (PoW + signature).
static CMutableTransaction MakeFeeSpend(TestChain100Setup& setup, const CTransactionRef& coinbase,
                                        const Consensus::Params& cp)
{
    CMutableTransaction spend = setup.CreateValidRelayPoolTransaction(
        coinbase, /*input_vout=*/0, /*input_height=*/0, setup.coinbaseKey,
        coinbase->vout[0].scriptPubKey,
        /*output_amount=*/coinbase->vout[0].nValue - 1 * COIN, /*submit=*/false);
    // #5c-1: anchor the proof to the current active tip (in-window) and grind over its hash.
    const CBlockIndex* tip = WITH_LOCK(::cs_main, return setup.m_node.chainman->ActiveChain().Tip());
    spend.nAnchorHeight = static_cast<uint32_t>(tip->nHeight);
    const auto pre = CTransaction(spend).PowPreimage(tip->GetBlockHash());
    cuckatoo::Cycle cyc{};
    uint32_t won = 0;
    BOOST_REQUIRE(cuckatoo::CuckatooSolveBytes(pre.data(), pre.size(), cp.nTxEdgeBits, 0, 1u << 20, cyc, won));
    spend.nCycle = cyc;
    spend.nPowNonce = won;
    return spend;
}

// Build a sealed sandbox block over `txns` whose coinbase output value is forced
// to `coinbase_value`, then re-commit and re-solve block PoW so the block is
// otherwise valid. Lets the test probe the exact bad-cb-amount boundary.
static CBlock SealWithCoinbaseValue(TestChain100Setup& setup,
                                    const std::vector<CMutableTransaction>& txns,
                                    const CScript& spk, CAmount coinbase_value)
{
    Chainstate& cs = setup.m_node.chainman->ActiveChainstate();
    CBlock block = setup.CreateBlock(txns, spk, cs);
    CMutableTransaction cb(*block.vtx[0]);
    cb.vout[0].nValue = coinbase_value;
    block.vtx[0] = MakeTransactionRef(cb);
    node::RegenerateCommitments(block, *setup.m_node.chainman);
    SolveBlockPoW(block, setup.m_node.chainman->GetConsensus());
    return block;
}

BOOST_FIXTURE_TEST_CASE(mint_ceiling_allows_exact_and_rejects_excess, TestChain100Setup)
{
    const Consensus::Params& cp = m_node.chainman->GetConsensus();
    const CScript spk = m_coinbase_txns[0]->vout[0].scriptPubKey;

    // One value-preserving (in==out) proven spend of coinbase 0: feeless, so the only
    // coinbase headroom above the subsidy is the mint C -- no fee term.
    CMutableTransaction spend = MakeProvenSpend(*this, m_coinbase_txns[0], cp);

    const CAmount subsidy = GetBlockSubsidy(101, cp);   // next height after 100 maturity blocks
    const CAmount ceiling = subsidy + 1 * cp.nTxPowMint;  // feeless: subsidy + one mint

    // (a) coinbase claims EXACTLY the ceiling -> connects, tip advances.
    {
        CBlock ok = SealWithCoinbaseValue(*this, {spend}, spk, ceiling);
        auto pblock = std::make_shared<const CBlock>(ok);
        BOOST_CHECK(m_node.chainman->ProcessNewBlock(pblock, true, true, nullptr));
        LOCK(cs_main);
        BOOST_CHECK(m_node.chainman->ActiveChain().Tip()->GetBlockHash() == ok.GetHash());
    }

    // (b) a sibling block claiming ONE cinnabar more -> rejected, tip unchanged.
    {
        // Re-spend coinbase 1 so inputs are unspent on this fork tip's view.
        CMutableTransaction spend2 = MakeProvenSpend(*this, m_coinbase_txns[1], cp);
        CBlock bad = SealWithCoinbaseValue(*this, {spend2}, spk, ceiling + 1);
        auto pblock = std::make_shared<const CBlock>(bad);
        m_node.chainman->ProcessNewBlock(pblock, true, true, nullptr);
        LOCK(cs_main);
        BOOST_CHECK(m_node.chainman->ActiveChain().Tip()->GetBlockHash() != bad.GetHash());
    }
}

BOOST_FIXTURE_TEST_CASE(double_include_same_spend_cannot_double_mint, TestChain100Setup)
{
    const Consensus::Params& cp = m_node.chainman->GetConsensus();
    const CScript spk = m_coinbase_txns[2]->vout[0].scriptPubKey;

    // One proven spend included TWICE in the same block. The second copy double-
    // spends the first's input, so the block is invalid -> no double-mint. Replay
    // protection is inherited from UTXO uniqueness, not from mint-specific state.
    CMutableTransaction spend = MakeProvenSpend(*this, m_coinbase_txns[2], cp);
    const CAmount subsidy = GetBlockSubsidy(101, cp);  // fresh fixture: tip=100, next block=101
    // Claim as if BOTH copies minted (2*C) to make the block attractive if the rule
    // were broken; feeless, so no fee term. It must still be rejected.
    const CAmount over = subsidy + 2 * cp.nTxPowMint;
    CBlock block = SealWithCoinbaseValue(*this, {spend, spend}, spk, over);

    auto pblock = std::make_shared<const CBlock>(block);
    m_node.chainman->ProcessNewBlock(pblock, true, true, nullptr);
    LOCK(cs_main);
    BOOST_CHECK(m_node.chainman->ActiveChain().Tip()->GetBlockHash() != block.GetHash());
}

BOOST_FIXTURE_TEST_CASE(feeless_rule_rejects_fee, TestChain100Setup)
{
    const Consensus::Params& cp = m_node.chainman->GetConsensus();
    const CScript spk = m_coinbase_txns[0]->vout[0].scriptPubKey;

    // A proven spend that leaves a 1-COIN fee (in > out). Pre-5b this was valid (the miner
    // claimed the fee); under feeless (in==out) it must be rejected bad-txns-not-feeless.
    // Uses coinbase 0 -- the only one MATURE at height 101 -- so the block is otherwise fully
    // valid (mature input, valid PoW + signature) and the rejection is unambiguously the
    // feeless rule (not a premature-coinbase-spend).
    CMutableTransaction fee_spend = MakeFeeSpend(*this, m_coinbase_txns[0], cp);
    const CAmount subsidy = GetBlockSubsidy(101, cp);
    CBlock block = SealWithCoinbaseValue(*this, {fee_spend}, spk, subsidy + 1 * COIN + cp.nTxPowMint);

    auto pblock = std::make_shared<const CBlock>(block);
    m_node.chainman->ProcessNewBlock(pblock, true, true, nullptr);
    LOCK(cs_main);
    BOOST_CHECK(m_node.chainman->ActiveChain().Tip()->GetBlockHash() != block.GetHash());
}

BOOST_FIXTURE_TEST_CASE(congestion_multiplier_is_cached_and_floored, TestChain100Setup)
{
    // The 100-block test chain is built by the fixture. Every connected block must
    // carry a cached m >= CONGESTION_ONE, and genesis must be exactly CONGESTION_ONE.
    LOCK(cs_main);
    const CBlockIndex* tip = m_node.chainman->ActiveChain().Tip();
    BOOST_REQUIRE(tip != nullptr);

    const CBlockIndex* genesis = m_node.chainman->ActiveChain().Genesis();
    BOOST_CHECK_EQUAL(genesis->m_congestion, CONGESTION_ONE);

    for (const CBlockIndex* p = tip; p != nullptr; p = p->pprev) {
        BOOST_CHECK(p->m_congestion >= CONGESTION_ONE); // never below the hardware floor
    }
    // The test-chain blocks are near-empty, so m must have decayed/stayed at the floor.
    BOOST_CHECK_EQUAL(tip->m_congestion, CONGESTION_ONE);
}

BOOST_FIXTURE_TEST_CASE(congestion_multiplier_consensus_rule_rejects_both_directions, TestChain100Setup)
{
    // #5c-1 Phase 2: ConnectBlock no longer COMPUTES the multiplier, it CHECKS the one
    // the header carries. Prove the check bites, in both directions, before trusting any
    // green run that depends on it — a check that only ever sees correct values is
    // indistinguishable from no check at all.
    const CScript spk = m_coinbase_txns[0]->vout[0].scriptPubKey;
    const CAmount subsidy = GetBlockSubsidy(101, m_node.chainman->GetConsensus());

    const uint64_t parent_m = WITH_LOCK(::cs_main,
        return m_node.chainman->ActiveChain().Tip()->m_congestion);
    BOOST_REQUIRE_EQUAL(parent_m, CONGESTION_ONE);  // near-empty test chain sits at the floor

    // The value consensus will demand, derived the same way the assembler derives it.
    const auto expected_for = [&](const CBlock& b) {
        return NextCongestionMultiplier(parent_m, GetBlockWeight(b),
                                        m_node.chainman->GetConsensus());
    };

    const auto submit = [&](CBlock b) {
        auto pblock = std::make_shared<const CBlock>(b);
        m_node.chainman->ProcessNewBlock(pblock, true, true, nullptr);
        return WITH_LOCK(::cs_main,
            return m_node.chainman->ActiveChain().Tip()->GetBlockHash() == b.GetHash());
    };

    // (a) too HIGH -> rejected. Re-grind after the mutation: nCongestion is inside the
    //     pre-pow, so touching it invalidates the existing proof; without the re-grind
    //     this would be rejected for bad PoW and would prove nothing about congestion.
    {
        CBlock high = SealWithCoinbaseValue(*this, {}, spk, subsidy);
        BOOST_REQUIRE_EQUAL(high.nCongestion, expected_for(high));
        high.nCongestion = static_cast<uint32_t>(expected_for(high) + 1);
        SolveBlockPoW(high, m_node.chainman->GetConsensus());
        BOOST_CHECK(!submit(high));
    }

    // (b) too LOW -> rejected.
    {
        CBlock low = SealWithCoinbaseValue(*this, {}, spk, subsidy);
        BOOST_REQUIRE_EQUAL(low.nCongestion, expected_for(low));
        low.nCongestion = static_cast<uint32_t>(expected_for(low) - 1);
        SolveBlockPoW(low, m_node.chainman->GetConsensus());
        BOOST_CHECK(!submit(low));
    }

    // (c) zero -> rejected. This is what a header built by a caller that forgot the field
    //     looks like (SetNull leaves it 0), so it is worth its own case.
    {
        CBlock zero = SealWithCoinbaseValue(*this, {}, spk, subsidy);
        zero.nCongestion = 0;
        SolveBlockPoW(zero, m_node.chainman->GetConsensus());
        BOOST_CHECK(!submit(zero));
    }

    // (d) EXACT -> connects. The oracle above only means something if the untouched
    //     block still passes.
    {
        CBlock good = SealWithCoinbaseValue(*this, {}, spk, subsidy);
        BOOST_CHECK_EQUAL(good.nCongestion, expected_for(good));
        BOOST_CHECK(submit(good));
    }
}

BOOST_FIXTURE_TEST_CASE(test_block_validity_rejects_bad_congestion, TestChain100Setup)
{
    // TestBlockValidity drives ConnectBlock's fJustCheck path. Keep this direct fence because
    // getblocktemplate proposal validation and the block assembler both rely on this dry run;
    // ProcessNewBlock coverage alone does not exercise its early-return behavior.
    const CScript spk = m_coinbase_txns[0]->vout[0].scriptPubKey;
    const CAmount subsidy = GetBlockSubsidy(101, m_node.chainman->GetConsensus());
    CBlock block = SealWithCoinbaseValue(*this, {}, spk, subsidy);
    block.nCongestion += 1;

    BlockValidationState state;
    const bool valid = WITH_LOCK(::cs_main, return TestBlockValidity(
        state,
        m_node.chainman->GetParams(),
        m_node.chainman->ActiveChainstate(),
        block,
        m_node.chainman->ActiveChain().Tip(),
        /*fCheckPOW=*/false,
        /*fCheckMerkleRoot=*/true));

    BOOST_CHECK(!valid);
    BOOST_CHECK_EQUAL(state.GetResult(), BlockValidationResult::BLOCK_CONSENSUS);
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-congestion");
}

BOOST_AUTO_TEST_SUITE_END()
