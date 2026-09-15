// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_TXRELAYPOOL_H
#define QUICKSILVER_TXRELAYPOOL_H

#include <coins.h>
#include <consensus/amount.h>
#include <indirectmap.h>
#include <kernel/cs_main.h>
#include <kernel/relaypool_entry.h>          // IWYU pragma: export
#include <kernel/relaypool_limits.h>         // IWYU pragma: export
#include <kernel/relaypool_options.h>        // IWYU pragma: export
#include <kernel/relaypool_removal_reason.h> // IWYU pragma: export
#include <policy/packages.h>
#include <primitives/transaction.h>
#include <sync.h>
#include <util/epochguard.h>
#include <util/hasher.h>
#include <util/result.h>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/identity.hpp>
#include <boost/multi_index/indexed_by.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index/tag.hpp>
#include <boost/multi_index_container.hpp>

#include <atomic>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class CChain;
class ValidationSignals;
namespace Consensus { struct Params; }

struct bilingual_str;

/** Fake height value used in Coin to signify they are only in the memory pool (since 0.8) */
static const uint32_t RELAYPOOL_HEIGHT = 0x7FFFFFFF;

/**
 * Test whether the LockPoints height and time are still valid on the current chain
 */
bool TestLockPointValidity(CChain& active_chain, const LockPoints& lp) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

// extracts a transaction hash from CTxRelayPoolEntry or CTransactionRef
struct relaypoolentry_txid
{
    typedef uint256 result_type;
    result_type operator() (const CTxRelayPoolEntry &entry) const
    {
        return entry.GetTx().GetHash();
    }

    result_type operator() (const CTransactionRef& tx) const
    {
        return tx->GetHash();
    }
};

// extracts a transaction witness-hash from CTxRelayPoolEntry or CTransactionRef
struct relaypoolentry_wtxid
{
    typedef uint256 result_type;
    result_type operator() (const CTxRelayPoolEntry &entry) const
    {
        return entry.GetTx().GetWitnessHash();
    }

    result_type operator() (const CTransactionRef& tx) const
    {
        return tx->GetWitnessHash();
    }
};


class CompareTxRelayPoolEntryByEntryTime
{
public:
    bool operator()(const CTxRelayPoolEntry& a, const CTxRelayPoolEntry& b) const
    {
        return a.GetTime() < b.GetTime();
    }
};

/** \class CompareTxRelayPoolEntryByTxWorkRate
 *
 *  #5c-2 per-tx individual inclusion ranking. Sort by surplus per-tx work rate
 *  (surplus/vsize) in DESCENDING order: the highest-rate tx is begin() (mined
 *  first), the lowest-rate tx is the last element (evicted first). Ties (equal
 *  rate) break by entry time, oldest first.
 *
 *  ⚠ That tie-break is read from OPPOSITE ENDS by the two consumers, so it does not
 *  mean the same thing to both. Mining walks forward from begin(), so among equals it
 *  takes the OLDEST first -- FIFO. TrimToSize evicts std::prev(end()), so among equals
 *  it drops the NEWEST. Eviction is therefore LIFO, and deliberately so; an earlier
 *  version of this comment claimed "FIFO fairness" without qualification, which
 *  described only the mining half and read as though it covered both.
 *
 *  On a feeless chain this is not a corner case. The ordinary state of the pool is
 *  "everything at the floor", surplus 0, so this tie-break IS the admission policy once
 *  the pool is full: first-come-first-served, with incumbents keeping their place until
 *  they expire. A newcomer's way in is to grind MORE surplus work, not to wait its turn.
 *  Decided 2026-09-04; pinned by txwork_ranking_tests/equal_rate_eviction_drops_the_newest.
 */
class CompareTxRelayPoolEntryByTxWorkRate
{
public:
    bool operator()(const CTxRelayPoolEntry& a, const CTxRelayPoolEntry& b) const
    {
        const double ra = a.GetTxWorkRate();
        const double rb = b.GetTxWorkRate();
        if (ra == rb) {
            return a.GetTime() < b.GetTime(); // oldest first among equals (see class comment)
        }
        return ra > rb; // higher rate first
    }
};

// Multi_index tag names.
struct entry_time {};
struct txwork_rate {};
struct index_by_wtxid {};

/**
 * Information about a relaypool transaction.
 */
struct TxRelayPoolInfo
{
    /** The transaction itself */
    CTransactionRef tx;

    /** Time the transaction entered the relaypool. */
    std::chrono::seconds m_time;

    /** Virtual size of the transaction. */
    int32_t vsize;
};

/**
 * CTxRelayPool stores valid-according-to-the-current-best-chain transactions
 * that may be included in the next block.
 *
 * Transactions are added when they are seen on the network (or created by the
 * local node), but not all transactions seen are added to the pool. For
 * example, the following new transactions will not be added to the relaypool:
 * - a transaction which doesn't meet current relaypool policy.
 * - a new transaction that double-spends an input of a transaction already in
 * the pool where the new transaction does not meet replacement
 * requirements as defined in doc/policy/relaypool-replacements.md.
 * - a non-standard transaction.
 *
 * CTxRelayPool::mapTx, and CTxRelayPoolEntry bookkeeping:
 *
 * mapTx is a boost::multi_index that sorts the relaypool on these criteria:
 * - transaction hash (txid)
 * - witness-transaction hash (wtxid)
 * - time in relaypool
 * - surplus-work rate
 *
 * Note: the term "descendant" refers to in-relaypool transactions that depend on
 * this one, while "ancestor" refers to in-relaypool transactions that a given
 * transaction depends on.
 *
 * To keep ancestor and descendant state correct, we update transactions in the
 * relaypool when new descendants arrive.  To facilitate this, we track the set of
 * in-relaypool direct parents and direct children in mapLinks. Within each
 * CTxRelayPoolEntry, we track the size of all descendants.
 *
 * Usually when a new transaction is added to the relaypool, it has no in-relaypool
 * children (because any such children would be an orphan).  So in
 * addNewTransaction(), we:
 * - update a new entry's m_parents to include all in-relaypool parents
 * - update each of those parent entries to include the new tx as a child
 * - update all ancestors of the transaction to include the new tx's size
 *
 * When a transaction is removed from the relaypool, we must:
 * - update all in-relaypool parents to not track the tx in their m_children
 * - update all ancestors to not include the tx's size in descendant state
 * - update all in-relaypool children to not include it as a parent
 *
 * These happen in UpdateForRemoveFromRelayPool().  (Note that when removing a
 * transaction along with its descendants, we must calculate that set of
 * transactions to be removed before doing the removal, or else the relaypool can
 * be in an inconsistent state where it's impossible to walk the ancestors of
 * a transaction.)
 *
 * In the event of a reorg, the assumption that a newly added tx has no
 * in-relaypool children is false.  In particular, the relaypool is in an
 * inconsistent state while new transactions are being added, because there may
 * be descendant transactions of a tx coming from a disconnected block that are
 * unreachable from just looking at transactions in the relaypool (the linking
 * transactions may also be in the disconnected block, waiting to be added).
 * Because of this, there's not much benefit in trying to search for in-relaypool
 * children in addNewTransaction().  Instead, in the special case of transactions
 * being added from a disconnected block, we require the caller to clean up the
 * state, to account for in-relaypool, out-of-block descendants for all the
 * in-block transactions by calling UpdateTransactionsFromBlock().  Note that
 * until this is called, the relaypool state is not consistent, and in particular
 * mapLinks may not be correct (and therefore functions like
 * CalculateRelayPoolAncestors() and CalculateDescendants() that rely
 * on them to walk the relaypool are not generally safe to use).
 *
 * Computational limits:
 *
 * Updating all in-relaypool ancestors of a newly added transaction can be slow,
 * if no bound exists on how many in-relaypool ancestors there may be.
 * CalculateRelayPoolAncestors() takes configurable limits that are designed to
 * prevent these calculations from being too CPU intensive.
 *
 */
class CTxRelayPool
{
protected:
    std::atomic<unsigned int> nTransactionsUpdated{0}; //!< Used by getblocktemplate to trigger CreateNewBlock() invocation

    uint64_t totalTxSize GUARDED_BY(cs){0};      //!< sum of all relaypool tx's virtual sizes. Differs from serialized tx size since witness data is discounted. Defined in BIP 141.
    uint64_t cachedInnerUsage GUARDED_BY(cs){0}; //!< sum of dynamic memory usage of all the map elements (NOT the maps themselves)

    mutable Epoch m_epoch GUARDED_BY(cs){};

    // In-memory counter for external relaypool tracking purposes.
    // This number is incremented once every time a transaction
    // is added or removed from the relaypool for any reason.
    mutable uint64_t m_sequence_number GUARDED_BY(cs){1};

    bool m_load_tried GUARDED_BY(cs){false};


public:

    // The index list is spelled out inline, rather than pulled up into a named
    // struct that derives from indexed_by<>. Boost 1.92 turned indexed_by into a
    // forward declaration only -- an Mp11 type list with no definition -- so
    // deriving from it no longer compiles. Naming it bought nothing but shorter
    // symbols; do not reintroduce the struct.
    using indexed_transaction_set = boost::multi_index_container<
        CTxRelayPoolEntry,
        boost::multi_index::indexed_by<
            // sorted by txid
            boost::multi_index::hashed_unique<relaypoolentry_txid, SaltedTxidHasher>,
            // sorted by wtxid
            boost::multi_index::hashed_unique<
                boost::multi_index::tag<index_by_wtxid>,
                relaypoolentry_wtxid,
                SaltedTxidHasher
            >,
            // sorted by entry time
            boost::multi_index::ordered_non_unique<
                boost::multi_index::tag<entry_time>,
                boost::multi_index::identity<CTxRelayPoolEntry>,
                CompareTxRelayPoolEntryByEntryTime
            >,
            // #5c-2: sorted by surplus per-tx work rate (descending: begin() = highest)
            boost::multi_index::ordered_non_unique<
                boost::multi_index::tag<txwork_rate>,
                boost::multi_index::identity<CTxRelayPoolEntry>,
                CompareTxRelayPoolEntryByTxWorkRate
            >
        >
    >;

    /**
     * This mutex needs to be locked when accessing `mapTx` or other members
     * that are guarded by it.
     *
     * @par Consistency guarantees
     * By design, it is guaranteed that:
     * 1. Locking both `cs_main` and `relaypool.cs` will give a view of relaypool
     *    that is consistent with current chain tip (`ActiveChain()` and
     *    `CoinsTip()`) and is fully populated. Fully populated means that if the
     *    current active chain is missing transactions that were present in a
     *    previously active chain, all the missing transactions will have been
     *    re-added to the relaypool and should be present if they meet size and
     *    consistency constraints.
     * 2. Locking `relaypool.cs` without `cs_main` will give a view of a relaypool
     *    consistent with some chain that was active since `cs_main` was last
     *    locked, and that is fully populated as described above. It is ok for
     *    code that only needs to query or remove transactions from the relaypool
     *    to lock just `relaypool.cs` without `cs_main`.
     *
     * To provide these guarantees, it is necessary to lock both `cs_main` and
     * `relaypool.cs` whenever adding transactions to the relaypool and whenever
     * changing the chain tip. It's necessary to keep both mutexes locked until
     * the relaypool is consistent with the new chain tip and fully populated.
     */
    mutable RecursiveMutex cs;
    indexed_transaction_set mapTx GUARDED_BY(cs);

    using txiter = indexed_transaction_set::nth_index<0>::type::const_iterator;
    std::vector<CTransactionRef> txns_randomized GUARDED_BY(cs); //!< All transactions in mapTx, in random order

    typedef std::set<txiter, CompareIteratorByHash> setEntries;

    using Limits = kernel::RelayPoolLimits;

    uint64_t CalculateDescendantMaximum(txiter entry) const EXCLUSIVE_LOCKS_REQUIRED(cs);
private:
    typedef std::map<txiter, setEntries, CompareIteratorByHash> cacheMap;


    void UpdateParent(txiter entry, txiter parent, bool add) EXCLUSIVE_LOCKS_REQUIRED(cs);
    void UpdateChild(txiter entry, txiter child, bool add) EXCLUSIVE_LOCKS_REQUIRED(cs);

    std::vector<indexed_transaction_set::const_iterator> GetSortedDepthAndScore() const EXCLUSIVE_LOCKS_REQUIRED(cs);

    /**
     * Track locally submitted transactions to periodically retry initial broadcast.
     */
    std::set<uint256> m_unbroadcast_txids GUARDED_BY(cs);


    /**
     * Helper function to calculate all in-relaypool ancestors of staged_ancestors and apply ancestor
     * and descendant limits (including staged_ancestors themselves, entry_size and entry_count).
     *
     * @param[in]   entry_size          Virtual size to include in the limits.
     * @param[in]   entry_count         How many entries to include in the limits.
     * @param[in]   staged_ancestors    Should contain entries in the relaypool.
     * @param[in]   limits              Maximum number and size of ancestors and descendants
     *
     * @return all in-relaypool ancestors, or an error if any ancestor or descendant limits were hit
     */
    util::Result<setEntries> CalculateAncestorsAndCheckLimits(int64_t entry_size,
                                                              size_t entry_count,
                                                              CTxRelayPoolEntry::Parents &staged_ancestors,
                                                              const Limits& limits
                                                              ) const EXCLUSIVE_LOCKS_REQUIRED(cs);

public:
    indirectmap<COutPoint, const CTransaction*> mapNextTx GUARDED_BY(cs);
    using Options = kernel::RelayPoolOptions;

    const Options m_opts;

    /** Create a new CTxRelayPool.
     * Sanity checks will be off by default for performance, because otherwise
     * accepting transactions becomes O(N^2) where N is the number of transactions
     * in the pool.
     */
    explicit CTxRelayPool(Options opts, bilingual_str& error);

    /**
     * If sanity-checking is turned on, check makes sure the pool is
     * consistent (does not contain two transactions that spend the same inputs,
     * all inputs are in the mapNextTx array). If sanity-checking is turned off,
     * check does nothing.
     */
    void check(const CCoinsViewCache& active_coins_tip, int64_t spendheight) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);


    void removeRecursive(const CTransaction& tx, RelayPoolRemovalReason reason) EXCLUSIVE_LOCKS_REQUIRED(cs);
    /** After reorg, filter the entries that would no longer be valid in the next block, and update
     * the entries' cached LockPoints if needed.  The relaypool does not have any knowledge of
     * consensus rules. It just applies the callable function and removes the ones for which it
     * returns true.
     * @param[in]   filter_final_and_mature   Predicate that checks the relevant validation rules
     *                                        and updates an entry's LockPoints.
     * */
    void removeForReorg(CChain& chain, std::function<bool(txiter)> filter_final_and_mature) EXCLUSIVE_LOCKS_REQUIRED(cs, cs_main);
    /** Evict txs whose per-tx PoW anchor has aged past nMaxAnchorAge (permanently unmineable),
     *  cascading to descendants. Called after a block connects. */
    void removeStaleAnchors(int tip_height, const Consensus::Params& params) EXCLUSIVE_LOCKS_REQUIRED(cs);
    void removeConflicts(const CTransaction& tx) EXCLUSIVE_LOCKS_REQUIRED(cs);
    void removeForBlock(const std::vector<CTransactionRef>& vtx) EXCLUSIVE_LOCKS_REQUIRED(cs);

    bool CompareDepthAndScore(const uint256& hasha, const uint256& hashb, bool wtxid=false);
    bool isSpent(const COutPoint& outpoint) const;
    unsigned int GetTransactionsUpdated() const;
    void AddTransactionsUpdated(unsigned int n);
    /** Get the transaction in the pool that spends the same prevout */
    const CTransaction* GetConflictTx(const COutPoint& prevout) const EXCLUSIVE_LOCKS_REQUIRED(cs);

    /** Returns an iterator to the given hash, if found */
    std::optional<txiter> GetIter(const uint256& txid) const EXCLUSIVE_LOCKS_REQUIRED(cs);

    /** Translate a set of hashes into a set of pool iterators to avoid repeated lookups.
     * Does not require that all of the hashes correspond to actual transactions in the relaypool,
     * only returns the ones that exist. */
    setEntries GetIterSet(const std::set<Txid>& hashes) const EXCLUSIVE_LOCKS_REQUIRED(cs);

    /** Translate a list of hashes into a list of relaypool iterators to avoid repeated lookups.
     * The nth element in txids becomes the nth element in the returned vector. If any of the txids
     * don't actually exist in the relaypool, returns an empty vector. */
    std::vector<txiter> GetIterVec(const std::vector<uint256>& txids) const EXCLUSIVE_LOCKS_REQUIRED(cs);

    /** UpdateTransactionsFromBlock is called when adding transactions from a
     * disconnected block back to the relaypool, new relaypool entries may have
     * children in the relaypool (which is generally not the case when otherwise
     * adding transactions).
     *  @post updated descendant state for descendants of each transaction in
     *        vHashesToUpdate (excluding any child transactions present in
     *        vHashesToUpdate, which are already accounted for). Updated state
     *        includes adding size information for such descendants to the
     *        parent and updated ancestor state to include the parent.
     *
     * @param[in] vHashesToUpdate          The set of txids from the
     *     disconnected block that have been accepted back into the relaypool.
     */
    void UpdateTransactionsFromBlock(const std::vector<uint256>& vHashesToUpdate) EXCLUSIVE_LOCKS_REQUIRED(cs, cs_main) LOCKS_EXCLUDED(m_epoch);

    /**
     * Try to calculate all in-relaypool ancestors of entry.
     * (these are all calculated including the tx itself)
     *
     * @param[in]   entry               CTxRelayPoolEntry of which all in-relaypool ancestors are calculated
     * @param[in]   limits              Maximum number and size of ancestors and descendants
     * @param[in]   fSearchForParents   Whether to search a tx's vin for in-relaypool parents, or look
     *                                  up parents from mapLinks. Must be true for entries not in
     *                                  the relaypool
     *
     * @return all in-relaypool ancestors, or an error if any ancestor or descendant limits were hit
     */
    util::Result<setEntries> CalculateRelayPoolAncestors(const CTxRelayPoolEntry& entry,
                                   const Limits& limits,
                                   bool fSearchForParents = true) const EXCLUSIVE_LOCKS_REQUIRED(cs);

    /**
     * Same as CalculateRelayPoolAncestors, but always returns a (non-optional) setEntries.
     * Should only be used when it is assumed CalculateRelayPoolAncestors would not fail. If
     * CalculateRelayPoolAncestors does unexpectedly fail, an empty setEntries is returned and the
     * error is logged to HgLog::RELAYPOOL with level HgLog::Level::Error. In debug builds, failure
     * of CalculateRelayPoolAncestors will lead to shutdown due to assertion failure.
     *
     * @param[in]   calling_fn_name     Name of calling function so we can properly log the call site
     *
     * @return a setEntries corresponding to the result of CalculateRelayPoolAncestors or an empty
     *         setEntries if it failed
     *
     * @see CTXRelayPool::CalculateRelayPoolAncestors()
     */
    setEntries AssumeCalculateRelayPoolAncestors(
        std::string_view calling_fn_name,
        const CTxRelayPoolEntry &entry,
        const Limits& limits,
        bool fSearchForParents = true) const EXCLUSIVE_LOCKS_REQUIRED(cs);

    /** Collect the entire cluster of connected transactions for each transaction in txids.
     * All txids must correspond to transaction entries in the relaypool, otherwise this returns an
     * empty vector. This call will also exit early and return an empty vector if it collects 500 or
     * more transactions as a DoS protection. */
    std::vector<txiter> GatherClusters(const std::vector<uint256>& txids) const EXCLUSIVE_LOCKS_REQUIRED(cs);

    /** Calculate all in-relaypool ancestors of a set of transactions not already in the relaypool and
     * check ancestor and descendant limits. Heuristics are used to estimate the ancestor and
     * descendant count of all entries if the package were to be added to the relaypool.  The limits
     * are applied to the union of all package transactions. For example, if the package has 3
     * transactions and limits.ancestor_count = 25, the union of all 3 sets of ancestors (including the
     * transactions themselves) must be <= 22.
     * @param[in]       package                 Transaction package being evaluated for acceptance
     *                                          to relaypool. The transactions need not be direct
     *                                          ancestors/descendants of each other.
     * @param[in]       total_vsize             Sum of virtual sizes for all transactions in package.
     * @returns {} or the error reason if a limit is hit.
     */
    util::Result<void> CheckPackageLimits(const Package& package,
                                          int64_t total_vsize) const EXCLUSIVE_LOCKS_REQUIRED(cs);

    /** Populate setDescendants with all in-relaypool descendants of hash.
     *  Assumes that setDescendants includes all in-relaypool descendants of anything
     *  already in it.  */
    void CalculateDescendants(txiter it, setEntries& setDescendants) const EXCLUSIVE_LOCKS_REQUIRED(cs);


    /** Remove transactions from the relaypool until its dynamic size is <= sizelimit.
      *  pvNoSpendsRemaining, if set, will be populated with the list of outpoints
      *  which are not in relaypool which no longer have any spends in this relaypool.
      */
    void TrimToSize(size_t sizelimit, std::vector<COutPoint>* pvNoSpendsRemaining = nullptr) EXCLUSIVE_LOCKS_REQUIRED(cs);

    /** Expire all transaction (and their dependencies) in the relaypool older than time. Return the number of removed transactions. */
    int Expire(std::chrono::seconds time) EXCLUSIVE_LOCKS_REQUIRED(cs);

    /**
     * Calculate the ancestor and descendant count for the given transaction.
     * The counts include the transaction itself.
     * When ancestors is non-zero (ie, the transaction itself is in the relaypool),
     * ancestorsize will also be set to the appropriate value.
     */
    void GetTransactionAncestry(const uint256& txid, size_t& ancestors, size_t& descendants, size_t* ancestorsize = nullptr) const;

    /**
     * @returns true if an initial attempt to load the persisted relaypool was made, regardless of
     *          whether the attempt was successful or not
     */
    bool GetLoadTried() const;

    /**
     * Set whether or not an initial attempt to load the persisted relaypool was made (regardless
     * of whether the attempt was successful or not)
     */
    void SetLoadTried(bool load_tried);

    unsigned long size() const
    {
        LOCK(cs);
        return mapTx.size();
    }

    uint64_t GetTotalTxSize() const EXCLUSIVE_LOCKS_REQUIRED(cs)
    {
        AssertLockHeld(cs);
        return totalTxSize;
    }

    bool exists(const GenTxid& gtxid) const
    {
        LOCK(cs);
        if (gtxid.IsWtxid()) {
            return (mapTx.get<index_by_wtxid>().count(gtxid.GetHash()) != 0);
        }
        return (mapTx.count(gtxid.GetHash()) != 0);
    }

    const CTxRelayPoolEntry* GetEntry(const Txid& txid) const LIFETIMEBOUND EXCLUSIVE_LOCKS_REQUIRED(cs);

    CTransactionRef get(const uint256& hash) const;
    txiter get_iter_from_wtxid(const uint256& wtxid) const EXCLUSIVE_LOCKS_REQUIRED(cs)
    {
        AssertLockHeld(cs);
        return mapTx.project<0>(mapTx.get<index_by_wtxid>().find(wtxid));
    }
    TxRelayPoolInfo info(const GenTxid& gtxid) const;

    /** Returns info for a transaction if its entry_sequence < last_sequence */
    TxRelayPoolInfo info_for_relay(const GenTxid& gtxid, uint64_t last_sequence) const;

    std::vector<CTxRelayPoolEntryRef> entryAll() const EXCLUSIVE_LOCKS_REQUIRED(cs);
    std::vector<TxRelayPoolInfo> infoAll() const;

    size_t DynamicMemoryUsage() const;

    /** Adds a transaction to the unbroadcast set */
    void AddUnbroadcastTx(const uint256& txid)
    {
        LOCK(cs);
        // Sanity check the transaction is in the relaypool & insert into
        // unbroadcast set.
        if (exists(GenTxid::Txid(txid))) m_unbroadcast_txids.insert(txid);
    };

    /** Removes a transaction from the unbroadcast set */
    void RemoveUnbroadcastTx(const uint256& txid, const bool unchecked = false);

    /** Returns transactions in unbroadcast set */
    std::set<uint256> GetUnbroadcastTxs() const
    {
        LOCK(cs);
        return m_unbroadcast_txids;
    }

    /** Returns whether a txid is in the unbroadcast set */
    bool IsUnbroadcastTx(const uint256& txid) const EXCLUSIVE_LOCKS_REQUIRED(cs)
    {
        AssertLockHeld(cs);
        return m_unbroadcast_txids.count(txid) != 0;
    }

    /** Guards this internal counter for external reporting */
    uint64_t GetAndIncrementSequence() const EXCLUSIVE_LOCKS_REQUIRED(cs) {
        return m_sequence_number++;
    }

    uint64_t GetSequence() const EXCLUSIVE_LOCKS_REQUIRED(cs) {
        return m_sequence_number;
    }

private:
    /** Remove a set of transactions from the relaypool.
     *  If a transaction is in this set, then all in-relaypool descendants must
     *  also be in the set, unless this transaction is being removed for being
     *  in a block.
     *  Set updateDescendants to true when removing a tx that was in a block, so
     *  that any in-relaypool descendants have their ancestor state updated.
     */
    void RemoveStaged(setEntries& stage, bool updateDescendants, RelayPoolRemovalReason reason) EXCLUSIVE_LOCKS_REQUIRED(cs);

    /** UpdateForDescendants is used by UpdateTransactionsFromBlock to update
     *  the descendants for a single transaction that has been added to the
     *  relaypool but may have child transactions in the relaypool, eg during a
     *  chain reorg.
     *
     * @pre CTxRelayPoolEntry::m_children is correct for the given tx and all
     *      descendants.
     * @pre cachedDescendants is an accurate cache where each entry has all
     *      descendants of the corresponding key, including those that should
     *      be removed for violation of ancestor limits.
     * @post if updateIt has any non-excluded descendants, cachedDescendants has
     *       a new cache line for updateIt.
     * @post descendants_to_remove has a new entry for any descendant which exceeded
     *       ancestor limits relative to updateIt.
     *
     * @param[in] updateIt the entry to update for its descendants
     * @param[in,out] cachedDescendants a cache where each line corresponds to all
     *     descendants. It will be updated with the descendants of the transaction
     *     being updated, so that future invocations don't need to walk the same
     *     transaction again, if encountered in another transaction chain.
     * @param[in] setExclude the set of descendant transactions in the relaypool
     *     that must not be accounted for (because any descendants in setExclude
     *     were added to the relaypool after the transaction being updated and hence
     *     their state is already reflected in the parent state).
     * @param[out] descendants_to_remove Populated with the txids of entries that
     *     exceed ancestor limits. It's the responsibility of the caller to
     *     removeRecursive them.
     */
    void UpdateForDescendants(txiter updateIt, cacheMap& cachedDescendants,
                              const std::set<uint256>& setExclude, std::set<uint256>& descendants_to_remove) EXCLUSIVE_LOCKS_REQUIRED(cs);
    /** Update ancestors of hash to add/remove it as a descendant transaction. */
    void UpdateAncestorsOf(bool add, txiter hash, setEntries &setAncestors) EXCLUSIVE_LOCKS_REQUIRED(cs);
    /** Set ancestor state for an entry */
    void UpdateEntryForAncestors(txiter it, const setEntries &setAncestors) EXCLUSIVE_LOCKS_REQUIRED(cs);
    /** For each transaction being removed, update ancestors and any direct children.
      * If updateDescendants is true, then also update in-relaypool descendants'
      * ancestor state. */
    void UpdateForRemoveFromRelayPool(const setEntries &entriesToRemove, bool updateDescendants) EXCLUSIVE_LOCKS_REQUIRED(cs);
    /** Sever link between specified transaction and direct children. */
    void UpdateChildrenForRemoval(txiter entry) EXCLUSIVE_LOCKS_REQUIRED(cs);

    /** Before calling removeUnchecked for a given transaction,
     *  UpdateForRemoveFromRelayPool must be called on the entire (dependent) set
     *  of transactions being removed at the same time.  We use each
     *  CTxRelayPoolEntry's m_parents in order to walk ancestors of a
     *  given transaction that is removed, so we can't remove intermediate
     *  transactions in a chain before we've updated all the state for the
     *  removal.
     */
    void removeUnchecked(txiter entry, RelayPoolRemovalReason reason) EXCLUSIVE_LOCKS_REQUIRED(cs);
public:
    /** visited marks a CTxRelayPoolEntry as having been traversed
     * during the lifetime of the most recently created Epoch::Guard
     * and returns false if we are the first visitor, true otherwise.
     *
     * An Epoch::Guard must be held when visited is called or an assert will be
     * triggered.
     *
     */
    bool visited(const txiter it) const EXCLUSIVE_LOCKS_REQUIRED(cs, m_epoch)
    {
        return m_epoch.visited(it->m_epoch_marker);
    }

    bool visited(std::optional<txiter> it) const EXCLUSIVE_LOCKS_REQUIRED(cs, m_epoch)
    {
        assert(m_epoch.guarded()); // verify guard even when it==nullopt
        return !it || visited(*it);
    }

    /*
     * CTxRelayPool::ChangeSet:
     *
     * This class is used for all relaypool additions and associated removals (eg
     * due to replacement). Removals that don't need to be evaluated for acceptance,
     * such as removing transactions that appear in a block, or due to reorg,
     * or removals related to relaypool limiting or expiry do not need to use
     * this.
     *
     * Callers can interleave calls to StageAddition()/StageRemoval(), and
     * removals may be invoked in any order, but additions must be done in a
     * topological order in the case of transaction packages (ie, parents must
     * be added before children).
     *
     * CalculateRelayPoolAncestors() calculates the in-relaypool (not including
     * what is in the change set itself) ancestors of a given transaction.
     *
     * Apply() will apply the removals and additions that are staged into the
     * relaypool.
     *
     * Only one changeset may exist at a time. While a changeset is
     * outstanding, no removals or additions may be made directly to the
     * relaypool.
     */
    class ChangeSet {
    public:
        explicit ChangeSet(CTxRelayPool* pool) : m_pool(pool) {}
        ~ChangeSet() EXCLUSIVE_LOCKS_REQUIRED(m_pool->cs) { m_pool->m_have_changeset = false; }

        ChangeSet(const ChangeSet&) = delete;
        ChangeSet& operator=(const ChangeSet&) = delete;

        using TxHandle = CTxRelayPool::txiter;

        TxHandle StageAddition(const CTransactionRef& tx, int64_t time, unsigned int entry_height, uint64_t entry_sequence, bool spends_coinbase, int64_t sigops_cost, LockPoints lp, arith_uint256 txwork_surplus = arith_uint256(0));
        void StageRemoval(CTxRelayPool::txiter it) { m_to_remove.insert(it); }

        const CTxRelayPool::setEntries& GetRemovals() const { return m_to_remove; }

        util::Result<CTxRelayPool::setEntries> CalculateRelayPoolAncestors(TxHandle tx, const Limits& limits)
        {
            // Look up transaction in our cache first
            auto it = m_ancestors.find(tx);
            if (it != m_ancestors.end()) return it->second;

            // If not found, try to have the relaypool calculate it, and cache
            // for later.
            LOCK(m_pool->cs);
            auto ret{m_pool->CalculateRelayPoolAncestors(*tx, limits)};
            if (ret) m_ancestors.try_emplace(tx, *ret);
            return ret;
        }

        std::vector<CTransactionRef> GetAddedTxns() const {
            std::vector<CTransactionRef> ret;
            ret.reserve(m_entry_vec.size());
            for (const auto& entry : m_entry_vec) {
                ret.emplace_back(entry->GetSharedTx());
            }
            return ret;
        }

        size_t GetTxCount() const { return m_entry_vec.size(); }
        const CTransaction& GetAddedTxn(size_t index) const { return m_entry_vec.at(index)->GetTx(); }

        void Apply() EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    private:
        CTxRelayPool* m_pool;
        CTxRelayPool::indexed_transaction_set m_to_add;
        std::vector<CTxRelayPool::txiter> m_entry_vec; // track the added transactions' insertion order
        // map from the m_to_add index to the ancestors for the transaction
        std::map<CTxRelayPool::txiter, CTxRelayPool::setEntries, CompareIteratorByHash> m_ancestors;
        CTxRelayPool::setEntries m_to_remove;

        friend class CTxRelayPool;
    };

    std::unique_ptr<ChangeSet> GetChangeSet() EXCLUSIVE_LOCKS_REQUIRED(cs) {
        Assume(!m_have_changeset);
        m_have_changeset = true;
        return std::make_unique<ChangeSet>(this);
    }

    bool m_have_changeset GUARDED_BY(cs){false};

    friend class CTxRelayPool::ChangeSet;

private:
    // Apply the given changeset to the relaypool, by removing transactions in
    // the to_remove set and adding transactions in the to_add set.
    void Apply(CTxRelayPool::ChangeSet* changeset) EXCLUSIVE_LOCKS_REQUIRED(cs);

    // addNewTransaction must update state for all ancestors of a given transaction,
    // to track size/count of descendant transactions.  First version of
    // addNewTransaction can be used to have it call CalculateRelayPoolAncestors(), and
    // then invoke the second version.
    // Note that addNewTransaction is ONLY called (via Apply()) from ATMP
    // outside of tests and any other callers may break vault's in-relaypool
    // tracking (due to lack of CValidationInterface::TransactionAddedToRelayPool
    // callbacks).
    void addNewTransaction(CTxRelayPool::txiter it) EXCLUSIVE_LOCKS_REQUIRED(cs);
    void addNewTransaction(CTxRelayPool::txiter it, CTxRelayPool::setEntries& setAncestors) EXCLUSIVE_LOCKS_REQUIRED(cs);
};

/**
 * CCoinsView that brings transactions from a relaypool into view.
 * It does not check for spendings by memory pool transactions.
 * Instead, it provides access to all Coins which are either unspent in the
 * base CCoinsView, are outputs from any relaypool transaction, or are
 * tracked temporarily to allow transaction dependencies in package validation.
 * This allows transaction replacement to work as expected, as you want to
 * have all inputs "available" to check signatures, and any cycles in the
 * dependency graph are checked directly in AcceptToMemoryPool.
 * It also allows you to sign a double-spend directly in
 * signrawtransactionwithkey and signrawtransactionwithvault,
 * as long as the conflicting transaction is not yet confirmed.
 */
class CCoinsViewRelayPool : public CCoinsViewBacked
{
    /**
    * Coins made available by transactions being validated. Tracking these allows for package
    * validation, since we can access transaction outputs without submitting them to relaypool.
    */
    std::unordered_map<COutPoint, Coin, SaltedOutpointHasher> m_temp_added;

    /**
     * Set of all coins that have been fetched from relaypool or created using PackageAddTransaction
     * (not base). Used to track the origin of a coin, see GetNonBaseCoins().
     */
    mutable std::unordered_set<COutPoint, SaltedOutpointHasher> m_non_base_coins;
protected:
    const CTxRelayPool& relaypool;

public:
    CCoinsViewRelayPool(CCoinsView* baseIn, const CTxRelayPool& relaypoolIn);
    /** GetCoin, returning whether it exists and is not spent. Also updates m_non_base_coins if the
     * coin is not fetched from base. */
    std::optional<Coin> GetCoin(const COutPoint& outpoint) const override;
    /** Add the coins created by this transaction. These coins are only temporarily stored in
     * m_temp_added and cannot be flushed to the back end. Only used for package validation. */
    void PackageAddTransaction(const CTransactionRef& tx);
    /** Get all coins in m_non_base_coins. */
    std::unordered_set<COutPoint, SaltedOutpointHasher> GetNonBaseCoins() const { return m_non_base_coins; }
    /** Clear m_temp_added and m_non_base_coins. */
    void Reset();
};
#endif // QUICKSILVER_TXRELAYPOOL_H
