// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_VALIDATION_H
#define QUICKSILVER_VALIDATION_H

#include <arith_uint256.h>
#include <attributes.h>
#include <chain.h>
#include <checkqueue.h>
#include <consensus/amount.h>
#include <cuckoocache.h>
#include <deploymentstatus.h>
#include <kernel/chain.h>
#include <kernel/chainparams.h>
#include <kernel/chainstatemanager_opts.h>
#include <kernel/cs_main.h> // IWYU pragma: export
#include <node/blockstorage.h>
#include <policy/packages.h>
#include <policy/policy.h>
#include <script/script_error.h>
#include <script/sigcache.h>
#include <sync.h>
#include <txdb.h>
#include <txrelaypool.h> // For CTxRelayPool::cs
#include <uint256.h>
#include <util/check.h>
#include <util/fs.h>
#include <util/hasher.h>
#include <util/translation.h>
#include <versionbits.h>

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdint.h>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

class Chainstate;
class CTxRelayPool;
class ChainstateManager;
struct ChainTxData;
class DisconnectedBlockTransactions;
struct PrecomputedTransactionData;
struct LockPoints;
namespace Consensus {
struct Params;
} // namespace Consensus
namespace util {
class SignalInterrupt;
} // namespace util

/** Block files containing a block-height within MIN_BLOCKS_TO_KEEP of ActiveChain().Tip() will not be pruned. */
static const unsigned int MIN_BLOCKS_TO_KEEP = 288;
static const signed int DEFAULT_CHECKBLOCKS = 6;
static constexpr int DEFAULT_CHECKLEVEL{3};
// Require that user allocate at least 550 MiB for block & undo files (blk???.dat and rev???.dat)
// Re-derived for Quicksilver's parameters, which differ from upstream's on both terms:
// blocks are larger and arrive twice as often (300 s spacing), so the daily figure is not
// upstream's. MIN_BLOCKS_TO_KEEP is 288 blocks, which is 24 hours here rather than 48.
// At 1,217,399 bytes of block + undo data per block (M5 honest 2-output shape, measured in
// tools/calibration/m5-disk-cost), 288 blocks = 350.6MB. Undo is already included in that
// measurement, so it is not added again.
// Add 20% for Orphan block rate = 420.7MB
// We want the low water mark after pruning to be at least 420.7 MB and since we prune in
// full block file chunks, we need the high water mark which triggers the prune to be
// one 128MiB block file + added 15% undo data = 154.3MB greater for a total of 575.0MB
// Setting the target to >= 550 MiB (576.7 MB) will make it likely we can respect the target.
// NOTE: that leaves only 0.3% of headroom, against 5.5% upstream, and it holds only for the
// honest traffic shape — a sustained many-output period stores more per block. A pruned node
// at exactly the minimum may briefly exceed its target. See doc/design/chain-storage.md.
static const uint64_t MIN_DISK_SPACE_FOR_BLOCK_FILES = 550 * 1024 * 1024;

/** Maximum number of dedicated script-checking threads allowed */
static constexpr int MAX_SCRIPTCHECK_THREADS{15};

/** Current sync state passed to tip changed callbacks. */
enum class SynchronizationState {
    INIT_REINDEX,
    INIT_DOWNLOAD,
    POST_INIT
};

/** Documentation for argument 'checklevel'. */
extern const std::vector<std::string> CHECKLEVEL_DOC;

CAmount GetBlockSubsidy(int nHeight, const Consensus::Params& consensusParams);

bool FatalError(kernel::Notifications& notifications, BlockValidationState& state, const bilingual_str& message);

/** Prune block files up to a given height */
void PruneBlockFilesManual(Chainstate& active_chainstate, int nManualPruneHeight);

/**
* Validation result for a transaction evaluated by RelayPoolAccept (single or package).
* Here are the expected fields and properties of a result depending on its ResultType, applicable to
* results returned from package evaluation:
*+---------------------------+----------------+-------------------+------------------+----------------+-------------------+
*| Field or property         |    VALID       |                 INVALID              |  RELAYPOOL_ENTRY | DIFFERENT_WITNESS |
*|                           |                |--------------------------------------|                |                   |
*|                           |                | TX_RECONSIDERABLE |     Other        |                |                   |
*+---------------------------+----------------+-------------------+------------------+----------------+-------------------+
*| txid in relaypool?          | yes            | no                | no*              | yes            | yes               |
*| wtxid in relaypool?         | yes            | no                | no*              | yes            | no                |
*| m_state                   | yes, IsValid() | yes, IsInvalid()  | yes, IsInvalid() | yes, IsValid() | yes, IsValid()    |
*| m_vsize                   | yes            | no                | no               | yes            | no                |
*| m_package_wtxids          | yes            | no                | no               | no             | no                |
*| m_other_wtxid             | no             | no                | no               | no             | yes               |
*+---------------------------+----------------+-------------------+------------------+----------------+-------------------+
* (*) Individual transaction acceptance doesn't return RELAYPOOL_ENTRY and DIFFERENT_WITNESS. It returns
* INVALID, with the errors txn-already-in-relaypool and txn-same-nonwitness-data-in-relaypool
* respectively. In those cases, the txid or wtxid may be in the relaypool for a TX_CONFLICT.
*/
struct RelayPoolAcceptResult {
    /** Used to indicate the results of relaypool validation. */
    enum class ResultType {
        VALID, //!> Fully validated, valid.
        INVALID, //!> Invalid.
        RELAYPOOL_ENTRY, //!> Valid, transaction was already in the relaypool.
        DIFFERENT_WITNESS, //!> Not validated. A same-txid-different-witness tx (see m_other_wtxid) already exists in the relaypool and was not replaced.
    };
    /** Result type. Present in all RelayPoolAcceptResults. */
    const ResultType m_result_type;

    /** Contains information about why the transaction failed. */
    const TxValidationState m_state;

    /** RelayPool transactions replaced by the tx. */
    const std::list<CTransactionRef> m_replaced_transactions;
    /** Virtual size as used by the relaypool, calculated using serialized size and sigops. */
    const std::optional<int64_t> m_vsize;
    /** Contains the wtxids of the transactions used for package validation bookkeeping. Includes this
     * transaction's wtxid and may include others if this transaction was validated as part of a
     * package. This is not necessarily equivalent to the list of transactions passed to
     * ProcessNewPackage().
     * Only present when m_result_type = ResultType::VALID. */
    const std::optional<std::vector<Wtxid>> m_package_wtxids;

    /** The wtxid of the transaction in the relaypool which has the same txid but different witness. */
    const std::optional<Wtxid> m_other_wtxid;

    static RelayPoolAcceptResult Failure(TxValidationState state) {
        return RelayPoolAcceptResult(state);
    }

    static RelayPoolAcceptResult Success(std::list<CTransactionRef>&& replaced_txns,
                                       int64_t vsize,
                                       const std::vector<Wtxid>& package_wtxids) {
        return RelayPoolAcceptResult(std::move(replaced_txns), vsize, package_wtxids);
    }

    static RelayPoolAcceptResult RelayPoolTx(int64_t vsize) {
        return RelayPoolAcceptResult(vsize);
    }

    static RelayPoolAcceptResult RelayPoolTxDifferentWitness(const Wtxid& other_wtxid) {
        return RelayPoolAcceptResult(other_wtxid);
    }

// Private constructors. Use static methods RelayPoolAcceptResult::Success, etc. to construct.
private:
    /** Constructor for failure case */
    explicit RelayPoolAcceptResult(TxValidationState state)
        : m_result_type(ResultType::INVALID), m_state(state) {
            Assume(!state.IsValid()); // Can be invalid or error
        }

    /** Constructor for success case */
    explicit RelayPoolAcceptResult(std::list<CTransactionRef>&& replaced_txns,
                                 int64_t vsize,
                                 const std::vector<Wtxid>& package_wtxids)
        : m_result_type(ResultType::VALID),
        m_replaced_transactions(std::move(replaced_txns)),
        m_vsize{vsize},
        m_package_wtxids(package_wtxids) {}

    /** Constructor for already-in-relaypool case. It wouldn't replace any transactions. */
    explicit RelayPoolAcceptResult(int64_t vsize)
        : m_result_type(ResultType::RELAYPOOL_ENTRY), m_vsize{vsize} {}

    /** Constructor for witness-swapped case. */
    explicit RelayPoolAcceptResult(const Wtxid& other_wtxid)
        : m_result_type(ResultType::DIFFERENT_WITNESS), m_other_wtxid(other_wtxid) {}
};

/**
* Validation result for package relaypool acceptance.
*/
struct PackageRelayPoolAcceptResult
{
    PackageValidationState m_state;
    /**
    * Map from wtxid to finished RelayPoolAcceptResults. The client is responsible
    * for keeping track of the transaction objects themselves. If a result is not
    * present, it means validation was unfinished for that transaction. If there
    * was a package-wide error (see result in m_state), m_tx_results will be empty.
    */
    std::map<Wtxid, RelayPoolAcceptResult> m_tx_results;

    explicit PackageRelayPoolAcceptResult(PackageValidationState state,
                                        std::map<Wtxid, RelayPoolAcceptResult>&& results)
        : m_state{state}, m_tx_results(std::move(results)) {}

    /** Constructor to create a PackageRelayPoolAcceptResult from a single RelayPoolAcceptResult */
    explicit PackageRelayPoolAcceptResult(const Wtxid& wtxid, const RelayPoolAcceptResult& result)
        : m_tx_results{ {wtxid, result} } {}
};

/**
 * Try to add a transaction to the relaypool. This is an internal function and is exposed only for testing.
 * Client code should use ChainstateManager::ProcessTransaction()
 *
 * @param[in]  active_chainstate  Reference to the active chainstate.
 * @param[in]  tx                 The transaction to submit for relaypool acceptance.
 * @param[in]  accept_time        The timestamp for adding the transaction to the relaypool.
 *                                It is also used to determine when the entry expires.
 * @param[in]  bypass_limits      When true, don't enforce relaypool capacity limits,
 *                                and set entry_sequence to zero.
 * @param[in]  test_accept        When true, run validation checks but don't submit to relaypool.
 *
 * @returns a RelayPoolAcceptResult indicating whether the transaction was accepted/rejected with reason.
 */
RelayPoolAcceptResult AcceptToMemoryPool(Chainstate& active_chainstate, const CTransactionRef& tx,
                                       int64_t accept_time, bool bypass_limits, bool test_accept)
    EXCLUSIVE_LOCKS_REQUIRED(cs_main);

/**
* Validate (and maybe submit) a package to the relaypool. See doc/policy/packages.md for full details
* on package validation rules.
* @param[in]    test_accept         When true, run validation checks but don't submit to relaypool.
* @returns a PackageRelayPoolAcceptResult which includes a RelayPoolAcceptResult for each transaction.
* If a transaction fails, validation will exit early and some results may be missing. It is also
* possible for the package to be partially submitted.
*/
PackageRelayPoolAcceptResult ProcessNewPackage(Chainstate& active_chainstate, CTxRelayPool& pool,
                                                   const Package& txns, bool test_accept)
                                                   EXCLUSIVE_LOCKS_REQUIRED(cs_main);

/* RelayPool validation helper functions */

/**
 * Check if transaction will be final in the next block to be created.
 */
bool CheckFinalTxAtTip(const CBlockIndex& active_chain_tip, const CTransaction& tx) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

/**
 * Calculate LockPoints required to check if transaction will be BIP68 final in the next block
 * to be created on top of tip.
 *
 * @param[in]   tip             Chain tip for which tx sequence locks are calculated. For
 *                              example, the tip of the current active chain.
 * @param[in]   coins_view      Any CCoinsView that provides access to the relevant coins for
 *                              checking sequence locks. For example, it can be a CCoinsViewCache
 *                              that isn't connected to anything but contains all the relevant
 *                              coins, or a CCoinsViewRelayPool that is connected to the
 *                              relaypool and chainstate UTXO set. In the latter case, the caller
 *                              is responsible for holding the appropriate locks to ensure that
 *                              calls to GetCoin() return correct coins.
 * @param[in]   tx              The transaction being evaluated.
 *
 * @returns The resulting height and time calculated and the hash of the block needed for
 *          calculation, or std::nullopt if there is an error.
 */
std::optional<LockPoints> CalculateLockPointsAtTip(
    CBlockIndex* tip,
    const CCoinsView& coins_view,
    const CTransaction& tx);

/**
 * Check if transaction will be BIP68 final in the next block to be created on top of tip.
 * @param[in]   tip             Chain tip to check tx sequence locks against. For example,
 *                              the tip of the current active chain.
 * @param[in]   lock_points     LockPoints containing the height and time at which this
 *                              transaction is final.
 * Simulates calling SequenceLocks() with data from the tip passed in.
 * The LockPoints should not be considered valid if CheckSequenceLocksAtTip returns false.
 */
bool CheckSequenceLocksAtTip(CBlockIndex* tip,
                             const LockPoints& lock_points);

/**
 * Closure representing one script verification
 * Note that this stores references to the spending transaction
 */
class CScriptCheck
{
private:
    CTxOut m_tx_out;
    const CTransaction *ptxTo;
    unsigned int nIn;
    unsigned int nFlags;
    bool cacheStore;
    PrecomputedTransactionData *txdata;
    SignatureCache* m_signature_cache;

public:
    CScriptCheck(const CTxOut& outIn, const CTransaction& txToIn, SignatureCache& signature_cache, unsigned int nInIn, unsigned int nFlagsIn, bool cacheIn, PrecomputedTransactionData* txdataIn) :
        m_tx_out(outIn), ptxTo(&txToIn), nIn(nInIn), nFlags(nFlagsIn), cacheStore(cacheIn), txdata(txdataIn), m_signature_cache(&signature_cache) { }

    CScriptCheck(const CScriptCheck&) = delete;
    CScriptCheck& operator=(const CScriptCheck&) = delete;
    CScriptCheck(CScriptCheck&&) = default;
    CScriptCheck& operator=(CScriptCheck&&) = default;

    std::optional<std::pair<ScriptError, std::string>> operator()();
};

// CScriptCheck is used a lot in std::vector, make sure that's efficient
static_assert(std::is_nothrow_move_assignable_v<CScriptCheck>);
static_assert(std::is_nothrow_move_constructible_v<CScriptCheck>);
static_assert(std::is_nothrow_destructible_v<CScriptCheck>);

/**
 * Convenience class for initializing and passing the script execution cache
 * and signature cache.
 */
class ValidationCache
{
private:
    //! Pre-initialized hasher to avoid having to recreate it for every hash calculation.
    CSHA256 m_script_execution_cache_hasher;

public:
    CuckooCache::cache<uint256, SignatureCacheHasher> m_script_execution_cache;
    SignatureCache m_signature_cache;

    ValidationCache(size_t script_execution_cache_bytes, size_t signature_cache_bytes);

    ValidationCache(const ValidationCache&) = delete;
    ValidationCache& operator=(const ValidationCache&) = delete;

    //! Return a copy of the pre-initialized hasher.
    CSHA256 ScriptExecutionCacheHasher() const { return m_script_execution_cache_hasher; }
};

/** Functions for validating blocks and updating the block tree */

/** Context-independent validity checks */
bool CheckBlock(const CBlock& block, BlockValidationState& state, const Consensus::Params& consensusParams, bool fCheckPOW = true, bool fCheckMerkleRoot = true);

/** Check a block is completely valid from start to finish (only works on top of our current best block) */
bool TestBlockValidity(BlockValidationState& state,
                       const CChainParams& chainparams,
                       Chainstate& chainstate,
                       const CBlock& block,
                       CBlockIndex* pindexPrev,
                       bool fCheckPOW = true,
                       bool fCheckMerkleRoot = true) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

/** Check with the proof of work on each blockheader matches the value in nBits */
bool HasValidProofOfWork(const std::vector<CBlockHeader>& headers, const Consensus::Params& consensusParams);

/** Check if a block has been mutated (with respect to its merkle root and witness commitments). */
bool IsBlockMutated(const CBlock& block, bool check_witness_root);

/** Return the sum of the claimed work on a given set of headers. No verification of PoW is done. */
arith_uint256 CalculateClaimedHeadersWork(std::span<const CBlockHeader> headers);

enum class VerifyDBResult {
    SUCCESS,
    CORRUPTED_BLOCK_DB,
    INTERRUPTED,
    SKIPPED_L3_CHECKS,
    SKIPPED_MISSING_BLOCKS,
};

/** RAII wrapper for VerifyDB: Verify consistency of the block and coin databases */
class CVerifyDB
{
private:
    kernel::Notifications& m_notifications;

public:
    explicit CVerifyDB(kernel::Notifications& notifications);
    ~CVerifyDB();
    [[nodiscard]] VerifyDBResult VerifyDB(
        Chainstate& chainstate,
        const Consensus::Params& consensus_params,
        CCoinsView& coinsview,
        int nCheckLevel,
        int nCheckDepth) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
};

enum DisconnectResult
{
    DISCONNECT_OK,      // All good.
    DISCONNECT_UNCLEAN, // Rolled back, but UTXO set was inconsistent with block.
    DISCONNECT_FAILED   // Something else went wrong.
};

class ConnectTrace;

/** @see Chainstate::FlushStateToDisk */
enum class FlushStateMode {
    NONE,
    IF_NEEDED,
    PERIODIC,
    ALWAYS
};

/**
 * A convenience class for constructing the CCoinsView* hierarchy used
 * to facilitate access to the UTXO set.
 *
 * This class consists of an arrangement of layered CCoinsView objects,
 * preferring to store and retrieve coins in memory via `m_cacheview` but
 * ultimately falling back on cache misses to the canonical store of UTXOs on
 * disk, `m_dbview`.
 */
class CoinsViews {

public:
    //! The lowest level of the CoinsViews cache hierarchy sits in a leveldb database on disk.
    //! All unspent coins reside in this store.
    CCoinsViewDB m_dbview GUARDED_BY(cs_main);

    //! This view wraps access to the leveldb instance and handles read errors gracefully.
    CCoinsViewErrorCatcher m_catcherview GUARDED_BY(cs_main);

    //! This is the top layer of the cache hierarchy - it keeps as many coins in memory as
    //! can fit per the dbcache setting.
    std::unique_ptr<CCoinsViewCache> m_cacheview GUARDED_BY(cs_main);

    //! This constructor initializes CCoinsViewDB and CCoinsViewErrorCatcher instances, but it
    //! *does not* create a CCoinsViewCache instance by default. This is done separately because the
    //! presence of the cache has implications on whether or not we're allowed to flush the cache's
    //! state to disk, which should not be done until the health of the database is verified.
    //!
    //! All arguments forwarded onto CCoinsViewDB.
    CoinsViews(DBParams db_params, CoinsViewOptions options);

    //! Initialize the CCoinsViewCache member.
    void InitCache() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
};

enum class CoinsCacheSizeState
{
    //! The coins cache is in immediate need of a flush.
    CRITICAL = 2,
    //! The cache is at >= 90% capacity.
    LARGE = 1,
    OK = 0
};

/**
 * Chainstate stores and provides an API to update our local knowledge of the
 * current best chain.
 *
 * Eventually, the API here is targeted at being exposed externally as a
 * consumable library, so any functions added must only call
 * other class member functions, pure functions in other parts of the consensus
 * library, callbacks via the validation interface, or read/write-to-disk
 * functions (eventually this will also be via callbacks).
 *
 * Anything that is contingent on the current tip of the chain is stored here,
 * whereas block information and metadata independent of the current tip is
 * kept in `BlockManager`.
 */
class Chainstate
{
protected:
    /**
     * The ChainState Mutex
     * A lock that must be held when modifying this ChainState - held in ActivateBestChain() and
     * InvalidateBlock()
     */
    Mutex m_chainstate_mutex;

    //! Optional relaypool that is kept in sync with the chain.
    //! Only the active chainstate has a relaypool.
    CTxRelayPool* m_relaypool;

    //! Manages the UTXO set, which is a reflection of the contents of `m_chain`.
    std::unique_ptr<CoinsViews> m_coins_views;

public:
    //! Reference to a BlockManager instance which itself is shared across all
    //! Chainstate instances.
    node::BlockManager& m_blockman;

    //! The chainstate manager that owns this chainstate. The reference is
    //! necessary so that this instance can check whether it is the active
    //! chainstate within deeply nested method calls.
    ChainstateManager& m_chainman;

    explicit Chainstate(
        CTxRelayPool* relaypool,
        node::BlockManager& blockman,
        ChainstateManager& chainman);

    /**
     * Initialize the CoinsViews UTXO set database management data structures. The in-memory
     * cache is initialized separately.
     *
     * All parameters forwarded to CoinsViews.
     */
    void InitCoinsDB(
        size_t cache_size_bytes,
        bool in_memory,
        bool should_wipe,
        fs::path leveldb_name = "chainstate");

    //! Initialize the in-memory coins cache (to be done after the health of the on-disk database
    //! is verified).
    void InitCoinsCache(size_t cache_size_bytes) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    //! @returns whether or not the CoinsViews object has been fully initialized and we can
    //!          safely flush this object to disk.
    bool CanFlushToDisk() const EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        AssertLockHeld(::cs_main);
        return m_coins_views && m_coins_views->m_cacheview;
    }

    //! The current chain of blockheaders we consult and build on.
    //! @see CChain, CBlockIndex.
    CChain m_chain;

    /**
     * The set of all CBlockIndex entries that have as much work as our current
     * tip or more, and transaction data needed to be validated (with
     * BLOCK_VALID_TRANSACTIONS for each block and its parents back to the
     * genesis block). Entries may be failed, though, and pruning nodes may be
     * missing the data for the block.
     */
    std::set<CBlockIndex*, node::CBlockIndexWorkComparator> setBlockIndexCandidates;

    //! @returns A reference to the in-memory cache of the UTXO set.
    CCoinsViewCache& CoinsTip() EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        AssertLockHeld(::cs_main);
        Assert(m_coins_views);
        return *Assert(m_coins_views->m_cacheview);
    }

    //! @returns A reference to the on-disk UTXO set database.
    CCoinsViewDB& CoinsDB() EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        AssertLockHeld(::cs_main);
        return Assert(m_coins_views)->m_dbview;
    }

    //! @returns A pointer to the relaypool.
    CTxRelayPool* GetRelayPool()
    {
        return m_relaypool;
    }

    //! @returns A reference to a wrapped view of the in-memory UTXO set that
    //!     handles disk read errors gracefully.
    CCoinsViewErrorCatcher& CoinsErrorCatcher() EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        AssertLockHeld(::cs_main);
        return Assert(m_coins_views)->m_catcherview;
    }

    //! Destructs all objects related to accessing the UTXO set.
    void ResetCoinsViews() { m_coins_views.reset(); }

    //! The cache size of the on-disk coins view.
    size_t m_coinsdb_cache_size_bytes{0};

    //! The cache size of the in-memory coins view.
    size_t m_coinstip_cache_size_bytes{0};

    //! Resize the CoinsViews caches dynamically and flush state to disk.
    //! @returns true unless an error occurred during the flush.
    bool ResizeCoinsCaches(size_t coinstip_size, size_t coinsdb_size)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /**
     * Update the on-disk chain state.
     * The caches and indexes are flushed depending on the mode we're called with
     * if they're too large, if it's been a while since the last write,
     * or always and in all cases if we're in prune mode and are deleting files.
     *
     * If FlushStateMode::NONE is used, then FlushStateToDisk(...) won't do anything
     * besides checking if we need to prune.
     *
     * @returns true unless a system error occurred
     */
    bool FlushStateToDisk(
        BlockValidationState& state,
        FlushStateMode mode,
        int nManualPruneHeight = 0);

    //! Unconditionally flush all changes to disk.
    void ForceFlushStateToDisk();

    //! Prune blockfiles from the disk if necessary and then flush chainstate changes
    //! if we pruned.
    void PruneAndFlush();

    /**
     * Find the best known block, and make it the tip of the block chain. The
     * result is either failure or an activated best chain. pblock is either
     * nullptr or a pointer to a block that is already loaded (to avoid loading
     * it again from disk).
     *
     * ActivateBestChain is split into steps (see ActivateBestChainStep) so that
     * we avoid holding cs_main for an extended period of time; the length of this
     * call may be quite long during reindexing or a substantial reorg.
     *
     * May not be called with cs_main held. May not be called in a
     * validationinterface callback.
     *
     * @returns true unless a system error occurred
     */
    bool ActivateBestChain(
        BlockValidationState& state,
        std::shared_ptr<const CBlock> pblock = nullptr)
        EXCLUSIVE_LOCKS_REQUIRED(!m_chainstate_mutex)
        LOCKS_EXCLUDED(::cs_main);

    // Block (dis)connection on a given view:
    DisconnectResult DisconnectBlock(const CBlock& block, const CBlockIndex* pindex, CCoinsViewCache& view)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    bool ConnectBlock(const CBlock& block, BlockValidationState& state, CBlockIndex* pindex,
                      CCoinsViewCache& view, bool fJustCheck = false) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    // Apply the effects of a block disconnection on the UTXO set.
    bool DisconnectTip(BlockValidationState& state, DisconnectedBlockTransactions* disconnectpool) EXCLUSIVE_LOCKS_REQUIRED(cs_main, m_relaypool->cs);

    // Manual block validity manipulation:
    /** Mark a block as precious and reorganize.
     *
     * May not be called in a validationinterface callback.
     */
    bool PreciousBlock(BlockValidationState& state, CBlockIndex* pindex)
        EXCLUSIVE_LOCKS_REQUIRED(!m_chainstate_mutex)
        LOCKS_EXCLUDED(::cs_main);

    /** Mark a block as invalid. */
    bool InvalidateBlock(BlockValidationState& state, CBlockIndex* pindex)
        EXCLUSIVE_LOCKS_REQUIRED(!m_chainstate_mutex)
        LOCKS_EXCLUDED(::cs_main);

    /** Set invalidity status to all descendants of a block */
    void SetBlockFailureFlags(CBlockIndex* pindex) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /** Remove invalidity status from a block and its descendants. */
    void ResetBlockFailureFlags(CBlockIndex* pindex) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /** Replay blocks that aren't fully applied to the database. */
    bool ReplayBlocks();

    /** Whether the chain state needs to be redownloaded due to lack of witness data */
    [[nodiscard]] bool NeedsRedownload() const EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    /** Ensures we have a genesis block in the block tree, possibly writing one to disk. */
    bool LoadGenesisBlock();

    void TryAddBlockIndexCandidate(CBlockIndex* pindex) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    void PruneBlockIndexCandidates();

    void ClearBlockIndexCandidates() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /** Find the last common block of this chain and a locator. */
    const CBlockIndex* FindForkInGlobalIndex(const CBlockLocator& locator) const EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /** Update the chain tip based on database information, i.e. CoinsTip()'s best block. */
    bool LoadChainTip() EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    //! Dictates whether we need to flush the cache to disk or not.
    //!
    //! @return the state of the size of the coins cache.
    CoinsCacheSizeState GetCoinsCacheSizeState() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    CoinsCacheSizeState GetCoinsCacheSizeState(
        size_t max_coins_cache_size_bytes,
        size_t max_relaypool_size_bytes) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    std::string ToString() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    //! Indirection necessary to make lock annotations work with an optional relaypool.
    RecursiveMutex* RelayPoolMutex() const LOCK_RETURNED(m_relaypool->cs)
    {
        return m_relaypool ? &m_relaypool->cs : nullptr;
    }

private:
    bool ActivateBestChainStep(BlockValidationState& state, CBlockIndex* pindexMostWork, const std::shared_ptr<const CBlock>& pblock, bool& fInvalidFound, ConnectTrace& connectTrace) EXCLUSIVE_LOCKS_REQUIRED(cs_main, m_relaypool->cs);
    bool ConnectTip(BlockValidationState& state, CBlockIndex* pindexNew, const std::shared_ptr<const CBlock>& pblock, ConnectTrace& connectTrace, DisconnectedBlockTransactions& disconnectpool) EXCLUSIVE_LOCKS_REQUIRED(cs_main, m_relaypool->cs);

    void InvalidBlockFound(CBlockIndex* pindex, const BlockValidationState& state) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    CBlockIndex* FindMostWorkChain() EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    bool RollforwardBlock(const CBlockIndex* pindex, CCoinsViewCache& inputs) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    void CheckForkWarningConditions() EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    void InvalidChainFound(CBlockIndex* pindexNew) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /**
     * Make relaypool consistent after a reorg, by re-adding or recursively erasing
     * disconnected block transactions from the relaypool, and also removing any
     * other transactions from the relaypool that are no longer valid given the new
     * tip/height.
     *
     * Note: we assume that disconnectpool only contains transactions that are NOT
     * confirmed in the current chain nor already in the relaypool (otherwise,
     * in-relaypool descendants of such transactions would be removed).
     *
     * Passing fAddToRelayPool=false will skip trying to add the transactions back,
     * and instead just erase from the relaypool as needed.
     */
    void MaybeUpdateRelayPoolForReorg(
        DisconnectedBlockTransactions& disconnectpool,
        bool fAddToRelayPool) EXCLUSIVE_LOCKS_REQUIRED(cs_main, m_relaypool->cs);

    /** Check warning conditions and do some notifications on new chain tip set. */
    void UpdateTip(const CBlockIndex* pindexNew)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    SteadyClock::time_point m_last_write{};
    SteadyClock::time_point m_last_flush{};

    friend ChainstateManager;
};

/**
 * Owns the single chainstate used for ordinary IBD and reindex.
 *
 * The active chainstate is the current most-work chain. Consulted by most
 * parts of the system (net_processing, vault) as a reflection of the
 * current chain and UTXO set.
 */
class ChainstateManager
{
private:
    //! The chainstate used under normal operation (ordinary IBD / reindex).
    //!
    //! Once this pointer is set to a corresponding chainstate, it will not
    //! be reset until init.cpp:Shutdown().
    //!
    //! It is important for the pointer to not be deleted until shutdown,
    //! because cs_main is not always held when the pointer is accessed, for
    //! example when calling ActivateBestChain, so there's no way you could
    //! prevent code from using the pointer while deleting it.
    std::unique_ptr<Chainstate> m_ibd_chainstate GUARDED_BY(::cs_main);

    //! Points to the single chainstate; indicates our most-work chain.
    Chainstate* m_active_chainstate GUARDED_BY(::cs_main) {nullptr};

    CBlockIndex* m_best_invalid GUARDED_BY(::cs_main){nullptr};

    /** The last header for which a headerTip notification was issued. */
    CBlockIndex* m_last_notified_header GUARDED_BY(GetMutex()){nullptr};

    bool NotifyHeaderTip() LOCKS_EXCLUDED(GetMutex());

    /**
     * If a block header hasn't already been seen, call CheckBlockHeader on it, ensure
     * that it doesn't descend from an invalid block, and then add it to m_block_index.
     * Caller must set min_pow_checked=true in order to add a new header to the
     * block index (permanent memory storage), indicating that the header is
     * known to be part of a sufficiently high-work chain (anti-dos check).
     */
    bool AcceptBlockHeader(
        const CBlockHeader& block,
        BlockValidationState& state,
        CBlockIndex** ppindex,
        bool min_pow_checked) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    friend Chainstate;

    /** Most recent headers presync progress update, for rate-limiting. */
    std::chrono::time_point<std::chrono::steady_clock> m_last_presync_update GUARDED_BY(::cs_main) {};

    std::array<ThresholdConditionCache, VERSIONBITS_NUM_BITS> m_warningcache GUARDED_BY(::cs_main);

    //! A queue for script verifications that have to be performed by worker threads.
    CCheckQueue<CScriptCheck> m_script_check_queue;

    //! Timers and counters used for benchmarking validation.
    SteadyClock::duration GUARDED_BY(::cs_main) time_check{};
    SteadyClock::duration GUARDED_BY(::cs_main) time_forks{};
    SteadyClock::duration GUARDED_BY(::cs_main) time_connect{};
    SteadyClock::duration GUARDED_BY(::cs_main) time_verify{};
    SteadyClock::duration GUARDED_BY(::cs_main) time_undo{};
    SteadyClock::duration GUARDED_BY(::cs_main) time_index{};
    SteadyClock::duration GUARDED_BY(::cs_main) time_total{};
    int64_t GUARDED_BY(::cs_main) num_blocks_total{0};
    SteadyClock::duration GUARDED_BY(::cs_main) time_connect_total{};
    SteadyClock::duration GUARDED_BY(::cs_main) time_flush{};
    SteadyClock::duration GUARDED_BY(::cs_main) time_chainstate{};
    SteadyClock::duration GUARDED_BY(::cs_main) time_post_connect{};

    /** Fill in a block's uncommitted structures (currently: only the coinbase witness
     *  reserved value).
     *
     *  Deliberately NOT public. Doing this to a block that arrived from outside changes
     *  its weight, and weight is an input to nCongestion -- so repairing a submitted
     *  block rejects a miner who ground the multiplier correctly over their own bytes.
     *  The only legitimate caller is GenerateCoinbaseCommitment, which runs while the
     *  block is still being assembled here and before its multiplier is derived. */
    void UpdateUncommittedBlockStructures(CBlock& block, const CBlockIndex* pindexPrev) const;

public:
    using Options = kernel::ChainstateManagerOpts;

    explicit ChainstateManager(const util::SignalInterrupt& interrupt, Options options, node::BlockManager::Options blockman_options);

    const CChainParams& GetParams() const { return m_options.chainparams; }
    const Consensus::Params& GetConsensus() const { return m_options.chainparams.GetConsensus(); }
    bool ShouldCheckBlockIndex() const;
    const arith_uint256& MinimumChainWork() const { return *Assert(m_options.minimum_chain_work); }
    const uint256& AssumedValidBlock() const { return *Assert(m_options.assumed_valid_block); }
    kernel::Notifications& GetNotifications() const { return m_options.notifications; };

    /**
     * Make various assertions about the state of the block index.
     *
     * By default this only executes fully when using the Sandbox chain; see: m_options.check_block_index.
     */
    void CheckBlockIndex();

    /**
     * Alias for ::cs_main.
     * Should be used in new code to make it easier to make ::cs_main a member
     * of this class.
     * Generally, methods of this class should be annotated to require this
     * mutex. This will make calling code more verbose, but also help to:
     * - Clarify that the method will acquire a mutex that heavily affects
     *   overall performance.
     * - Force call sites to think how long they need to acquire the mutex to
     *   get consistent results.
     */
    RecursiveMutex& GetMutex() const LOCK_RETURNED(::cs_main) { return ::cs_main; }

    const util::SignalInterrupt& m_interrupt;
    const Options m_options;
    //! A single BlockManager instance is shared across each constructed
    //! chainstate to avoid duplicating block metadata.
    node::BlockManager m_blockman;

    ValidationCache m_validation_cache;

    /**
     * Whether initial block download has ended and IsInitialBlockDownload
     * should return false from now on.
     *
     * Mutable because we need to be able to mark IsInitialBlockDownload()
     * const, which latches this for caching purposes.
     */
    mutable std::atomic<bool> m_cached_finished_ibd{false};

    /**
     * Every received block is assigned a unique and increasing identifier, so we
     * know which one to give priority in case of a fork.
     */
    /** Blocks loaded from disk are assigned id 0, so start the counter at 1. */
    int32_t nBlockSequenceId GUARDED_BY(::cs_main) = 1;
    /** Decreasing counter (used by subsequent preciousblock calls). */
    int32_t nBlockReverseSequenceId = -1;
    /** chainwork for the last block that preciousblock has been applied to. */
    arith_uint256 nLastPreciousChainwork = 0;

    // Reset the memory-only sequence counters we use to track block arrival
    // (used by tests to reset state)
    void ResetBlockSequenceCounters() EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        AssertLockHeld(::cs_main);
        nBlockSequenceId = 1;
        nBlockReverseSequenceId = -1;
    }


    /**
     * In order to efficiently track invalidity of headers, we keep the set of
     * blocks which we tried to connect and found to be invalid here (ie which
     * were set to BLOCK_FAILED_VALID since the last restart). We can then
     * walk this set and check if a new header is a descendant of something in
     * this set, preventing us from having to walk m_block_index when we try
     * to connect a bad block and fail.
     *
     * While this is more complicated than marking everything which descends
     * from an invalid block as invalid at the time we discover it to be
     * invalid, doing so would require walking all of m_block_index to find all
     * descendants. Since this case should be very rare, keeping track of all
     * BLOCK_FAILED_VALID blocks in a set should be just fine and work just as
     * well.
     *
     * Because we already walk m_block_index in height-order at startup, we go
     * ahead and mark descendants of invalid blocks as FAILED_CHILD at that time,
     * instead of putting things in this set.
     */
    std::set<CBlockIndex*> m_failed_blocks;

    /** Best header we've seen so far (used for getheaders queries' starting points). */
    CBlockIndex* m_best_header GUARDED_BY(::cs_main){nullptr};

    //! The total number of bytes available for the in-memory coins cache.
    size_t m_total_coinstip_cache{0};
    //
    //! The total number of bytes available for the leveldb coins database.
    size_t m_total_coinsdb_cache{0};

    //! Instantiate a new chainstate.
    //!
    //! @param[in] relaypool              The relaypool to pass to the chainstate
    //                                  constructor
    Chainstate& InitializeChainstate(CTxRelayPool* relaypool) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    //! Get all chainstates currently being used. This is the single IBD
    //! chainstate, if initialized.
    std::vector<Chainstate*> GetAll();

    //! The most-work chain.
    Chainstate& ActiveChainstate() const;
    CChain& ActiveChain() const EXCLUSIVE_LOCKS_REQUIRED(GetMutex()) { return ActiveChainstate().m_chain; }
    int ActiveHeight() const EXCLUSIVE_LOCKS_REQUIRED(GetMutex()) { return ActiveChain().Height(); }
    CBlockIndex* ActiveTip() const EXCLUSIVE_LOCKS_REQUIRED(GetMutex()) { return ActiveChain().Tip(); }

    node::BlockMap& BlockIndex() EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        AssertLockHeld(::cs_main);
        return m_blockman.m_block_index;
    }

    /**
     * Track versionbit status
     */
    mutable VersionBitsCache m_versionbitscache;

    /** Check whether we are doing an initial block download (synchronizing from disk or network) */
    bool IsInitialBlockDownload() const;

    /** Guess verification progress (as a fraction between 0.0=genesis and 1.0=current tip). */
    double GuessVerificationProgress(const CBlockIndex* pindex) const;

    /**
     * Import blocks from an external file
     *
     * During reindexing, this function is called for each block file (datadir/blocks/blk?????.dat).
     * It reads all blocks contained in the given file and attempts to process them (add them to the
     * block index). The blocks may be out of order within each file and across files. Often this
     * function reads a block but finds that its parent hasn't been read yet, so the block can't be
     * processed yet. The function will add an entry to the blocks_with_unknown_parent map (which is
     * passed as an argument), so that when the block's parent is later read and processed, this
     * function can re-read the child block from disk and process it.
     *
     * Because a block's parent may be in a later file, not just later in the same file, the
     * blocks_with_unknown_parent map must be passed in and out with each call. It's a multimap,
     * rather than just a map, because multiple blocks may have the same parent (when chain splits
     * or stale blocks exist). It maps from parent-hash to child-disk-position.
     *
     * This function can also be used to read blocks from user-specified block files using the
     * -loadblock= option. There's no unknown-parent tracking, so the last two arguments are omitted.
     *
     *
     * @param[in]     file_in                       File containing blocks to read
     * @param[in]     dbp                           (optional) Disk block position (only for reindex)
     * @param[in,out] blocks_with_unknown_parent    (optional) Map of disk positions for blocks with
     *                                              unknown parent, key is parent block hash
     *                                              (only used for reindex)
     * */
    void LoadExternalBlockFile(
        AutoFile& file_in,
        FlatFilePos* dbp = nullptr,
        std::multimap<uint256, FlatFilePos>* blocks_with_unknown_parent = nullptr);

    /**
     * Process an incoming block. This only returns after the best known valid
     * block is made active. Note that it does not, however, guarantee that the
     * specific block passed to it has been checked for validity!
     *
     * If you want to *possibly* get feedback on whether block is valid, you must
     * install a CValidationInterface (see validationinterface.h) - this will have
     * its BlockChecked method called whenever *any* block completes validation.
     *
     * Note that we guarantee that either the proof-of-work is valid on block, or
     * (and possibly also) BlockChecked will have been called.
     *
     * May not be called in a validationinterface callback.
     *
     * @param[in]   block The block we want to process.
     * @param[in]   force_processing Process this block even if unrequested; used for non-network block sources.
     * @param[in]   min_pow_checked  True if proof-of-work anti-DoS checks have
     *                               been done by caller for headers chain
     *                               (note: only affects headers acceptance; if
     *                               block header is already present in block
     *                               index then this parameter has no effect)
     * @param[out]  new_block A boolean which is set to indicate if the block was first received via this call
     * @returns     If the block was processed, independently of block validity
     */
    bool ProcessNewBlock(const std::shared_ptr<const CBlock>& block, bool force_processing, bool min_pow_checked, bool* new_block) LOCKS_EXCLUDED(cs_main);

    /**
     * Process incoming block headers.
     *
     * May not be called in a
     * validationinterface callback.
     *
     * @param[in]  headers The block headers themselves
     * @param[in]  min_pow_checked  True if proof-of-work anti-DoS checks have been done by caller for headers chain
     * @param[out] state This may be set to an Error state if any error occurred processing them
     * @param[out] ppindex If set, the pointer will be set to point to the last new block index object for the given headers
     */
    bool ProcessNewBlockHeaders(std::span<const CBlockHeader> headers, bool min_pow_checked, BlockValidationState& state, const CBlockIndex** ppindex = nullptr) LOCKS_EXCLUDED(cs_main);

    /**
     * Sufficiently validate a block for disk storage (and store on disk).
     *
     * @param[in]   pblock          The block we want to process.
     * @param[in]   fRequested      Whether we requested this block from a
     *                              peer.
     * @param[in]   dbp             The location on disk, if we are importing
     *                              this block from prior storage.
     * @param[in]   min_pow_checked True if proof-of-work anti-DoS checks have
     *                              been done by caller for headers chain
     *
     * @param[out]  state       The state of the block validation.
     * @param[out]  ppindex     Optional return parameter to get the
     *                          CBlockIndex pointer for this block.
     * @param[out]  fNewBlock   Optional return parameter to indicate if the
     *                          block is new to our storage.
     *
     * @returns   False if the block or header is invalid, or if saving to disk fails (likely a fatal error); true otherwise.
     */
    bool AcceptBlock(const std::shared_ptr<const CBlock>& pblock, BlockValidationState& state, CBlockIndex** ppindex, bool fRequested, const FlatFilePos* dbp, bool* fNewBlock, bool min_pow_checked) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    void ReceivedBlockTransactions(const CBlock& block, CBlockIndex* pindexNew, const FlatFilePos& pos) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /**
     * Try to add a transaction to the memory pool.
     *
     * @param[in]  tx              The transaction to submit for relaypool acceptance.
     * @param[in]  test_accept     When true, run validation checks but don't submit to relaypool.
     */
    [[nodiscard]] RelayPoolAcceptResult ProcessTransaction(const CTransactionRef& tx, bool test_accept=false)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    //! Load the block tree and coins database from disk, initializing state if we're running with -reindex
    bool LoadBlockIndex() EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    //! Check to see if caches are out of balance and if so, call
    //! ResizeCoinsCaches() as needed.
    void MaybeRebalanceCaches() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /** Produce the necessary coinbase commitment for a block (modifies the hash, don't call for mined blocks). */
    std::vector<unsigned char> GenerateCoinbaseCommitment(CBlock& block, const CBlockIndex* pindexPrev) const;

    /** This is used by net_processing to report pre-synchronization progress of headers, as
     *  headers are not yet fed to validation during that time, but validation is (for now)
     *  responsible for logging and signalling through NotifyHeaderTip, so it needs this
     *  information. */
    void ReportHeadersPresync(const arith_uint256& work, int64_t height, int64_t timestamp);

    //! @returns the chainstate that indexes should consult. With a single
    //!   fully-validating chainstate this is the active chainstate.
    Chainstate& GetChainstateForIndexing() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    //! Return the [start, end] (inclusive) of block heights we can prune.
    //!
    //! start > end is possible, meaning no blocks can be pruned.
    std::pair<int, int> GetPruneRange(
        const Chainstate& chainstate, int last_height_can_prune) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    //! If, due to invalidation / reconsideration of blocks, the previous
    //! best header is no longer valid / guaranteed to be the most-work
    //! header in our block-index not known to be invalid, recalculate it.
    void RecalculateBestHeader() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    CCheckQueue<CScriptCheck>& GetCheckQueue() { return m_script_check_queue; }

    ~ChainstateManager();
};

/** Deployment* info via ChainstateManager */
template<typename DEP>
bool DeploymentActiveAfter(const CBlockIndex* pindexPrev, const ChainstateManager& chainman, DEP dep)
{
    return DeploymentActiveAfter(pindexPrev, chainman.GetConsensus(), dep, chainman.m_versionbitscache);
}

template<typename DEP>
bool DeploymentActiveAt(const CBlockIndex& index, const ChainstateManager& chainman, DEP dep)
{
    return DeploymentActiveAt(index, chainman.GetConsensus(), dep, chainman.m_versionbitscache);
}

template<typename DEP>
bool DeploymentEnabled(const ChainstateManager& chainman, DEP dep)
{
    return DeploymentEnabled(chainman.GetConsensus(), dep);
}

#endif // QUICKSILVER_VALIDATION_H
