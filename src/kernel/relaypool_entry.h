// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_KERNEL_RELAYPOOL_ENTRY_H
#define QUICKSILVER_KERNEL_RELAYPOOL_ENTRY_H

#include <arith_uint256.h>
#include <consensus/amount.h>
#include <consensus/validation.h>
#include <core_memusage.h>
#include <policy/policy.h>
#include <policy/settings.h>
#include <primitives/transaction.h>
#include <util/epochguard.h>
#include <util/overflow.h>

#include <chrono>
#include <functional>
#include <memory>
#include <set>
#include <stddef.h>
#include <stdint.h>

class CBlockIndex;

struct LockPoints {
    // Will be set to the blockchain height and median time past
    // values that would be necessary to satisfy all relative locktime
    // constraints (BIP68) of this tx given our view of block chain history
    int height{0};
    int64_t time{0};
    // As long as the current chain descends from the highest height block
    // containing one of the inputs used in the calculation, then the cached
    // values are still valid even after a reorg.
    CBlockIndex* maxInputBlock{nullptr};
};

struct CompareIteratorByHash {
    // SFINAE for T where T is either a pointer type (e.g., a txiter) or a reference_wrapper<T>
    // (e.g. a wrapped CTxRelayPoolEntry&)
    template <typename T>
    bool operator()(const std::reference_wrapper<T>& a, const std::reference_wrapper<T>& b) const
    {
        return a.get().GetTx().GetHash() < b.get().GetTx().GetHash();
    }
    template <typename T>
    bool operator()(const T& a, const T& b) const
    {
        return a->GetTx().GetHash() < b->GetTx().GetHash();
    }
};

/** \class CTxRelayPoolEntry
 *
 * CTxRelayPoolEntry stores data about the corresponding transaction, as well
 * as data about all in-relaypool transactions that depend on the transaction
 * ("descendant" transactions).
 *
 * When a new entry is added to the relaypool, we update the descendant state
 * (m_count_with_descendants and nSizeWithDescendants) for
 * all ancestors of the newly added transaction.
 *
 */

class CTxRelayPoolEntry
{
public:
    typedef std::reference_wrapper<const CTxRelayPoolEntry> CTxRelayPoolEntryRef;
    // two aliases, should the types ever diverge
    typedef std::set<CTxRelayPoolEntryRef, CompareIteratorByHash> Parents;
    typedef std::set<CTxRelayPoolEntryRef, CompareIteratorByHash> Children;

private:
    const CTransactionRef tx;
    mutable Parents m_parents;
    mutable Children m_children;
    const int32_t nTxWeight;         //!< ... and avoid recomputing tx weight (also used for GetTxSize())
    const size_t nUsageSize;        //!< ... and total memory usage
    const int64_t nTime;            //!< Local time when entering the relaypool
    const uint64_t entry_sequence;  //!< Sequence number used to determine whether this transaction is too recent for relay
    const unsigned int entryHeight; //!< Chain height when entering the relaypool
    const bool spendsCoinbase;      //!< keep track of transactions that spend a coinbase
    const int64_t sigOpCost;        //!< Total sigop cost
    mutable LockPoints lockPoints;  //!< Track the height and time at which tx was final
    const arith_uint256 m_txwork_surplus; //!< #5c-2 surplus per-tx work (actual - floor); the inclusion-ranking key

    // Information about descendants of this transaction that are in the
    // relaypool; if we remove this transaction we must remove all of these
    // descendants as well.
    int64_t m_count_with_descendants{1}; //!< number of descendant transactions
    // Using int64_t instead of int32_t to avoid signed integer overflow issues.
    int64_t nSizeWithDescendants;      //!< ... and size

    // Analogous statistics for ancestor transactions
    int64_t m_count_with_ancestors{1};
    // Using int64_t instead of int32_t to avoid signed integer overflow issues.
    int64_t nSizeWithAncestors;
    int64_t nSigOpCostWithAncestors;

public:
    CTxRelayPoolEntry(const CTransactionRef& tx,
                    int64_t time, unsigned int entry_height, uint64_t entry_sequence,
                    bool spends_coinbase,
                    int64_t sigops_cost, LockPoints lp,
                    arith_uint256 txwork_surplus = arith_uint256(0))
        : tx{tx},
          nTxWeight{GetTransactionWeight(*tx)},
          nUsageSize{RecursiveDynamicUsage(tx)},
          nTime{time},
          entry_sequence{entry_sequence},
          entryHeight{entry_height},
          spendsCoinbase{spends_coinbase},
          sigOpCost{sigops_cost},
          lockPoints{lp},
          m_txwork_surplus{txwork_surplus},
          nSizeWithDescendants{GetTxSize()},
          nSizeWithAncestors{GetTxSize()},
          nSigOpCostWithAncestors{sigOpCost} {}

    CTxRelayPoolEntry(const CTxRelayPoolEntry&) = delete;
    CTxRelayPoolEntry& operator=(const CTxRelayPoolEntry&) = delete;
    CTxRelayPoolEntry(CTxRelayPoolEntry&&) = delete;
    CTxRelayPoolEntry& operator=(CTxRelayPoolEntry&&) = delete;

    const CTransaction& GetTx() const { return *this->tx; }
    CTransactionRef GetSharedTx() const { return this->tx; }
    int32_t GetTxSize() const
    {
        return GetVirtualTransactionSize(nTxWeight, sigOpCost, ::nBytesPerSigOp);
    }
    int32_t GetTxWeight() const { return nTxWeight; }
    std::chrono::seconds GetTime() const { return std::chrono::seconds{nTime}; }
    unsigned int GetHeight() const { return entryHeight; }
    uint64_t GetSequence() const { return entry_sequence; }
    int64_t GetSigOpCost() const { return sigOpCost; }
    //! #5c-2 inclusion-ranking key: surplus per-tx work per vsize byte. Higher = included first.
    double GetTxWorkRate() const { return m_txwork_surplus.getdouble() / GetTxSize(); }
    const arith_uint256& GetTxWorkSurplusValue() const { return m_txwork_surplus; }
    size_t DynamicMemoryUsage() const { return nUsageSize; }
    const LockPoints& GetLockPoints() const { return lockPoints; }

    // Adjusts the descendant state.
    void UpdateDescendantState(int32_t modifySize, int64_t modifyCount);
    // Adjusts the ancestor state
    void UpdateAncestorState(int32_t modifySize, int64_t modifyCount, int64_t modifySigOps);

    // Update the LockPoints after a reorg
    void UpdateLockPoints(const LockPoints& lp) const
    {
        lockPoints = lp;
    }

    uint64_t GetCountWithDescendants() const { return m_count_with_descendants; }
    int64_t GetSizeWithDescendants() const { return nSizeWithDescendants; }

    bool GetSpendsCoinbase() const { return spendsCoinbase; }

    uint64_t GetCountWithAncestors() const { return m_count_with_ancestors; }
    int64_t GetSizeWithAncestors() const { return nSizeWithAncestors; }
    int64_t GetSigOpCostWithAncestors() const { return nSigOpCostWithAncestors; }

    const Parents& GetRelayPoolParentsConst() const { return m_parents; }
    const Children& GetRelayPoolChildrenConst() const { return m_children; }
    Parents& GetRelayPoolParents() const { return m_parents; }
    Children& GetRelayPoolChildren() const { return m_children; }

    mutable size_t idx_randomized; //!< Index in relaypool's txns_randomized
    mutable Epoch::Marker m_epoch_marker; //!< epoch when last touched, useful for graph algorithms
};

using CTxRelayPoolEntryRef = CTxRelayPoolEntry::CTxRelayPoolEntryRef;

#endif // QUICKSILVER_KERNEL_RELAYPOOL_ENTRY_H
