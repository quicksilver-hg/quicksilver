// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// #5c-2 surplus-work inclusion ranking: work-space helpers, relaypool entry
// ranking key, by_txwork_rate index, miner/eviction ordering.

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <consensus/amount.h>
#include <crypto/cuckatoo/cuckatoo.h>
#include <node/miner.h>
#include <pow.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <test/util/txrelaypool.h>
#include <txrelaypool.h>
#include <uint256.h>
#include <validation.h>

#include <algorithm>
#include <chrono>
#include <set>
#include <vector>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(txwork_ranking_tests, BasicTestingSetup)

namespace {
// A tx with an arbitrary (need-not-be-valid) cycle. GetTxActualWork / GetTxWorkSurplus
// are pure functions of nCycle + the anchor, so no solver is required to test them.
CTransaction TxWithCycle(uint32_t seed)
{
    CMutableTransaction m;
    m.vin.resize(1);
    m.vout.resize(1);
    for (uint32_t i = 0; i < 42; ++i) m.nCycle[i] = seed * 131 + i * 7 + 1;
    return CTransaction(m);
}
} // namespace

// GetTxActualWork applies the GetBlockProof idiom 2^256/(h+1) to the proof hash.
BOOST_AUTO_TEST_CASE(actual_work_matches_block_proof_idiom)
{
    const CTransaction tx = TxWithCycle(3);
    const arith_uint256 h = UintToArith256(cuckatoo::CuckatooProofHash(tx.nCycle));
    const arith_uint256 expected = (~h / (h + 1)) + 1;
    BOOST_CHECK_EQUAL(GetTxActualWork(tx).GetHex(), expected.GetHex());
}

// GetTxWorkSurplus == max(0, actual - RequiredTxWork(tx, anchor)), for any anchor.
BOOST_AUTO_TEST_CASE(surplus_is_clamped_actual_minus_floor)
{
    const Consensus::Params& cp = Params().GetConsensus();

    // Synthetic chain (no mining): BaseTxWork only reads nBits, pprev, m_congestion.
    std::vector<CBlockIndex> chain(101);
    for (int h = 0; h <= 100; ++h) {
        chain[h].nHeight = h;
        chain[h].pprev = (h ? &chain[h - 1] : nullptr);
        chain[h].nBits = UintToArith256(cp.powLimit).GetCompact(); // easiest target
        chain[h].m_congestion = CONGESTION_ONE;                    // multiplier 1.0
    }
    const CBlockIndex* anchor = &chain[100];

    const CTransaction tx = TxWithCycle(11);
    const arith_uint256 actual = GetTxActualWork(tx);
    const arith_uint256 floor = RequiredTxWork(tx, anchor, cp);
    const arith_uint256 expected = (actual > floor) ? (actual - floor) : arith_uint256(0);

    BOOST_CHECK_EQUAL(GetTxWorkSurplus(tx, anchor, cp).GetHex(), expected.GetHex());

    // A null anchor floors to 1 (permissive context); surplus must still be the identity.
    const arith_uint256 floor_null = RequiredTxWork(tx, nullptr, cp);
    const arith_uint256 expected_null = (actual > floor_null) ? (actual - floor_null) : arith_uint256(0);
    BOOST_CHECK_EQUAL(GetTxWorkSurplus(tx, nullptr, cp).GetHex(), expected_null.GetHex());
}

// The relaypool entry caches the surplus and exposes it as a per-vsize rate.
BOOST_AUTO_TEST_CASE(entry_rate_is_surplus_over_vsize)
{
    CMutableTransaction mtx;
    mtx.vin.resize(1);
    mtx.vout.resize(1);
    for (uint32_t i = 0; i < 42; ++i) mtx.nCycle[i] = i * 7 + 1;

    TestRelayPoolEntryHelper helper;
    const CTxRelayPoolEntry e_lo = helper.TxWorkSurplus(arith_uint256(10)).FromTx(mtx);
    const CTxRelayPoolEntry e_hi = helper.TxWorkSurplus(arith_uint256(100)).FromTx(mtx);

    BOOST_CHECK(e_hi.GetTxWorkRate() > e_lo.GetTxWorkRate());
    BOOST_CHECK_CLOSE(e_lo.GetTxWorkRate(), 10.0 / e_lo.GetTxSize(), 0.001);
    BOOST_CHECK_EQUAL(e_lo.GetTxWorkSurplusValue().GetHex(), arith_uint256(10).GetHex());
}

// The by_txwork_rate index orders highest surplus-work rate first (begin()).
BOOST_FIXTURE_TEST_CASE(index_orders_by_descending_rate, TestingSetup)
{
    CTxRelayPool& pool = *Assert(m_node.relaypool);
    LOCK2(cs_main, pool.cs);
    TestRelayPoolEntryHelper entry;

    auto make_tx = [](int n) {
        CMutableTransaction t;
        t.vout.resize(1);
        t.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
        t.vout[0].nValue = (n + 1) * COIN; // distinct value -> distinct txid
        return t;
    };

    const CMutableTransaction tx_lo = make_tx(0);
    const CMutableTransaction tx_mid = make_tx(1);
    const CMutableTransaction tx_hi = make_tx(2);
    AddToRelayPool(pool, entry.TxWorkSurplus(arith_uint256(5)).FromTx(tx_lo));
    AddToRelayPool(pool, entry.TxWorkSurplus(arith_uint256(500)).FromTx(tx_hi));
    AddToRelayPool(pool, entry.TxWorkSurplus(arith_uint256(50)).FromTx(tx_mid));

    auto it = pool.mapTx.get<txwork_rate>().begin();
    BOOST_CHECK_EQUAL(it->GetTx().GetHash().ToString(), CTransaction(tx_hi).GetHash().ToString());
    ++it;
    BOOST_CHECK_EQUAL(it->GetTx().GetHash().ToString(), CTransaction(tx_mid).GetHash().ToString());
    ++it;
    BOOST_CHECK_EQUAL(it->GetTx().GetHash().ToString(), CTransaction(tx_lo).GetHash().ToString());
}

// The miner includes relaypool txs in descending surplus-work-rate order. Uses
// several independent txs with surpluses assigned in an order uncorrelated to
// txid, so the old fee-based (txid-tiebreak) ordering reliably disagrees.
BOOST_FIXTURE_TEST_CASE(miner_includes_highest_surplus_first, TestChain100Setup)
{
    CTxRelayPool& pool = *Assert(m_node.relaypool);
    TestRelayPoolEntryHelper entry;

    const uint32_t anchor_height{WITH_LOCK(::cs_main, return static_cast<uint32_t>(m_node.chainman->ActiveChain().Tip()->nHeight);)};
    auto make_tx = [anchor_height](int n) {
        CMutableTransaction t;
        t.vin.resize(1);
        t.vin[0].scriptSig = CScript() << OP_1;
        t.vout.resize(1);
        t.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
        t.vout[0].nValue = (n + 1) * COIN; // distinct value -> distinct txid; independent
        t.nAnchorHeight = anchor_height;
        return t;
    };

    // Surplus values, NOT monotonic in creation index, so descending-surplus order
    // is unrelated to insertion/txid order.
    const std::vector<uint64_t> surplus{300, 50, 900, 10, 120};
    std::vector<CMutableTransaction> txs;
    for (size_t i = 0; i < surplus.size(); ++i) txs.push_back(make_tx((int)i));
    {
        LOCK2(cs_main, pool.cs);
        for (size_t i = 0; i < txs.size(); ++i) {
            AddToRelayPool(pool, entry.TxWorkSurplus(arith_uint256(surplus[i])).FromTx(txs[i]));
        }
    }

    // Expected order: tx indices sorted by descending surplus.
    std::vector<size_t> expected{0, 1, 2, 3, 4};
    std::sort(expected.begin(), expected.end(),
              [&](size_t a, size_t b) { return surplus[a] > surplus[b]; });

    node::BlockAssembler::Options options;
    options.coinbase_output_script = CScript() << OP_TRUE;
    options.test_block_validity = false; // ordering test: skip PoW/validity (covered elsewhere)
    const auto tmpl = node::BlockAssembler{m_node.chainman->ActiveChainstate(), &pool, options}.CreateNewBlock();
    BOOST_REQUIRE(tmpl);
    const CBlock& block = tmpl->block;

    // vtx[0] is the coinbase; vtx[1..] are relaypool txs in descending surplus order.
    BOOST_REQUIRE_EQUAL(block.vtx.size(), surplus.size() + 1);
    for (size_t rank = 0; rank < expected.size(); ++rank) {
        BOOST_CHECK_EQUAL(block.vtx[rank + 1]->GetHash().ToString(),
                          CTransaction(txs[expected[rank]]).GetHash().ToString());
    }
}

// TrimToSize evicts the lowest surplus-work rate first.
BOOST_FIXTURE_TEST_CASE(eviction_drops_lowest_surplus_first, TestingSetup)
{
    CTxRelayPool& pool = *Assert(m_node.relaypool);
    LOCK2(cs_main, pool.cs);
    TestRelayPoolEntryHelper entry;

    auto mk = [](int n) {
        CMutableTransaction t;
        t.vin.resize(1);
        t.vin[0].scriptSig = CScript() << OP_1;
        t.vout.resize(1);
        t.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
        t.vout[0].nValue = (n + 1) * COIN; // distinct value -> distinct txid; independent
        return t;
    };
    const CMutableTransaction hi = mk(0);
    const CMutableTransaction lo = mk(1);
    // hi added first (older) so the OLD fee/time tiebreak would evict it, not lo.
    AddToRelayPool(pool, entry.TxWorkSurplus(arith_uint256(1000)).FromTx(hi));
    AddToRelayPool(pool, entry.TxWorkSurplus(arith_uint256(10)).FromTx(lo));

    pool.TrimToSize(pool.DynamicMemoryUsage()); // fits everything: no eviction
    BOOST_CHECK(pool.exists(GenTxid::Txid(CTransaction(hi).GetHash())));
    BOOST_CHECK(pool.exists(GenTxid::Txid(CTransaction(lo).GetHash())));

    pool.TrimToSize(pool.DynamicMemoryUsage() * 3 / 4); // drop the lowest-surplus tx
    BOOST_CHECK(pool.exists(GenTxid::Txid(CTransaction(hi).GetHash())));
    BOOST_CHECK(!pool.exists(GenTxid::Txid(CTransaction(lo).GetHash())));
}

// F-138 (policy DECIDED 2026-09-04, not a bug): among entries at the SAME surplus-work
// rate, eviction deliberately takes the NEWEST -- i.e. it refuses the newcomer and keeps
// the incumbent.
//
// This is not an edge case here. On a feeless chain the ordinary state of the pool is
// "everything at the floor", surplus 0, so this tie-break IS the admission policy once
// the pool is full: first-come-first-served, incumbents keep their place until they
// expire, and the way in for a newcomer is to grind MORE surplus work rather than to
// wait its turn.
//
// Note it is the exact reverse of the MINING tie-break, which takes the oldest of an
// equal-rate group first. The two orders are intentionally different, and the class
// comment on CompareTxRelayPoolEntryByTxWorkRate previously described only the mining
// half as though it covered both -- which is how the policy came to be re-litigated from
// a comment rather than from the code. This test pins the decided behaviour so it cannot
// drift again silently.
BOOST_FIXTURE_TEST_CASE(equal_rate_eviction_drops_the_newest, TestingSetup)
{
    CTxRelayPool& pool = *Assert(m_node.relaypool);
    LOCK2(cs_main, pool.cs);
    TestRelayPoolEntryHelper entry;

    auto mk = [](int n) {
        CMutableTransaction t;
        t.vin.resize(1);
        t.vin[0].scriptSig = CScript() << OP_1;
        t.vout.resize(1);
        t.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
        t.vout[0].nValue = (n + 1) * COIN; // distinct value -> distinct txid; identical size
        return t;
    };
    const CMutableTransaction older = mk(0);
    const CMutableTransaction newer = mk(1);

    // Identical surplus (the floor) and identical size => identical rate. Time is the
    // ONLY thing separating them, which is exactly the case this pins down.
    AddToRelayPool(pool, entry.TxWorkSurplus(arith_uint256(0)).Time(NodeSeconds{std::chrono::seconds{1000}}).FromTx(older));
    AddToRelayPool(pool, entry.TxWorkSurplus(arith_uint256(0)).Time(NodeSeconds{std::chrono::seconds{2000}}).FromTx(newer));

    pool.TrimToSize(pool.DynamicMemoryUsage()); // fits everything: no eviction
    BOOST_CHECK(pool.exists(GenTxid::Txid(CTransaction(older).GetHash())));
    BOOST_CHECK(pool.exists(GenTxid::Txid(CTransaction(newer).GetHash())));

    pool.TrimToSize(pool.DynamicMemoryUsage() * 3 / 4);
    BOOST_CHECK(pool.exists(GenTxid::Txid(CTransaction(older).GetHash())));   // incumbent stays
    BOOST_CHECK(!pool.exists(GenTxid::Txid(CTransaction(newer).GetHash())));  // newcomer refused
}

// Consistency guarantee (the "mined set ⊆ eviction-survivor set" property):
// the miner includes txs highest-surplus first, and eviction drops them
// lowest-surplus first — exact reverses. So for any block (top-K by surplus) and
// any survivor set after trimming (top-L, L>=K), every mined tx survives:
// mined ⊆ survivors, and evicted ∩ mined = empty. Verified through public
// behavior: a block template vs. repeated TrimToSize.
BOOST_FIXTURE_TEST_CASE(mined_order_is_reverse_of_eviction_order, TestChain100Setup)
{
    CTxRelayPool& pool = *Assert(m_node.relaypool);
    TestRelayPoolEntryHelper entry;

    const uint32_t anchor_height{WITH_LOCK(::cs_main, return static_cast<uint32_t>(m_node.chainman->ActiveChain().Tip()->nHeight);)};
    auto mk = [anchor_height](int n) {
        CMutableTransaction t;
        t.vin.resize(1);
        t.vin[0].scriptSig = CScript() << OP_1;
        t.vout.resize(1);
        t.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
        t.vout[0].nValue = (n + 1) * COIN; // distinct value -> distinct txid; identical size
        t.nAnchorHeight = anchor_height;
        return t;
    };
    // Surpluses uncorrelated to creation/txid order.
    const std::vector<uint64_t> surplus{300, 50, 900, 10, 120, 700};
    std::vector<CMutableTransaction> txs;
    for (size_t i = 0; i < surplus.size(); ++i) txs.push_back(mk((int)i));
    {
        LOCK2(cs_main, pool.cs);
        for (size_t i = 0; i < txs.size(); ++i) {
            AddToRelayPool(pool, entry.TxWorkSurplus(arith_uint256(surplus[i])).FromTx(txs[i]));
        }
    }

    // Miner inclusion order (block large enough to hold all): highest surplus first.
    node::BlockAssembler::Options options;
    options.coinbase_output_script = CScript() << OP_TRUE;
    options.test_block_validity = false;
    const auto tmpl = node::BlockAssembler{m_node.chainman->ActiveChainstate(), &pool, options}.CreateNewBlock();
    BOOST_REQUIRE(tmpl);
    std::vector<uint256> mined_order;
    for (size_t i = 1; i < tmpl->block.vtx.size(); ++i) {
        mined_order.push_back(tmpl->block.vtx[i]->GetHash());
    }
    BOOST_REQUIRE_EQUAL(mined_order.size(), txs.size());

    // Eviction order: trim one tx at a time, recording each removed txid.
    std::vector<uint256> eviction_order;
    {
        LOCK2(cs_main, pool.cs);
        while (pool.size() > 0) {
            std::set<uint256> before;
            for (const auto& e : pool.mapTx) before.insert(e.GetTx().GetHash());
            pool.TrimToSize(pool.DynamicMemoryUsage() - 1);
            // Identical-size independent txs: exactly one removed per call.
            BOOST_REQUIRE_EQUAL(pool.size(), before.size() - 1);
            for (const auto& e : pool.mapTx) before.erase(e.GetTx().GetHash());
            BOOST_REQUIRE_EQUAL(before.size(), 1U);
            eviction_order.push_back(*before.begin());
        }
    }

    // Eviction removes lowest-surplus first => exact reverse of mining order.
    BOOST_REQUIRE_EQUAL(eviction_order.size(), mined_order.size());
    for (size_t i = 0; i < mined_order.size(); ++i) {
        BOOST_CHECK_EQUAL(eviction_order[i].ToString(),
                          mined_order[mined_order.size() - 1 - i].ToString());
    }
}

BOOST_AUTO_TEST_SUITE_END()
