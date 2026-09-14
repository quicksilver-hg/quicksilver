// Copyright (c) 2016-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_POLICY_REPLACEMENT_H
#define QUICKSILVER_POLICY_REPLACEMENT_H

#include <arith_uint256.h>
#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <threadsafety.h>
#include <txrelaypool.h>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>

class uint256;

/** Maximum number of transactions that can be replaced (Rule #5). This includes all
 * relaypool conflicts and their descendants. */
static constexpr uint32_t MAX_REPLACEMENT_CANDIDATES{100};

/** Get all descendants of iters_conflicting. Checks that there are no more than
 * MAX_REPLACEMENT_CANDIDATES potential entries. May overestimate if the entries in
 * iters_conflicting have overlapping descendants.
 * @param[in]   iters_conflicting   The set of iterators to relaypool entries.
 * @param[out]  all_conflicts       Populated with all the relaypool entries that would be replaced,
 *                                  which includes iters_conflicting and all entries' descendants.
 *                                  Not cleared at the start; any existing relaypool entries will
 *                                  remain in the set.
 * @returns an error message if MAX_REPLACEMENT_CANDIDATES may be exceeded, otherwise a std::nullopt.
 */
std::optional<std::string> GetEntriesForConflicts(const CTransaction& tx, CTxRelayPool& pool,
                                                  const CTxRelayPool::setEntries& iters_conflicting,
                                                  CTxRelayPool::setEntries& all_conflicts)
    EXCLUSIVE_LOCKS_REQUIRED(pool.cs);

/** The replacement transaction may only include an unconfirmed input if that input was included in
 * one of the original transactions.
 * @returns error message if tx spends unconfirmed inputs not also spent by iters_conflicting,
 * otherwise std::nullopt. */
std::optional<std::string> HasNoNewUnconfirmed(const CTransaction& tx, const CTxRelayPool& pool,
                                               const CTxRelayPool::setEntries& iters_conflicting)
    EXCLUSIVE_LOCKS_REQUIRED(pool.cs);

/** Check the intersection between two sets of transactions (a set of relaypool entries and a set of
 * txids) to make sure they are disjoint.
 * @param[in]   ancestors           Set of relaypool entries corresponding to ancestors of the
 *                                  replacement transactions.
 * @param[in]   direct_conflicts    Set of txids corresponding to the relaypool conflicts
 *                                  (candidates to be replaced).
 * @param[in]   txid                Transaction ID, included in the error message if violation occurs.
 * @returns error message if the sets intersect, std::nullopt if they are disjoint.
 */
std::optional<std::string> EntriesAndTxidsDisjoint(const CTxRelayPool::setEntries& ancestors,
                                                   const std::set<Txid>& direct_conflicts,
                                                   const uint256& txid);

/** #6: rate test (BIP125 Rule 6 analog in surplus-work units). The replacement's
 * surplus-work rate must strictly exceed every direct conflict's GetTxWorkRate().
 * Keeps replacement consistent with the 5c-2 by_txwork_rate eviction/mining order.
 * @returns error message if insufficient, otherwise std::nullopt. */
std::optional<std::string> PaysMoreWorkThanConflicts(const CTxRelayPool::setEntries& iters_conflicting,
                                                     double replacement_work_rate, const uint256& txid);

/** #6: absolute-sum guard (BIP125 Rule 3 analog). The replacement's absolute surplus
 * work must strictly exceed the summed surplus of all replaced txs. There is no Rule 4 /
 * relay term: each rarer cycle is a full Cuckatoo solve, so the PoW cost is the
 * anti-cycling toll and Rule 5 caps the chain.
 * @returns error message if insufficient, otherwise std::nullopt. */
std::optional<std::string> PaysMoreWorkForReplacement(const arith_uint256& original_surplus_sum,
                                              const arith_uint256& replacement_surplus,
                                              const uint256& txid);

#endif // QUICKSILVER_POLICY_REPLACEMENT_H
