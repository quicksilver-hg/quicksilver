// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/system.h>
#include <policy/policy.h>
#include <test/util/txrelaypool.h>
#include <txrelaypool.h>
#include <util/time.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(relaypool_tests, TestingSetup)

static constexpr auto REMOVAL_REASON_DUMMY = RelayPoolRemovalReason::REPLACED;

BOOST_AUTO_TEST_CASE(RelayPoolRemoveTest)
{
    // Test CTxRelayPool::remove functionality

    TestRelayPoolEntryHelper entry;
    // Parent transaction with three children,
    // and three grand-children:
    CMutableTransaction txParent;
    txParent.vin.resize(1);
    txParent.vin[0].scriptSig = CScript() << OP_11;
    txParent.vout.resize(3);
    for (int i = 0; i < 3; i++)
    {
        txParent.vout[i].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
        txParent.vout[i].nValue = 33000LL;
    }
    CMutableTransaction txChild[3];
    for (int i = 0; i < 3; i++)
    {
        txChild[i].vin.resize(1);
        txChild[i].vin[0].scriptSig = CScript() << OP_11;
        txChild[i].vin[0].prevout.hash = txParent.GetHash();
        txChild[i].vin[0].prevout.n = i;
        txChild[i].vout.resize(1);
        txChild[i].vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
        txChild[i].vout[0].nValue = 11000LL;
    }
    CMutableTransaction txGrandChild[3];
    for (int i = 0; i < 3; i++)
    {
        txGrandChild[i].vin.resize(1);
        txGrandChild[i].vin[0].scriptSig = CScript() << OP_11;
        txGrandChild[i].vin[0].prevout.hash = txChild[i].GetHash();
        txGrandChild[i].vin[0].prevout.n = 0;
        txGrandChild[i].vout.resize(1);
        txGrandChild[i].vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
        txGrandChild[i].vout[0].nValue = 11000LL;
    }


    CTxRelayPool& testPool = *Assert(m_node.relaypool);
    LOCK2(::cs_main, testPool.cs);

    // Nothing in pool, remove should do nothing:
    unsigned int poolSize = testPool.size();
    testPool.removeRecursive(CTransaction(txParent), REMOVAL_REASON_DUMMY);
    BOOST_CHECK_EQUAL(testPool.size(), poolSize);

    // Just the parent:
    AddToRelayPool(testPool, entry.FromTx(txParent));
    poolSize = testPool.size();
    testPool.removeRecursive(CTransaction(txParent), REMOVAL_REASON_DUMMY);
    BOOST_CHECK_EQUAL(testPool.size(), poolSize - 1);

    // Parent, children, grandchildren:
    AddToRelayPool(testPool, entry.FromTx(txParent));
    for (int i = 0; i < 3; i++)
    {
        AddToRelayPool(testPool, entry.FromTx(txChild[i]));
        AddToRelayPool(testPool, entry.FromTx(txGrandChild[i]));
    }
    // Remove Child[0], GrandChild[0] should be removed:
    poolSize = testPool.size();
    testPool.removeRecursive(CTransaction(txChild[0]), REMOVAL_REASON_DUMMY);
    BOOST_CHECK_EQUAL(testPool.size(), poolSize - 2);
    // ... make sure grandchild and child are gone:
    poolSize = testPool.size();
    testPool.removeRecursive(CTransaction(txGrandChild[0]), REMOVAL_REASON_DUMMY);
    BOOST_CHECK_EQUAL(testPool.size(), poolSize);
    poolSize = testPool.size();
    testPool.removeRecursive(CTransaction(txChild[0]), REMOVAL_REASON_DUMMY);
    BOOST_CHECK_EQUAL(testPool.size(), poolSize);
    // Remove parent, all children/grandchildren should go:
    poolSize = testPool.size();
    testPool.removeRecursive(CTransaction(txParent), REMOVAL_REASON_DUMMY);
    BOOST_CHECK_EQUAL(testPool.size(), poolSize - 5);
    BOOST_CHECK_EQUAL(testPool.size(), 0U);

    // Add children and grandchildren, but NOT the parent (simulate the parent being in a block)
    for (int i = 0; i < 3; i++)
    {
        AddToRelayPool(testPool, entry.FromTx(txChild[i]));
        AddToRelayPool(testPool, entry.FromTx(txGrandChild[i]));
    }
    // Now remove the parent, as might happen if a block-re-org occurs but the parent cannot be
    // put into the relaypool (maybe because it is non-standard):
    poolSize = testPool.size();
    testPool.removeRecursive(CTransaction(txParent), REMOVAL_REASON_DUMMY);
    BOOST_CHECK_EQUAL(testPool.size(), poolSize - 6);
    BOOST_CHECK_EQUAL(testPool.size(), 0U);
}


BOOST_AUTO_TEST_CASE(RelayPoolSizeLimitTest)
{
    // #5c-2 migration: TrimToSize now evicts by surplus-work rate (lowest first),
    // not by feerate. Two behaviors this test used to cover no longer exist and
    // their assertions are intentionally removed:
    //   - child-rescues-parent eviction: per-tx individual scoring has no
    //     package re-ranking (5c-2 decision 1.1.1).
    //   - the rolling-minimum-fee floor (GetMinFee dynamics): the chain is
    //     feeless (#5b), so there is no fee floor to bump or decay.
    // What remains is the core size-limiting contract: evict lowest priority
    // first, and never orphan a child (descendant cascade), now keyed on surplus.
    auto& pool = *Assert(m_node.relaypool);
    LOCK2(cs_main, pool.cs);
    TestRelayPoolEntryHelper entry;

    // Two independent txs: tx1 higher surplus, tx2 lower.
    CMutableTransaction tx1 = CMutableTransaction();
    tx1.vin.resize(1);
    tx1.vin[0].scriptSig = CScript() << OP_1;
    tx1.vout.resize(1);
    tx1.vout[0].scriptPubKey = CScript() << OP_1 << OP_EQUAL;
    tx1.vout[0].nValue = 10 * COIN;
    AddToRelayPool(pool, entry.TxWorkSurplus(arith_uint256(1000)).FromTx(tx1));

    CMutableTransaction tx2 = CMutableTransaction();
    tx2.vin.resize(1);
    tx2.vin[0].scriptSig = CScript() << OP_2;
    tx2.vout.resize(1);
    tx2.vout[0].scriptPubKey = CScript() << OP_2 << OP_EQUAL;
    tx2.vout[0].nValue = 10 * COIN;
    AddToRelayPool(pool, entry.TxWorkSurplus(arith_uint256(500)).FromTx(tx2));

    pool.TrimToSize(pool.DynamicMemoryUsage()); // should do nothing
    BOOST_CHECK(pool.exists(GenTxid::Txid(tx1.GetHash())));
    BOOST_CHECK(pool.exists(GenTxid::Txid(tx2.GetHash())));

    pool.TrimToSize(pool.DynamicMemoryUsage() * 3 / 4); // should remove the lower-surplus tx
    BOOST_CHECK(pool.exists(GenTxid::Txid(tx1.GetHash())));
    BOOST_CHECK(!pool.exists(GenTxid::Txid(tx2.GetHash())));

    // Limit below a single tx: everything is evicted.
    pool.TrimToSize(GetVirtualTransactionSize(CTransaction(tx1)) / 2);
    BOOST_CHECK(!pool.exists(GenTxid::Txid(tx1.GetHash())));
    BOOST_CHECK(!pool.exists(GenTxid::Txid(tx2.GetHash())));

    // Descendant cascade (no orphaned children): a low-surplus PARENT is the
    // lowest-rate entry and is evicted first; its high-surplus child is removed
    // with it. An independent high-surplus tx survives.
    CMutableTransaction txIndep = CMutableTransaction();
    txIndep.vin.resize(1);
    txIndep.vin[0].scriptSig = CScript() << OP_8;
    txIndep.vout.resize(1);
    txIndep.vout[0].scriptPubKey = CScript() << OP_8 << OP_EQUAL;
    txIndep.vout[0].nValue = 10 * COIN;

    CMutableTransaction txParent = CMutableTransaction();
    txParent.vin.resize(1);
    txParent.vin[0].scriptSig = CScript() << OP_9;
    txParent.vout.resize(1);
    txParent.vout[0].scriptPubKey = CScript() << OP_9 << OP_EQUAL;
    txParent.vout[0].nValue = 10 * COIN;

    CMutableTransaction txChild = CMutableTransaction();
    txChild.vin.resize(1);
    txChild.vin[0].prevout = COutPoint(txParent.GetHash(), 0);
    txChild.vin[0].scriptSig = CScript() << OP_9;
    txChild.vout.resize(1);
    txChild.vout[0].scriptPubKey = CScript() << OP_10 << OP_EQUAL;
    txChild.vout[0].nValue = 10 * COIN;

    AddToRelayPool(pool, entry.TxWorkSurplus(arith_uint256(5000)).FromTx(txIndep)); // highest
    AddToRelayPool(pool, entry.TxWorkSurplus(arith_uint256(100)).FromTx(txParent)); // lowest
    AddToRelayPool(pool, entry.TxWorkSurplus(arith_uint256(900)).FromTx(txChild));  // high, but child of parent

    // Trim so the lowest-rate package (parent+child) is evicted, leaving the
    // independent high-surplus tx.
    pool.TrimToSize(pool.DynamicMemoryUsage() * 2 / 5);
    BOOST_CHECK(pool.exists(GenTxid::Txid(txIndep.GetHash())));
    BOOST_CHECK(!pool.exists(GenTxid::Txid(txParent.GetHash())));
    BOOST_CHECK(!pool.exists(GenTxid::Txid(txChild.GetHash()))); // cascaded out with its parent
}

inline CTransactionRef make_tx(std::vector<CAmount>&& output_values, std::vector<CTransactionRef>&& inputs=std::vector<CTransactionRef>(), std::vector<uint32_t>&& input_indices=std::vector<uint32_t>())
{
    CMutableTransaction tx = CMutableTransaction();
    tx.vin.resize(inputs.size());
    tx.vout.resize(output_values.size());
    for (size_t i = 0; i < inputs.size(); ++i) {
        tx.vin[i].prevout.hash = inputs[i]->GetHash();
        tx.vin[i].prevout.n = input_indices.size() > i ? input_indices[i] : 0;
    }
    for (size_t i = 0; i < output_values.size(); ++i) {
        tx.vout[i].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
        tx.vout[i].nValue = output_values[i];
    }
    return MakeTransactionRef(tx);
}


BOOST_AUTO_TEST_CASE(RelayPoolAncestryTests)
{
    size_t ancestors, descendants;

    CTxRelayPool& pool = *Assert(m_node.relaypool);
    LOCK2(cs_main, pool.cs);
    TestRelayPoolEntryHelper entry;

    /* Base transaction */
    //
    // [tx1]
    //
    CTransactionRef tx1 = make_tx(/*output_values=*/{10 * COIN});
    AddToRelayPool(pool, entry.FromTx(tx1));

    // Ancestors / descendants should be 1 / 1 (itself / itself)
    pool.GetTransactionAncestry(tx1->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 1ULL);
    BOOST_CHECK_EQUAL(descendants, 1ULL);

    /* Child transaction */
    //
    // [tx1].0 <- [tx2]
    //
    CTransactionRef tx2 = make_tx(/*output_values=*/{495 * CENT, 5 * COIN}, /*inputs=*/{tx1});
    AddToRelayPool(pool, entry.FromTx(tx2));

    // Ancestors / descendants should be:
    // transaction  ancestors   descendants
    // ============ =========== ===========
    // tx1          1 (tx1)     2 (tx1,2)
    // tx2          2 (tx1,2)   2 (tx1,2)
    pool.GetTransactionAncestry(tx1->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 1ULL);
    BOOST_CHECK_EQUAL(descendants, 2ULL);
    pool.GetTransactionAncestry(tx2->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 2ULL);
    BOOST_CHECK_EQUAL(descendants, 2ULL);

    /* Grand-child 1 */
    //
    // [tx1].0 <- [tx2].0 <- [tx3]
    //
    CTransactionRef tx3 = make_tx(/*output_values=*/{290 * CENT, 200 * CENT}, /*inputs=*/{tx2});
    AddToRelayPool(pool, entry.FromTx(tx3));

    // Ancestors / descendants should be:
    // transaction  ancestors   descendants
    // ============ =========== ===========
    // tx1          1 (tx1)     3 (tx1,2,3)
    // tx2          2 (tx1,2)   3 (tx1,2,3)
    // tx3          3 (tx1,2,3) 3 (tx1,2,3)
    pool.GetTransactionAncestry(tx1->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 1ULL);
    BOOST_CHECK_EQUAL(descendants, 3ULL);
    pool.GetTransactionAncestry(tx2->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 2ULL);
    BOOST_CHECK_EQUAL(descendants, 3ULL);
    pool.GetTransactionAncestry(tx3->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 3ULL);
    BOOST_CHECK_EQUAL(descendants, 3ULL);

    /* Grand-child 2 */
    //
    // [tx1].0 <- [tx2].0 <- [tx3]
    //              |
    //              \---1 <- [tx4]
    //
    CTransactionRef tx4 = make_tx(/*output_values=*/{290 * CENT, 250 * CENT}, /*inputs=*/{tx2}, /*input_indices=*/{1});
    AddToRelayPool(pool, entry.FromTx(tx4));

    // Ancestors / descendants should be:
    // transaction  ancestors   descendants
    // ============ =========== ===========
    // tx1          1 (tx1)     4 (tx1,2,3,4)
    // tx2          2 (tx1,2)   4 (tx1,2,3,4)
    // tx3          3 (tx1,2,3) 4 (tx1,2,3,4)
    // tx4          3 (tx1,2,4) 4 (tx1,2,3,4)
    pool.GetTransactionAncestry(tx1->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 1ULL);
    BOOST_CHECK_EQUAL(descendants, 4ULL);
    pool.GetTransactionAncestry(tx2->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 2ULL);
    BOOST_CHECK_EQUAL(descendants, 4ULL);
    pool.GetTransactionAncestry(tx3->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 3ULL);
    BOOST_CHECK_EQUAL(descendants, 4ULL);
    pool.GetTransactionAncestry(tx4->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 3ULL);
    BOOST_CHECK_EQUAL(descendants, 4ULL);

    /* Make an alternate branch that is longer and connect it to tx3 */
    //
    // [ty1].0 <- [ty2].0 <- [ty3].0 <- [ty4].0 <- [ty5].0
    //                                              |
    // [tx1].0 <- [tx2].0 <- [tx3].0 <- [ty6] --->--/
    //              |
    //              \---1 <- [tx4]
    //
    CTransactionRef ty1, ty2, ty3, ty4, ty5;
    CTransactionRef* ty[5] = {&ty1, &ty2, &ty3, &ty4, &ty5};
    CAmount v = 5 * COIN;
    for (uint64_t i = 0; i < 5; i++) {
        CTransactionRef& tyi = *ty[i];
        tyi = make_tx(/*output_values=*/{v}, /*inputs=*/i > 0 ? std::vector<CTransactionRef>{*ty[i - 1]} : std::vector<CTransactionRef>{});
        v -= 50 * CENT;
        AddToRelayPool(pool, entry.FromTx(tyi));
        pool.GetTransactionAncestry(tyi->GetHash(), ancestors, descendants);
        BOOST_CHECK_EQUAL(ancestors, i+1);
        BOOST_CHECK_EQUAL(descendants, i+1);
    }
    CTransactionRef ty6 = make_tx(/*output_values=*/{5 * COIN}, /*inputs=*/{tx3, ty5});
    AddToRelayPool(pool, entry.FromTx(ty6));

    // Ancestors / descendants should be:
    // transaction  ancestors           descendants
    // ============ =================== ===========
    // tx1          1 (tx1)             5 (tx1,2,3,4, ty6)
    // tx2          2 (tx1,2)           5 (tx1,2,3,4, ty6)
    // tx3          3 (tx1,2,3)         5 (tx1,2,3,4, ty6)
    // tx4          3 (tx1,2,4)         5 (tx1,2,3,4, ty6)
    // ty1          1 (ty1)             6 (ty1,2,3,4,5,6)
    // ty2          2 (ty1,2)           6 (ty1,2,3,4,5,6)
    // ty3          3 (ty1,2,3)         6 (ty1,2,3,4,5,6)
    // ty4          4 (y1234)           6 (ty1,2,3,4,5,6)
    // ty5          5 (y12345)          6 (ty1,2,3,4,5,6)
    // ty6          9 (tx123, ty123456) 6 (ty1,2,3,4,5,6)
    pool.GetTransactionAncestry(tx1->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 1ULL);
    BOOST_CHECK_EQUAL(descendants, 5ULL);
    pool.GetTransactionAncestry(tx2->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 2ULL);
    BOOST_CHECK_EQUAL(descendants, 5ULL);
    pool.GetTransactionAncestry(tx3->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 3ULL);
    BOOST_CHECK_EQUAL(descendants, 5ULL);
    pool.GetTransactionAncestry(tx4->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 3ULL);
    BOOST_CHECK_EQUAL(descendants, 5ULL);
    pool.GetTransactionAncestry(ty1->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 1ULL);
    BOOST_CHECK_EQUAL(descendants, 6ULL);
    pool.GetTransactionAncestry(ty2->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 2ULL);
    BOOST_CHECK_EQUAL(descendants, 6ULL);
    pool.GetTransactionAncestry(ty3->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 3ULL);
    BOOST_CHECK_EQUAL(descendants, 6ULL);
    pool.GetTransactionAncestry(ty4->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 4ULL);
    BOOST_CHECK_EQUAL(descendants, 6ULL);
    pool.GetTransactionAncestry(ty5->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 5ULL);
    BOOST_CHECK_EQUAL(descendants, 6ULL);
    pool.GetTransactionAncestry(ty6->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 9ULL);
    BOOST_CHECK_EQUAL(descendants, 6ULL);
}

BOOST_AUTO_TEST_CASE(RelayPoolAncestryTestsDiamond)
{
    size_t ancestors, descendants;

    CTxRelayPool& pool = *Assert(m_node.relaypool);
    LOCK2(::cs_main, pool.cs);
    TestRelayPoolEntryHelper entry;

    /* Ancestors represented more than once ("diamond") */
    //
    // [ta].0 <- [tb].0 -----<------- [td].0
    //            |                    |
    //            \---1 <- [tc].0 --<--/
    //
    CTransactionRef ta, tb, tc, td;
    ta = make_tx(/*output_values=*/{10 * COIN});
    tb = make_tx(/*output_values=*/{5 * COIN, 3 * COIN}, /*inputs=*/ {ta});
    tc = make_tx(/*output_values=*/{2 * COIN}, /*inputs=*/{tb}, /*input_indices=*/{1});
    td = make_tx(/*output_values=*/{6 * COIN}, /*inputs=*/{tb, tc}, /*input_indices=*/{0, 0});
    AddToRelayPool(pool, entry.FromTx(ta));
    AddToRelayPool(pool, entry.FromTx(tb));
    AddToRelayPool(pool, entry.FromTx(tc));
    AddToRelayPool(pool, entry.FromTx(td));

    // Ancestors / descendants should be:
    // transaction  ancestors           descendants
    // ============ =================== ===========
    // ta           1 (ta               4 (ta,tb,tc,td)
    // tb           2 (ta,tb)           4 (ta,tb,tc,td)
    // tc           3 (ta,tb,tc)        4 (ta,tb,tc,td)
    // td           4 (ta,tb,tc,td)     4 (ta,tb,tc,td)
    pool.GetTransactionAncestry(ta->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 1ULL);
    BOOST_CHECK_EQUAL(descendants, 4ULL);
    pool.GetTransactionAncestry(tb->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 2ULL);
    BOOST_CHECK_EQUAL(descendants, 4ULL);
    pool.GetTransactionAncestry(tc->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 3ULL);
    BOOST_CHECK_EQUAL(descendants, 4ULL);
    pool.GetTransactionAncestry(td->GetHash(), ancestors, descendants);
    BOOST_CHECK_EQUAL(ancestors, 4ULL);
    BOOST_CHECK_EQUAL(descendants, 4ULL);
}

BOOST_AUTO_TEST_SUITE_END()
