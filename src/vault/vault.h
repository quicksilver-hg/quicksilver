// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_VAULT_VAULT_H
#define QUICKSILVER_VAULT_VAULT_H

#include <addresstype.h>
#include <consensus/amount.h>
#include <interfaces/chain.h>
#include <interfaces/handler.h>
#include <kernel/cs_main.h>
#include <logging.h>
#include <outputtype.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <support/allocators/secure.h>
#include <sync.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/fs.h>
#include <util/hasher.h>
#include <util/result.h>
#include <util/string.h>
#include <util/time.h>
#include <util/ui_change_type.h>
#include <vault/crypter.h>
#include <vault/db.h>
#include <vault/scriptpubkeyman.h>
#include <vault/transaction.h>
#include <vault/types.h>
#include <vault/vaultutil.h>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/signals2/signal.hpp>

class CKey;
class CKeyID;
class CPubKey;
class Coin;
class SigningProvider;
enum class RelayPoolRemovalReason;
enum class SigningResult;
namespace common {
enum class PSQTError;
} // namespace common
namespace interfaces {
class Vault;
}
namespace vault {
class CVault;
class VaultBatch;
enum class DBErrors : int;
} // namespace vault
struct CBlockLocator;
struct CExtKey;
struct FlatSigningProvider;
struct KeyOriginInfo;
struct PartiallySignedQuicksilverTransaction;
struct SignatureData;

using LoadVaultFn = std::function<void(std::unique_ptr<interfaces::Vault> vault)>;

struct bilingual_str;

namespace vault {
struct VaultContext;

enum class TxSubmissionStatus {
    NOT_ATTEMPTED,
    SUCCEEDED,
    FAILED,
};

struct TxSubmissionResult {
    TxSubmissionStatus status{TxSubmissionStatus::NOT_ATTEMPTED};
    node::TransactionError error{node::TransactionError::OK};
};

struct CommitTransactionResult {
    node::TransactionError error{node::TransactionError::OK};
    std::string reject_reason;

    explicit operator bool() const { return error == node::TransactionError::OK; }
};

//! Explicitly delete the vault.
//! Blocks the current thread until the vault is destructed.
void WaitForDeleteVault(std::shared_ptr<CVault>&& vault);

bool AddVault(VaultContext& context, const std::shared_ptr<CVault>& vault);
bool RemoveVault(VaultContext& context, const std::shared_ptr<CVault>& vault, std::optional<bool> load_on_start, std::vector<bilingual_str>& warnings);
bool RemoveVault(VaultContext& context, const std::shared_ptr<CVault>& vault, std::optional<bool> load_on_start);
std::vector<std::shared_ptr<CVault>> GetVaults(VaultContext& context);
std::shared_ptr<CVault> GetDefaultVault(VaultContext& context, size_t& count);
std::shared_ptr<CVault> GetVault(VaultContext& context, const std::string& name);
std::shared_ptr<CVault> LoadVault(VaultContext& context, const std::string& name, std::optional<bool> load_on_start, const DatabaseOptions& options, DatabaseStatus& status, bilingual_str& error, std::vector<bilingual_str>& warnings);
std::shared_ptr<CVault> CreateVault(VaultContext& context, const std::string& name, std::optional<bool> load_on_start, DatabaseOptions& options, DatabaseStatus& status, bilingual_str& error, std::vector<bilingual_str>& warnings);
std::shared_ptr<CVault> RestoreVault(VaultContext& context, const fs::path& backup_file, const std::string& vault_name, std::optional<bool> load_on_start, DatabaseStatus& status, bilingual_str& error, std::vector<bilingual_str>& warnings, bool load_after_restore = true);
std::unique_ptr<interfaces::Handler> HandleLoadVault(VaultContext& context, LoadVaultFn load_vault);
void NotifyVaultLoaded(VaultContext& context, const std::shared_ptr<CVault>& vault);
std::unique_ptr<VaultDatabase> MakeVaultDatabase(const std::string& name, const DatabaseOptions& options, DatabaseStatus& status, bilingual_str& error);

//! Default for -spendzeroconfchange
static const bool DEFAULT_SPEND_ZEROCONF_CHANGE = true;
//! Default for -vaultrejectlongchains
static const bool DEFAULT_VAULT_REJECT_LONG_CHAINS{true};
static const bool DEFAULT_VAULTBROADCAST = true;
static const bool DEFAULT_DISABLE_VAULT = false;
static const bool DEFAULT_VAULTCROSSCHAIN = false;
class CCoinControl;

//! Default for -addresstype
constexpr OutputType DEFAULT_ADDRESS_TYPE{OutputType::BECH32};

static constexpr uint64_t KNOWN_VAULT_FLAGS =
    VAULT_FLAG_AVOID_REUSE | VAULT_FLAG_BLANK_VAULT | VAULT_FLAG_DISABLE_PRIVATE_KEYS | VAULT_FLAG_DESCRIPTORS | VAULT_FLAG_EXTERNAL_SIGNER;

static constexpr uint64_t MUTABLE_VAULT_FLAGS =
    VAULT_FLAG_AVOID_REUSE;

static const std::map<std::string, VaultFlags> VAULT_FLAG_MAP{
    {"avoid_reuse", VAULT_FLAG_AVOID_REUSE},
    {"blank", VAULT_FLAG_BLANK_VAULT},
    {"disable_private_keys", VAULT_FLAG_DISABLE_PRIVATE_KEYS},
    {"external_signer", VAULT_FLAG_EXTERNAL_SIGNER}};

/** A wrapper to reserve an address from a vault
 *
 * ReserveDestination is used to reserve an address.
 * It is currently only used inside of CreateTransaction.
 *
 * Instantiating a ReserveDestination does not reserve an address. To do so,
 * GetReservedDestination() needs to be called on the object. Once an address has been
 * reserved, call KeepDestination() on the ReserveDestination object to make sure it is not
 * returned. Call ReturnDestination() to return the address so it can be reused (for
 * example, if the address was used in a new transaction
 * and that transaction was not completed and needed to be aborted).
 *
 * If an address is reserved and KeepDestination() is not called, then the address will be
 * returned when the ReserveDestination goes out of scope.
 */
class ReserveDestination
{
protected:
    //! The vault to reserve from
    const CVault* const pvault;
    //! The ScriptPubKeyMan to reserve from. Based on type when GetReservedDestination is called
    ScriptPubKeyMan* m_spk_man{nullptr};
    OutputType const type;
    //! The index of the address's key in the keypool
    int64_t nIndex{-1};
    //! The destination
    CTxDestination address;
    //! Whether this is from the internal (change output) keypool
    bool fInternal{false};

public:
    //! Construct a ReserveDestination object. This does NOT reserve an address yet
    explicit ReserveDestination(CVault* pvault, OutputType type)
        : pvault(pvault), type(type) {}

    ReserveDestination(const ReserveDestination&) = delete;
    ReserveDestination& operator=(const ReserveDestination&) = delete;

    //! Destructor. If a key has been reserved and not KeepKey'ed, it will be returned to the keypool
    ~ReserveDestination()
    {
        ReturnDestination();
    }

    //! Reserve an address
    util::Result<CTxDestination> GetReservedDestination(bool internal);
    //! Return reserved address
    void ReturnDestination();
    //! Keep the address. Do not return its key to the keypool when this object goes out of scope
    void KeepDestination();
};

/**
 * Address book data.
 */
struct CAddressBookData {
    /**
     * Address label which is always nullopt for change addresses. For sending
     * and receiving addresses, it will be set to an arbitrary label string
     * provided by the user, or to "", which is the default label. The presence
     * or absence of a label is used to distinguish change addresses from
     * non-change addresses by vault transaction listing code.
     */
    std::optional<std::string> label;

    /**
     * Address purpose: a cached IsMine value telling one of our own receiving
     * addresses from a payee we have sent to. Set for every labelled entry --
     * SetAddressBookWithDB and the vault load both derive one when the caller or
     * the record on disk supplies none. Change addresses carry no purpose, the
     * same way they carry no label.
     */
    std::optional<AddressPurpose> purpose;

    /**
     * Whether coins with this address have previously been spent. Set when the
     * the vault avoid_reuse option is enabled and this is an IsMine address
     * that has already received funds and spent them. This is used during coin
     * selection to increase privacy by not creating different transactions
     * that spend from the same addresses.
     */
    bool previously_spent{false};

    /**
     * Map containing data about previously generated receive requests
     * requesting funds to be sent to this address. Only present for IsMine
     * addresses. Map keys are decimal numbers uniquely identifying each
     * request, and map values are serialized RecentRequestEntry objects
     * containing BIP21 URI information including message and amount.
     */
    std::map<std::string, std::string> receive_requests{};

    /** Accessor methods. */
    bool IsChange() const { return !label.has_value(); }
    std::string GetLabel() const { return label ? *label : std::string{}; }
    void SetLabel(std::string name) { label = std::move(name); }
};

inline std::string PurposeToString(AddressPurpose p)
{
    switch (p) {
    case AddressPurpose::RECEIVE: return "receive";
    case AddressPurpose::SEND: return "send";
    } // no default case so the compiler will warn when a new enum as added
    assert(false);
}

inline std::optional<AddressPurpose> PurposeFromString(std::string_view s)
{
    if (s == "receive")
        return AddressPurpose::RECEIVE;
    else if (s == "send")
        return AddressPurpose::SEND;
    return {};
}

struct CRecipient {
    CTxDestination dest;
    CAmount nAmount;
};

class VaultRescanReserver; // forward declarations for ScanForVaultTransactions/RescanFromTime
/**
 * A CVault maintains a set of transactions and balances, and provides the ability to create new transactions.
 */
class CVault final : public VaultStorage, public interfaces::Chain::Notifications
{
private:
    CKeyingMaterial vMasterKey GUARDED_BY(cs_vault);

    bool Unlock(const CKeyingMaterial& vMasterKeyIn);

    std::atomic<bool> fAbortRescan{false};
    std::atomic<bool> fScanningVault{false}; // controlled by VaultRescanReserver
    // Serializes the best-block write with the decision to ignore
    // chainStateFlushed. A shutdown-interrupted import rescan rewinds the
    // locator under this lock; the flush Shutdown() runs after the RPC returns
    // then cannot put that locator back at the tip.
    Mutex m_best_block_mutex;
    std::atomic<bool> m_attaching_chain{false};
    std::atomic<bool> m_scanning_with_passphrase{false};
    std::atomic<SteadyClock::time_point> m_scanning_start{SteadyClock::time_point{}};
    std::atomic<double> m_scanning_progress{0};
    friend class VaultRescanReserver;

    //! the current vault version: clients below this version are not able to load the vault
    int nVaultVersion GUARDED_BY(cs_vault){0};

    /** The next scheduled rebroadcast of vault transactions. */
    NodeClock::time_point m_next_resend{GetDefaultNextResend()};
    /** Whether this vault will submit newly created transactions to the node's relaypool and
     * prompt rebroadcasts (see ResendVaultTransactions()). */
    bool fBroadcastTransactions = false;
    // Local time that the tip block was received. Used to schedule vault rebroadcasts.
    std::atomic<int64_t> m_best_block_time{0};

    // First created key time. Used to skip blocks prior to this time.
    // 'std::numeric_limits<int64_t>::max()' if vault is blank.
    std::atomic<int64_t> m_birth_time{std::numeric_limits<int64_t>::max()};

    /**
     * Used to keep track of spent outpoints, and
     * detect and report conflicts (double-spends or
     * mutated transactions where the mutant gets mined).
     */
    typedef std::unordered_multimap<COutPoint, uint256, SaltedOutpointHasher> TxSpends;
    TxSpends mapTxSpends GUARDED_BY(cs_vault);
    void AddToSpends(const COutPoint& outpoint, const uint256& wtxid, VaultBatch* batch = nullptr) EXCLUSIVE_LOCKS_REQUIRED(cs_vault);
    void AddToSpends(const CVaultTx& wtx, VaultBatch* batch = nullptr) EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    /**
     * Add a transaction to the vault, or update it.  confirm.block_* should
     * be set when the transaction was known to be included in a block.  When
     * block_hash.IsNull(), then vault state is not updated in AddToVault, but
     * notifications happen and cached balances are marked dirty.
     *
     * If fUpdate is true, existing transactions will be updated.
     * TODO: One exception to this is that the abandoned state is cleared under the
     * assumption that any further notification of a transaction that was considered
     * abandoned is an indication that it is not safe to be considered abandoned.
     * Abandoned state should probably be more carefully tracked via different
     * chain notifications or by checking relaypool presence when necessary.
     *
     * Should be called with rescanning_old_block set to true, if the transaction is
     * not discovered in real time, but during a rescan of old blocks.
     */
    bool AddToVaultIfInvolvingMe(const CTransactionRef& tx, const SyncTxState& state, bool fUpdate, bool rescanning_old_block) EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    /** Mark a transaction (and its in-vault descendants) as conflicting with a particular block. */
    void MarkConflicted(const uint256& hashBlock, int conflicting_height, const uint256& hashTx);

    enum class TxUpdate { UNCHANGED,
                          CHANGED,
                          NOTIFY_CHANGED };

    using TryUpdatingStateFn = std::function<TxUpdate(CVaultTx& wtx)>;

    /** Mark a transaction (and its in-vault descendants) as a particular tx state. */
    void RecursiveUpdateTxState(const uint256& tx_hash, const TryUpdatingStateFn& try_updating_state) EXCLUSIVE_LOCKS_REQUIRED(cs_vault);
    void RecursiveUpdateTxState(VaultBatch* batch, const uint256& tx_hash, const TryUpdatingStateFn& try_updating_state) EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    /** Mark a transaction's inputs dirty, thus forcing the outputs to be recomputed */
    void MarkInputsDirty(const CTransactionRef& tx) EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    void SyncMetaData(std::pair<TxSpends::iterator, TxSpends::iterator>) EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    void SyncTransaction(const CTransactionRef& tx, const SyncTxState& state, bool update_tx = true, bool rescanning_old_block = false) EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    /** VaultFlags set on this vault. */
    std::atomic<uint64_t> m_vault_flags{0};

    bool SetAddressBookWithDB(VaultBatch& batch, const CTxDestination& address, const std::string& strName, const std::optional<AddressPurpose>& strPurpose);

    //! Unsets a vault flag and saves it to disk
    void UnsetVaultFlagWithDB(VaultBatch& batch, uint64_t flag);

    //! Unset the blank vault flag and saves it to disk
    void UnsetBlankVaultFlag(VaultBatch& batch) override;

    /** Interface for accessing chain state. */
    interfaces::Chain* m_chain;

    /** Vault name: relative directory name, or "" for a vault.dat in vaultdir itself. */
    std::string m_name;

    /** Internal database handle. */
    std::unique_ptr<VaultDatabase> m_database;

    /**
     * The following is used to keep track of how far behind the vault is
     * from the chain sync, and to allow clients to block on us being caught up.
     *
     * Processed hash is a pointer on node's tip and doesn't imply that the vault
     * has scanned sequentially all blocks up to this one.
     */
    uint256 m_last_block_processed GUARDED_BY(cs_vault);

    /** Height of last block processed is used by vault to know depth of transactions
     * without relying on Chain interface beyond asynchronous updates. For safety, we
     * initialize it to -1. Height is a pointer on node's tip and doesn't imply
     * that the vault has scanned sequentially all blocks up to this one.
     */
    int m_last_block_processed_height GUARDED_BY(cs_vault) = -1;

    /** Header time for the current height/hash clock. This can come from a
     * full block notification or the separately validated thin header source. */
    int64_t m_last_block_processed_time GUARDED_BY(cs_vault) = -1;

    std::map<OutputType, ScriptPubKeyMan*> m_external_spk_managers;
    std::map<OutputType, ScriptPubKeyMan*> m_internal_spk_managers;

    // Indexed by a unique identifier produced by each ScriptPubKeyMan using
    // ScriptPubKeyMan::GetID. In many cases it will be the hash of an internal structure
    std::map<uint256, std::unique_ptr<ScriptPubKeyMan>> m_spk_managers;

    // Appends spk managers into the main 'm_spk_managers'.
    // Must be the only method adding data to it.
    void AddScriptPubKeyMan(const uint256& id, std::unique_ptr<ScriptPubKeyMan> spkm_man);

    // Same as 'AddActiveScriptPubKeyMan' but designed for use within a batch transaction context
    void AddActiveScriptPubKeyManWithDb(VaultBatch& batch, uint256 id, OutputType type, bool internal);

    /** Store vault flags */
    void SetVaultFlagWithDB(VaultBatch& batch, uint64_t flags);

    //! Cache of descriptor ScriptPubKeys used for IsMine. Maps ScriptPubKey to set of spkms
    std::unordered_map<CScript, std::vector<ScriptPubKeyMan*>, SaltedSipHasher> m_cached_spks;

    /**
     * Write the best-block sync point for a vault the calling thread has not
     * locked, during creation or chain attachment.
     *
     * chainStateFlushed() declares EXCLUSIVE_LOCKS_REQUIRED(!m_best_block_mutex)
     * so that no CVault member can self-deadlock on that non-recursive Mutex.
     * Create() and AttachChain() both have to write the sync point, and neither
     * can satisfy the requirement in an attribute: the capability belongs to
     * another object, and in Create() that object is a function-local
     * shared_ptr, which an attribute cannot name at all.
     *
     * Both satisfy it in fact. Neither takes m_best_block_mutex anywhere, and
     * neither can be reached by a thread that already holds it: it is private,
     * and the only members that lock it are chainStateFlushed() and
     * RescanFromTime(), neither of which calls a factory. Keeping the exemption
     * to this one statement leaves the analysis on the whole of both callers,
     * and DEBUG_LOCKORDER still reports a genuine self-deadlock at runtime.
     */
    static void FlushSyncPointDuringLoad(CVault& vault, const CBlockLocator& locator) NO_THREAD_SAFETY_ANALYSIS;

    /**
     * Catch vault up to current chain, scanning new blocks, updating the best
     * block locator and m_last_block_processed, and registering for
     * notifications about new blocks and transactions.
     */
    static bool AttachChain(const std::shared_ptr<CVault>& vault, interfaces::Chain& chain, const bool rescan_required, bilingual_str& error, std::vector<bilingual_str>& warnings);

    static NodeClock::time_point GetDefaultNextResend();

public:
    /**
     * Main vault lock.
     * This lock protects all the fields added by CVault.
     */
    mutable RecursiveMutex cs_vault;

    VaultDatabase& GetDatabase() const override
    {
        assert(static_cast<bool>(m_database));
        return *m_database;
    }

    /** Get a name for this vault for logging/debugging purposes.
     */
    const std::string& GetName() const { return m_name; }

    typedef std::map<unsigned int, CMasterKey> MasterKeyMap;
    MasterKeyMap mapMasterKeys;
    unsigned int nMasterKeyMaxID = 0;

    /** Construct vault with specified name and database implementation. */
    CVault(interfaces::Chain* chain, const std::string& name, std::unique_ptr<VaultDatabase> database)
        : m_chain(chain),
          m_name(name),
          m_database(std::move(database))
    {
    }

    ~CVault()
    {
        // Should not have slots connected at this point.
        assert(NotifyUnload.empty());
    }

    bool IsCrypted() const;
    bool IsLocked() const override;
    bool Lock();

    /** Interface to assert chain access */
    bool HaveChain() const { return m_chain ? true : false; }

    /** Map from txid to CVaultTx for all transactions this vault is
     * interested in, including received and sent transactions. */
    std::unordered_map<uint256, CVaultTx, SaltedTxidHasher> mapVault GUARDED_BY(cs_vault);

    typedef std::multimap<int64_t, CVaultTx*> TxItems;
    TxItems wtxOrdered;

    int64_t nOrderPosNext GUARDED_BY(cs_vault) = 0;

    std::map<CTxDestination, CAddressBookData> m_address_book GUARDED_BY(cs_vault);
    const CAddressBookData* FindAddressBookEntry(const CTxDestination&, bool allow_change = false) const EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    /** Set of Coins owned by this vault that we won't try to spend from. A
     * Coin may be locked if it has already been used to fund a transaction
     * that hasn't confirmed yet. We wouldn't consider the Coin spent already,
     * but also shouldn't try to use it again. */
    std::set<COutPoint> setLockedCoins GUARDED_BY(cs_vault);

    /** Registered interfaces::Chain::Notifications handler. */
    std::unique_ptr<interfaces::Handler> m_chain_notifications_handler;

    /** Interface for accessing chain state. */
    interfaces::Chain& chain() const
    {
        assert(m_chain);
        return *m_chain;
    }

    const CVaultTx* GetVaultTx(const uint256& hash) const EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    std::set<uint256> GetTxConflicts(const CVaultTx& wtx) const EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    /**
     * Return depth of transaction in blockchain:
     * <0  : conflicts with a transaction this deep in the blockchain
     *  0  : in memory pool, waiting to be included in a block
     * >=1 : this many blocks deep in the main chain
     *
     * Preconditions: it is only valid to call this function when the vault is
     * online and the block index is loaded. So this cannot be called by
     * quicksilver-vault tool code. If this is called
     * without the vault being online, it won't be able able to determine the
     * the height of the last block processed, or the heights of blocks
     * referenced in transaction, and might cause assert failures.
     */
    int GetTxDepthInMainChain(const CVaultTx& wtx) const EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    /**
     * @return number of blocks to maturity for this transaction:
     *  0 : is not a coinbase transaction, or is a mature coinbase transaction
     * >0 : is a coinbase transaction which matures in this many blocks
     */
    int GetTxBlocksToMaturity(const CVaultTx& wtx) const EXCLUSIVE_LOCKS_REQUIRED(cs_vault);
    bool IsTxImmatureCoinBase(const CVaultTx& wtx) const EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    bool IsSpent(const COutPoint& outpoint) const EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    // Whether this or any known scriptPubKey with the same single key has been spent.
    bool IsSpentKey(const CScript& scriptPubKey) const EXCLUSIVE_LOCKS_REQUIRED(cs_vault);
    void SetSpentKeyState(VaultBatch& batch, const uint256& hash, unsigned int n, bool used, std::set<CTxDestination>& tx_destinations) EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    /** Display address on an external signer. */
    util::Result<void> DisplayAddress(const CTxDestination& dest) EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    bool IsLockedCoin(const COutPoint& output) const EXCLUSIVE_LOCKS_REQUIRED(cs_vault);
    bool LockCoin(const COutPoint& output, VaultBatch* batch = nullptr) EXCLUSIVE_LOCKS_REQUIRED(cs_vault);
    bool UnlockCoin(const COutPoint& output, VaultBatch* batch = nullptr) EXCLUSIVE_LOCKS_REQUIRED(cs_vault);
    bool UnlockAllCoins() EXCLUSIVE_LOCKS_REQUIRED(cs_vault);
    void ListLockedCoins(std::vector<COutPoint>& vOutpts) const EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    /*
     * Rescan abort properties
     */
    void AbortRescan() { fAbortRescan = true; }
    bool IsAbortingRescan() const { return fAbortRescan; }
    bool IsScanning() const { return fScanningVault; }
    bool IsScanningWithPassphrase() const { return m_scanning_with_passphrase; }
    SteadyClock::duration ScanningDuration() const { return fScanningVault ? SteadyClock::now() - m_scanning_start.load() : SteadyClock::duration{}; }
    double ScanningProgress() const { return fScanningVault ? (double)m_scanning_progress : 0; }

    bool LoadMinVersion(int nVersion) EXCLUSIVE_LOCKS_REQUIRED(cs_vault)
    {
        AssertLockHeld(cs_vault);
        nVaultVersion = nVersion;
        return true;
    }

    //! Marks destination as previously spent.
    void LoadAddressPreviouslySpent(const CTxDestination& dest) EXCLUSIVE_LOCKS_REQUIRED(cs_vault);
    //! Appends payment request to destination.
    void LoadAddressReceiveRequest(const CTxDestination& dest, const std::string& id, const std::string& request) EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    //! Holds a timestamp at which point the vault is scheduled (externally) to be relocked. Caller must arrange for actual relocking to occur via Lock().
    int64_t nRelockTime GUARDED_BY(cs_vault){0};

    // Used to prevent concurrent calls to vaultpassphrase RPC.
    Mutex m_unlock_mutex;
    // Used to prevent deleting the passphrase from memory when it is still in use.
    RecursiveMutex m_relock_mutex;

    bool Unlock(const SecureString& strVaultPassphrase);
    bool ChangeVaultPassphrase(const SecureString& strOldVaultPassphrase, const SecureString& strNewVaultPassphrase);
    bool EncryptVault(const SecureString& strVaultPassphrase);

    unsigned int ComputeTimeSmart(const CVaultTx& wtx, bool rescanning_old_block) const;

    /**
     * Increment the next transaction order id
     * @return next transaction order id
     */
    int64_t IncOrderPosNext(VaultBatch* batch = nullptr) EXCLUSIVE_LOCKS_REQUIRED(cs_vault);
    void MarkDirty();

    //! Callback for updating transaction metadata in mapVault.
    //!
    //! @param wtx - reference to mapVault transaction to update
    //! @param new_tx - true if wtx is newly inserted, false if it previously existed
    //!
    //! @return true if wtx is changed and needs to be saved to disk, otherwise false
    using UpdateVaultTxFn = std::function<bool(CVaultTx& wtx, bool new_tx)>;

    /**
     * Add the transaction to the vault, wrapping it up inside a CVaultTx
     * @return the recently added wtx pointer or nullptr if there was a db write error.
     */
    CVaultTx* AddToVault(CTransactionRef tx, const TxState& state, const UpdateVaultTxFn& update_wtx = nullptr, bool fFlushOnClose = true, bool rescanning_old_block = false);
    bool LoadToVault(const uint256& hash, const UpdateVaultTxFn& fill_wtx) EXCLUSIVE_LOCKS_REQUIRED(cs_vault);
    void transactionAddedToRelayPool(const CTransactionRef& tx) override;
    void blockConnected(const interfaces::BlockInfo& block) override;
    void blockDisconnected(const interfaces::BlockInfo& block) override;
    void updatedBlockTip() override;
    int64_t RescanFromTime(int64_t startTime, const VaultRescanReserver& reserver, bool update) EXCLUSIVE_LOCKS_REQUIRED(!m_best_block_mutex);

    struct ScanResult {
        enum { SUCCESS,
               FAILURE,
               USER_ABORT } status = SUCCESS;

        //! Hash and height of most recent block that was successfully scanned.
        //! Unset if no blocks were scanned due to read errors or the chain
        //! being empty.
        uint256 last_scanned_block;
        std::optional<int> last_scanned_height;

        //! Height of the most recent block that could not be scanned due to
        //! read errors or pruning. Will be set if status is FAILURE, unset if
        //! status is SUCCESS, and may or may not be set if status is
        //! USER_ABORT.
        uint256 last_failed_block;
    };
    ScanResult ScanForVaultTransactions(const uint256& start_block, int start_height, std::optional<int> max_height, const VaultRescanReserver& reserver, bool fUpdate, const bool save_progress);
    void transactionRemovedFromRelayPool(const CTransactionRef& tx, RelayPoolRemovalReason reason) override;
    /** Set the next time this vault should resend transactions to 12-36 hours from now, ~1 day on average. */
    void SetNextResend() { m_next_resend = GetDefaultNextResend(); }
    /** Return true if all conditions for periodically resending transactions are met. */
    bool ShouldResend() const;
    void ResubmitVaultTransactions(bool relay, bool force);

    OutputType TransactionChangeType(const std::optional<OutputType>& change_type, const std::vector<CRecipient>& vecSend) const;

    /** Fetch the inputs and sign with SIGHASH_ALL. */
    bool SignTransaction(CMutableTransaction& tx) const EXCLUSIVE_LOCKS_REQUIRED(cs_vault);
    /** Sign the tx given the input coins and sighash. */
    bool SignTransaction(CMutableTransaction& tx, const std::map<COutPoint, Coin>& coins, int sighash, std::map<int, bilingual_str>& input_errors) const;
    SigningResult SignMessage(const std::string& message, const PKHash& pkhash, std::string& str_sig) const;

    /**
     * Fills out a PSQT with information from the vault. Fills in UTXOs if we have
     * them. Tries to sign if sign=true. Sets `complete` if the PSQT is now complete
     * (i.e. has all required signatures or signature-parts, and is ready to
     * finalize.) Sets `error` and returns false if something goes wrong.
     *
     * @param[in]  psqtx PartiallySignedQuicksilverTransaction to fill in
     * @param[out] complete indicates whether the PSQT is now complete
     * @param[in]  sighash_type the sighash type to use when signing (if PSQT does not specify)
     * @param[in]  sign whether to sign or not
     * @param[in]  bip32derivs whether to fill in bip32 derivation information if available
     * @param[out] n_signed the number of inputs signed by this vault
     * @param[in] finalize whether to create the final scriptSig or scriptWitness if possible
     * return error
     */
    std::optional<common::PSQTError> FillPSQT(PartiallySignedQuicksilverTransaction& psqtx,
                                              bool& complete,
                                              int sighash_type = SIGHASH_DEFAULT,
                                              bool sign = true,
                                              bool bip32derivs = true,
                                              size_t* n_signed = nullptr,
                                              bool finalize = true) const;

    /**
     * Submit the transaction to the node's relaypool and then relay to peers.
     * Should be called after CreateTransaction unless you want to abort
     * broadcasting the transaction.
     *
     * @param[in] tx The transaction to be broadcast.
     * @param[in] mapValue key-values to be set on the transaction.
     * @param[in] orderForm BIP 70 / BIP 21 order form details to be set on the transaction.
     */
    CommitTransactionResult CommitTransaction(CTransactionRef tx, mapValue_t mapValue, std::vector<std::pair<std::string, std::string>> orderForm);

    /** Pass this transaction to node for relaypool insertion and relay to peers if flag set to true */
    TxSubmissionResult SubmitTxMemoryPoolAndRelay(CVaultTx& wtx, std::string& err_string, bool relay) const
        EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    /** Updates vault birth time if 'time' is below it */
    void MaybeUpdateBirthTime(int64_t time);

    /** Allow Coin Selection to pick unconfirmed UTXOs that were sent from our own vault if it
     * cannot fund the transaction otherwise. */
    bool m_spend_zero_conf_change{DEFAULT_SPEND_ZEROCONF_CHANGE};

    OutputType m_default_address_type{DEFAULT_ADDRESS_TYPE};
    /**
     * Default output type for change outputs. When unset, automatically choose type
     * based on address type setting and the types other of non-change outputs
     * (see -changetype option documentation and implementation in
     * CVault::TransactionChangeType for details).
     */
    std::optional<OutputType> m_default_change_type{};

    /** Number of pre-generated keys/scripts by each spkm (part of the look-ahead process, used to detect payments) */
    int64_t m_keypool_size{DEFAULT_KEYPOOL_SIZE};

    /** Notify external script when a vault transaction comes in or is updated (handled by -vaultnotify) */
    std::string m_notify_tx_changed_script;

    size_t KeypoolCountExternalKeys() const EXCLUSIVE_LOCKS_REQUIRED(cs_vault);
    bool TopUpKeyPool(unsigned int kpSize = 0);

    std::optional<int64_t> GetOldestKeyPoolTime() const;

    // Filter struct for 'ListAddrBookAddresses'
    struct AddrBookFilter {
        // Fetch addresses with the provided label
        std::optional<std::string> m_op_label{std::nullopt};
        // Don't include change addresses by default
        bool ignore_change{true};
    };

    /**
     * Filter and retrieve destinations stored in the addressbook
     */
    std::vector<CTxDestination> ListAddrBookAddresses(const std::optional<AddrBookFilter>& filter) const EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    /**
     * Retrieve all the known labels in the address book
     */
    std::set<std::string> ListAddrBookLabels(const std::optional<AddressPurpose> purpose) const EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    /**
     * Walk-through the address book entries.
     * Stops when the provided 'ListAddrBookFunc' returns false.
     */
    using ListAddrBookFunc = std::function<void(const CTxDestination& dest, const std::string& label, bool is_change, const std::optional<AddressPurpose> purpose)>;
    void ForEachAddrBookEntry(const ListAddrBookFunc& func) const EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    /**
     * Marks all outputs in each one of the destinations dirty, so their cache is
     * reset and does not return outdated information.
     */
    void MarkDestinationsDirty(const std::set<CTxDestination>& destinations) EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    util::Result<CTxDestination> GetNewDestination(const OutputType type, const std::string label);
    util::Result<CTxDestination> GetNewChangeDestination(const OutputType type);

    isminetype IsMine(const CTxDestination& dest) const EXCLUSIVE_LOCKS_REQUIRED(cs_vault);
    isminetype IsMine(const CScript& script) const EXCLUSIVE_LOCKS_REQUIRED(cs_vault);
    /**
     * Returns amount of debit if the input matches the
     * filter, otherwise returns 0
     */
    CAmount GetDebit(const CTxIn& txin, const isminefilter& filter) const;
    isminetype IsMine(const CTxOut& txout) const EXCLUSIVE_LOCKS_REQUIRED(cs_vault);
    bool IsMine(const CTransaction& tx) const EXCLUSIVE_LOCKS_REQUIRED(cs_vault);
    isminetype IsMine(const COutPoint& outpoint) const EXCLUSIVE_LOCKS_REQUIRED(cs_vault);
    /** should probably be renamed to IsRelevantToMe */
    bool IsFromMe(const CTransaction& tx) const;
    CAmount GetDebit(const CTransaction& tx, const isminefilter& filter) const;
    void chainStateFlushed(const CBlockLocator& loc) override EXCLUSIVE_LOCKS_REQUIRED(!m_best_block_mutex);

    DBErrors LoadVault();

    /** Erases the provided transactions from the vault. */
    util::Result<void> RemoveTxs(std::vector<uint256>& txs_to_remove) EXCLUSIVE_LOCKS_REQUIRED(cs_vault);
    util::Result<void> RemoveTxs(VaultBatch& batch, std::vector<uint256>& txs_to_remove) EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    bool SetAddressBook(const CTxDestination& address, const std::string& strName, const std::optional<AddressPurpose>& purpose);

    bool DelAddressBook(const CTxDestination& address);
    bool DelAddressBookWithDB(VaultBatch& batch, const CTxDestination& address);

    bool IsAddressPreviouslySpent(const CTxDestination& dest) const EXCLUSIVE_LOCKS_REQUIRED(cs_vault);
    bool SetAddressPreviouslySpent(VaultBatch& batch, const CTxDestination& dest, bool used) EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    std::vector<std::string> GetAddressReceiveRequests() const EXCLUSIVE_LOCKS_REQUIRED(cs_vault);
    bool SetAddressReceiveRequest(VaultBatch& batch, const CTxDestination& dest, const std::string& id, const std::string& value) EXCLUSIVE_LOCKS_REQUIRED(cs_vault);
    bool EraseAddressReceiveRequest(VaultBatch& batch, const CTxDestination& dest, const std::string& id) EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    unsigned int GetKeyPoolSize() const EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    //! Stamp this vault at VAULT_FILE_VERSION and write minversion.
    void SetMinVersion(VaultBatch* batch_in = nullptr);

    //! Return the vault file version stamp (VAULT_FILE_VERSION).
    int GetVersion() const
    {
        LOCK(cs_vault);
        return nVaultVersion;
    }

    //! Get vault transactions that conflict with given transaction (spend same outputs)
    std::set<uint256> GetConflicts(const uint256& txid) const EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    //! Check if a given transaction has any of its outputs spent by another transaction in the vault
    bool HasVaultSpend(const CTransactionRef& tx) const EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    //! Flush vault (bitdb flush)
    void Flush();

    //! Close vault database
    void Close();

    /** Vault is about to be unloaded */
    boost::signals2::signal<void()> NotifyUnload;

    /**
     * Address book entry changed.
     * @note called without lock cs_vault held.
     */
    boost::signals2::signal<void(const CTxDestination& address,
                                 const std::string& label, bool isMine,
                                 AddressPurpose purpose, ChangeType status)>
        NotifyAddressBookChanged;

    /**
     * Vault transaction added, removed or updated.
     * @note called with lock cs_vault held.
     */
    boost::signals2::signal<void(const uint256& hashTx, ChangeType status)> NotifyTransactionChanged;

    /** Show progress e.g. for rescan */
    boost::signals2::signal<void(const std::string& title, int nProgress)> ShowProgress;

    /** Keypool has new keys */
    boost::signals2::signal<void()> NotifyCanGetAddressesChanged;

    /**
     * Vault status (encrypted, locked) changed.
     * Note: Called without locks held.
     */
    boost::signals2::signal<void(CVault* vault)> NotifyStatusChanged;

    /** Inquire whether this vault broadcasts transactions. */
    bool GetBroadcastTransactions() const { return fBroadcastTransactions; }
    /** Set whether this vault broadcasts transactions. */
    void SetBroadcastTransactions(bool broadcast) { fBroadcastTransactions = broadcast; }

    /** Return whether transaction can be abandoned */
    bool TransactionCanBeAbandoned(const uint256& hashTx) const;

    /* Mark a transaction (and it in-vault descendants) as abandoned so its inputs may be respent. */
    bool AbandonTransaction(const uint256& hashTx);
    bool AbandonTransaction(CVaultTx& tx) EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    /* Initializes the vault, returns a new CVault instance or a null pointer in case of an error */
    static std::shared_ptr<CVault> Create(VaultContext& context, const std::string& name, std::unique_ptr<VaultDatabase> database, uint64_t vault_creation_flags, bilingual_str& error, std::vector<bilingual_str>& warnings);

    /**
     * Vault post-init setup
     * Gives the vault a chance to register repetitive tasks and complete post-init tasks
     */
    void postInitProcess();

    bool BackupVault(const std::string& strDest) const;
    bool IsBackupRecorded() const;
    bool SetBackupRecorded(bool recorded) const;
    util::Result<AgentAllotmentRecord> RecordAgentAllotmentSetup(const std::string& label, CAmount funding_limit, CAmount daily_limit);
    std::vector<AgentAllotmentRecord> ListAgentAllotmentRecords() const;
    std::string AgentAllotmentPolicyRequest(const AgentAllotmentRecord& record, CAmount funding_available) const;
    util::Result<AgentAllotmentPolicyRequestMetadata> ValidateAgentAllotmentPolicyRequest(const std::string& request_json) const;
    util::Result<AgentAllotmentPolicyBundle> ExportAgentAllotmentPolicyBundle(const std::string& request_json) EXCLUSIVE_LOCKS_REQUIRED(!cs_vault);

    /* Returns true if HD is enabled */
    bool IsHDEnabled() const;

    /* Returns true if the vault can give out new addresses. This means it has keys in the keypool or can generate new keys */
    bool CanGetAddresses(bool internal = false) const;

    /* Returns the time of the first created key or, in case of an import, it could be the time of the first received transaction */
    int64_t GetBirthTime() const { return m_birth_time; }

    /**
     * Blocks until the vault state is up-to-date to /at least/ the current
     * chain at the time this function is entered
     * Obviously holding cs_main/cs_vault when going into this call may cause
     * deadlock
     */
    void BlockUntilSyncedToCurrentChain() const LOCKS_EXCLUDED(::cs_main) EXCLUSIVE_LOCKS_REQUIRED(!cs_vault);

    /** set a single vault flag */
    void SetVaultFlag(uint64_t flags);

    /** Unsets a single vault flag */
    void UnsetVaultFlag(uint64_t flag);

    /** check if a certain vault flag is set */
    bool IsVaultFlagSet(uint64_t flag) const override;

    /** overwrite all flags by the given uint64_t
       flags must be uninitialised (or 0)
       only known flags may be present */
    void InitVaultFlags(uint64_t flags);
    /** Loads the flags into the vault. (used by LoadVault) */
    bool LoadVaultFlags(uint64_t flags);

    /** Returns the vault name for use as a log field or display label, "default" if the vault has no name */
    std::string GetDisplayName() const override
    {
        return GetName().length() == 0 ? "default" : GetName();
    };

    /** Tags vault log output with the vault name to ease debugging in multi-vault use cases */
    template <typename... Params>
    void VaultLogPrintf(util::ConstevalFormatString<sizeof...(Params)> vault_fmt, const Params&... params) const
    {
        std::string msg{tfm::format(vault_fmt, params...)};
        while (msg.ends_with('\n'))
            msg.pop_back();
        LogInfo(HgLog::VAULT, "%s vault=%s", msg, GetDisplayName());
    };

    //! Returns all unique ScriptPubKeyMans in m_internal_spk_managers and m_external_spk_managers
    std::set<ScriptPubKeyMan*> GetActiveScriptPubKeyMans() const;
    bool IsActiveScriptPubKeyMan(const ScriptPubKeyMan& spkm) const;

    //! Returns all unique ScriptPubKeyMans
    std::set<ScriptPubKeyMan*> GetAllScriptPubKeyMans() const;

    //! Get the ScriptPubKeyMan for the given OutputType and internal/external chain.
    ScriptPubKeyMan* GetScriptPubKeyMan(const OutputType& type, bool internal) const;

    //! Get all the ScriptPubKeyMans for a script
    std::set<ScriptPubKeyMan*> GetScriptPubKeyMans(const CScript& script) const;
    //! Get the ScriptPubKeyMan by id
    ScriptPubKeyMan* GetScriptPubKeyMan(const uint256& id) const;

    //! Get the SigningProvider for a script
    std::unique_ptr<SigningProvider> GetSolvingProvider(const CScript& script) const;
    std::unique_ptr<SigningProvider> GetSolvingProvider(const CScript& script, SignatureData& sigdata) const;

    //! Get the vault descriptors for a script.
    std::vector<VaultDescriptor> GetVaultDescriptors(const CScript& script) const;

    bool WithEncryptionKey(std::function<bool(const CKeyingMaterial&)> cb) const override;

    bool HasEncryptionKeys() const override;

    /** Get last block processed height */
    bool TryGetLastBlockProcessed(int& block_height, uint256& block_hash) const EXCLUSIVE_LOCKS_REQUIRED(cs_vault)
    {
        AssertLockHeld(cs_vault);
        if (m_last_block_processed_height < 0) return false;
        block_height = m_last_block_processed_height;
        block_hash = m_last_block_processed;
        return true;
    }
    bool TryGetLastBlockTime(int64_t& block_time) const EXCLUSIVE_LOCKS_REQUIRED(cs_vault)
    {
        AssertLockHeld(cs_vault);
        if (m_last_block_processed_height < 0 || m_last_block_processed_time < 0) return false;
        block_time = m_last_block_processed_time;
        return true;
    }
    bool TryGetLastBlockHash(uint256& block_hash) const EXCLUSIVE_LOCKS_REQUIRED(cs_vault)
    {
        AssertLockHeld(cs_vault);
        if (m_last_block_processed_height < 0) return false;
        block_hash = m_last_block_processed;
        return true;
    }
    int GetLastBlockHeight() const EXCLUSIVE_LOCKS_REQUIRED(cs_vault)
    {
        AssertLockHeld(cs_vault);
        assert(m_last_block_processed_height >= 0);
        return m_last_block_processed_height;
    };
    uint256 GetLastBlockHash() const EXCLUSIVE_LOCKS_REQUIRED(cs_vault)
    {
        AssertLockHeld(cs_vault);
        assert(m_last_block_processed_height >= 0);
        return m_last_block_processed;
    }
    /** Set the validated height/hash clock used for refresh and confirmation depth. */
    void SetLastBlockProcessed(int block_height, uint256 block_hash, int64_t block_time = -1) EXCLUSIVE_LOCKS_REQUIRED(cs_vault)
    {
        AssertLockHeld(cs_vault);
        m_last_block_processed_height = block_height;
        m_last_block_processed = block_hash;
        m_last_block_processed_time = block_time;
    };
    /** Apply a thin-chain tip only when it cannot make a stored transaction's
     * confirmation height or an already newer tip nonsensical. */
    bool ApplyHeaderTip(int block_height, const uint256& block_hash, int64_t block_time) EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    //! Connect the signals from ScriptPubKeyMans to the signals in CVault
    void ConnectScriptPubKeyManNotifiers();

    //! Instantiate a descriptor ScriptPubKeyMan from the VaultDescriptor and load it
    DescriptorScriptPubKeyMan& LoadDescriptorScriptPubKeyMan(uint256 id, VaultDescriptor& desc);

    //! Adds the active ScriptPubKeyMan for the specified type and internal. Writes it to the vault file
    //! @param[in] id The unique id for the ScriptPubKeyMan
    //! @param[in] type The OutputType this ScriptPubKeyMan provides addresses for
    //! @param[in] internal Whether this ScriptPubKeyMan provides change addresses
    void AddActiveScriptPubKeyMan(uint256 id, OutputType type, bool internal);

    //! Loads an active ScriptPubKeyMan for the specified type and internal. (used by LoadVault)
    //! @param[in] id The unique id for the ScriptPubKeyMan
    //! @param[in] type The OutputType this ScriptPubKeyMan provides addresses for
    //! @param[in] internal Whether this ScriptPubKeyMan provides change addresses
    void LoadActiveScriptPubKeyMan(uint256 id, OutputType type, bool internal);

    //! Remove specified ScriptPubKeyMan from set of active SPK managers. Writes the change to the vault file.
    //! @param[in] id The unique id for the ScriptPubKeyMan
    //! @param[in] type The OutputType this ScriptPubKeyMan provides addresses for
    //! @param[in] internal Whether this ScriptPubKeyMan provides change addresses
    void DeactivateScriptPubKeyMan(uint256 id, OutputType type, bool internal);

    //! Create new DescriptorScriptPubKeyMan and add it to the vault
    DescriptorScriptPubKeyMan& SetupDescriptorScriptPubKeyMan(VaultBatch& batch, const CExtKey& master_key, const OutputType& output_type, bool internal) EXCLUSIVE_LOCKS_REQUIRED(cs_vault);
    //! Create new DescriptorScriptPubKeyMans and add them to the vault
    void SetupDescriptorScriptPubKeyMans(VaultBatch& batch, const CExtKey& master_key) EXCLUSIVE_LOCKS_REQUIRED(cs_vault);
    void SetupDescriptorScriptPubKeyMans() EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    //! Create new seed and default DescriptorScriptPubKeyMans for this vault
    void SetupOwnDescriptorScriptPubKeyMans(VaultBatch& batch) EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    //! Return the DescriptorScriptPubKeyMan for a VaultDescriptor if it is already in the vault
    DescriptorScriptPubKeyMan* GetDescriptorScriptPubKeyMan(const VaultDescriptor& desc) const;

    //! Returns whether the provided ScriptPubKeyMan is internal
    //! @param[in] spk_man The ScriptPubKeyMan to test
    //! @return contains value only for active DescriptorScriptPubKeyMan, otherwise undefined
    std::optional<bool> IsInternalScriptPubKeyMan(ScriptPubKeyMan* spk_man) const;

    //! Add a descriptor to the vault, return a ScriptPubKeyMan & associated output type
    ScriptPubKeyMan* AddVaultDescriptor(VaultDescriptor& desc, const FlatSigningProvider& signing_provider, const std::string& label, bool internal) EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    //! Whether the (external) signer performs R-value signature grinding
    bool CanGrindR() const;

    //! Add scriptPubKeys for this ScriptPubKeyMan into the scriptPubKey cache
    void CacheNewScriptPubKeys(const std::set<CScript>& spks, ScriptPubKeyMan* spkm);

    void TopUpCallback(const std::set<CScript>& spks, ScriptPubKeyMan* spkm) override;

    //! Retrieve the xpubs in use by the active descriptors
    std::set<CExtPubKey> GetActiveHDPubKeys() const EXCLUSIVE_LOCKS_REQUIRED(cs_vault);

    //! Find the private key for the given key id from the vault's descriptors, if available
    //! Returns nullopt when no descriptor has the key or if the vault is locked.
    std::optional<CKey> GetKey(const CKeyID& keyid) const;
};

/**
 * Called periodically by the schedule thread. Prompts individual vaults to resend
 * their transactions. Actual rebroadcast schedule is managed by the vaults themselves.
 */
void MaybeResendVaultTxs(VaultContext& context);

/** RAII object to check and reserve a vault rescan */
class VaultRescanReserver
{
private:
    using Clock = std::chrono::steady_clock;
    using NowFn = std::function<Clock::time_point()>;
    CVault& m_vault;
    bool m_could_reserve{false};
    NowFn m_now;

public:
    explicit VaultRescanReserver(CVault& w) : m_vault(w) {}

    bool reserve(bool with_passphrase = false)
    {
        assert(!m_could_reserve);
        if (m_vault.fScanningVault.exchange(true)) {
            return false;
        }
        m_vault.m_scanning_with_passphrase.exchange(with_passphrase);
        m_vault.m_scanning_start = SteadyClock::now();
        m_vault.m_scanning_progress = 0;
        m_could_reserve = true;
        return true;
    }

    bool isReserved() const
    {
        return (m_could_reserve && m_vault.fScanningVault);
    }

    Clock::time_point now() const { return m_now ? m_now() : Clock::now(); };

    void setNow(NowFn now) { m_now = std::move(now); }

    ~VaultRescanReserver()
    {
        if (m_could_reserve) {
            m_vault.fScanningVault = false;
            m_vault.m_scanning_with_passphrase = false;
        }
    }
};

//! Add vault name to persistent configuration so it will be loaded on startup.
bool AddVaultSetting(interfaces::Chain& chain, const std::string& vault_name);

//! Remove vault name from persistent configuration so it will not be loaded on startup.
bool RemoveVaultSetting(interfaces::Chain& chain, const std::string& vault_name);

} // namespace vault

#endif // QUICKSILVER_VAULT_VAULT_H
