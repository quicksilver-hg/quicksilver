// Copyright (c) 2016-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <policy/replacement.h>

#include <consensus/amount.h>
#include <kernel/relaypool_entry.h>
#include <primitives/transaction.h>
#include <sync.h>
#include <tinyformat.h>
#include <txrelaypool.h>
#include <uint256.h>
#include <util/check.h>
#include <util/moneystr.h>

#include <limits>
#include <vector>

#include <compare>

std::optional<std::string> GetEntriesForConflicts(const CTransaction& tx,
                                                  CTxRelayPool& pool,
                                                  const CTxRelayPool::setEntries& iters_conflicting,
                                                  CTxRelayPool::setEntries& all_conflicts)
{
    AssertLockHeld(pool.cs);
    const uint256 txid = tx.GetHash();
    uint64_t nConflictingCount = 0;
    for (const auto& mi : iters_conflicting) {
        nConflictingCount += mi->GetCountWithDescendants();
        // Rule #5: don't consider replacing more than MAX_REPLACEMENT_CANDIDATES
        // entries from the relaypool. This potentially overestimates the number of actual
        // descendants (i.e. if multiple conflicts share a descendant, it will be counted multiple
        // times), but we just want to be conservative to avoid doing too much work.
        if (nConflictingCount > MAX_REPLACEMENT_CANDIDATES) {
            return strprintf("rejecting replacement %s; too many potential replacements (%d > %d)",
                             txid.ToString(),
                             nConflictingCount,
                             MAX_REPLACEMENT_CANDIDATES);
        }
    }
    // Calculate the set of all transactions that would have to be evicted.
    for (CTxRelayPool::txiter it : iters_conflicting) {
        pool.CalculateDescendants(it, all_conflicts);
    }
    return std::nullopt;
}

std::optional<std::string> HasNoNewUnconfirmed(const CTransaction& tx,
                                               const CTxRelayPool& pool,
                                               const CTxRelayPool::setEntries& iters_conflicting)
{
    AssertLockHeld(pool.cs);
    std::set<uint256> parents_of_conflicts;
    for (const auto& mi : iters_conflicting) {
        for (const CTxIn& txin : mi->GetTx().vin) {
            parents_of_conflicts.insert(txin.prevout.hash);
        }
    }

    for (unsigned int j = 0; j < tx.vin.size(); j++) {
        // Rule #2: We don't want to accept replacements that require low workrate junk to be
        // mined first. Ideally we'd keep track of the ancestor workrates and make the decision
        // based on that, but for now requiring all new inputs to be confirmed works.
        //
        // Note that if you relax this to make replacement a little more useful, this may break the
        // CalculateRelayPoolAncestors replacement relaxation which subtracts the conflict count/size from the
        // descendant limit.
        if (!parents_of_conflicts.count(tx.vin[j].prevout.hash)) {
            // Rather than check the UTXO set - potentially expensive - it's cheaper to just check
            // if the new input refers to a tx that's in the relaypool.
            if (pool.exists(GenTxid::Txid(tx.vin[j].prevout.hash))) {
                return strprintf("replacement %s adds unconfirmed input, idx %d",
                                 tx.GetHash().ToString(), j);
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> EntriesAndTxidsDisjoint(const CTxRelayPool::setEntries& ancestors,
                                                   const std::set<Txid>& direct_conflicts,
                                                   const uint256& txid)
{
    for (CTxRelayPool::txiter ancestorIt : ancestors) {
        const Txid& hashAncestor = ancestorIt->GetTx().GetHash();
        if (direct_conflicts.count(hashAncestor)) {
            return strprintf("%s spends conflicting transaction %s",
                             txid.ToString(),
                             hashAncestor.ToString());
        }
    }
    return std::nullopt;
}

std::optional<std::string> PaysMoreWorkThanConflicts(const CTxRelayPool::setEntries& iters_conflicting,
                                                     double replacement_work_rate,
                                                     const uint256& txid)
{
    for (const auto& mi : iters_conflicting) {
        // #6: the replacement must out-rank every direct conflict in surplus-work rate
        // (surplus/vsize), the same key 5c-2 uses for eviction/mining — so replacement
        // can never admit a tx that eviction would immediately drop below a conflict.
        const double original_rate = mi->GetTxWorkRate();
        if (replacement_work_rate <= original_rate) {
            return strprintf("rejecting replacement %s; new work-rate %f <= old work-rate %f",
                             txid.ToString(), replacement_work_rate, original_rate);
        }
    }
    return std::nullopt;
}

std::optional<std::string> PaysMoreWorkForReplacement(const arith_uint256& original_surplus_sum,
                                              const arith_uint256& replacement_surplus,
                                              const uint256& txid)
{
    // Out-work the whole replaced set. Each rarer cycle is a full Cuckatoo solve,
    // so proof-of-work supplies the anti-cycling cost while the conflict cap bounds
    // the replacement chain.
    if (replacement_surplus <= original_surplus_sum) {
        return strprintf("rejecting replacement %s, insufficient surplus work; %s <= %s",
                         txid.ToString(), replacement_surplus.ToString(), original_surplus_sum.ToString());
    }
    return std::nullopt;
}
