// Copyright (c) 2021-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <common/system.h>
#include <policy/replacement.h>
#include <random.h>
#include <test/util/txrelaypool.h>
#include <txrelaypool.h>
#include <util/time.h>
#include <util/workfrac.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>
#include <optional>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(replacement_tests, TestingSetup)

static inline CTransactionRef make_tx(const std::vector<CTransactionRef>& inputs,
                                      const std::vector<CAmount>& output_values)
{
    CMutableTransaction tx = CMutableTransaction();
    tx.vin.resize(inputs.size());
    tx.vout.resize(output_values.size());
    for (size_t i = 0; i < inputs.size(); ++i) {
        tx.vin[i].prevout.hash = inputs[i]->GetHash();
        tx.vin[i].prevout.n = 0;
        // Add a witness so wtxid != txid
        CScriptWitness witness;
        witness.stack.emplace_back(i + 10);
        tx.vin[i].scriptWitness = witness;
    }
    for (size_t i = 0; i < output_values.size(); ++i) {
        tx.vout[i].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
        tx.vout[i].nValue = output_values[i];
    }
    return MakeTransactionRef(tx);
}


static CTransactionRef add_descendants(const CTransactionRef& tx, int32_t num_descendants, CTxRelayPool& pool)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main, pool.cs)
{
    AssertLockHeld(::cs_main);
    AssertLockHeld(pool.cs);
    TestRelayPoolEntryHelper entry;
    // Assumes this isn't already spent in relaypool
    auto tx_to_spend = tx;
    for (int32_t i{0}; i < num_descendants; ++i) {
        auto next_tx = make_tx(/*inputs=*/{tx_to_spend}, /*output_values=*/{(50 - i) * CENT});
        AddToRelayPool(pool, entry.FromTx(next_tx));
        tx_to_spend = next_tx;
    }
    // Return last created tx
    return tx_to_spend;
}


BOOST_FIXTURE_TEST_CASE(replacement_helper_functions, TestChain100Setup)
{
    CTxRelayPool& pool = *Assert(m_node.relaypool);
    LOCK2(::cs_main, pool.cs);
    TestRelayPoolEntryHelper entry;

    // Create a parent tx1 and child tx2.
    const auto tx1 = make_tx(/*inputs=*/ {m_coinbase_txns[0]}, /*output_values=*/ {10 * COIN});
    AddToRelayPool(pool, entry.FromTx(tx1));
    const auto tx2 = make_tx(/*inputs=*/ {tx1}, /*output_values=*/ {995 * CENT});
    AddToRelayPool(pool, entry.FromTx(tx2));

    // Create a parent tx3 and child tx4.
    const auto tx3 = make_tx(/*inputs=*/ {m_coinbase_txns[1]}, /*output_values=*/ {1099 * CENT});
    AddToRelayPool(pool, entry.FromTx(tx3));
    const auto tx4 = make_tx(/*inputs=*/ {tx3}, /*output_values=*/ {999 * CENT});
    AddToRelayPool(pool, entry.FromTx(tx4));

    // Create a parent tx5 and child tx6.
    const auto tx5 = make_tx(/*inputs=*/ {m_coinbase_txns[2]}, /*output_values=*/ {1099 * CENT});
    AddToRelayPool(pool, entry.FromTx(tx5));
    const auto tx6 = make_tx(/*inputs=*/ {tx5}, /*output_values=*/ {1098 * CENT});
    AddToRelayPool(pool, entry.FromTx(tx6));

    // Two independent transactions, tx7 and tx8.
    const auto tx7 = make_tx(/*inputs=*/ {m_coinbase_txns[3]}, /*output_values=*/ {999 * CENT});
    AddToRelayPool(pool, entry.FromTx(tx7));
    const auto tx8 = make_tx(/*inputs=*/ {m_coinbase_txns[4]}, /*output_values=*/ {999 * CENT});
    AddToRelayPool(pool, entry.FromTx(tx8));


    const auto entry1_normal = pool.GetIter(tx1->GetHash()).value();
    const auto entry2_normal = pool.GetIter(tx2->GetHash()).value();
    const auto entry3_low = pool.GetIter(tx3->GetHash()).value();
    const auto entry4_high = pool.GetIter(tx4->GetHash()).value();
    const auto entry5_low = pool.GetIter(tx5->GetHash()).value();
    const auto entry6_low_prioritised = pool.GetIter(tx6->GetHash()).value();
    const auto entry7_high = pool.GetIter(tx7->GetHash()).value();
    const auto entry8_high = pool.GetIter(tx8->GetHash()).value();

    CTxRelayPool::setEntries set_12_normal{entry1_normal, entry2_normal};
    CTxRelayPool::setEntries all_entries{entry1_normal, entry2_normal, entry3_low, entry4_high,
                                       entry5_low, entry6_low_prioritised, entry7_high, entry8_high};
    CTxRelayPool::setEntries empty_set;

    const auto unused_txid{GetRandHash()};

    // Tests for EntriesAndTxidsDisjoint
    BOOST_CHECK(EntriesAndTxidsDisjoint(empty_set, {tx1->GetHash()}, unused_txid) == std::nullopt);
    BOOST_CHECK(EntriesAndTxidsDisjoint(set_12_normal, {tx3->GetHash()}, unused_txid) == std::nullopt);
    BOOST_CHECK(EntriesAndTxidsDisjoint({entry2_normal}, {tx2->GetHash()}, unused_txid).has_value());
    BOOST_CHECK(EntriesAndTxidsDisjoint(set_12_normal, {tx1->GetHash()}, unused_txid).has_value());
    BOOST_CHECK(EntriesAndTxidsDisjoint(set_12_normal, {tx2->GetHash()}, unused_txid).has_value());
    // EntriesAndTxidsDisjoint does not calculate descendants of iters_conflicting; it uses whatever
    // the caller passed in. As such, no error is returned even though entry2_normal is a descendant of tx1.
    BOOST_CHECK(EntriesAndTxidsDisjoint({entry2_normal}, {tx1->GetHash()}, unused_txid) == std::nullopt);

    // Tests for GetEntriesForConflicts
    CTxRelayPool::setEntries all_parents{entry1_normal, entry3_low, entry5_low, entry7_high, entry8_high};
    CTxRelayPool::setEntries all_children{entry2_normal, entry4_high, entry6_low_prioritised};
    const std::vector<CTransactionRef> parent_inputs({m_coinbase_txns[0], m_coinbase_txns[1], m_coinbase_txns[2],
                                                m_coinbase_txns[3], m_coinbase_txns[4]});
    const auto conflicts_with_parents = make_tx(parent_inputs, {50 * CENT});
    CTxRelayPool::setEntries all_conflicts;
    BOOST_CHECK(GetEntriesForConflicts(/*tx=*/ *conflicts_with_parents.get(),
                                       /*pool=*/ pool,
                                       /*iters_conflicting=*/ all_parents,
                                       /*all_conflicts=*/ all_conflicts) == std::nullopt);
    BOOST_CHECK(all_conflicts == all_entries);
    auto conflicts_size = all_conflicts.size();
    all_conflicts.clear();

    add_descendants(tx2, 23, pool);
    BOOST_CHECK(GetEntriesForConflicts(*conflicts_with_parents.get(), pool, all_parents, all_conflicts) == std::nullopt);
    conflicts_size += 23;
    BOOST_CHECK_EQUAL(all_conflicts.size(), conflicts_size);
    all_conflicts.clear();

    add_descendants(tx4, 23, pool);
    BOOST_CHECK(GetEntriesForConflicts(*conflicts_with_parents.get(), pool, all_parents, all_conflicts) == std::nullopt);
    conflicts_size += 23;
    BOOST_CHECK_EQUAL(all_conflicts.size(), conflicts_size);
    all_conflicts.clear();

    add_descendants(tx6, 23, pool);
    BOOST_CHECK(GetEntriesForConflicts(*conflicts_with_parents.get(), pool, all_parents, all_conflicts) == std::nullopt);
    conflicts_size += 23;
    BOOST_CHECK_EQUAL(all_conflicts.size(), conflicts_size);
    all_conflicts.clear();

    add_descendants(tx7, 23, pool);
    BOOST_CHECK(GetEntriesForConflicts(*conflicts_with_parents.get(), pool, all_parents, all_conflicts) == std::nullopt);
    conflicts_size += 23;
    BOOST_CHECK_EQUAL(all_conflicts.size(), conflicts_size);
    BOOST_CHECK_EQUAL(all_conflicts.size(), 100);
    all_conflicts.clear();

    // Exceeds maximum number of conflicts.
    add_descendants(tx8, 1, pool);
    BOOST_CHECK(GetEntriesForConflicts(*conflicts_with_parents.get(), pool, all_parents, all_conflicts).has_value());

    // Tests for HasNoNewUnconfirmed
    const auto spends_unconfirmed = make_tx({tx1}, {36 * CENT});
    for (const auto& input : spends_unconfirmed->vin) {
        // Spends unconfirmed inputs.
        BOOST_CHECK(pool.exists(GenTxid::Txid(input.prevout.hash)));
    }
    BOOST_CHECK(HasNoNewUnconfirmed(/*tx=*/ *spends_unconfirmed.get(),
                                    /*pool=*/ pool,
                                    /*iters_conflicting=*/ all_entries) == std::nullopt);
    BOOST_CHECK(HasNoNewUnconfirmed(*spends_unconfirmed.get(), pool, {entry2_normal}) == std::nullopt);
    BOOST_CHECK(HasNoNewUnconfirmed(*spends_unconfirmed.get(), pool, empty_set).has_value());

    const auto spends_new_unconfirmed = make_tx({tx1, tx8}, {36 * CENT});
    BOOST_CHECK(HasNoNewUnconfirmed(*spends_new_unconfirmed.get(), pool, {entry2_normal}).has_value());
    BOOST_CHECK(HasNoNewUnconfirmed(*spends_new_unconfirmed.get(), pool, all_entries).has_value());

    const auto spends_conflicting_confirmed = make_tx({m_coinbase_txns[0], m_coinbase_txns[1]}, {45 * CENT});
    BOOST_CHECK(HasNoNewUnconfirmed(*spends_conflicting_confirmed.get(), pool, {entry1_normal, entry3_low}) == std::nullopt);

}

BOOST_AUTO_TEST_CASE(workrate_chunks_utilities)
{
    // Sanity check the correctness of the workrate chunks comparison.

    // A strictly better case.
    std::vector<WorkFrac> old_chunks{{{950, 300}, {100, 100}}};
    std::vector<WorkFrac> new_chunks{{{1000, 300}, {50, 100}}};

    BOOST_CHECK(std::is_lt(CompareChunks(old_chunks, new_chunks)));
    BOOST_CHECK(std::is_gt(CompareChunks(new_chunks, old_chunks)));

    // Incomparable diagrams
    old_chunks = {{950, 300}, {100, 100}};
    new_chunks = {{1000, 300}, {0, 100}};

    BOOST_CHECK(CompareChunks(old_chunks, new_chunks) == std::partial_ordering::unordered);
    BOOST_CHECK(CompareChunks(new_chunks, old_chunks) == std::partial_ordering::unordered);

    // Strictly better but smaller size.
    old_chunks = {{950, 300}, {100, 100}};
    new_chunks = {{1100, 300}};

    BOOST_CHECK(std::is_lt(CompareChunks(old_chunks, new_chunks)));
    BOOST_CHECK(std::is_gt(CompareChunks(new_chunks, old_chunks)));

    // New diagram is strictly better due to the first chunk, even though
    // the second chunk contributes no work.
    old_chunks = {{950, 300}, {100, 100}};
    new_chunks = {{1100, 100}, {0, 100}};

    BOOST_CHECK(std::is_lt(CompareChunks(old_chunks, new_chunks)));
    BOOST_CHECK(std::is_gt(CompareChunks(new_chunks, old_chunks)));

    // Work of the first new chunk is better, but the second chunk is worse
    old_chunks = {{950, 300}, {100, 100}};
    new_chunks = {{750, 100}, {249, 250}, {151, 650}};

    BOOST_CHECK(CompareChunks(old_chunks, new_chunks) == std::partial_ordering::unordered);
    BOOST_CHECK(CompareChunks(new_chunks, old_chunks) == std::partial_ordering::unordered);

    // If we make the second chunk slightly better, the new diagram now wins.
    old_chunks = {{950, 300}, {100, 100}};
    new_chunks = {{750, 100}, {250, 250}, {150, 150}};

    BOOST_CHECK(std::is_lt(CompareChunks(old_chunks, new_chunks)));
    BOOST_CHECK(std::is_gt(CompareChunks(new_chunks, old_chunks)));

    // Identical diagrams, cannot be strictly better
    old_chunks = {{950, 300}, {100, 100}};
    new_chunks = {{950, 300}, {100, 100}};

    BOOST_CHECK(std::is_eq(CompareChunks(old_chunks, new_chunks)));
    BOOST_CHECK(std::is_eq(CompareChunks(new_chunks, old_chunks)));

    // Same aggregate work, but different total size (trigger single tail work check step)
    old_chunks = {{950, 300}, {100, 99}};
    new_chunks = {{950, 300}, {100, 100}};

    // No change in evaluation when tail check needed.
    BOOST_CHECK(std::is_gt(CompareChunks(old_chunks, new_chunks)));
    BOOST_CHECK(std::is_lt(CompareChunks(new_chunks, old_chunks)));

    // Trigger multiple tail work check steps
    old_chunks = {{950, 300}, {100, 99}};
    new_chunks = {{950, 300}, {100, 100}, {0, 1}, {0, 1}};

    BOOST_CHECK(std::is_gt(CompareChunks(old_chunks, new_chunks)));
    BOOST_CHECK(std::is_lt(CompareChunks(new_chunks, old_chunks)));

    // Multiple tail work check steps, unordered result
    new_chunks = {{950, 300}, {100, 100}, {0, 1}, {0, 1}, {1, 1}};
    BOOST_CHECK(CompareChunks(old_chunks, new_chunks) == std::partial_ordering::unordered);
    BOOST_CHECK(CompareChunks(new_chunks, old_chunks) == std::partial_ordering::unordered);
}

// #6: surplus-work replacement comparators (replace-by-work -> replace-by-surplus-work).
BOOST_FIXTURE_TEST_CASE(pays_more_work_rate, TestChain100Setup)
{
    CTxRelayPool& pool = *Assert(m_node.relaypool);
    LOCK2(::cs_main, pool.cs);
    TestRelayPoolEntryHelper entry;

    // A conflict carrying a known surplus. Its rate is surplus/vsize.
    const auto txc = make_tx(/*inputs=*/{m_coinbase_txns[0]}, /*output_values=*/{10 * COIN});
    AddToRelayPool(pool, entry.TxWorkSurplus(arith_uint256(1000)).FromTx(txc));
    const auto entryc = pool.GetIter(txc->GetHash()).value();
    const double rate_c = entryc->GetTxWorkRate();
    BOOST_CHECK_GT(rate_c, 0.0);

    CTxRelayPool::setEntries conflicts{entryc};
    const auto txid{GetRandHash()};

    // Strictly greater rate passes; equal and lower are rejected.
    BOOST_CHECK(PaysMoreWorkThanConflicts(conflicts, /*replacement_work_rate=*/rate_c * 1.01, txid) == std::nullopt);
    BOOST_CHECK(PaysMoreWorkThanConflicts(conflicts, /*replacement_work_rate=*/rate_c, txid).has_value());
    BOOST_CHECK(PaysMoreWorkThanConflicts(conflicts, /*replacement_work_rate=*/rate_c * 0.99, txid).has_value());

    // Must beat EVERY conflict: a second, higher-rate conflict raises the bar.
    const auto txc2 = make_tx(/*inputs=*/{m_coinbase_txns[1]}, /*output_values=*/{10 * COIN});
    AddToRelayPool(pool, entry.TxWorkSurplus(arith_uint256(2000)).FromTx(txc2));
    const auto entryc2 = pool.GetIter(txc2->GetHash()).value();
    const double rate_c2 = entryc2->GetTxWorkRate();
    BOOST_CHECK_GT(rate_c2, rate_c);
    CTxRelayPool::setEntries two_conflicts{entryc, entryc2};
    // Beats the lower but not the higher -> rejected.
    BOOST_CHECK(PaysMoreWorkThanConflicts(two_conflicts, rate_c * 1.01, txid).has_value());
    // Beats both -> passes.
    BOOST_CHECK(PaysMoreWorkThanConflicts(two_conflicts, rate_c2 * 1.01, txid) == std::nullopt);
}

BOOST_AUTO_TEST_CASE(pays_more_work_abs)
{
    const auto txid{GetRandHash()};
    // Strictly greater absolute surplus passes; equal and lower are rejected.
    BOOST_CHECK(PaysMoreWorkForReplacement(/*original_surplus_sum=*/arith_uint256(50), /*replacement_surplus=*/arith_uint256(51), txid) == std::nullopt);
    BOOST_CHECK(PaysMoreWorkForReplacement(arith_uint256(50), arith_uint256(50), txid).has_value());
    BOOST_CHECK(PaysMoreWorkForReplacement(arith_uint256(50), arith_uint256(49), txid).has_value());
}

BOOST_AUTO_TEST_SUITE_END()
