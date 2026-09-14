// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <quicksilver-build-config.h> // IWYU pragma: keep

#include <vault/vaultdb.h>

#include <common/system.h>
#include <key_io.h>
#include <protocol.h>
#include <script/script.h>
#include <serialize.h>
#include <sync.h>
#include <util/bip32.h>
#include <util/check.h>
#include <util/fs.h>
#include <util/time.h>
#include <util/translation.h>
#ifdef USE_SQLITE
#include <vault/sqlite.h>
#endif
#include <vault/vault.h>

#include <atomic>
#include <optional>
#include <string>

namespace vault {
namespace DBKeys {
const std::string ACTIVEEXTERNALSPK{"activeexternalspk"};
const std::string ACTIVEINTERNALSPK{"activeinternalspk"};
const std::string BESTBLOCK{"bestblock"};
const std::string DESTDATA{"destdata"};
const std::string FLAGS{"flags"};
const std::string LOCKED_UTXO{"lockedutxo"};
const std::string MASTER_KEY{"mkey"};
const std::string MINVERSION{"minversion"};
const std::string NAME{"name"};
const std::string ORDERPOSNEXT{"orderposnext"};
const std::string PURPOSE{"purpose"};
const std::string SETTINGS{"settings"};
const std::string TX{"tx"};
const std::string VERSION{"version"};
const std::string VAULTDESCRIPTOR{"vaultdescriptor"};
const std::string VAULTDESCRIPTORCACHE{"vaultdescriptorcache"};
const std::string VAULTDESCRIPTORLHCACHE{"vaultdescriptorlhcache"};
const std::string VAULTDESCRIPTORCKEY{"vaultdescriptorckey"};
const std::string VAULTDESCRIPTORKEY{"vaultdescriptorkey"};
} // namespace DBKeys

//
// VaultBatch
//

bool VaultBatch::WriteName(const std::string& strAddress, const std::string& strName)
{
    return WriteIC(std::make_pair(DBKeys::NAME, strAddress), strName);
}

bool VaultBatch::EraseName(const std::string& strAddress)
{
    // This should only be used for sending addresses, never for receiving addresses,
    // receiving addresses must always have an address book entry if they're not change return.
    return EraseIC(std::make_pair(DBKeys::NAME, strAddress));
}

bool VaultBatch::WritePurpose(const std::string& strAddress, const std::string& strPurpose)
{
    return WriteIC(std::make_pair(DBKeys::PURPOSE, strAddress), strPurpose);
}

bool VaultBatch::ErasePurpose(const std::string& strAddress)
{
    return EraseIC(std::make_pair(DBKeys::PURPOSE, strAddress));
}

bool VaultBatch::WriteTx(const CVaultTx& wtx)
{
    return WriteIC(std::make_pair(DBKeys::TX, wtx.GetHash()), wtx);
}

bool VaultBatch::EraseTx(uint256 hash)
{
    return EraseIC(std::make_pair(DBKeys::TX, hash));
}

bool VaultBatch::WriteMasterKey(unsigned int nID, const CMasterKey& kMasterKey)
{
    return WriteIC(std::make_pair(DBKeys::MASTER_KEY, nID), kMasterKey, true);
}

bool VaultBatch::WriteBestBlock(const CBlockLocator& locator)
{
    return WriteIC(DBKeys::BESTBLOCK, locator);
}

bool VaultBatch::ReadBestBlock(CBlockLocator& locator)
{
    return m_batch->Read(DBKeys::BESTBLOCK, locator);
}

bool VaultBatch::WriteBackupRecorded(bool recorded)
{
    return WriteIC(std::make_pair(DBKeys::SETTINGS, std::string{"backup_recorded"}), recorded);
}

bool VaultBatch::ReadBackupRecorded(bool& recorded)
{
    return m_batch->Read(std::make_pair(DBKeys::SETTINGS, std::string{"backup_recorded"}), recorded);
}

bool VaultBatch::WriteAgentAllotmentRecords(const std::vector<AgentAllotmentRecord>& records)
{
    return WriteIC(std::make_pair(DBKeys::SETTINGS, std::string{"agent_allotment_records"}), records);
}

bool VaultBatch::ReadAgentAllotmentRecords(std::vector<AgentAllotmentRecord>& records)
{
    records.clear();
    return m_batch->Read(std::make_pair(DBKeys::SETTINGS, std::string{"agent_allotment_records"}), records);
}

bool VaultBatch::WriteOrderPosNext(int64_t nOrderPosNext)
{
    return WriteIC(DBKeys::ORDERPOSNEXT, nOrderPosNext);
}

bool VaultBatch::WriteMinVersion(int nVersion)
{
    return WriteIC(DBKeys::MINVERSION, nVersion);
}

bool VaultBatch::WriteActiveScriptPubKeyMan(uint8_t type, const uint256& id, bool internal)
{
    std::string key = internal ? DBKeys::ACTIVEINTERNALSPK : DBKeys::ACTIVEEXTERNALSPK;
    return WriteIC(make_pair(key, type), id);
}

bool VaultBatch::EraseActiveScriptPubKeyMan(uint8_t type, bool internal)
{
    const std::string key{internal ? DBKeys::ACTIVEINTERNALSPK : DBKeys::ACTIVEEXTERNALSPK};
    return EraseIC(make_pair(key, type));
}

bool VaultBatch::WriteDescriptorKey(const uint256& desc_id, const CPubKey& pubkey, const CPrivKey& privkey)
{
    // hash pubkey/privkey to accelerate vault load
    std::vector<unsigned char> key;
    key.reserve(pubkey.size() + privkey.size());
    key.insert(key.end(), pubkey.begin(), pubkey.end());
    key.insert(key.end(), privkey.begin(), privkey.end());

    return WriteIC(std::make_pair(DBKeys::VAULTDESCRIPTORKEY, std::make_pair(desc_id, pubkey)), std::make_pair(privkey, Hash(key)), false);
}

bool VaultBatch::WriteCryptedDescriptorKey(const uint256& desc_id, const CPubKey& pubkey, const std::vector<unsigned char>& secret)
{
    if (!WriteIC(std::make_pair(DBKeys::VAULTDESCRIPTORCKEY, std::make_pair(desc_id, pubkey)), secret, false)) {
        return false;
    }
    EraseIC(std::make_pair(DBKeys::VAULTDESCRIPTORKEY, std::make_pair(desc_id, pubkey)));
    return true;
}

bool VaultBatch::WriteDescriptor(const uint256& desc_id, const VaultDescriptor& descriptor)
{
    return WriteIC(make_pair(DBKeys::VAULTDESCRIPTOR, desc_id), descriptor);
}

bool VaultBatch::WriteDescriptorDerivedCache(const CExtPubKey& xpub, const uint256& desc_id, uint32_t key_exp_index, uint32_t der_index)
{
    std::vector<unsigned char> ser_xpub(BIP32_EXTKEY_SIZE);
    xpub.Encode(ser_xpub.data());
    return WriteIC(std::make_pair(std::make_pair(DBKeys::VAULTDESCRIPTORCACHE, desc_id), std::make_pair(key_exp_index, der_index)), ser_xpub);
}

bool VaultBatch::WriteDescriptorParentCache(const CExtPubKey& xpub, const uint256& desc_id, uint32_t key_exp_index)
{
    std::vector<unsigned char> ser_xpub(BIP32_EXTKEY_SIZE);
    xpub.Encode(ser_xpub.data());
    return WriteIC(std::make_pair(std::make_pair(DBKeys::VAULTDESCRIPTORCACHE, desc_id), key_exp_index), ser_xpub);
}

bool VaultBatch::WriteDescriptorLastHardenedCache(const CExtPubKey& xpub, const uint256& desc_id, uint32_t key_exp_index)
{
    std::vector<unsigned char> ser_xpub(BIP32_EXTKEY_SIZE);
    xpub.Encode(ser_xpub.data());
    return WriteIC(std::make_pair(std::make_pair(DBKeys::VAULTDESCRIPTORLHCACHE, desc_id), key_exp_index), ser_xpub);
}

bool VaultBatch::WriteDescriptorCacheItems(const uint256& desc_id, const DescriptorCache& cache)
{
    for (const auto& parent_xpub_pair : cache.GetCachedParentExtPubKeys()) {
        if (!WriteDescriptorParentCache(parent_xpub_pair.second, desc_id, parent_xpub_pair.first)) {
            return false;
        }
    }
    for (const auto& derived_xpub_map_pair : cache.GetCachedDerivedExtPubKeys()) {
        for (const auto& derived_xpub_pair : derived_xpub_map_pair.second) {
            if (!WriteDescriptorDerivedCache(derived_xpub_pair.second, desc_id, derived_xpub_map_pair.first, derived_xpub_pair.first)) {
                return false;
            }
        }
    }
    for (const auto& lh_xpub_pair : cache.GetCachedLastHardenedExtPubKeys()) {
        if (!WriteDescriptorLastHardenedCache(lh_xpub_pair.second, desc_id, lh_xpub_pair.first)) {
            return false;
        }
    }
    return true;
}

bool VaultBatch::WriteLockedUTXO(const COutPoint& output)
{
    return WriteIC(std::make_pair(DBKeys::LOCKED_UTXO, std::make_pair(output.hash, output.n)), uint8_t{'1'});
}

bool VaultBatch::EraseLockedUTXO(const COutPoint& output)
{
    return EraseIC(std::make_pair(DBKeys::LOCKED_UTXO, std::make_pair(output.hash, output.n)));
}

bool LoadEncryptionKey(CVault* pvault, DataStream& ssKey, DataStream& ssValue, std::string& strErr)
{
    LOCK(pvault->cs_vault);
    try {
        // Master encryption key is loaded into only the vault and not any of the ScriptPubKeyMans.
        unsigned int nID;
        ssKey >> nID;
        CMasterKey kMasterKey;
        ssValue >> kMasterKey;
        if(pvault->mapMasterKeys.count(nID) != 0)
        {
            strErr = strprintf("Error reading vault database: duplicate CMasterKey id %u", nID);
            return false;
        }
        pvault->mapMasterKeys[nID] = kMasterKey;
        if (pvault->nMasterKeyMaxID < nID)
            pvault->nMasterKeyMaxID = nID;

    } catch (const std::exception& e) {
        if (strErr.empty()) {
            strErr = e.what();
        }
        return false;
    }
    return true;
}

static DBErrors LoadVaultFlags(CVault* pvault, DatabaseBatch& batch) EXCLUSIVE_LOCKS_REQUIRED(pvault->cs_vault)
{
    AssertLockHeld(pvault->cs_vault);
    uint64_t flags;
    if (batch.Read(DBKeys::FLAGS, flags)) {
        if (!pvault->LoadVaultFlags(flags)) {
            pvault->VaultLogPrintf("loaded ok=0 reason=unknown-nontolerable-flags");
            return DBErrors::TOO_NEW;
        }
    }
    return DBErrors::LOAD_OK;
}

struct LoadResult
{
    DBErrors m_result{DBErrors::LOAD_OK};
    int m_records{0};
};

using LoadFunc = std::function<DBErrors(CVault* pvault, DataStream& key, DataStream& value, std::string& err)>;
static LoadResult LoadRecords(CVault* pvault, DatabaseBatch& batch, const std::string& key, DataStream& prefix, LoadFunc load_func)
{
    LoadResult result;
    DataStream ssKey;
    DataStream ssValue{};

    Assume(!prefix.empty());
    std::unique_ptr<DatabaseCursor> cursor = batch.GetNewPrefixCursor(prefix);
    if (!cursor) {
        pvault->VaultLogPrintf("loaded ok=0 reason=cursor-failed record=%s", key);
        result.m_result = DBErrors::CORRUPT;
        return result;
    }

    while (true) {
        DatabaseCursor::Status status = cursor->Next(ssKey, ssValue);
        if (status == DatabaseCursor::Status::DONE) {
            break;
        } else if (status == DatabaseCursor::Status::FAIL) {
            pvault->VaultLogPrintf("loaded ok=0 reason=record-read-failed record=%s", key);
            result.m_result = DBErrors::CORRUPT;
            return result;
        }
        std::string type;
        ssKey >> type;
        assert(type == key);
        std::string error;
        DBErrors record_res = load_func(pvault, ssKey, ssValue, error);
        if (record_res != DBErrors::LOAD_OK) {
            pvault->VaultLogPrintf("loaded ok=0 detail=%s", error);
        }
        result.m_result = std::max(result.m_result, record_res);
        ++result.m_records;
    }
    return result;
}

static LoadResult LoadRecords(CVault* pvault, DatabaseBatch& batch, const std::string& key, LoadFunc load_func)
{
    DataStream prefix;
    prefix << key;
    return LoadRecords(pvault, batch, key, prefix, load_func);
}

template<typename... Args>
static DataStream PrefixStream(const Args&... args)
{
    DataStream prefix;
    SerializeMany(prefix, args...);
    return prefix;
}

static DBErrors LoadDescriptorVaultRecords(CVault* pvault, DatabaseBatch& batch, int last_client) EXCLUSIVE_LOCKS_REQUIRED(pvault->cs_vault)
{
    AssertLockHeld(pvault->cs_vault);

    // Load descriptor record
    int num_keys = 0;
    int num_ckeys= 0;
    LoadResult desc_res = LoadRecords(pvault, batch, DBKeys::VAULTDESCRIPTOR,
        [&batch, &num_keys, &num_ckeys, &last_client] (CVault* pvault, DataStream& key, DataStream& value, std::string& strErr) {
        DBErrors result = DBErrors::LOAD_OK;

        uint256 id;
        key >> id;
        VaultDescriptor desc;
        try {
            value >> desc;
        } catch (const std::ios_base::failure& e) {
            strErr = strprintf("Error: Unrecognized descriptor found in vault %s. ", pvault->GetName());
            strErr += (last_client > CLIENT_VERSION) ? "The vault might had been created on a newer version. " :
                    "The database might be corrupted or the software version is not compatible with one of your vault descriptors. ";
            strErr += "Please try running the latest software version";
            // Also include error details
            strErr = strprintf("%s\nDetails: %s", strErr, e.what());
            return DBErrors::UNKNOWN_DESCRIPTOR;
        }
        DescriptorScriptPubKeyMan& spkm = pvault->LoadDescriptorScriptPubKeyMan(id, desc);

        // Prior to doing anything with this spkm, verify ID compatibility
        if (id != spkm.GetID()) {
            strErr = "The descriptor ID calculated by the vault differs from the one in DB";
            return DBErrors::CORRUPT;
        }

        DescriptorCache cache;

        // Get key cache for this descriptor
        DataStream prefix = PrefixStream(DBKeys::VAULTDESCRIPTORCACHE, id);
        LoadResult key_cache_res = LoadRecords(pvault, batch, DBKeys::VAULTDESCRIPTORCACHE, prefix,
            [&id, &cache] (CVault* pvault, DataStream& key, DataStream& value, std::string& err) {
            bool parent = true;
            uint256 desc_id;
            uint32_t key_exp_index;
            uint32_t der_index;
            key >> desc_id;
            assert(desc_id == id);
            key >> key_exp_index;

            // if the der_index exists, it's a derived xpub
            try
            {
                key >> der_index;
                parent = false;
            }
            catch (...) {}

            std::vector<unsigned char> ser_xpub(BIP32_EXTKEY_SIZE);
            value >> ser_xpub;
            CExtPubKey xpub;
            xpub.Decode(ser_xpub.data());
            if (parent) {
                cache.CacheParentExtPubKey(key_exp_index, xpub);
            } else {
                cache.CacheDerivedExtPubKey(key_exp_index, der_index, xpub);
            }
            return DBErrors::LOAD_OK;
        });
        result = std::max(result, key_cache_res.m_result);

        // Get last hardened cache for this descriptor
        prefix = PrefixStream(DBKeys::VAULTDESCRIPTORLHCACHE, id);
        LoadResult lh_cache_res = LoadRecords(pvault, batch, DBKeys::VAULTDESCRIPTORLHCACHE, prefix,
            [&id, &cache] (CVault* pvault, DataStream& key, DataStream& value, std::string& err) {
            uint256 desc_id;
            uint32_t key_exp_index;
            key >> desc_id;
            assert(desc_id == id);
            key >> key_exp_index;

            std::vector<unsigned char> ser_xpub(BIP32_EXTKEY_SIZE);
            value >> ser_xpub;
            CExtPubKey xpub;
            xpub.Decode(ser_xpub.data());
            cache.CacheLastHardenedExtPubKey(key_exp_index, xpub);
            return DBErrors::LOAD_OK;
        });
        result = std::max(result, lh_cache_res.m_result);

        // Set the cache for this descriptor
        auto spk_man = (DescriptorScriptPubKeyMan*)pvault->GetScriptPubKeyMan(id);
        assert(spk_man);
        spk_man->SetCache(cache);

        // Get unencrypted keys
        prefix = PrefixStream(DBKeys::VAULTDESCRIPTORKEY, id);
        LoadResult key_res = LoadRecords(pvault, batch, DBKeys::VAULTDESCRIPTORKEY, prefix,
            [&id, &spk_man] (CVault* pvault, DataStream& key, DataStream& value, std::string& strErr) {
            uint256 desc_id;
            CPubKey pubkey;
            key >> desc_id;
            assert(desc_id == id);
            key >> pubkey;
            if (!pubkey.IsValid())
            {
                strErr = "Error reading vault database: descriptor unencrypted key CPubKey corrupt";
                return DBErrors::CORRUPT;
            }
            CKey privkey;
            CPrivKey pkey;
            uint256 hash;

            value >> pkey;
            value >> hash;

            // hash pubkey/privkey to accelerate vault load
            std::vector<unsigned char> to_hash;
            to_hash.reserve(pubkey.size() + pkey.size());
            to_hash.insert(to_hash.end(), pubkey.begin(), pubkey.end());
            to_hash.insert(to_hash.end(), pkey.begin(), pkey.end());

            if (Hash(to_hash) != hash)
            {
                strErr = "Error reading vault database: descriptor unencrypted key CPubKey/CPrivKey corrupt";
                return DBErrors::CORRUPT;
            }

            if (!privkey.Load(pkey, pubkey, true))
            {
                strErr = "Error reading vault database: descriptor unencrypted key CPrivKey corrupt";
                return DBErrors::CORRUPT;
            }
            spk_man->AddKey(pubkey.GetID(), privkey);
            return DBErrors::LOAD_OK;
        });
        result = std::max(result, key_res.m_result);
        num_keys = key_res.m_records;

        // Get encrypted keys
        prefix = PrefixStream(DBKeys::VAULTDESCRIPTORCKEY, id);
        LoadResult ckey_res = LoadRecords(pvault, batch, DBKeys::VAULTDESCRIPTORCKEY, prefix,
            [&id, &spk_man] (CVault* pvault, DataStream& key, DataStream& value, std::string& err) {
            uint256 desc_id;
            CPubKey pubkey;
            key >> desc_id;
            assert(desc_id == id);
            key >> pubkey;
            if (!pubkey.IsValid())
            {
                err = "Error reading vault database: descriptor encrypted key CPubKey corrupt";
                return DBErrors::CORRUPT;
            }
            std::vector<unsigned char> privkey;
            value >> privkey;

            spk_man->AddCryptedKey(pubkey.GetID(), pubkey, privkey);
            return DBErrors::LOAD_OK;
        });
        result = std::max(result, ckey_res.m_result);
        num_ckeys = ckey_res.m_records;

        return result;
    });

    if (desc_res.m_result <= DBErrors::NONCRITICAL_ERROR) {
        // Only log if there are no critical errors
        pvault->VaultLogPrintf("loaded descriptors=%u keys_plaintext=%u keys_encrypted=%u keys_total=%u",
               desc_res.m_records, num_keys, num_ckeys, num_keys + num_ckeys);
    }

    return desc_res.m_result;
}

static DBErrors LoadAddressBookRecords(CVault* pvault, DatabaseBatch& batch) EXCLUSIVE_LOCKS_REQUIRED(pvault->cs_vault)
{
    AssertLockHeld(pvault->cs_vault);
    DBErrors result = DBErrors::LOAD_OK;

    // Load name record
    LoadResult name_res = LoadRecords(pvault, batch, DBKeys::NAME,
        [] (CVault* pvault, DataStream& key, DataStream& value, std::string& err) EXCLUSIVE_LOCKS_REQUIRED(pvault->cs_vault) {
        std::string strAddress;
        key >> strAddress;
        std::string label;
        value >> label;
        pvault->m_address_book[DecodeDestination(strAddress)].SetLabel(label);
        return DBErrors::LOAD_OK;
    });
    result = std::max(result, name_res.m_result);

    // Load purpose record
    LoadResult purpose_res = LoadRecords(pvault, batch, DBKeys::PURPOSE,
        [] (CVault* pvault, DataStream& key, DataStream& value, std::string& err) EXCLUSIVE_LOCKS_REQUIRED(pvault->cs_vault) {
        std::string strAddress;
        key >> strAddress;
        std::string purpose_str;
        value >> purpose_str;
        std::optional<AddressPurpose> purpose{PurposeFromString(purpose_str)};
        if (!purpose) {
            pvault->VaultLogPrintf("loaded purpose=%s address=%s warn=nonstandard-purpose", purpose_str, strAddress);
        }
        pvault->m_address_book[DecodeDestination(strAddress)].purpose = purpose;
        return DBErrors::LOAD_OK;
    });
    result = std::max(result, purpose_res.m_result);

    // A name record can outlive its purpose record: an archive restores addresses whose
    // purpose was never recorded, and a purpose string this build does not recognise
    // loads as unset above. Settle it here so every labelled entry carries a purpose and
    // readers do not each invent their own answer. Change entries keep none -- the
    // absent label is what makes them change.
    for (auto& [dest, entry] : pvault->m_address_book) {
        if (entry.IsChange() || entry.purpose.has_value()) continue;
        entry.purpose = pvault->IsMine(dest) != ISMINE_NO ? AddressPurpose::RECEIVE : AddressPurpose::SEND;
    }

    // Load destination data record
    LoadResult dest_res = LoadRecords(pvault, batch, DBKeys::DESTDATA,
        [] (CVault* pvault, DataStream& key, DataStream& value, std::string& err) EXCLUSIVE_LOCKS_REQUIRED(pvault->cs_vault) {
        std::string strAddress, strKey, strValue;
        key >> strAddress;
        key >> strKey;
        value >> strValue;
        const CTxDestination& dest{DecodeDestination(strAddress)};
        if (strKey.compare("used") == 0) {
            // Load "used" key indicating if an IsMine address has
            // previously been spent from with avoid_reuse option enabled.
            // The strValue is not used for anything currently, but could
            // hold more information in the future. Current values are just
            // "1" or "p" for present (which was written prior to
            // f5ba424cd44619d9b9be88b8593d69a7ba96db26).
            pvault->LoadAddressPreviouslySpent(dest);
        } else if (strKey.starts_with("rr")) {
            // Load "rr##" keys where ## is a decimal number, and strValue
            // is a serialized RecentRequestEntry object.
            pvault->LoadAddressReceiveRequest(dest, strKey.substr(2), strValue);
        }
        return DBErrors::LOAD_OK;
    });
    result = std::max(result, dest_res.m_result);

    return result;
}

static DBErrors LoadTxRecords(CVault* pvault, DatabaseBatch& batch) EXCLUSIVE_LOCKS_REQUIRED(pvault->cs_vault)
{
    AssertLockHeld(pvault->cs_vault);
    DBErrors result = DBErrors::LOAD_OK;

    // Load tx record
    LoadResult tx_res = LoadRecords(pvault, batch, DBKeys::TX,
        [] (CVault* pvault, DataStream& key, DataStream& value, std::string& err) EXCLUSIVE_LOCKS_REQUIRED(pvault->cs_vault) {
        DBErrors result = DBErrors::LOAD_OK;
        uint256 hash;
        key >> hash;
        // LoadToVault call below creates a new CVaultTx that fill_wtx
        // callback fills with transaction metadata.
        auto fill_wtx = [&](CVaultTx& wtx, bool new_tx) {
            if(!new_tx) {
                // There's some corruption here since the tx we just tried to load was already in the vault.
                err = "Error: Corrupt transaction found. This can be fixed by removing transactions from vault and rescanning.";
                result = DBErrors::CORRUPT;
                return false;
            }
            value >> wtx;
            if (wtx.GetHash() != hash)
                return false;

            if (wtx.nOrderPos == -1) {
                err = "Transaction record is missing its vault order position";
                result = DBErrors::CORRUPT;
                return false;
            }

            return true;
        };
        if (!pvault->LoadToVault(hash, fill_wtx)) {
            // Use std::max as fill_wtx may have already set result to CORRUPT
            result = std::max(result, DBErrors::NEED_RESCAN);
        }
        return result;
    });
    result = std::max(result, tx_res.m_result);

    // Load locked utxo record
    LoadResult locked_utxo_res = LoadRecords(pvault, batch, DBKeys::LOCKED_UTXO,
        [] (CVault* pvault, DataStream& key, DataStream& value, std::string& err) EXCLUSIVE_LOCKS_REQUIRED(pvault->cs_vault) {
        Txid hash;
        uint32_t n;
        key >> hash;
        key >> n;
        pvault->LockCoin(COutPoint(hash, n));
        return DBErrors::LOAD_OK;
    });
    result = std::max(result, locked_utxo_res.m_result);

    // Load orderposnext record
    // Note: There should only be one ORDERPOSNEXT record with nothing trailing the type
    LoadResult order_pos_res = LoadRecords(pvault, batch, DBKeys::ORDERPOSNEXT,
        [] (CVault* pvault, DataStream& key, DataStream& value, std::string& err) EXCLUSIVE_LOCKS_REQUIRED(pvault->cs_vault) {
        try {
            value >> pvault->nOrderPosNext;
        } catch (const std::exception& e) {
            err = e.what();
            return DBErrors::NONCRITICAL_ERROR;
        }
        return DBErrors::LOAD_OK;
    });
    result = std::max(result, order_pos_res.m_result);

    // After loading all tx records, abandon any coinbase that is no longer in the active chain.
    // This could happen during an external vault load, or if the user replaced the chain data.
    for (auto& [id, wtx] : pvault->mapVault) {
        if (wtx.IsCoinBase() && wtx.isInactive()) {
            pvault->AbandonTransaction(wtx);
        }
    }

    return result;
}

static DBErrors LoadActiveSPKMs(CVault* pvault, DatabaseBatch& batch) EXCLUSIVE_LOCKS_REQUIRED(pvault->cs_vault)
{
    AssertLockHeld(pvault->cs_vault);
    DBErrors result = DBErrors::LOAD_OK;

    // Load spk records
    std::set<std::pair<OutputType, bool>> seen_spks;
    for (const auto& spk_key : {DBKeys::ACTIVEEXTERNALSPK, DBKeys::ACTIVEINTERNALSPK}) {
        LoadResult spkm_res = LoadRecords(pvault, batch, spk_key,
            [&seen_spks, &spk_key] (CVault* pvault, DataStream& key, DataStream& value, std::string& strErr) {
            uint8_t output_type;
            key >> output_type;
            uint256 id;
            value >> id;

            bool internal = spk_key == DBKeys::ACTIVEINTERNALSPK;
            const std::optional<OutputType> type{OutputTypeFromStored(output_type)};
            if (!type) {
                strErr = strprintf("Unknown stored address type %d for ScriptPubKeyMan %s", output_type, id.ToString());
                return DBErrors::CORRUPT;
            }
            auto [it, insert] = seen_spks.emplace(*type, internal);
            if (!insert) {
                strErr = "Multiple ScriptpubKeyMans specified for a single type";
                return DBErrors::CORRUPT;
            }
            pvault->LoadActiveScriptPubKeyMan(id, *type, /*internal=*/internal);
            return DBErrors::LOAD_OK;
        });
        result = std::max(result, spkm_res.m_result);
    }
    return result;
}

static DBErrors LoadMinVersion(CVault* pvault, DatabaseBatch& batch) EXCLUSIVE_LOCKS_REQUIRED(pvault->cs_vault)
{
    AssertLockHeld(pvault->cs_vault);
    int nMinVersion = 0;
    if (batch.Read(DBKeys::MINVERSION, nMinVersion)) {
        pvault->VaultLogPrintf("loaded vault_file_version=%d", nMinVersion);
        if (nMinVersion > VAULT_FILE_VERSION) {
            return DBErrors::TOO_NEW;
        }
        pvault->LoadMinVersion(nMinVersion);
    }
    return DBErrors::LOAD_OK;
}

static DBErrors LoadDecryptionKeys(CVault* pvault, DatabaseBatch& batch) EXCLUSIVE_LOCKS_REQUIRED(pvault->cs_vault)
{
    AssertLockHeld(pvault->cs_vault);

    // Load decryption key (mkey) records
    LoadResult mkey_res = LoadRecords(pvault, batch, DBKeys::MASTER_KEY,
        [] (CVault* pvault, DataStream& key, DataStream& value, std::string& err) {
        if (!LoadEncryptionKey(pvault, key, value, err)) {
            return DBErrors::CORRUPT;
        }
        return DBErrors::LOAD_OK;
    });
    return mkey_res.m_result;
}

DBErrors VaultBatch::LoadVault(CVault* pvault)
{
    DBErrors result = DBErrors::LOAD_OK;

    LOCK(pvault->cs_vault);

    // Last client version to open this vault
    int last_client = CLIENT_VERSION;
    bool has_last_client = m_batch->Read(DBKeys::VERSION, last_client);
    if (has_last_client) pvault->VaultLogPrintf("loaded last_client_version=%d", last_client);

    try {
        if ((result = LoadMinVersion(pvault, *m_batch)) != DBErrors::LOAD_OK) return result;

        // Load vault flags, so they are known when processing other records.
        // The FLAGS key is absent during vault creation.
        if ((result = LoadVaultFlags(pvault, *m_batch)) != DBErrors::LOAD_OK) return result;

#ifndef ENABLE_EXTERNAL_SIGNER
        if (pvault->IsVaultFlagSet(VAULT_FLAG_EXTERNAL_SIGNER)) {
            pvault->VaultLogPrintf("loaded ok=0 reason=external-signer-support-not-compiled");
            return DBErrors::EXTERNAL_SIGNER_SUPPORT_REQUIRED;
        }
#endif

        // Load descriptors
        result = std::max(LoadDescriptorVaultRecords(pvault, *m_batch, last_client), result);
        // Early return if there are unknown descriptors. Later loading of ACTIVEINTERNALSPK and ACTIVEEXTERNALEXPK
        // may reference the unknown descriptor's ID which can result in a misleading corruption error
        // when in reality the vault is simply too new.
        if (result == DBErrors::UNKNOWN_DESCRIPTOR) return result;

        // Load address book
        result = std::max(LoadAddressBookRecords(pvault, *m_batch), result);

        // Load tx records
        result = std::max(LoadTxRecords(pvault, *m_batch), result);

        // Load SPKMs
        result = std::max(LoadActiveSPKMs(pvault, *m_batch), result);

        // Load decryption keys
        result = std::max(LoadDecryptionKeys(pvault, *m_batch), result);
    } catch (...) {
        // Exceptions that can be ignored or treated as non-critical are handled by the individual loading functions.
        // Any uncaught exceptions will be caught here and treated as critical.
        result = DBErrors::CORRUPT;
    }

    // Any vault corruption at all: skip any rewriting or
    // upgrading, we don't want to make it worse.
    if (result != DBErrors::LOAD_OK)
        return result;

    if (pvault->IsVaultFlagSet(VAULT_FLAG_DISABLE_PRIVATE_KEYS) && pvault->HasEncryptionKeys()) {
        pvault->VaultLogPrintf("loaded ok=0 reason=encryption-keys-with-private-keys-disabled");
        return DBErrors::CORRUPT;
    }

    if (!has_last_client || last_client != CLIENT_VERSION) // Update
        m_batch->Write(DBKeys::VERSION, CLIENT_VERSION);

    return result;
}

static bool RunWithinTxn(VaultBatch& batch, std::string_view process_desc, const std::function<bool(VaultBatch&)>& func)
{
    if (!batch.TxnBegin()) {
        LogDebug(HgLog::VAULTDB, "Error: cannot create db txn for %s\n", process_desc);
        return false;
    }

    // Run procedure
    if (!func(batch)) {
        LogDebug(HgLog::VAULTDB, "Error: %s failed\n", process_desc);
        batch.TxnAbort();
        return false;
    }

    if (!batch.TxnCommit()) {
        LogDebug(HgLog::VAULTDB, "Error: cannot commit db txn for %s\n", process_desc);
        return false;
    }

    // All good
    return true;
}

bool RunWithinTxn(VaultDatabase& database, std::string_view process_desc, const std::function<bool(VaultBatch&)>& func)
{
    VaultBatch batch(database);
    return RunWithinTxn(batch, process_desc, func);
}

void MaybeCompactVaultDB(VaultContext& context)
{
    static std::atomic<bool> fOneThread(false);
    if (fOneThread.exchange(true)) {
        return;
    }

    for (const std::shared_ptr<CVault>& pvault : GetVaults(context)) {
        VaultDatabase& dbh = pvault->GetDatabase();

        unsigned int nUpdateCounter = dbh.nUpdateCounter;

        if (dbh.nLastSeen != nUpdateCounter) {
            dbh.nLastSeen = nUpdateCounter;
            dbh.nLastVaultUpdate = GetTime();
        }

        if (dbh.nLastFlushed != nUpdateCounter && GetTime() - dbh.nLastVaultUpdate >= 2) {
            if (dbh.PeriodicFlush()) {
                dbh.nLastFlushed = nUpdateCounter;
            }
        }
    }

    fOneThread = false;
}

bool VaultBatch::WriteAddressPreviouslySpent(const CTxDestination& dest, bool previously_spent)
{
    auto key{std::make_pair(DBKeys::DESTDATA, std::make_pair(EncodeDestination(dest), std::string("used")))};
    return previously_spent ? WriteIC(key, std::string("1")) : EraseIC(key);
}

bool VaultBatch::WriteAddressReceiveRequest(const CTxDestination& dest, const std::string& id, const std::string& receive_request)
{
    return WriteIC(std::make_pair(DBKeys::DESTDATA, std::make_pair(EncodeDestination(dest), "rr" + id)), receive_request);
}

bool VaultBatch::EraseAddressReceiveRequest(const CTxDestination& dest, const std::string& id)
{
    return EraseIC(std::make_pair(DBKeys::DESTDATA, std::make_pair(EncodeDestination(dest), "rr" + id)));
}

bool VaultBatch::EraseAddressData(const CTxDestination& dest)
{
    DataStream prefix;
    prefix << DBKeys::DESTDATA << EncodeDestination(dest);
    return m_batch->ErasePrefix(prefix);
}

bool VaultBatch::WriteVaultFlags(const uint64_t flags)
{
    return WriteIC(DBKeys::FLAGS, flags);
}

bool VaultBatch::TxnBegin()
{
    return m_batch->TxnBegin();
}

bool VaultBatch::TxnCommit()
{
    bool res = m_batch->TxnCommit();
    if (res) {
        for (const auto& listener : m_txn_listeners) {
            listener.on_commit();
        }
        // txn finished, clear listeners
        m_txn_listeners.clear();
    }
    return res;
}

bool VaultBatch::TxnAbort()
{
    bool res = m_batch->TxnAbort();
    if (res) {
        for (const auto& listener : m_txn_listeners) {
            listener.on_abort();
        }
        // txn finished, clear listeners
        m_txn_listeners.clear();
    }
    return res;
}

void VaultBatch::RegisterTxnListener(const DbTxnListener& l)
{
    assert(m_batch->HasActiveTxn());
    m_txn_listeners.emplace_back(l);
}

std::unique_ptr<VaultDatabase> MakeDatabase(const fs::path& path, const DatabaseOptions& options, DatabaseStatus& status, bilingual_str& error)
{
    bool exists;
    try {
        exists = fs::symlink_status(path).type() != fs::file_type::not_found;
    } catch (const fs::filesystem_error& e) {
        error = Untranslated(strprintf("Failed to access database path '%s': %s", fs::PathToString(path), fsbridge::get_filesystem_error_message(e)));
        status = DatabaseStatus::FAILED_BAD_PATH;
        return nullptr;
    }

    std::optional<DatabaseFormat> format;
    if (exists) {
        if (IsSQLiteFile(SQLiteDataFile(path))) {
            format = DatabaseFormat::SQLITE;
        }
    } else if (options.require_existing) {
        error = Untranslated(strprintf("Failed to load database path '%s'. Path does not exist.", fs::PathToString(path)));
        status = DatabaseStatus::FAILED_NOT_FOUND;
        return nullptr;
    }

    if (!format && options.require_existing) {
        error = Untranslated(strprintf("Failed to load database path '%s'. Data is not in recognized format.", fs::PathToString(path)));
        status = DatabaseStatus::FAILED_BAD_FORMAT;
        return nullptr;
    }

    if (format && options.require_create) {
        error = Untranslated(strprintf("Failed to create database path '%s'. Database already exists.", fs::PathToString(path)));
        status = DatabaseStatus::FAILED_ALREADY_EXISTS;
        return nullptr;
    }

    // A db already exists so format is set, but options also specifies the format, so make sure they agree
    if (format && options.require_format && format != options.require_format) {
        error = Untranslated(strprintf("Failed to load database path '%s'. Data is not in required format.", fs::PathToString(path)));
        status = DatabaseStatus::FAILED_BAD_FORMAT;
        return nullptr;
    }

    // Format is not set when a db doesn't already exist, so use the format specified by the options if it is set.
    if (!format && options.require_format) format = options.require_format;

    // If the format is not specified or detected, choose the only supported vault format.
    if (!format) {
#ifdef USE_SQLITE
        format = DatabaseFormat::SQLITE;
#endif
    }

    if (format == DatabaseFormat::SQLITE) {
#ifdef USE_SQLITE
        if constexpr (true) {
            return MakeSQLiteDatabase(path, options, status, error);
        } else
#endif
        {
            error = Untranslated(strprintf("Failed to open database path '%s'. Build does not support SQLite database format.", fs::PathToString(path)));
            status = DatabaseStatus::FAILED_BAD_FORMAT;
            return nullptr;
        }
    }

    error = Untranslated(strprintf("Failed to open database path '%s'. Data is not in supported SQLite vault format.", fs::PathToString(path)));
    status = DatabaseStatus::FAILED_BAD_FORMAT;
    return nullptr;
}
} // namespace vault
