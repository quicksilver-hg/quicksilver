// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_VAULT_VAULTDB_H
#define QUICKSILVER_VAULT_VAULTDB_H

#include <script/sign.h>
#include <vault/db.h>
#include <vault/types.h>
#include <vault/vaultutil.h>
#include <key.h>

#include <stdint.h>
#include <string>
#include <vector>

class CScript;
class uint160;
class uint256;
struct CBlockLocator;

namespace vault {
class CMasterKey;
class CVault;
class CVaultTx;
struct VaultContext;

/**
 * Overview of vault database classes:
 *
 * - VaultBatch is an abstract modifier object for the vault database, and encapsulates a database
 *   batch update as well as methods to act on the database. It should be agnostic to the database implementation.
 *
 * The following classes are implementation specific:
 * - SQLiteDatabase represents a vault database.
 * - SQLiteBatch is a low-level database batch update.
 */

static const bool DEFAULT_FLUSHVAULT = true;

/** Error statuses for the vault database.
 * Values are in order of severity. When multiple errors occur, the most severe (highest value) will be returned.
 */
enum class DBErrors : int
{
    LOAD_OK = 0,
    NEED_RESCAN = 1,
    EXTERNAL_SIGNER_SUPPORT_REQUIRED = 3,
    NONCRITICAL_ERROR = 4,
    TOO_NEW = 5,
    UNKNOWN_DESCRIPTOR = 6,
    CORRUPT = 9,
};

namespace DBKeys {
extern const std::string ACTIVEEXTERNALSPK;
extern const std::string ACTIVEINTERNALSPK;
extern const std::string BESTBLOCK;
extern const std::string DESTDATA;
extern const std::string FLAGS;
extern const std::string LOCKED_UTXO;
extern const std::string MASTER_KEY;
extern const std::string MINVERSION;
extern const std::string NAME;
extern const std::string ORDERPOSNEXT;
extern const std::string PURPOSE;
extern const std::string SETTINGS;
extern const std::string TX;
extern const std::string VERSION;
extern const std::string VAULTDESCRIPTOR;
extern const std::string VAULTDESCRIPTORCKEY;
extern const std::string VAULTDESCRIPTORKEY;
} // namespace DBKeys

class CKeyMetadata
{
public:
    int64_t nCreateTime; // 0 means unknown
    KeyOriginInfo key_origin; // Key origin info with path and fingerprint
    bool has_key_origin = false; //!< Whether the key_origin is useful

    CKeyMetadata()
    {
        SetNull();
    }
    explicit CKeyMetadata(int64_t nCreateTime_)
    {
        SetNull();
        nCreateTime = nCreateTime_;
    }

    void SetNull()
    {
        nCreateTime = 0;
        key_origin.clear();
        has_key_origin = false;
    }
};

struct DbTxnListener
{
    std::function<void()> on_commit, on_abort;
};

/** Access to the vault database.
 * Opens the database and provides read and write access to it. Each read and write is its own transaction.
 * Multiple operation transactions can be started using TxnBegin() and committed using TxnCommit()
 * Otherwise the transaction will be committed when the object goes out of scope.
 * Optionally (on by default) it will flush to disk on close.
 * Every 1000 writes will automatically trigger a flush to disk.
 */
class VaultBatch
{
private:
    template <typename K, typename T>
    bool WriteIC(const K& key, const T& value, bool fOverwrite = true)
    {
        if (!m_batch->Write(key, value, fOverwrite)) {
            return false;
        }
        m_database.IncrementUpdateCounter();
        if (m_database.nUpdateCounter % 1000 == 0) {
            m_batch->Flush();
        }
        return true;
    }

    template <typename K>
    bool EraseIC(const K& key)
    {
        if (!m_batch->Erase(key)) {
            return false;
        }
        m_database.IncrementUpdateCounter();
        if (m_database.nUpdateCounter % 1000 == 0) {
            m_batch->Flush();
        }
        return true;
    }

public:
    explicit VaultBatch(VaultDatabase &database, bool _fFlushOnClose = true) :
        m_batch(database.MakeBatch(_fFlushOnClose)),
        m_database(database)
    {
    }
    VaultBatch(const VaultBatch&) = delete;
    VaultBatch& operator=(const VaultBatch&) = delete;

    bool WriteName(const std::string& strAddress, const std::string& strName);
    bool EraseName(const std::string& strAddress);

    bool WritePurpose(const std::string& strAddress, const std::string& purpose);
    bool ErasePurpose(const std::string& strAddress);

    bool WriteTx(const CVaultTx& wtx);
    bool EraseTx(uint256 hash);

    bool WriteMasterKey(unsigned int nID, const CMasterKey& kMasterKey);

    bool WriteBestBlock(const CBlockLocator& locator);
    bool ReadBestBlock(CBlockLocator& locator);

    bool WriteBackupRecorded(bool recorded);
    bool ReadBackupRecorded(bool& recorded);
    bool WriteAgentAllotmentRecords(const std::vector<AgentAllotmentRecord>& records);
    bool ReadAgentAllotmentRecords(std::vector<AgentAllotmentRecord>& records);

    bool WriteOrderPosNext(int64_t nOrderPosNext);

    bool WriteMinVersion(int nVersion);

    bool WriteDescriptorKey(const uint256& desc_id, const CPubKey& pubkey, const CPrivKey& privkey);
    bool WriteCryptedDescriptorKey(const uint256& desc_id, const CPubKey& pubkey, const std::vector<unsigned char>& secret);
    bool WriteDescriptor(const uint256& desc_id, const VaultDescriptor& descriptor);
    bool WriteDescriptorDerivedCache(const CExtPubKey& xpub, const uint256& desc_id, uint32_t key_exp_index, uint32_t der_index);
    bool WriteDescriptorParentCache(const CExtPubKey& xpub, const uint256& desc_id, uint32_t key_exp_index);
    bool WriteDescriptorLastHardenedCache(const CExtPubKey& xpub, const uint256& desc_id, uint32_t key_exp_index);
    bool WriteDescriptorCacheItems(const uint256& desc_id, const DescriptorCache& cache);

    bool WriteLockedUTXO(const COutPoint& output);
    bool EraseLockedUTXO(const COutPoint& output);

    bool WriteAddressPreviouslySpent(const CTxDestination& dest, bool previously_spent);
    bool WriteAddressReceiveRequest(const CTxDestination& dest, const std::string& id, const std::string& receive_request);
    bool EraseAddressReceiveRequest(const CTxDestination& dest, const std::string& id);
    bool EraseAddressData(const CTxDestination& dest);

    bool WriteActiveScriptPubKeyMan(uint8_t type, const uint256& id, bool internal);
    bool EraseActiveScriptPubKeyMan(uint8_t type, bool internal);

    DBErrors LoadVault(CVault* pvault);

    bool WriteVaultFlags(const uint64_t flags);
    //! Begin a new transaction
    bool TxnBegin();
    //! Commit current transaction
    bool TxnCommit();
    //! Abort current transaction
    bool TxnAbort();
    bool HasActiveTxn() { return m_batch->HasActiveTxn(); }

    //! Registers db txn callback functions
    void RegisterTxnListener(const DbTxnListener& l);

private:
    std::unique_ptr<DatabaseBatch> m_batch;
    VaultDatabase& m_database;

    // External functions listening to the current db txn outcome.
    // Listeners are cleared at the end of the transaction.
    std::vector<DbTxnListener> m_txn_listeners;
};

/**
 * Executes the provided function 'func' within a database transaction context.
 *
 * This function ensures that all db modifications performed within 'func()' are
 * atomically committed to the db at the end of the process. And, in case of a
 * failure during execution, all performed changes are rolled back.
 *
 * @param database The db connection instance to perform the transaction on.
 * @param process_desc A description of the process being executed, used for logging purposes in the event of a failure.
 * @param func The function to be executed within the db txn context. It returns a boolean indicating whether to commit or roll back the txn.
 * @return true if the db txn executed successfully, false otherwise.
 */
bool RunWithinTxn(VaultDatabase& database, std::string_view process_desc, const std::function<bool(VaultBatch&)>& func);

//! Compacts vault database state if there are changes.
void MaybeCompactVaultDB(VaultContext& context);

bool LoadEncryptionKey(CVault* pvault, DataStream& ssKey, DataStream& ssValue, std::string& strErr);
} // namespace vault

#endif // QUICKSILVER_VAULT_VAULTDB_H
