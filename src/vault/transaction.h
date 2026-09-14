// Copyright (c) 2021-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_VAULT_TRANSACTION_H
#define QUICKSILVER_VAULT_TRANSACTION_H

#include <attributes.h>
#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/overloaded.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <vault/types.h>

#include <bitset>
#include <cassert>
#include <cstdint>
#include <map>
#include <utility>
#include <variant>
#include <vector>

namespace interfaces {
class Chain;
} // namespace interfaces

namespace vault {
//! State of transaction confirmed in a block.
struct TxStateConfirmed {
    uint256 confirmed_block_hash;
    int confirmed_block_height;
    int position_in_block;

    explicit TxStateConfirmed(const uint256& block_hash, int height, int index) : confirmed_block_hash(block_hash), confirmed_block_height(height), position_in_block(index)
    {
        assert(height >= 0);
        assert(index >= 0);
    }
    std::string toString() const { return strprintf("Confirmed (block=%s, height=%i, index=%i)", confirmed_block_hash.ToString(), confirmed_block_height, position_in_block); }
};

//! State of transaction added to relaypool.
struct TxStateInRelayPool {
    std::string toString() const { return strprintf("InRelayPool"); }
};

//! State of rejected transaction that conflicts with a confirmed block.
struct TxStateBlockConflicted {
    uint256 conflicting_block_hash;
    int conflicting_block_height;

    explicit TxStateBlockConflicted(const uint256& block_hash, int height) : conflicting_block_hash(block_hash), conflicting_block_height(height)
    {
        assert(height >= 0);
    }
    std::string toString() const { return strprintf("BlockConflicted (block=%s, height=%i)", conflicting_block_hash.ToString(), conflicting_block_height); }
};

//! State of transaction not confirmed or conflicting with a known block and
//! not in the relaypool. May conflict with the relaypool, or with an unknown block,
//! or be abandoned, never broadcast, or rejected from the relaypool for another
//! reason.
struct TxStateInactive {
    bool abandoned;

    explicit TxStateInactive(bool abandoned = false) : abandoned(abandoned) {}
    std::string toString() const { return strprintf("Inactive (abandoned=%i)", abandoned); }
};

//! A block reference read from disk but not yet checked against an active
//! chain. The on-disk format stores a hash and position, but no height, so this
//! state must be resolved before it can become confirmed or conflicted.
struct TxStateBlockUnresolved {
    uint256 block_hash;
    int index;

    TxStateBlockUnresolved(const uint256& block_hash, int index) : block_hash(block_hash), index(index)
    {
        assert(index >= -1);
    }
    std::string toString() const { return strprintf("BlockUnresolved (block=%s, index=%i)", block_hash.ToString(), index); }
};

//! State of transaction loaded in an unrecognized state with unexpected hash or
//! index values. Treated as inactive (with serialized hash and index values
//! preserved) by default, but may enter another state if transaction is added
//! to the relaypool, or confirmed, or abandoned, or found conflicting.
struct TxStateUnrecognized {
    uint256 block_hash;
    int index;

    TxStateUnrecognized(const uint256& block_hash, int index) : block_hash(block_hash), index(index) {}
    std::string toString() const { return strprintf("Unrecognized (block=%s, index=%i)", block_hash.ToString(), index); }
};

//! All possible CVaultTx states
using TxState = std::variant<TxStateConfirmed, TxStateInRelayPool, TxStateBlockConflicted, TxStateInactive, TxStateBlockUnresolved, TxStateUnrecognized>;

//! Subset of states transaction sync logic is implemented to handle.
using SyncTxState = std::variant<TxStateConfirmed, TxStateInRelayPool, TxStateInactive>;

//! Interpret serialized state without claiming a block is active before a
//! chain lookup supplies the height omitted by the disk format.
static inline TxState TxStateInterpretSerialized(TxStateUnrecognized data)
{
    if (data.block_hash == uint256::ZERO) {
        if (data.index == 0) return TxStateInactive{};
    } else if (data.block_hash == uint256::ONE) {
        if (data.index == -1) return TxStateInactive{/*abandoned=*/true};
    } else if (data.index >= -1) {
        return TxStateBlockUnresolved{data.block_hash, data.index};
    }
    return data;
}

//! Get TxState serialized block hash. Inverse of TxStateInterpretSerialized.
static inline uint256 TxStateSerializedBlockHash(const TxState& state)
{
    return std::visit(util::Overloaded{
        [](const TxStateInactive& inactive) { return inactive.abandoned ? uint256::ONE : uint256::ZERO; },
        [](const TxStateInRelayPool& in_relaypool) { return uint256::ZERO; },
        [](const TxStateConfirmed& confirmed) { return confirmed.confirmed_block_hash; },
        [](const TxStateBlockConflicted& conflicted) { return conflicted.conflicting_block_hash; },
        [](const TxStateBlockUnresolved& unresolved) { return unresolved.block_hash; },
        [](const TxStateUnrecognized& unrecognized) { return unrecognized.block_hash; }
    }, state);
}

//! Get TxState serialized block index. Inverse of TxStateInterpretSerialized.
static inline int TxStateSerializedIndex(const TxState& state)
{
    return std::visit(util::Overloaded{
        [](const TxStateInactive& inactive) { return inactive.abandoned ? -1 : 0; },
        [](const TxStateInRelayPool& in_relaypool) { return 0; },
        [](const TxStateConfirmed& confirmed) { return confirmed.position_in_block; },
        [](const TxStateBlockConflicted& conflicted) { return -1; },
        [](const TxStateBlockUnresolved& unresolved) { return unresolved.index; },
        [](const TxStateUnrecognized& unrecognized) { return unrecognized.index; }
    }, state);
}

//! Return TxState or SyncTxState as a string for logging or debugging.
template<typename T>
std::string TxStateString(const T& state)
{
    return std::visit([](const auto& s) { return s.toString(); }, state);
}

/**
 * Amount cached per isminefilter combination.
 */
struct CachableAmount
{
    // The ISMINE_NO slot is never (supposed to be) cached
    std::bitset<ISMINE_ENUM_ELEMENTS> m_cached;
    CAmount m_value[ISMINE_ENUM_ELEMENTS];
    inline void Reset()
    {
        m_cached.reset();
    }
    void Set(isminefilter filter, CAmount value)
    {
        m_cached.set(filter);
        m_value[filter] = value;
    }
};


typedef std::map<std::string, std::string> mapValue_t;

/**
 * A transaction with additional info that only the owner cares about.
 */
class CVaultTx
{
public:
    /**
     * Key/value map with information about the transaction.
     *
     * The following keys can be read and written through the map and are
     * serialized in the vault database:
     *
     *     "comment", "to"   - comment strings provided to sendtoaddress,
     *                         and sendmany vault RPCs
     *
     * The following keys are serialized in the vault database, but shouldn't
     * be read or written through the map (they will be temporarily added and
     * removed from the map during serialization):
     *
     *     "n"               - serialized nOrderPos value
     *     "timesmart"       - serialized nTimeSmart value
     */
    mapValue_t mapValue;
    std::vector<std::pair<std::string, std::string> > vOrderForm;
    unsigned int nTimeReceived; //!< time received by this node
    /**
     * Stable timestamp that never changes, and reflects the order a transaction
     * was added to the vault. Timestamp is based on the block time for a
     * transaction added as part of a block, or else the time when the
     * transaction was received if it wasn't part of a block, with the timestamp
     * adjusted in both cases so timestamp order matches the order transactions
     * were added to the vault. More details can be found in
     * CVault::ComputeTimeSmart().
     */
    unsigned int nTimeSmart;
    /**
     * From me flag is set to 1 for transactions that were created by the vault
     * on this Quicksilver node, and set to 0 for transactions that were created
     * externally and came in through the network or sendrawtransaction RPC.
     */
    bool fFromMe;
    int64_t nOrderPos; //!< position in ordered transaction list
    std::multimap<int64_t, CVaultTx*>::const_iterator m_it_wtxOrdered;

    // memory only
    enum AmountType { DEBIT, CREDIT, IMMATURE_CREDIT, AVAILABLE_CREDIT, AMOUNTTYPE_ENUM_ELEMENTS };
    mutable CachableAmount m_amounts[AMOUNTTYPE_ENUM_ELEMENTS];
    /**
     * This flag is true if all m_amounts caches are empty. This is particularly
     * useful in places where MarkDirty is conditionally called and the
     * condition can be expensive and thus can be skipped if the flag is true.
     * See MarkDestinationsDirty.
     */
    mutable bool m_is_cache_empty{true};
    mutable bool fChangeCached;
    mutable CAmount nChangeCached;

    CVaultTx(CTransactionRef tx, const TxState& state) : tx(std::move(tx)), m_state(state)
    {
        Init();
    }

    void Init()
    {
        mapValue.clear();
        vOrderForm.clear();
        nTimeReceived = 0;
        nTimeSmart = 0;
        fFromMe = false;
        fChangeCached = false;
        nChangeCached = 0;
        nOrderPos = -1;
    }

    CTransactionRef tx;
    TxState m_state;

    // Set of relaypool transactions that conflict
    // directly with the transaction, or that conflict
    // with an ancestor transaction. This set will be
    // empty if state is InRelayPool or Confirmed, but
    // can be nonempty if state is Inactive or
    // BlockConflicted.
    std::set<Txid> relaypool_conflicts;

    template<typename Stream>
    void Serialize(Stream& s) const
    {
        mapValue_t mapValueCopy = mapValue;

        if (nOrderPos != -1) {
            mapValueCopy["n"] = util::ToString(nOrderPos);
        }
        if (nTimeSmart) {
            mapValueCopy["timesmart"] = strprintf("%u", nTimeSmart);
        }

        uint256 serializedHash = TxStateSerializedBlockHash(m_state);
        int serializedIndex = TxStateSerializedIndex(m_state);
        s << TX_WITH_WITNESS(tx) << serializedHash << serializedIndex << mapValueCopy << vOrderForm << nTimeReceived << fFromMe;
    }

    template<typename Stream>
    void Unserialize(Stream& s)
    {
        Init();

        uint256 serialized_block_hash;
        int serializedIndex;
        s >> TX_WITH_WITNESS(tx) >> serialized_block_hash >> serializedIndex >> mapValue >> vOrderForm >> nTimeReceived >> fFromMe;

        m_state = TxStateInterpretSerialized({serialized_block_hash, serializedIndex});

        const auto it_op = mapValue.find("n");
        nOrderPos = (it_op != mapValue.end()) ? LocaleIndependentAtoi<int64_t>(it_op->second) : -1;
        const auto it_ts = mapValue.find("timesmart");
        nTimeSmart = (it_ts != mapValue.end()) ? static_cast<unsigned int>(LocaleIndependentAtoi<int64_t>(it_ts->second)) : 0;

        mapValue.erase("n");
        mapValue.erase("timesmart");
    }

    void SetTx(CTransactionRef arg)
    {
        tx = std::move(arg);
    }

    //! make sure balances are recalculated
    void MarkDirty()
    {
        m_amounts[DEBIT].Reset();
        m_amounts[CREDIT].Reset();
        m_amounts[IMMATURE_CREDIT].Reset();
        m_amounts[AVAILABLE_CREDIT].Reset();
        fChangeCached = false;
        m_is_cache_empty = true;
    }

    /** True if only scriptSigs are different */
    bool IsEquivalentTo(const CVaultTx& tx) const;

    bool InRelayPool() const;

    int64_t GetTxTime() const;

    template<typename T> const T* state() const { return std::get_if<T>(&m_state); }
    template<typename T> T* state() { return std::get_if<T>(&m_state); }

    //! Validate stored block references against a chain, filling in heights
    //! only for blocks on the active chain.
    void updateState(interfaces::Chain& chain);

    bool isAbandoned() const { return state<TxStateInactive>() && state<TxStateInactive>()->abandoned; }
    bool isRelayPoolConflicted() const { return !relaypool_conflicts.empty(); }
    bool isBlockConflicted() const { return state<TxStateBlockConflicted>(); }
    bool isBlockUnresolved() const { return state<TxStateBlockUnresolved>(); }
    bool isInactive() const { return state<TxStateInactive>(); }
    bool isUnconfirmed() const { return !isAbandoned() && !isBlockConflicted() && !isBlockUnresolved() && !isRelayPoolConflicted() && !isConfirmed(); }
    bool isConfirmed() const { return state<TxStateConfirmed>(); }
    const Txid& GetHash() const LIFETIMEBOUND { return tx->GetHash(); }
    const Wtxid& GetWitnessHash() const LIFETIMEBOUND { return tx->GetWitnessHash(); }
    bool IsCoinBase() const { return tx->IsCoinBase(); }

private:
    // Disable copying of CVaultTx objects to prevent bugs where instances get
    // copied in and out of the mapVault map, and fields are updated in the
    // wrong copy.
    CVaultTx(const CVaultTx&) = default;
    CVaultTx& operator=(const CVaultTx&) = default;
};

struct VaultTxOrderComparator {
    bool operator()(const CVaultTx* a, const CVaultTx* b) const
    {
        return a->nOrderPos < b->nOrderPos;
    }
};
} // namespace vault

#endif // QUICKSILVER_VAULT_TRANSACTION_H
