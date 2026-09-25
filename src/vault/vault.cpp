// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <vault/vault.h>

#include <addresstype.h>
#include <agent/allotmentpolicy.h>
#include <blockfilter.h>
#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <common/args.h>
#include <common/messages.h>
#include <common/settings.h>
#include <common/signmessage.h>
#include <common/system.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <consensus/validation.h>
#include <external_signer.h>
#include <interfaces/chain.h>
#include <interfaces/handler.h>
#include <interfaces/vault.h>
#include <kernel/chain.h>
#include <kernel/relaypool_removal_reason.h>
#include <key.h>
#include <key_io.h>
#include <logging.h>
#include <node/types.h>
#include <outputtype.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <psqt.h>
#include <pubkey.h>
#include <quicksilver-build-config.h> // IWYU pragma: keep
#include <random.h>
#include <script/descriptor.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <script/solver.h>
#include <serialize.h>
#include <span.h>
#include <streams.h>
#include <support/allocators/secure.h>
#include <support/allocators/zeroafterfree.h>
#include <support/cleanse.h>
#include <sync.h>
#include <tinyformat.h>
#include <uint256.h>
#include <univalue.h>
#include <util/check.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/moneystr.h>
#include <util/result.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/time.h>
#include <util/translation.h>
#include <vault/coincontrol.h>
#include <vault/context.h>
#include <vault/crypter.h>
#include <vault/db.h>
#include <vault/external_signer_scriptpubkeyman.h>
#include <vault/scriptpubkeyman.h>
#include <vault/transaction.h>
#include <vault/types.h>
#include <vault/vaultdb.h>
#include <vault/vaultutil.h>

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <exception>
#include <map>
#include <optional>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>

struct KeyOriginInfo;

using common::AmountErrMsg;
using common::AmountHighWarn;
using common::PSQTError;
using interfaces::FoundBlock;
using util::ReplaceAll;
using util::ToString;

namespace vault {

bool AddVaultSetting(interfaces::Chain& chain, const std::string& vault_name)
{
    const auto update_function = [&vault_name](common::SettingsValue& setting_value) {
        if (!setting_value.isArray()) setting_value.setArray();
        for (const auto& value : setting_value.getValues()) {
            if (value.isStr() && value.get_str() == vault_name) return interfaces::SettingsAction::SKIP_WRITE;
        }
        setting_value.push_back(vault_name);
        return interfaces::SettingsAction::WRITE;
    };
    return chain.updateRwSetting("vault", update_function);
}

bool RemoveVaultSetting(interfaces::Chain& chain, const std::string& vault_name)
{
    const auto update_function = [&vault_name](common::SettingsValue& setting_value) {
        if (!setting_value.isArray()) return interfaces::SettingsAction::SKIP_WRITE;
        common::SettingsValue new_value(common::SettingsValue::VARR);
        for (const auto& value : setting_value.getValues()) {
            if (!value.isStr() || value.get_str() != vault_name) new_value.push_back(value);
        }
        if (new_value.size() == setting_value.size()) return interfaces::SettingsAction::SKIP_WRITE;
        setting_value = std::move(new_value);
        return interfaces::SettingsAction::WRITE;
    };
    return chain.updateRwSetting("vault", update_function);
}

static void UpdateVaultSetting(interfaces::Chain& chain,
                               const std::string& vault_name,
                               std::optional<bool> load_on_startup,
                               std::vector<bilingual_str>& warnings)
{
    if (!load_on_startup) return;
    if (load_on_startup.value() && !AddVaultSetting(chain, vault_name)) {
        warnings.emplace_back(Untranslated("Vault load on startup setting could not be updated, so vault may not be loaded next node startup."));
    } else if (!load_on_startup.value() && !RemoveVaultSetting(chain, vault_name)) {
        warnings.emplace_back(Untranslated("Vault load on startup setting could not be updated, so vault may still be loaded next node startup."));
    }
}

/**
 * Refresh relaypool status so the vault is in an internally consistent state and
 * immediately knows the transaction's status: Whether it can be considered
 * trusted and is eligible to be abandoned ...
 */
static void RefreshRelayPoolStatus(CVaultTx& tx, interfaces::Chain& chain)
{
    if (chain.isInRelayPool(tx.GetHash())) {
        tx.m_state = TxStateInRelayPool();
    } else if (tx.state<TxStateInRelayPool>()) {
        tx.m_state = TxStateInactive();
    }
}

bool AddVault(VaultContext& context, const std::shared_ptr<CVault>& vault)
{
    LOCK(context.vaults_mutex);
    assert(vault);
    std::vector<std::shared_ptr<CVault>>::const_iterator i = std::find(context.vaults.begin(), context.vaults.end(), vault);
    if (i != context.vaults.end()) return false;
    context.vaults.push_back(vault);
    vault->ConnectScriptPubKeyManNotifiers();
    vault->NotifyCanGetAddressesChanged();
    return true;
}

bool RemoveVault(VaultContext& context, const std::shared_ptr<CVault>& vault, std::optional<bool> load_on_start, std::vector<bilingual_str>& warnings)
{
    assert(vault);

    interfaces::Chain& chain = vault->chain();
    std::string name = vault->GetName();

    // Unregister with the validation interface which also drops shared pointers.
    vault->m_chain_notifications_handler.reset();
    {
        LOCK(context.vaults_mutex);
        std::vector<std::shared_ptr<CVault>>::iterator i = std::find(context.vaults.begin(), context.vaults.end(), vault);
        if (i == context.vaults.end()) return false;
        context.vaults.erase(i);
    }
    // Notify unload so that upper layers release the shared pointer.
    vault->NotifyUnload();

    // Write the vault setting
    UpdateVaultSetting(chain, name, load_on_start, warnings);

    return true;
}

bool RemoveVault(VaultContext& context, const std::shared_ptr<CVault>& vault, std::optional<bool> load_on_start)
{
    std::vector<bilingual_str> warnings;
    return RemoveVault(context, vault, load_on_start, warnings);
}

std::vector<std::shared_ptr<CVault>> GetVaults(VaultContext& context)
{
    LOCK(context.vaults_mutex);
    return context.vaults;
}

std::shared_ptr<CVault> GetDefaultVault(VaultContext& context, size_t& count)
{
    LOCK(context.vaults_mutex);
    count = context.vaults.size();
    return count == 1 ? context.vaults[0] : nullptr;
}

std::shared_ptr<CVault> GetVault(VaultContext& context, const std::string& name)
{
    LOCK(context.vaults_mutex);
    for (const std::shared_ptr<CVault>& vault : context.vaults) {
        if (vault->GetName() == name) return vault;
    }
    return nullptr;
}

std::unique_ptr<interfaces::Handler> HandleLoadVault(VaultContext& context, LoadVaultFn load_vault)
{
    LOCK(context.vaults_mutex);
    auto it = context.vault_load_fns.emplace(context.vault_load_fns.end(), std::move(load_vault));
    return interfaces::MakeCleanupHandler([&context, it] { LOCK(context.vaults_mutex); context.vault_load_fns.erase(it); });
}

void NotifyVaultLoaded(VaultContext& context, const std::shared_ptr<CVault>& vault)
{
    LOCK(context.vaults_mutex);
    for (auto& load_vault : context.vault_load_fns) {
        load_vault(interfaces::MakeVault(context, vault));
    }
}

static GlobalMutex g_loading_vault_mutex;
static GlobalMutex g_vault_release_mutex;
static std::condition_variable g_vault_release_cv;
static std::set<std::string> g_loading_vault_set GUARDED_BY(g_loading_vault_mutex);
static std::set<std::string> g_unloading_vault_set GUARDED_BY(g_vault_release_mutex);

// Custom deleter for shared_ptr<CVault>.
static void FlushAndDeleteVault(CVault* vault)
{
    const std::string name = vault->GetName();
    vault->VaultLogPrintf("down reason=released");
    vault->Flush();
    delete vault;
    // Vault is now released, notify WaitForDeleteVault, if any.
    {
        LOCK(g_vault_release_mutex);
        if (g_unloading_vault_set.erase(name) == 0) {
            // WaitForDeleteVault was not called for this vault, all done.
            return;
        }
    }
    g_vault_release_cv.notify_all();
}

void WaitForDeleteVault(std::shared_ptr<CVault>&& vault)
{
    // Mark vault for unloading.
    const std::string name = vault->GetName();
    {
        LOCK(g_vault_release_mutex);
        g_unloading_vault_set.insert(name);
        // Do not expect to be the only one removing this vault.
        // Multiple threads could simultaneously be waiting for deletion.
    }

    // Time to ditch our shared_ptr and wait for FlushAndDeleteVault call.
    vault.reset();
    {
        WAIT_LOCK(g_vault_release_mutex, lock);
        while (g_unloading_vault_set.count(name) == 1) {
            g_vault_release_cv.wait(lock);
        }
    }
}

namespace {
std::shared_ptr<CVault> LoadVaultInternal(VaultContext& context, const std::string& name, std::optional<bool> load_on_start, const DatabaseOptions& options, DatabaseStatus& status, bilingual_str& error, std::vector<bilingual_str>& warnings)
{
    try {
        std::unique_ptr<VaultDatabase> database = MakeVaultDatabase(name, options, status, error);
        if (!database) {
            error = Untranslated("Vault file verification failed.") + Untranslated(" ") + error;
            return nullptr;
        }

        context.chain->initMessage(_("Loading vault…"));
        std::shared_ptr<CVault> vault = CVault::Create(context, name, std::move(database), options.create_flags, error, warnings);
        if (!vault) {
            error = Untranslated("Vault loading failed.") + Untranslated(" ") + error;
            status = DatabaseStatus::FAILED_LOAD;
            return nullptr;
        }

        if (!vault->IsVaultFlagSet(VAULT_FLAG_DESCRIPTORS)) {
            error = Untranslated("Vault is missing required descriptor support.");
            status = DatabaseStatus::FAILED_LOAD;
            return nullptr;
        }

        NotifyVaultLoaded(context, vault);
        AddVault(context, vault);
        vault->postInitProcess();

        // Write the vault setting
        UpdateVaultSetting(*context.chain, name, load_on_start, warnings);

        return vault;
    } catch (const std::runtime_error& e) {
        error = Untranslated(e.what());
        status = DatabaseStatus::FAILED_LOAD;
        return nullptr;
    }
}

class FastVaultRescanFilter
{
public:
    FastVaultRescanFilter(const CVault& vault) : m_vault(vault)
    {
        assert(m_vault.IsVaultFlagSet(VAULT_FLAG_DESCRIPTORS));

        // create initial filter with scripts from all ScriptPubKeyMans
        for (auto spkm : m_vault.GetAllScriptPubKeyMans()) {
            auto desc_spkm{dynamic_cast<DescriptorScriptPubKeyMan*>(spkm)};
            assert(desc_spkm != nullptr);
            AddScriptPubKeys(desc_spkm);
            // save each range descriptor's end for possible future filter updates
            if (desc_spkm->IsHDEnabled()) {
                m_last_range_ends.emplace(desc_spkm->GetID(), desc_spkm->GetEndRange());
            }
        }
    }

    void UpdateIfNeeded()
    {
        // repopulate filter with new scripts if top-up has happened since last iteration
        for (const auto& [desc_spkm_id, last_range_end] : m_last_range_ends) {
            auto desc_spkm{dynamic_cast<DescriptorScriptPubKeyMan*>(m_vault.GetScriptPubKeyMan(desc_spkm_id))};
            assert(desc_spkm != nullptr);
            int32_t current_range_end{desc_spkm->GetEndRange()};
            if (current_range_end > last_range_end) {
                AddScriptPubKeys(desc_spkm, last_range_end);
                m_last_range_ends.at(desc_spkm->GetID()) = current_range_end;
            }
        }
    }

    std::optional<bool> MatchesBlock(const uint256& block_hash) const
    {
        return m_vault.chain().blockFilterMatchesAny(BlockFilterType::BASIC, block_hash, m_filter_set);
    }

private:
    const CVault& m_vault;
    /** Map for keeping track of each range descriptor's last seen end range.
     * This information is used to detect whether new addresses were derived
     * (that is, if the current end range is larger than the saved end range)
     * after processing a block and hence a filter set update is needed to
     * take possible keypool top-ups into account.
     */
    std::map<uint256, int32_t> m_last_range_ends;
    GCSFilter::ElementSet m_filter_set;

    void AddScriptPubKeys(const DescriptorScriptPubKeyMan* desc_spkm, int32_t last_range_end = 0)
    {
        for (const auto& script_pub_key : desc_spkm->GetScriptPubKeys(last_range_end)) {
            m_filter_set.emplace(script_pub_key.begin(), script_pub_key.end());
        }
    }
};
} // namespace

std::shared_ptr<CVault> LoadVault(VaultContext& context, const std::string& name, std::optional<bool> load_on_start, const DatabaseOptions& options, DatabaseStatus& status, bilingual_str& error, std::vector<bilingual_str>& warnings)
{
    auto result = WITH_LOCK(g_loading_vault_mutex, return g_loading_vault_set.insert(name));
    if (!result.second) {
        error = Untranslated("Vault already loading.");
        status = DatabaseStatus::FAILED_LOAD;
        return nullptr;
    }
    auto vault = LoadVaultInternal(context, name, load_on_start, options, status, error, warnings);
    WITH_LOCK(g_loading_vault_mutex, g_loading_vault_set.erase(result.first));
    return vault;
}

std::shared_ptr<CVault> CreateVault(VaultContext& context, const std::string& name, std::optional<bool> load_on_start, DatabaseOptions& options, DatabaseStatus& status, bilingual_str& error, std::vector<bilingual_str>& warnings)
{
    uint64_t vault_creation_flags = options.create_flags | VAULT_FLAG_DESCRIPTORS;
    const SecureString& passphrase = options.create_passphrase;

    options.require_format = DatabaseFormat::SQLITE;

    // Indicate that the vault is actually supposed to be blank and not just blank to make it encrypted
    bool create_blank = (vault_creation_flags & VAULT_FLAG_BLANK_VAULT);

    // Born encrypted vaults need to be created blank first.
    if (!passphrase.empty()) {
        vault_creation_flags |= VAULT_FLAG_BLANK_VAULT;
    }

    // Private keys must be disabled for an external signer vault
    if ((vault_creation_flags & VAULT_FLAG_EXTERNAL_SIGNER) && !(vault_creation_flags & VAULT_FLAG_DISABLE_PRIVATE_KEYS)) {
        error = Untranslated("Private keys must be disabled when using an external signer");
        status = DatabaseStatus::FAILED_CREATE;
        return nullptr;
    }

    // Do not allow a passphrase when private keys are disabled
    if (!passphrase.empty() && (vault_creation_flags & VAULT_FLAG_DISABLE_PRIVATE_KEYS)) {
        error = Untranslated("Passphrase provided but private keys are disabled. A passphrase is only used to encrypt private keys, so cannot be used for vaults with private keys disabled.");
        status = DatabaseStatus::FAILED_CREATE;
        return nullptr;
    }

    // Vault::Verify will check if we're trying to create a vault with a duplicate name.
    std::unique_ptr<VaultDatabase> database = MakeVaultDatabase(name, options, status, error);
    if (!database) {
        error = Untranslated("Vault file verification failed.") + Untranslated(" ") + error;
        status = DatabaseStatus::FAILED_VERIFY;
        return nullptr;
    }

    // Make the vault
    context.chain->initMessage(_("Loading vault…"));
    std::shared_ptr<CVault> vault = CVault::Create(context, name, std::move(database), vault_creation_flags, error, warnings);
    if (!vault) {
        error = Untranslated("Vault creation failed.") + Untranslated(" ") + error;
        status = DatabaseStatus::FAILED_CREATE;
        return nullptr;
    }

    // Encrypt the vault
    if (!passphrase.empty() && !(vault_creation_flags & VAULT_FLAG_DISABLE_PRIVATE_KEYS)) {
        if (!vault->EncryptVault(passphrase)) {
            error = Untranslated("Error: Vault created but failed to encrypt.");
            status = DatabaseStatus::FAILED_ENCRYPT;
            return nullptr;
        }
        if (!create_blank) {
            // Unlock the vault
            if (!vault->Unlock(passphrase)) {
                error = Untranslated("Error: Vault was encrypted but could not be unlocked");
                status = DatabaseStatus::FAILED_ENCRYPT;
                return nullptr;
            }

            // Set a seed for the vault
            {
                LOCK(vault->cs_vault);
                vault->SetupDescriptorScriptPubKeyMans();
            }

            // Relock the vault
            vault->Lock();
        }
    }

    NotifyVaultLoaded(context, vault);
    AddVault(context, vault);
    vault->postInitProcess();

    // Write the vault settings
    UpdateVaultSetting(*context.chain, name, load_on_start, warnings);

    status = DatabaseStatus::SUCCESS;
    return vault;
}

// Re-creates vault from the backup file by renaming and moving it into the vault's directory.
// If 'load_after_restore=true', the vault object will be fully initialized and appended to the context.
std::shared_ptr<CVault> RestoreVault(VaultContext& context, const fs::path& backup_file, const std::string& vault_name, std::optional<bool> load_on_start, DatabaseStatus& status, bilingual_str& error, std::vector<bilingual_str>& warnings, bool load_after_restore)
{
    DatabaseOptions options;
    ReadDatabaseArgs(*context.args, options);
    options.require_existing = true;

    const fs::path vault_path = fsbridge::AbsPathJoin(GetVaultDir(), fs::u8path(vault_name));
    auto vault_file = vault_path / "vault.dat";
    std::shared_ptr<CVault> vault;

    try {
        if (!fs::exists(backup_file)) {
            error = Untranslated("Backup file does not exist");
            status = DatabaseStatus::FAILED_INVALID_BACKUP_FILE;
            return nullptr;
        }

        if (fs::exists(vault_path) || !TryCreateDirectories(vault_path)) {
            error = Untranslated(strprintf("Failed to create database path '%s'. Database already exists.", fs::PathToString(vault_path)));
            status = DatabaseStatus::FAILED_ALREADY_EXISTS;
            return nullptr;
        }

        fs::copy_file(backup_file, vault_file, fs::copy_options::none);

        if (load_after_restore) {
            vault = LoadVault(context, vault_name, load_on_start, options, status, error, warnings);
        }
    } catch (const std::exception& e) {
        assert(!vault);
        if (!error.empty()) error += Untranslated("\n");
        error += Untranslated(strprintf("Unexpected exception: %s", e.what()));
    }

    // Remove created vault path only when loading fails
    if (load_after_restore && !vault) {
        fs::remove_all(vault_path);
    }

    return vault;
}

/** @defgroup mapVault
 *
 * @{
 */

const CVaultTx* CVault::GetVaultTx(const uint256& hash) const
{
    AssertLockHeld(cs_vault);
    const auto it = mapVault.find(hash);
    if (it == mapVault.end())
        return nullptr;
    return &(it->second);
}

bool CVault::Unlock(const SecureString& strVaultPassphrase)
{
    CCrypter crypter;
    CKeyingMaterial _vMasterKey;

    {
        LOCK(cs_vault);
        for (const MasterKeyMap::value_type& pMasterKey : mapMasterKeys) {
            if (!crypter.SetKeyFromPassphrase(strVaultPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations))
                return false;
            if (!crypter.Decrypt(pMasterKey.second.vchCryptedKey, _vMasterKey))
                continue; // try another master key
            if (Unlock(_vMasterKey)) {
                return true;
            }
        }
    }
    return false;
}

bool CVault::ChangeVaultPassphrase(const SecureString& strOldVaultPassphrase, const SecureString& strNewVaultPassphrase)
{
    bool fWasLocked = IsLocked();

    {
        LOCK2(m_relock_mutex, cs_vault);
        Lock();

        CCrypter crypter;
        CKeyingMaterial _vMasterKey;
        for (MasterKeyMap::value_type& pMasterKey : mapMasterKeys) {
            if (!crypter.SetKeyFromPassphrase(strOldVaultPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations))
                return false;
            if (!crypter.Decrypt(pMasterKey.second.vchCryptedKey, _vMasterKey))
                return false;
            if (Unlock(_vMasterKey)) {
                constexpr MillisecondsDouble target{100};
                auto start{SteadyClock::now()};
                crypter.SetKeyFromPassphrase(strNewVaultPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations);
                pMasterKey.second.nDeriveIterations = static_cast<unsigned int>(pMasterKey.second.nDeriveIterations * target / (SteadyClock::now() - start));

                start = SteadyClock::now();
                crypter.SetKeyFromPassphrase(strNewVaultPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations);
                pMasterKey.second.nDeriveIterations = (pMasterKey.second.nDeriveIterations + static_cast<unsigned int>(pMasterKey.second.nDeriveIterations * target / (SteadyClock::now() - start))) / 2;

                if (pMasterKey.second.nDeriveIterations < 25000)
                    pMasterKey.second.nDeriveIterations = 25000;

                VaultLogPrintf("ready passphrase_changed=1 derive_iterations=%i", pMasterKey.second.nDeriveIterations);

                if (!crypter.SetKeyFromPassphrase(strNewVaultPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations))
                    return false;
                if (!crypter.Encrypt(_vMasterKey, pMasterKey.second.vchCryptedKey))
                    return false;
                VaultBatch(GetDatabase()).WriteMasterKey(pMasterKey.first, pMasterKey.second);
                if (fWasLocked)
                    Lock();
                return true;
            }
        }
    }

    return false;
}

void CVault::chainStateFlushed(const CBlockLocator& loc)
{
    // Don't update the best block until the chain is attached so that in case of a shutdown,
    // the rescan will be restarted at next startup.
    LOCK(m_best_block_mutex);
    if (m_attaching_chain) {
        return;
    }
    VaultBatch batch(GetDatabase());
    batch.WriteBestBlock(loc);
}

void CVault::SetMinVersion(VaultBatch* batch_in)
{
    LOCK(cs_vault);
    if (nVaultVersion >= VAULT_FILE_VERSION)
        return;
    VaultLogPrintf("ready minversion=%d", VAULT_FILE_VERSION);
    nVaultVersion = VAULT_FILE_VERSION;

    VaultBatch* batch = batch_in ? batch_in : new VaultBatch(GetDatabase());
    batch->WriteMinVersion(nVaultVersion);
    if (!batch_in)
        delete batch;
}

std::set<uint256> CVault::GetConflicts(const uint256& txid) const
{
    std::set<uint256> result;
    AssertLockHeld(cs_vault);

    const auto it = mapVault.find(txid);
    if (it == mapVault.end())
        return result;
    const CVaultTx& wtx = it->second;

    std::pair<TxSpends::const_iterator, TxSpends::const_iterator> range;

    for (const CTxIn& txin : wtx.tx->vin) {
        if (mapTxSpends.count(txin.prevout) <= 1)
            continue; // No conflict if zero or one spends
        range = mapTxSpends.equal_range(txin.prevout);
        for (TxSpends::const_iterator _it = range.first; _it != range.second; ++_it)
            result.insert(_it->second);
    }
    return result;
}

bool CVault::HasVaultSpend(const CTransactionRef& tx) const
{
    AssertLockHeld(cs_vault);
    const Txid& txid = tx->GetHash();
    for (unsigned int i = 0; i < tx->vout.size(); ++i) {
        if (IsSpent(COutPoint(txid, i))) {
            return true;
        }
    }
    return false;
}

void CVault::Flush()
{
    GetDatabase().Flush();
}

void CVault::Close()
{
    GetDatabase().Close();
}

void CVault::SyncMetaData(std::pair<TxSpends::iterator, TxSpends::iterator> range)
{
    // We want all the vault transactions in range to have the same metadata as
    // the oldest (smallest nOrderPos).
    // So: find smallest nOrderPos:

    int nMinOrderPos = std::numeric_limits<int>::max();
    const CVaultTx* copyFrom = nullptr;
    for (TxSpends::iterator it = range.first; it != range.second; ++it) {
        const CVaultTx* wtx = &mapVault.at(it->second);
        if (wtx->nOrderPos < nMinOrderPos) {
            nMinOrderPos = wtx->nOrderPos;
            copyFrom = wtx;
        }
    }

    if (!copyFrom) {
        return;
    }

    // Now copy data from copyFrom to rest:
    for (TxSpends::iterator it = range.first; it != range.second; ++it) {
        const uint256& hash = it->second;
        CVaultTx* copyTo = &mapVault.at(hash);
        if (copyFrom == copyTo) continue;
        assert(copyFrom && "Oldest vault transaction in range assumed to have been found.");
        if (!copyFrom->IsEquivalentTo(*copyTo)) continue;
        copyTo->mapValue = copyFrom->mapValue;
        copyTo->vOrderForm = copyFrom->vOrderForm;
        // nTimeReceived not copied on purpose
        copyTo->nTimeSmart = copyFrom->nTimeSmart;
        copyTo->fFromMe = copyFrom->fFromMe;
        // nOrderPos not copied on purpose
        // cached members not copied on purpose
    }
}

/**
 * Outpoint is spent if any non-conflicted transaction
 * spends it:
 */
bool CVault::IsSpent(const COutPoint& outpoint) const
{
    std::pair<TxSpends::const_iterator, TxSpends::const_iterator> range;
    range = mapTxSpends.equal_range(outpoint);

    for (TxSpends::const_iterator it = range.first; it != range.second; ++it) {
        const uint256& wtxid = it->second;
        const auto mit = mapVault.find(wtxid);
        if (mit != mapVault.end()) {
            const auto& wtx = mit->second;
            if (!wtx.isAbandoned() && !wtx.isBlockConflicted() && !wtx.isRelayPoolConflicted())
                return true; // Spent
        }
    }
    return false;
}

void CVault::AddToSpends(const COutPoint& outpoint, const uint256& wtxid, VaultBatch* batch)
{
    mapTxSpends.insert(std::make_pair(outpoint, wtxid));

    if (batch) {
        UnlockCoin(outpoint, batch);
    } else {
        VaultBatch temp_batch(GetDatabase());
        UnlockCoin(outpoint, &temp_batch);
    }

    std::pair<TxSpends::iterator, TxSpends::iterator> range;
    range = mapTxSpends.equal_range(outpoint);
    SyncMetaData(range);
}


void CVault::AddToSpends(const CVaultTx& wtx, VaultBatch* batch)
{
    if (wtx.IsCoinBase()) // Coinbases don't spend anything!
        return;

    for (const CTxIn& txin : wtx.tx->vin)
        AddToSpends(txin.prevout, wtx.GetHash(), batch);
}

bool CVault::EncryptVault(const SecureString& strVaultPassphrase)
{
    if (IsCrypted())
        return false;

    CKeyingMaterial _vMasterKey;

    _vMasterKey.resize(VAULT_CRYPTO_KEY_SIZE);
    GetStrongRandBytes(_vMasterKey);

    CMasterKey kMasterKey;

    kMasterKey.vchSalt.resize(VAULT_CRYPTO_SALT_SIZE);
    GetStrongRandBytes(kMasterKey.vchSalt);

    CCrypter crypter;
    constexpr MillisecondsDouble target{100};
    auto start{SteadyClock::now()};
    crypter.SetKeyFromPassphrase(strVaultPassphrase, kMasterKey.vchSalt, 25000);
    kMasterKey.nDeriveIterations = static_cast<unsigned int>(25000 * target / (SteadyClock::now() - start));

    start = SteadyClock::now();
    crypter.SetKeyFromPassphrase(strVaultPassphrase, kMasterKey.vchSalt, kMasterKey.nDeriveIterations);
    kMasterKey.nDeriveIterations = (kMasterKey.nDeriveIterations + static_cast<unsigned int>(kMasterKey.nDeriveIterations * target / (SteadyClock::now() - start))) / 2;

    if (kMasterKey.nDeriveIterations < 25000)
        kMasterKey.nDeriveIterations = 25000;

    VaultLogPrintf("ready encrypted=1 derive_iterations=%i", kMasterKey.nDeriveIterations);

    if (!crypter.SetKeyFromPassphrase(strVaultPassphrase, kMasterKey.vchSalt, kMasterKey.nDeriveIterations))
        return false;
    if (!crypter.Encrypt(_vMasterKey, kMasterKey.vchCryptedKey))
        return false;

    {
        LOCK2(m_relock_mutex, cs_vault);
        mapMasterKeys[++nMasterKeyMaxID] = kMasterKey;
        VaultBatch* encrypted_batch = new VaultBatch(GetDatabase());
        if (!encrypted_batch->TxnBegin()) {
            delete encrypted_batch;
            encrypted_batch = nullptr;
            return false;
        }
        encrypted_batch->WriteMasterKey(nMasterKeyMaxID, kMasterKey);

        for (const auto& spk_man_pair : m_spk_managers) {
            auto spk_man = spk_man_pair.second.get();
            if (!spk_man->Encrypt(_vMasterKey, encrypted_batch)) {
                encrypted_batch->TxnAbort();
                delete encrypted_batch;
                encrypted_batch = nullptr;
                // We now probably have half of our keys encrypted in memory, and half not...
                // die and let the user reload the unencrypted vault.
                assert(false);
            }
        }

        if (!encrypted_batch->TxnCommit()) {
            delete encrypted_batch;
            encrypted_batch = nullptr;
            // We now have keys encrypted in memory, but not on disk...
            // die to avoid confusion and let the user reload the unencrypted vault.
            assert(false);
        }

        delete encrypted_batch;
        encrypted_batch = nullptr;

        Lock();
        Unlock(strVaultPassphrase);

        // If we are using descriptors, make new descriptors with a new seed
        if (!IsVaultFlagSet(VAULT_FLAG_BLANK_VAULT)) {
            SetupDescriptorScriptPubKeyMans();
        }
        Lock();

        // Rewrite vault storage to avoid leaving unencrypted private key material in slack space.
        GetDatabase().Rewrite();
        GetDatabase().ReloadDbEnv();
    }
    NotifyStatusChanged(this);

    return true;
}

int64_t CVault::IncOrderPosNext(VaultBatch* batch)
{
    AssertLockHeld(cs_vault);
    int64_t nRet = nOrderPosNext++;
    if (batch) {
        batch->WriteOrderPosNext(nOrderPosNext);
    } else {
        VaultBatch(GetDatabase()).WriteOrderPosNext(nOrderPosNext);
    }
    return nRet;
}

void CVault::MarkDirty()
{
    {
        LOCK(cs_vault);
        for (std::pair<const uint256, CVaultTx>& item : mapVault)
            item.second.MarkDirty();
    }
}

void CVault::SetSpentKeyState(VaultBatch& batch, const uint256& hash, unsigned int n, bool used, std::set<CTxDestination>& tx_destinations)
{
    AssertLockHeld(cs_vault);
    const CVaultTx* srctx = GetVaultTx(hash);
    if (!srctx) return;

    CTxDestination dst;
    if (ExtractDestination(srctx->tx->vout[n].scriptPubKey, dst)) {
        if (IsMine(dst)) {
            if (used != IsAddressPreviouslySpent(dst)) {
                if (used) {
                    tx_destinations.insert(dst);
                }
                SetAddressPreviouslySpent(batch, dst, used);
            }
        }
    }
}

bool CVault::IsSpentKey(const CScript& scriptPubKey) const
{
    AssertLockHeld(cs_vault);
    CTxDestination dest;
    if (!ExtractDestination(scriptPubKey, dest)) {
        return false;
    }
    if (IsAddressPreviouslySpent(dest)) {
        return true;
    }

    return false;
}

CVaultTx* CVault::AddToVault(CTransactionRef tx, const TxState& state, const UpdateVaultTxFn& update_wtx, bool fFlushOnClose, bool rescanning_old_block)
{
    LOCK(cs_vault);

    VaultBatch batch(GetDatabase(), fFlushOnClose);

    uint256 hash = tx->GetHash();

    if (IsVaultFlagSet(VAULT_FLAG_AVOID_REUSE)) {
        // Mark used destinations
        std::set<CTxDestination> tx_destinations;

        for (const CTxIn& txin : tx->vin) {
            const COutPoint& op = txin.prevout;
            SetSpentKeyState(batch, op.hash, op.n, true, tx_destinations);
        }

        MarkDestinationsDirty(tx_destinations);
    }

    // Inserts only if not already there, returns tx inserted or tx found
    auto ret = mapVault.emplace(std::piecewise_construct, std::forward_as_tuple(hash), std::forward_as_tuple(tx, state));
    CVaultTx& wtx = (*ret.first).second;
    bool fInsertedNew = ret.second;
    bool fUpdated = update_wtx && update_wtx(wtx, fInsertedNew);
    if (fInsertedNew) {
        wtx.nTimeReceived = GetTime();
        wtx.nOrderPos = IncOrderPosNext(&batch);
        wtx.m_it_wtxOrdered = wtxOrdered.insert(std::make_pair(wtx.nOrderPos, &wtx));
        wtx.nTimeSmart = ComputeTimeSmart(wtx, rescanning_old_block);
        AddToSpends(wtx, &batch);

        // Update birth time when tx time is older than it.
        MaybeUpdateBirthTime(wtx.GetTxTime());
    }

    if (!fInsertedNew) {
        if (state.index() != wtx.m_state.index()) {
            wtx.m_state = state;
            fUpdated = true;
        } else {
            assert(TxStateSerializedIndex(wtx.m_state) == TxStateSerializedIndex(state));
            assert(TxStateSerializedBlockHash(wtx.m_state) == TxStateSerializedBlockHash(state));
        }
        // If we have a witness-stripped version of this transaction, and we
        // see a new version with a witness, then we must be upgrading a pre-segwit
        // vault.  Store the new version of the transaction with the witness,
        // as the stripped-version must be invalid.
        // TODO: Store all versions of the transaction, instead of just one.
        if (tx->HasWitness() && !wtx.tx->HasWitness()) {
            wtx.SetTx(tx);
            fUpdated = true;
        }
    }

    // Mark inactive coinbase transactions and their descendants as abandoned
    if (wtx.IsCoinBase() && wtx.isInactive()) {
        std::vector<CVaultTx*> txs{&wtx};

        TxStateInactive inactive_state = TxStateInactive{/*abandoned=*/true};

        while (!txs.empty()) {
            CVaultTx* desc_tx = txs.back();
            txs.pop_back();
            desc_tx->m_state = inactive_state;
            // Break caches since we have changed the state
            desc_tx->MarkDirty();
            batch.WriteTx(*desc_tx);
            MarkInputsDirty(desc_tx->tx);
            for (unsigned int i = 0; i < desc_tx->tx->vout.size(); ++i) {
                COutPoint outpoint(desc_tx->GetHash(), i);
                std::pair<TxSpends::const_iterator, TxSpends::const_iterator> range = mapTxSpends.equal_range(outpoint);
                for (TxSpends::const_iterator it = range.first; it != range.second; ++it) {
                    const auto wit = mapVault.find(it->second);
                    if (wit != mapVault.end()) {
                        txs.push_back(&wit->second);
                    }
                }
            }
        }
    }

    //// debug print
    VaultLogPrintf("saved tx=%s new=%d updated=%d state=%s", hash.ToString(), fInsertedNew, fUpdated, TxStateString(state));

    // Write to disk
    if (fInsertedNew || fUpdated)
        if (!batch.WriteTx(wtx))
            return nullptr;

    // Break debit/credit balance caches:
    wtx.MarkDirty();

    // Notify UI of new or updated transaction
    NotifyTransactionChanged(hash, fInsertedNew ? CT_NEW : CT_UPDATED);

#if HAVE_SYSTEM
    // notify an external script when a vault transaction comes in or is updated
    std::string strCmd = m_notify_tx_changed_script;

    if (!strCmd.empty()) {
        ReplaceAll(strCmd, "%s", hash.GetHex());
        if (auto* conf = wtx.state<TxStateConfirmed>()) {
            ReplaceAll(strCmd, "%b", conf->confirmed_block_hash.GetHex());
            ReplaceAll(strCmd, "%h", ToString(conf->confirmed_block_height));
        } else {
            ReplaceAll(strCmd, "%b", "unconfirmed");
            ReplaceAll(strCmd, "%h", "-1");
        }
#ifndef WIN32
        // Substituting the vault name isn't currently supported on windows
        // because windows shell escaping has not been implemented yet.
        ReplaceAll(strCmd, "%w", ShellEscape(GetName()));
#endif
        std::thread t(runCommand, strCmd);
        t.detach(); // thread runs free
    }
#endif

    return &wtx;
}

bool CVault::LoadToVault(const uint256& hash, const UpdateVaultTxFn& fill_wtx)
{
    const auto& ins = mapVault.emplace(std::piecewise_construct, std::forward_as_tuple(hash), std::forward_as_tuple(nullptr, TxStateInactive{}));
    CVaultTx& wtx = ins.first->second;
    if (!fill_wtx(wtx, ins.second)) {
        return false;
    }
    // Without an active chain (for example in the node-free desktop or
    // quicksilver-vault tool), preserve the stored confirmation state until a
    // validated header source can advance it.
    if (HaveChain() && chain().hasChainstate()) {
        wtx.updateState(chain());
    }
    if (/* insertion took place */ ins.second) {
        wtx.m_it_wtxOrdered = wtxOrdered.insert(std::make_pair(wtx.nOrderPos, &wtx));
    }
    AddToSpends(wtx);
    for (const CTxIn& txin : wtx.tx->vin) {
        auto it = mapVault.find(txin.prevout.hash);
        if (it != mapVault.end()) {
            CVaultTx& prevtx = it->second;
            if (auto* prev = prevtx.state<TxStateBlockConflicted>()) {
                MarkConflicted(prev->conflicting_block_hash, prev->conflicting_block_height, wtx.GetHash());
            }
        }
    }

    // Update birth time when tx time is older than it.
    MaybeUpdateBirthTime(wtx.GetTxTime());

    return true;
}

bool CVault::AddToVaultIfInvolvingMe(const CTransactionRef& ptx, const SyncTxState& state, bool fUpdate, bool rescanning_old_block)
{
    const CTransaction& tx = *ptx;
    {
        AssertLockHeld(cs_vault);

        if (auto* conf = std::get_if<TxStateConfirmed>(&state)) {
            for (const CTxIn& txin : tx.vin) {
                std::pair<TxSpends::const_iterator, TxSpends::const_iterator> range = mapTxSpends.equal_range(txin.prevout);
                while (range.first != range.second) {
                    if (range.first->second != tx.GetHash()) {
                        VaultLogPrintf("conflicted tx=%s block=%s vault_tx=%s outpoint=%s:%i", tx.GetHash().ToString(), conf->confirmed_block_hash.ToString(), range.first->second.ToString(), range.first->first.hash.ToString(), range.first->first.n);
                        MarkConflicted(conf->confirmed_block_hash, conf->confirmed_block_height, range.first->second);
                    }
                    range.first++;
                }
            }
        }

        bool fExisted = mapVault.count(tx.GetHash()) != 0;
        if (fExisted && !fUpdate) return false;
        if (fExisted || IsMine(tx) || IsFromMe(tx)) {
            /* Check if any keys in the vault keypool that were supposed to be unused
             * have appeared in a new transaction. If so, remove those keys from the keypool.
             * This can happen when restoring an old vault backup that does not contain
             * the mostly recently created transactions from newer versions of the vault.
             */

            // loop though all outputs
            for (const CTxOut& txout : tx.vout) {
                for (const auto& spk_man : GetScriptPubKeyMans(txout.scriptPubKey)) {
                    for (auto& dest : spk_man->MarkUnusedAddresses(txout.scriptPubKey)) {
                        // If internal flag is not defined try to infer it from the ScriptPubKeyMan
                        if (!dest.internal.has_value()) {
                            dest.internal = IsInternalScriptPubKeyMan(spk_man);
                        }

                        // skip if can't determine whether it's a receiving address or not
                        if (!dest.internal.has_value()) continue;

                        // If this is a receiving address and it's not in the address book yet
                        // (e.g. it wasn't generated on this node or we're restoring from backup)
                        // add it to the address book for proper transaction accounting
                        if (!*dest.internal && !FindAddressBookEntry(dest.dest, /* allow_change= */ false)) {
                            SetAddressBook(dest.dest, "", AddressPurpose::RECEIVE);
                        }
                    }
                }
            }

            // Block disconnection override an abandoned tx as unconfirmed
            // which means user may have to call abandontransaction again
            TxState tx_state = std::visit([](auto&& s) -> TxState { return s; }, state);
            CVaultTx* wtx = AddToVault(MakeTransactionRef(tx), tx_state, /*update_wtx=*/nullptr, /*fFlushOnClose=*/false, rescanning_old_block);
            if (!wtx) {
                // Can only be nullptr if there was a db write error (missing db, read-only db or a db engine internal writing error).
                // As we only store arriving transaction in this process, and we don't want an inconsistent state, let's throw an error.
                throw std::runtime_error("DB error adding transaction to vault, write failed");
            }
            return true;
        }
    }
    return false;
}

bool CVault::TransactionCanBeAbandoned(const uint256& hashTx) const
{
    LOCK(cs_vault);
    const CVaultTx* wtx = GetVaultTx(hashTx);
    return wtx && !wtx->isAbandoned() && GetTxDepthInMainChain(*wtx) == 0 && !wtx->InRelayPool();
}

void CVault::MarkInputsDirty(const CTransactionRef& tx)
{
    for (const CTxIn& txin : tx->vin) {
        auto it = mapVault.find(txin.prevout.hash);
        if (it != mapVault.end()) {
            it->second.MarkDirty();
        }
    }
}

bool CVault::AbandonTransaction(const uint256& hashTx)
{
    LOCK(cs_vault);
    auto it = mapVault.find(hashTx);
    assert(it != mapVault.end());
    return AbandonTransaction(it->second);
}

bool CVault::AbandonTransaction(CVaultTx& tx)
{
    // Can't mark abandoned if confirmed, in relaypool, or carrying a stored
    // block reference that has not yet been checked against an active chain.
    if (GetTxDepthInMainChain(tx) != 0 || tx.InRelayPool() || tx.isBlockUnresolved()) {
        return false;
    }

    auto try_updating_state = [](CVaultTx& wtx) EXCLUSIVE_LOCKS_REQUIRED(cs_vault) {
        // If the orig tx was not in block/relaypool, none of its spends can be.
        assert(!wtx.isConfirmed());
        assert(!wtx.InRelayPool());
        // If already conflicted or abandoned, no need to set abandoned
        if (!wtx.isBlockConflicted() && !wtx.isAbandoned()) {
            wtx.m_state = TxStateInactive{/*abandoned=*/true};
            return TxUpdate::NOTIFY_CHANGED;
        }
        return TxUpdate::UNCHANGED;
    };

    // Iterate over all its outputs, and mark transactions in the vault that spend them abandoned too.
    // States are not permanent, so these transactions can become unabandoned if they are re-added to the
    // relaypool, or confirmed in a block, or conflicted.
    // Note: If the reorged coinbase is re-added to the main chain, the descendants that have not had their
    // states change will remain abandoned and will require manual broadcast if the user wants them.

    RecursiveUpdateTxState(tx.GetHash(), try_updating_state);

    return true;
}

void CVault::MarkConflicted(const uint256& hashBlock, int conflicting_height, const uint256& hashTx)
{
    LOCK(cs_vault);

    // If number of conflict confirms cannot be determined, this means
    // that the block is still unknown or not yet part of the main chain,
    // for example when loading the vault during a reindex. Do nothing in that
    // case.
    if (m_last_block_processed_height < 0 || conflicting_height < 0) {
        return;
    }
    int conflictconfirms = (m_last_block_processed_height - conflicting_height + 1) * -1;
    if (conflictconfirms >= 0)
        return;

    auto try_updating_state = [&](CVaultTx& wtx) EXCLUSIVE_LOCKS_REQUIRED(cs_vault) {
        if (conflictconfirms < GetTxDepthInMainChain(wtx)) {
            // Block is 'more conflicted' than current confirm; update.
            // Mark transaction as conflicted with this block.
            wtx.m_state = TxStateBlockConflicted{hashBlock, conflicting_height};
            return TxUpdate::CHANGED;
        }
        return TxUpdate::UNCHANGED;
    };

    // Iterate over all its outputs, and mark transactions in the vault that spend them conflicted too.
    RecursiveUpdateTxState(hashTx, try_updating_state);
}

void CVault::RecursiveUpdateTxState(const uint256& tx_hash, const TryUpdatingStateFn& try_updating_state)
{
    // Do not flush the vault here for performance reasons
    VaultBatch batch(GetDatabase(), false);
    RecursiveUpdateTxState(&batch, tx_hash, try_updating_state);
}

void CVault::RecursiveUpdateTxState(VaultBatch* batch, const uint256& tx_hash, const TryUpdatingStateFn& try_updating_state)
{
    std::set<uint256> todo;
    std::set<uint256> done;

    todo.insert(tx_hash);

    while (!todo.empty()) {
        uint256 now = *todo.begin();
        todo.erase(now);
        done.insert(now);
        auto it = mapVault.find(now);
        assert(it != mapVault.end());
        CVaultTx& wtx = it->second;

        TxUpdate update_state = try_updating_state(wtx);
        if (update_state != TxUpdate::UNCHANGED) {
            wtx.MarkDirty();
            if (batch) batch->WriteTx(wtx);
            // Iterate over all its outputs, and update those tx states as well (if applicable)
            for (unsigned int i = 0; i < wtx.tx->vout.size(); ++i) {
                std::pair<TxSpends::const_iterator, TxSpends::const_iterator> range = mapTxSpends.equal_range(COutPoint(Txid::FromUint256(now), i));
                for (TxSpends::const_iterator iter = range.first; iter != range.second; ++iter) {
                    if (!done.count(iter->second)) {
                        todo.insert(iter->second);
                    }
                }
            }

            if (update_state == TxUpdate::NOTIFY_CHANGED) {
                NotifyTransactionChanged(wtx.GetHash(), CT_UPDATED);
            }

            // If a transaction changes its tx state, that usually changes the balance
            // available of the outputs it spends. So force those to be recomputed
            MarkInputsDirty(wtx.tx);
        }
    }
}

void CVault::SyncTransaction(const CTransactionRef& ptx, const SyncTxState& state, bool update_tx, bool rescanning_old_block)
{
    if (!AddToVaultIfInvolvingMe(ptx, state, update_tx, rescanning_old_block))
        return; // Not one of ours

    // If a transaction changes 'conflicted' state, that changes the balance
    // available of the outputs it spends. So force those to be
    // recomputed, also:
    MarkInputsDirty(ptx);
}

void CVault::transactionAddedToRelayPool(const CTransactionRef& tx)
{
    LOCK(cs_vault);
    SyncTransaction(tx, TxStateInRelayPool{});

    auto it = mapVault.find(tx->GetHash());
    if (it != mapVault.end()) {
        RefreshRelayPoolStatus(it->second, chain());
    }

    const Txid& txid = tx->GetHash();

    for (const CTxIn& tx_in : tx->vin) {
        // For each vault transaction spending this prevout..
        for (auto range = mapTxSpends.equal_range(tx_in.prevout); range.first != range.second; range.first++) {
            const uint256& spent_id = range.first->second;
            // Skip the recently added tx
            if (spent_id == txid) continue;
            RecursiveUpdateTxState(/*batch=*/nullptr, spent_id, [&txid](CVaultTx& wtx) EXCLUSIVE_LOCKS_REQUIRED(cs_vault) {
                return wtx.relaypool_conflicts.insert(txid).second ? TxUpdate::CHANGED : TxUpdate::UNCHANGED;
            });
        }
    }
}

void CVault::transactionRemovedFromRelayPool(const CTransactionRef& tx, RelayPoolRemovalReason reason)
{
    LOCK(cs_vault);
    auto it = mapVault.find(tx->GetHash());
    if (it != mapVault.end()) {
        RefreshRelayPoolStatus(it->second, chain());
    }
    // Handle transactions that were removed from the relaypool because they
    // conflict with transactions in a newly connected block.
    if (reason == RelayPoolRemovalReason::CONFLICT) {
        // Trigger external -vaultnotify notifications for these transactions.
        // Set Status::UNCONFIRMED instead of Status::CONFLICTED for a few reasons:
        //
        // 1. The transactionRemovedFromRelayPool callback does not currently
        //    provide the conflicting block's hash and height, and for backwards
        //    compatibility reasons it may not be not safe to store conflicted
        //    vault transactions with a null block hash.
        // 2. For most of these transactions, the vault's internal conflict
        //    detection in the blockConnected handler will subsequently call
        //    MarkConflicted and update them with CONFLICTED status anyway. This
        //    applies to any vault transaction that has inputs spent in the
        //    block, or that has ancestors in the vault with inputs spent by
        //    the block.
        // 3. Longstanding behavior since the sync implementation and the prior
        //    sync implementation before that was to mark these transactions
        //    unconfirmed rather than conflicted.
        //
        // Nothing described above should be seen as an unchangeable requirement
        // when improving this code in the future. The vault's heuristics for
        // distinguishing between conflicted and unconfirmed transactions are
        // imperfect, and could be improved in general.
        SyncTransaction(tx, TxStateInactive{});
    }

    const Txid& txid = tx->GetHash();

    for (const CTxIn& tx_in : tx->vin) {
        // Iterate over all vault transactions spending txin.prev
        // and recursively mark them as no longer conflicting with
        // txid
        for (auto range = mapTxSpends.equal_range(tx_in.prevout); range.first != range.second; range.first++) {
            const uint256& spent_id = range.first->second;

            RecursiveUpdateTxState(/*batch=*/nullptr, spent_id, [&txid](CVaultTx& wtx) EXCLUSIVE_LOCKS_REQUIRED(cs_vault) {
                return wtx.relaypool_conflicts.erase(txid) ? TxUpdate::CHANGED : TxUpdate::UNCHANGED;
            });
        }
    }
}

void CVault::blockConnected(const interfaces::BlockInfo& block)
{
    assert(block.data);
    LOCK(cs_vault);

    m_last_block_processed_height = block.height;
    m_last_block_processed = block.hash;
    m_last_block_processed_time = block.data->GetBlockTime();

    // No need to scan block if it was created before the vault birthday.
    // Uses chain max time and twice the grace period to adjust time for block time variability.
    if (block.chain_time_max < m_birth_time.load() - (TIMESTAMP_WINDOW * 2)) return;

    // Scan block
    for (size_t index = 0; index < block.data->vtx.size(); index++) {
        SyncTransaction(block.data->vtx[index], TxStateConfirmed{block.hash, block.height, static_cast<int>(index)});
        transactionRemovedFromRelayPool(block.data->vtx[index], RelayPoolRemovalReason::BLOCK);
    }
}

void CVault::blockDisconnected(const interfaces::BlockInfo& block)
{
    assert(block.data);
    LOCK(cs_vault);

    // At block disconnection, this will change an abandoned transaction to
    // be unconfirmed, whether or not the transaction is added back to the relaypool.
    // User may have to call abandontransaction again. It may be addressed in the
    // future with a stickier abandoned state or even removing abandontransaction call.
    m_last_block_processed_height = block.height - 1;
    m_last_block_processed = *Assert(block.prev_hash);
    m_last_block_processed_time = -1;

    int disconnect_height = block.height;

    for (size_t index = 0; index < block.data->vtx.size(); index++) {
        const CTransactionRef& ptx = Assert(block.data)->vtx[index];
        // Coinbase transactions are not only inactive but also abandoned,
        // meaning they should never be relayed standalone via the p2p protocol.
        SyncTransaction(ptx, TxStateInactive{/*abandoned=*/index == 0});

        for (const CTxIn& tx_in : ptx->vin) {
            // No other vault transactions conflicted with this transaction
            if (mapTxSpends.count(tx_in.prevout) < 1) continue;

            std::pair<TxSpends::const_iterator, TxSpends::const_iterator> range = mapTxSpends.equal_range(tx_in.prevout);

            // For all of the spends that conflict with this transaction
            for (TxSpends::const_iterator _it = range.first; _it != range.second; ++_it) {
                CVaultTx& wtx = mapVault.find(_it->second)->second;

                if (!wtx.isBlockConflicted()) continue;

                auto try_updating_state = [&](CVaultTx& tx) {
                    if (!tx.isBlockConflicted()) return TxUpdate::UNCHANGED;
                    if (tx.state<TxStateBlockConflicted>()->conflicting_block_height >= disconnect_height) {
                        tx.m_state = TxStateInactive{};
                        return TxUpdate::CHANGED;
                    }
                    return TxUpdate::UNCHANGED;
                };

                RecursiveUpdateTxState(wtx.tx->GetHash(), try_updating_state);
            }
        }
    }
}

bool CVault::ApplyHeaderTip(int block_height, const uint256& block_hash, int64_t block_time)
{
    AssertLockHeld(cs_vault);
    if (block_height < 0 || block_hash.IsNull() || block_time < 0) return false;
    if (m_last_block_processed_height > block_height) return false;
    if (m_last_block_processed_height == block_height &&
        !m_last_block_processed.IsNull() && m_last_block_processed != block_hash) {
        return false;
    }
    for (const auto& entry : mapVault) {
        const CVaultTx& tx{entry.second};
        if (const auto* confirmed = tx.state<TxStateConfirmed>();
            confirmed && confirmed->confirmed_block_height > block_height) {
            return false;
        }
        if (const auto* conflicted = tx.state<TxStateBlockConflicted>();
            conflicted && conflicted->conflicting_block_height > block_height) {
            return false;
        }
    }
    SetLastBlockProcessed(block_height, block_hash, block_time);
    return true;
}

void CVault::updatedBlockTip()
{
    m_best_block_time = GetTime();
}

void CVault::BlockUntilSyncedToCurrentChain() const
{
    AssertLockNotHeld(cs_vault);
    // Skip the queue-draining stuff if we know we're caught up with
    // chain().Tip(), otherwise put a callback in the validation interface queue and wait
    // for the queue to drain enough to execute it (indicating we are caught up
    // at least with the time we entered this function).
    // With no chainstate there is no tip to be behind and no validation signals
    // object to drain: waitForNotificationsIfTipChanged short-circuits its own
    // chainman() use on a null tip but then dereferences validation_signals(),
    // which is absent for the same reason chainman is.
    if (!chain().hasChainstate()) return;
    uint256 last_block_hash = WITH_LOCK(cs_vault, return m_last_block_processed);
    chain().waitForNotificationsIfTipChanged(last_block_hash);
}

// Note that this function doesn't distinguish between a 0-valued input,
// and a not-"is mine" (according to the filter) input.
CAmount CVault::GetDebit(const CTxIn& txin, const isminefilter& filter) const
{
    {
        LOCK(cs_vault);
        const auto mi = mapVault.find(txin.prevout.hash);
        if (mi != mapVault.end()) {
            const CVaultTx& prev = (*mi).second;
            if (txin.prevout.n < prev.tx->vout.size())
                if (IsMine(prev.tx->vout[txin.prevout.n]) & filter)
                    return prev.tx->vout[txin.prevout.n].nValue;
        }
    }
    return 0;
}

isminetype CVault::IsMine(const CTxOut& txout) const
{
    AssertLockHeld(cs_vault);
    return IsMine(txout.scriptPubKey);
}

isminetype CVault::IsMine(const CTxDestination& dest) const
{
    AssertLockHeld(cs_vault);
    return IsMine(GetScriptForDestination(dest));
}

isminetype CVault::IsMine(const CScript& script) const
{
    AssertLockHeld(cs_vault);

    // Search the cache so that IsMine is called only on the relevant SPKMs instead of on everything in m_spk_managers
    const auto& it = m_cached_spks.find(script);
    if (it != m_cached_spks.end()) {
        isminetype res = ISMINE_NO;
        for (const auto& spkm : it->second) {
            res = std::max(res, spkm->IsMine(script));
        }
        Assume(res == ISMINE_SPENDABLE);
        return res;
    }

    return ISMINE_NO;
}

bool CVault::IsMine(const CTransaction& tx) const
{
    AssertLockHeld(cs_vault);
    for (const CTxOut& txout : tx.vout)
        if (IsMine(txout))
            return true;
    return false;
}

isminetype CVault::IsMine(const COutPoint& outpoint) const
{
    AssertLockHeld(cs_vault);
    auto wtx = GetVaultTx(outpoint.hash);
    if (!wtx) {
        return ISMINE_NO;
    }
    if (outpoint.n >= wtx->tx->vout.size()) {
        return ISMINE_NO;
    }
    return IsMine(wtx->tx->vout[outpoint.n]);
}

bool CVault::IsFromMe(const CTransaction& tx) const
{
    return (GetDebit(tx, ISMINE_SPENDABLE) > 0);
}

CAmount CVault::GetDebit(const CTransaction& tx, const isminefilter& filter) const
{
    CAmount nDebit = 0;
    for (const CTxIn& txin : tx.vin) {
        nDebit += GetDebit(txin, filter);
        if (!MoneyRange(nDebit))
            throw std::runtime_error(std::string(__func__) + ": value out of range");
    }
    return nDebit;
}

bool CVault::IsHDEnabled() const
{
    // All Active ScriptPubKeyMans must be HD for this to be true
    bool result = false;
    for (const auto& spk_man : GetActiveScriptPubKeyMans()) {
        if (!spk_man->IsHDEnabled()) return false;
        result = true;
    }
    return result;
}

bool CVault::CanGetAddresses(bool internal) const
{
    LOCK(cs_vault);
    if (m_spk_managers.empty()) return false;
    for (OutputType t : OUTPUT_TYPES) {
        auto spk_man = GetScriptPubKeyMan(t, internal);
        if (spk_man && spk_man->CanGetAddresses(internal)) {
            return true;
        }
    }
    return false;
}

void CVault::SetVaultFlag(uint64_t flags)
{
    VaultBatch batch(GetDatabase());
    return SetVaultFlagWithDB(batch, flags);
}

void CVault::SetVaultFlagWithDB(VaultBatch& batch, uint64_t flags)
{
    LOCK(cs_vault);
    m_vault_flags |= flags;
    if (!batch.WriteVaultFlags(m_vault_flags))
        throw std::runtime_error(std::string(__func__) + ": writing vault flags failed");
}

void CVault::UnsetVaultFlag(uint64_t flag)
{
    VaultBatch batch(GetDatabase());
    UnsetVaultFlagWithDB(batch, flag);
}

void CVault::UnsetVaultFlagWithDB(VaultBatch& batch, uint64_t flag)
{
    LOCK(cs_vault);
    m_vault_flags &= ~flag;
    if (!batch.WriteVaultFlags(m_vault_flags))
        throw std::runtime_error(std::string(__func__) + ": writing vault flags failed");
}

void CVault::UnsetBlankVaultFlag(VaultBatch& batch)
{
    UnsetVaultFlagWithDB(batch, VAULT_FLAG_BLANK_VAULT);
}

bool CVault::IsVaultFlagSet(uint64_t flag) const
{
    return (m_vault_flags & flag);
}

bool CVault::LoadVaultFlags(uint64_t flags)
{
    LOCK(cs_vault);
    if (((flags & KNOWN_VAULT_FLAGS) >> 32) ^ (flags >> 32)) {
        // contains unknown non-tolerable vault flags
        return false;
    }
    m_vault_flags = flags;

    return true;
}

void CVault::InitVaultFlags(uint64_t flags)
{
    LOCK(cs_vault);

    // We should never be writing unknown non-tolerable vault flags
    assert(((flags & KNOWN_VAULT_FLAGS) >> 32) == (flags >> 32));
    // This should only be used once, when creating a new vault - so current flags are expected to be blank
    assert(m_vault_flags == 0);

    if (!VaultBatch(GetDatabase()).WriteVaultFlags(flags)) {
        throw std::runtime_error(std::string(__func__) + ": writing vault flags failed");
    }

    if (!LoadVaultFlags(flags)) assert(false);
}

void CVault::MaybeUpdateBirthTime(int64_t time)
{
    int64_t birthtime = m_birth_time.load();
    if (time < birthtime) {
        m_birth_time = time;
    }
}

/**
 * Scan active chain for relevant transactions after importing keys. This should
 * be called whenever new keys are added to the vault, with the oldest key
 * creation time.
 *
 * @return Earliest timestamp that could be successfully scanned from. Timestamp
 * returned will be higher than startTime if relevant blocks could not be read
 * or the scan stopped before it reached them.
 */
int64_t CVault::RescanFromTime(int64_t startTime, const VaultRescanReserver& reserver, bool update)
{
    // Find starting block. May be null if nCreateTime is greater than the
    // highest blockchain timestamp, in which case there is nothing that needs
    // to be scanned.
    int start_height = 0;
    uint256 start_block;
    bool start = chain().findFirstBlockWithTimeAndHeight(startTime - TIMESTAMP_WINDOW, 0, FoundBlock().hash(start_block).height(start_height));
    VaultLogPrintf("rescanning blocks=%i", start ? WITH_LOCK(cs_vault, return GetLastBlockHeight()) - start_height + 1 : 0);

    if (start) {
        ScanResult result = ScanForVaultTransactions(start_block, start_height, /*max_height=*/{}, reserver, /*fUpdate=*/update, /*save_progress=*/false);
        if (result.status == ScanResult::FAILURE) {
            int64_t time_max;
            CHECK_NONFATAL(chain().findBlock(result.last_failed_block, FoundBlock().maxTime(time_max)));
            return time_max + TIMESTAMP_WINDOW + 1;
        }
        if (result.status == ScanResult::USER_ABORT) {
            // last_failed_block may be unset here. The first block that was not
            // scanned is the one after last_scanned, or start_block when none
            // was. Matching the synced height means the requested range was
            // scanned and the abort only raced the completion check.
            const int synced_height = WITH_LOCK(cs_vault, return GetLastBlockHeight());
            uint256 unscanned_block = start_block;
            int unscanned_height = start_height;
            if (result.last_scanned_height.has_value()) {
                if (*result.last_scanned_height >= synced_height) {
                    return startTime;
                }
                unscanned_height = *result.last_scanned_height + 1;
                unscanned_block = chain().getBlockHash(unscanned_height);
            }
            int64_t time_max = 0;
            CHECK_NONFATAL(chain().findBlock(unscanned_block, FoundBlock().maxTime(time_max)));

            // A user abort leaves the sync point alone. Shutdown does not set
            // fAbortRescan, and a synced vault skips its startup rescan, so
            // record the first unscanned block unless the stored locator is
            // already there or behind it. Ignore later flushes, including the
            // one Shutdown() runs after this RPC returns. No stored locator
            // already makes startup scan from genesis; do not write a later one.
            if (!IsAbortingRescan() && chain().shutdownRequested()) {
                CBlockLocator replacement = chain().getActiveChainLocator(unscanned_block);
                LOCK(m_best_block_mutex);
                m_attaching_chain = true;
                CBlockLocator existing;
                const bool had_locator = VaultBatch{GetDatabase()}.ReadBestBlock(existing);
                const std::optional<int> fork = had_locator ? chain().findLocatorFork(existing) : std::nullopt;
                if (had_locator && (!fork || *fork > unscanned_height)) {
                    if (replacement.IsNull() || !VaultBatch{GetDatabase()}.WriteBestBlock(replacement)) {
                        m_attaching_chain = false;
                        VaultLogPrintf("rescanned ok=0 reason=shutdown-requested best_block_write=failed");
                    }
                }
            }
            return time_max + TIMESTAMP_WINDOW + 1;
        }
    }
    return startTime;
}

/**
 * Scan the block chain (starting in start_block) for transactions
 * from or to us. If fUpdate is true, found transactions that already
 * exist in the vault will be updated. If max_height is not set, the
 * relaypool will be scanned as well.
 *
 * @param[in] start_block Scan starting block. If block is not on the active
 *                        chain, the scan will return SUCCESS immediately.
 * @param[in] start_height Height of start_block
 * @param[in] max_height  Optional max scanning height. If unset there is
 *                        no maximum and scanning can continue to the tip
 *
 * @return ScanResult returning scan information and indicating success or
 *         failure. Return status will be set to SUCCESS if scan was
 *         successful. FAILURE if a complete rescan was not possible (due to
 *         pruning or corruption). USER_ABORT if the rescan was aborted before
 *         it could complete.
 *
 * @pre Caller needs to make sure start_block (and the optional stop_block) are on
 * the main chain after to the addition of any new keys you want to detect
 * transactions for.
 */
CVault::ScanResult CVault::ScanForVaultTransactions(const uint256& start_block, int start_height, std::optional<int> max_height, const VaultRescanReserver& reserver, bool fUpdate, const bool save_progress)
{
    constexpr auto INTERVAL_TIME{60s};
    auto current_time{reserver.now()};
    auto start_time{reserver.now()};

    assert(reserver.isReserved());

    uint256 block_hash = start_block;
    ScanResult result;

    std::unique_ptr<FastVaultRescanFilter> fast_rescan_filter;
    if (chain().hasBlockFilterIndex(BlockFilterType::BASIC)) fast_rescan_filter = std::make_unique<FastVaultRescanFilter>(*this);

    VaultLogPrintf("rescanning start_block=%s variant=%s", start_block.ToString(),
                   fast_rescan_filter ? "fast" : "slow");

    fAbortRescan = false;
    ShowProgress(strprintf("[%s] %s", GetDisplayName(), _("Rescanning…")), 0); // show rescan progress in GUI as dialog or on splashscreen, if rescan required on startup (e.g. due to corruption)
    uint256 tip_hash = WITH_LOCK(cs_vault, return GetLastBlockHash());
    uint256 end_hash = tip_hash;
    if (max_height) chain().findAncestorByHeight(tip_hash, *max_height, FoundBlock().hash(end_hash));
    double progress_begin = chain().guessVerificationProgress(block_hash);
    double progress_end = chain().guessVerificationProgress(end_hash);
    double progress_current = progress_begin;
    int block_height = start_height;
    while (!fAbortRescan && !chain().shutdownRequested()) {
        if (progress_end - progress_begin > 0.0) {
            m_scanning_progress = (progress_current - progress_begin) / (progress_end - progress_begin);
        } else { // avoid divide-by-zero for single block scan range (i.e. start and stop hashes are equal)
            m_scanning_progress = 0;
        }
        if (block_height % 100 == 0 && progress_end - progress_begin > 0.0) {
            ShowProgress(strprintf("[%s] %s", GetDisplayName(), _("Rescanning…")), std::max(1, std::min(99, (int)(m_scanning_progress * 100))));
        }

        bool next_interval = reserver.now() >= current_time + INTERVAL_TIME;
        if (next_interval) {
            current_time = reserver.now();
            VaultLogPrintf("rescanning height=%d progress=%f", block_height, progress_current);
        }

        bool fetch_block{true};
        if (fast_rescan_filter) {
            fast_rescan_filter->UpdateIfNeeded();
            auto matches_block{fast_rescan_filter->MatchesBlock(block_hash)};
            if (matches_block.has_value()) {
                if (*matches_block) {
                    LogDebug(HgLog::SCAN, "Fast rescan: inspect block %d [%s] (filter matched)\n", block_height, block_hash.ToString());
                } else {
                    result.last_scanned_block = block_hash;
                    result.last_scanned_height = block_height;
                    fetch_block = false;
                }
            } else {
                LogDebug(HgLog::SCAN, "Fast rescan: inspect block %d [%s] (WARNING: block filter not found!)\n", block_height, block_hash.ToString());
            }
        }

        // Find next block separately from reading data above, because reading
        // is slow and there might be a reorg while it is read.
        bool block_still_active = false;
        bool next_block = false;
        uint256 next_block_hash;
        chain().findBlock(block_hash, FoundBlock().inActiveChain(block_still_active).nextBlock(FoundBlock().inActiveChain(next_block).hash(next_block_hash)));

        if (fetch_block) {
            // Read block data
            CBlock block;
            chain().findBlock(block_hash, FoundBlock().data(block));

            if (!block.IsNull()) {
                LOCK(cs_vault);
                if (!block_still_active) {
                    // Abort scan if current block is no longer active, to prevent
                    // marking transactions as coming from the wrong block.
                    result.last_failed_block = block_hash;
                    result.status = ScanResult::FAILURE;
                    break;
                }
                for (size_t posInBlock = 0; posInBlock < block.vtx.size(); ++posInBlock) {
                    SyncTransaction(block.vtx[posInBlock], TxStateConfirmed{block_hash, block_height, static_cast<int>(posInBlock)}, fUpdate, /*rescanning_old_block=*/true);
                }
                // scan succeeded, record block as most recent successfully scanned
                result.last_scanned_block = block_hash;
                result.last_scanned_height = block_height;

                if (save_progress && next_interval) {
                    CBlockLocator loc = m_chain->getActiveChainLocator(block_hash);

                    if (!loc.IsNull()) {
                        VaultLogPrintf("saved scan_progress_height=%d", block_height);
                        VaultBatch batch(GetDatabase());
                        batch.WriteBestBlock(loc);
                    }
                }
            } else {
                // could not scan block, keep scanning but record this block as the most recent failure
                result.last_failed_block = block_hash;
                result.status = ScanResult::FAILURE;
            }
        }
        if (max_height && block_height >= *max_height) {
            break;
        }
        // If rescanning was triggered with cs_vault permanently locked (AttachChain), additional blocks that were connected during the rescan
        // aren't processed here but will be processed with the pending blockConnected notifications after the lock is released.
        // If rescanning without a permanent cs_vault lock, additional blocks that were added during the rescan will be re-processed if
        // the notification was processed and the last block height was updated.
        if (block_height >= WITH_LOCK(cs_vault, return GetLastBlockHeight())) {
            break;
        }

        {
            if (!next_block) {
                // break successfully when rescan has reached the tip, or
                // previous block is no longer on the chain due to a reorg
                break;
            }

            // increment block and verification progress
            block_hash = next_block_hash;
            ++block_height;
            progress_current = chain().guessVerificationProgress(block_hash);

            // handle updated tip hash
            const uint256 prev_tip_hash = tip_hash;
            tip_hash = WITH_LOCK(cs_vault, return GetLastBlockHash());
            if (!max_height && prev_tip_hash != tip_hash) {
                // in case the tip has changed, update progress max
                progress_end = chain().guessVerificationProgress(tip_hash);
            }
        }
    }
    if (!max_height) {
        VaultLogPrintf("rescanning stage=relaypool");
        WITH_LOCK(cs_vault, chain().requestRelayPoolTransactions(*this));
    }
    ShowProgress(strprintf("[%s] %s", GetDisplayName(), _("Rescanning…")), 100); // hide progress dialog in GUI
    if (block_height && fAbortRescan) {
        VaultLogPrintf("rescanned ok=0 reason=aborted height=%d progress=%f", block_height, progress_current);
        result.status = ScanResult::USER_ABORT;
    } else if (block_height && chain().shutdownRequested()) {
        VaultLogPrintf("rescanned ok=0 reason=shutdown-requested height=%d progress=%f", block_height, progress_current);
        result.status = ScanResult::USER_ABORT;
    } else {
        VaultLogPrintf("rescanned ok=1 elapsed_ms=%d", Ticks<std::chrono::milliseconds>(reserver.now() - start_time));
    }
    return result;
}

TxSubmissionResult CVault::SubmitTxMemoryPoolAndRelay(CVaultTx& wtx, std::string& err_string, bool relay) const
{
    AssertLockHeld(cs_vault);

    // Can't relay if vault is not broadcasting
    if (!GetBroadcastTransactions()) return {};
    // Don't relay abandoned transactions
    if (wtx.isAbandoned()) return {};
    // Don't try to submit coinbase transactions. These would fail anyway but would
    // cause log spam.
    if (wtx.IsCoinBase()) return {};
    // Don't try to submit conflicted or confirmed transactions.
    if (GetTxDepthInMainChain(wtx) != 0) return {};
    // BroadcastTransaction asserts on chainman. An existing vault holding an
    // unconfirmed transaction reaches here from postInitProcess when it is loaded
    // in the consensus-off shell, which is a load away from the create path this
    // change is about and aborts the same way.
    if (!chain().hasChainstate()) return {};

    // Submit transaction to relaypool for relay
    VaultLogPrintf("submitted tx=%s target=relaypool", wtx.GetHash().ToString());
    // We must set TxStateInRelayPool here. Even though it will also be set later by the
    // entered-relaypool callback, if we did not there would be a race where a
    // user could call sendmoney in a loop and hit spurious out of funds errors
    // because we think that this newly generated transaction's change is
    // unavailable as we're not yet aware that it is in the relaypool.
    //
    // If broadcast fails for any reason, trying to set wtx.m_state here would be incorrect.
    // If transaction was previously in the relaypool, it should be updated when
    // TransactionRemovedFromRelayPool fires.
    const node::TransactionError error{chain().broadcastTransaction(wtx.tx, relay, err_string)};
    if (error == node::TransactionError::OK) {
        wtx.m_state = TxStateInRelayPool{};
        return {TxSubmissionStatus::SUCCEEDED, error};
    }
    return {TxSubmissionStatus::FAILED, error};
}

std::set<uint256> CVault::GetTxConflicts(const CVaultTx& wtx) const
{
    AssertLockHeld(cs_vault);

    const uint256 myHash{wtx.GetHash()};
    std::set<uint256> result{GetConflicts(myHash)};
    result.erase(myHash);
    return result;
}

bool CVault::ShouldResend() const
{
    // Don't attempt to resubmit if the vault is configured to not broadcast
    if (!fBroadcastTransactions) return false;

    // Nothing to resend onto, and isReadyToBroadcast() would abort: a vault in
    // the consensus-off shell still has this scheduled every minute from
    // LoadVaults, so without this guard creating a vault before consensus kills
    // the application about a minute later rather than immediately.
    if (!chain().hasChainstate()) return false;

    // During reindex, importing and IBD, old vault transactions become
    // unconfirmed. Don't resend them as that would spam other nodes.
    // We only allow forcing relaypool submission when not relaying to avoid this spam.
    if (!chain().isReadyToBroadcast()) return false;

    // Do this infrequently and randomly to avoid giving away
    // that these are our transactions.
    if (NodeClock::now() < m_next_resend) return false;

    return true;
}

NodeClock::time_point CVault::GetDefaultNextResend() { return FastRandomContext{}.rand_uniform_delay(NodeClock::now() + 12h, 24h); }

// Resubmit transactions from the vault to the relaypool, optionally asking the
// relaypool to relay them. On startup, we will do this for all unconfirmed
// transactions but will not ask the relaypool to relay them. We do this on startup
// to ensure that our own relaypool is aware of our transactions. There
// is a privacy side effect here as not broadcasting on startup also means that we won't
// inform the world of our vault's state, particularly if the vault (or node) is not
// yet synced.
//
// Otherwise this function is called periodically in order to relay our unconfirmed txs.
// We do this on a random timer to slightly obfuscate which transactions
// come from our vault.
//
// TODO: Ideally, we'd only resend transactions that we think should have been
// mined in the most recent block. Any transaction that wasn't in the top
// blockweight of transactions in the relaypool shouldn't have been mined,
// and so is probably just sitting in the relaypool waiting to be confirmed.
// Rebroadcasting does nothing to speed up confirmation and only damages
// privacy.
//
// The `force` option results in all unconfirmed transactions being submitted to
// the relaypool. This does not necessarily result in those transactions being relayed,
// that depends on the `relay` option. Periodic rebroadcast uses the pattern
// relay=true force=false, while loading into the relaypool
// (on start, or after import) uses relay=false force=true.
void CVault::ResubmitVaultTransactions(bool relay, bool force)
{
    // Don't attempt to resubmit if the vault is configured to not broadcast,
    // even if forcing.
    if (!fBroadcastTransactions) return;

    int submitted_tx_count = 0;

    { // cs_vault scope
        LOCK(cs_vault);

        // First filter for the transactions we want to rebroadcast.
        // We use a set with VaultTxOrderComparator so that rebroadcasting occurs in insertion order
        std::set<CVaultTx*, VaultTxOrderComparator> to_submit;
        for (auto& [txid, wtx] : mapVault) {
            // Only rebroadcast unconfirmed txs
            if (!wtx.isUnconfirmed()) continue;

            // Attempt to rebroadcast all txes more than 5 minutes older than
            // the last block, or all txs if forcing.
            if (!force && wtx.nTimeReceived > m_best_block_time - 5 * 60) continue;
            to_submit.insert(&wtx);
        }
        // Now try submitting the transactions to the memory pool and (optionally) relay them.
        for (auto wtx : to_submit) {
            std::string unused_err_string;
            if (SubmitTxMemoryPoolAndRelay(*wtx, unused_err_string, relay).status == TxSubmissionStatus::SUCCEEDED) ++submitted_tx_count;
        }
    } // cs_vault

    if (submitted_tx_count > 0) {
        VaultLogPrintf("submitted unconfirmed_txs=%u reason=resubmit", submitted_tx_count);
    }
}

/** @} */ // end of mapVault

void MaybeResendVaultTxs(VaultContext& context)
{
    for (const std::shared_ptr<CVault>& pvault : GetVaults(context)) {
        if (!pvault->ShouldResend()) continue;
        pvault->ResubmitVaultTransactions(/*relay=*/true, /*force=*/false);
        pvault->SetNextResend();
    }
}


/** @defgroup Actions
 *
 * @{
 */

bool CVault::SignTransaction(CMutableTransaction& tx) const
{
    AssertLockHeld(cs_vault);

    // Build coins map
    std::map<COutPoint, Coin> coins;
    for (auto& input : tx.vin) {
        const auto mi = mapVault.find(input.prevout.hash);
        if (mi == mapVault.end() || input.prevout.n >= mi->second.tx->vout.size()) {
            return false;
        }
        const CVaultTx& wtx = mi->second;
        int prev_height = wtx.state<TxStateConfirmed>() ? wtx.state<TxStateConfirmed>()->confirmed_block_height : 0;
        coins[input.prevout] = Coin(wtx.tx->vout[input.prevout.n], prev_height, wtx.IsCoinBase());
    }
    std::map<int, bilingual_str> input_errors;
    return SignTransaction(tx, coins, SIGHASH_DEFAULT, input_errors);
}

bool CVault::SignTransaction(CMutableTransaction& tx, const std::map<COutPoint, Coin>& coins, int sighash, std::map<int, bilingual_str>& input_errors) const
{
    // Try to sign with all ScriptPubKeyMans
    for (ScriptPubKeyMan* spk_man : GetAllScriptPubKeyMans()) {
        // spk_man->SignTransaction will return true if the transaction is complete,
        // so we can exit early and return true if that happens
        if (spk_man->SignTransaction(tx, coins, sighash, input_errors)) {
            return true;
        }
    }

    // At this point, one input was not fully signed otherwise we would have exited already
    return false;
}

std::optional<PSQTError> CVault::FillPSQT(PartiallySignedQuicksilverTransaction& psqtx, bool& complete, int sighash_type, bool sign, bool bip32derivs, size_t* n_signed, bool finalize) const
{
    if (n_signed) {
        *n_signed = 0;
    }
    LOCK(cs_vault);
    // Get all of the previous transactions
    for (unsigned int i = 0; i < psqtx.tx->vin.size(); ++i) {
        const CTxIn& txin = psqtx.tx->vin[i];
        PSQTInput& input = psqtx.inputs.at(i);

        if (PSQTInputSigned(input)) {
            continue;
        }

        // If we have no utxo, grab it from the vault.
        if (!input.non_witness_utxo) {
            const uint256& txhash = txin.prevout.hash;
            const auto it = mapVault.find(txhash);
            if (it != mapVault.end()) {
                const CVaultTx& wtx = it->second;
                // We only need the non_witness_utxo, which is a superset of the witness_utxo.
                //   The signing code will switch to the smaller witness_utxo if this is ok.
                input.non_witness_utxo = wtx.tx;
            }
        }
    }

    const PrecomputedTransactionData txdata = PrecomputePSQTData(psqtx);

    // Fill in information from ScriptPubKeyMans
    for (ScriptPubKeyMan* spk_man : GetAllScriptPubKeyMans()) {
        int n_signed_this_spkm = 0;
        const auto error{spk_man->FillPSQT(psqtx, txdata, sighash_type, sign, bip32derivs, &n_signed_this_spkm, finalize)};
        if (error) {
            return error;
        }

        if (n_signed) {
            (*n_signed) += n_signed_this_spkm;
        }
    }

    RemoveUnnecessaryTransactions(psqtx, sighash_type);

    // Complete if every input is now signed
    complete = true;
    for (size_t i = 0; i < psqtx.inputs.size(); ++i) {
        complete &= PSQTInputSignedAndVerified(psqtx, i, &txdata);
    }

    return {};
}

SigningResult CVault::SignMessage(const std::string& message, const PKHash& pkhash, std::string& str_sig) const
{
    SignatureData sigdata;
    CScript script_pub_key = GetScriptForDestination(pkhash);
    for (const auto& spk_man_pair : m_spk_managers) {
        if (spk_man_pair.second->CanProvide(script_pub_key, sigdata)) {
            LOCK(cs_vault); // DescriptorScriptPubKeyMan calls IsLocked which can lock cs_vault in a deadlocking order
            return spk_man_pair.second->SignMessage(message, pkhash, str_sig);
        }
    }
    return SigningResult::PRIVATE_KEY_NOT_AVAILABLE;
}

OutputType CVault::TransactionChangeType(const std::optional<OutputType>& change_type, const std::vector<CRecipient>& vecSend) const
{
    // If -changetype is specified, always use that change type.
    if (change_type) {
        return *change_type;
    }

    // If the default address type is PKH, use PKH change.
    if (m_default_address_type == OutputType::BASE58) {
        return OutputType::BASE58;
    }

    bool any_tr{false};
    bool any_wpkh{false};
    bool any_sh{false};
    bool any_pkh{false};

    for (const auto& recipient : vecSend) {
        if (std::get_if<WitnessV1Taproot>(&recipient.dest)) {
            any_tr = true;
        } else if (std::get_if<WitnessV0KeyHash>(&recipient.dest)) {
            any_wpkh = true;
        } else if (std::get_if<ScriptHash>(&recipient.dest)) {
            any_sh = true;
        } else if (std::get_if<PKHash>(&recipient.dest)) {
            any_pkh = true;
        }
    }

    const bool has_bech32m_spkman(GetScriptPubKeyMan(OutputType::BECH32M, /*internal=*/true));
    if (has_bech32m_spkman && any_tr) {
        // Currently tr is the only type supported by the BECH32M spkman
        return OutputType::BECH32M;
    }
    const bool has_bech32_spkman(GetScriptPubKeyMan(OutputType::BECH32, /*internal=*/true));
    if (has_bech32_spkman && any_wpkh) {
        // Currently wpkh is the only type supported by the BECH32 spkman
        return OutputType::BECH32;
    }
    const bool has_legacy_spkman(GetScriptPubKeyMan(OutputType::BASE58, /*internal=*/true));
    if (has_legacy_spkman && (any_pkh || any_sh)) {
        // Base58 covers both key-hash and script-hash destinations.
        return OutputType::BASE58;
    }

    if (has_bech32m_spkman) {
        return OutputType::BECH32M;
    }
    if (has_bech32_spkman) {
        return OutputType::BECH32;
    }
    // else use m_default_address_type for change
    return m_default_address_type;
}

CommitTransactionResult CVault::CommitTransaction(CTransactionRef tx, mapValue_t mapValue, std::vector<std::pair<std::string, std::string>> orderForm)
{
    LOCK(cs_vault);
    VaultLogPrintf("committing detail=tx-dump\n%s", util::RemoveSuffixView(tx->ToString(), "\n"));

    // Add tx to vault, because if it has change it's also ours,
    // otherwise just for transaction history.
    CVaultTx* wtx = AddToVault(tx, TxStateInactive{}, [&](CVaultTx& wtx, bool new_tx) {
        CHECK_NONFATAL(wtx.mapValue.empty());
        CHECK_NONFATAL(wtx.vOrderForm.empty());
        wtx.mapValue = std::move(mapValue);
        wtx.vOrderForm = std::move(orderForm);
        wtx.fFromMe = true;
        return true;
    });

    // wtx can only be null if the db write failed.
    if (!wtx) {
        throw std::runtime_error(std::string(__func__) + ": Vault db error, transaction commit failed");
    }

    // Notify that old coins are spent
    for (const CTxIn& txin : tx->vin) {
        CVaultTx& coin = mapVault.at(txin.prevout.hash);
        coin.MarkDirty();
        NotifyTransactionChanged(coin.GetHash(), CT_UPDATED);
    }

    if (!fBroadcastTransactions) {
        // Don't submit tx to the relaypool
        return {};
    }

    std::string err_string;
    const TxSubmissionResult submission{SubmitTxMemoryPoolAndRelay(*wtx, err_string, true)};
    if (submission.status == TxSubmissionStatus::FAILED) {
        // A rejection that may not hold later is the case this vault was always
        // built for: keep the transaction, and let ResubmitVaultTransactions try
        // again once the locktime matures or the pool has room. Reporting those
        // as a failed transfer would be wrong, and dropping them would discard a
        // transaction the caller deliberately built early.
        if (submission.error != node::TransactionError::CONSENSUS_INVALID) {
            VaultLogPrintf("submitted ok=0 reason=retryable error=%d detail=%s", static_cast<int>(submission.error), err_string);
            return {};
        }

        // Consensus invalidity is a property of these bytes, so no later attempt
        // can succeed. Holding the transaction would keep its inputs reserved
        // against something that can never confirm, and would report a transfer
        // as sent that will never arrive.
        VaultLogPrintf("submitted ok=0 reason=consensus-invalid detail=%s", err_string);
        std::vector<uint256> remove{wtx->GetHash()};
        if (auto removed{RemoveTxs(remove)}; !removed) {
            throw std::runtime_error(strprintf("%s: rejected transaction cleanup failed: %s", __func__, util::ErrorString(removed).original));
        }
        return {submission.error, std::move(err_string)};
    }
    return {};
}

DBErrors CVault::LoadVault()
{
    LOCK(cs_vault);

    Assert(m_spk_managers.empty());
    Assert(m_vault_flags == 0);
    DBErrors nLoadVaultRet = VaultBatch(GetDatabase()).LoadVault(this);

    if (m_spk_managers.empty()) {
        assert(m_external_spk_managers.empty());
        assert(m_internal_spk_managers.empty());
    }

    return nLoadVaultRet;
}

util::Result<void> CVault::RemoveTxs(std::vector<uint256>& txs_to_remove)
{
    AssertLockHeld(cs_vault);
    bilingual_str str_err; // future: make RunWithinTxn return a util::Result
    bool was_txn_committed = RunWithinTxn(GetDatabase(), /*process_desc=*/"remove transactions", [&](VaultBatch& batch) EXCLUSIVE_LOCKS_REQUIRED(cs_vault) {
        util::Result<void> result{RemoveTxs(batch, txs_to_remove)};
        if (!result) str_err = util::ErrorString(result);
        return result.has_value();
    });
    if (!str_err.empty()) return util::Error{str_err};
    if (!was_txn_committed) return util::Error{_("Error starting/committing db txn for vault transactions removal process")};
    return {}; // all good
}

util::Result<void> CVault::RemoveTxs(VaultBatch& batch, std::vector<uint256>& txs_to_remove)
{
    AssertLockHeld(cs_vault);
    if (!batch.HasActiveTxn()) return util::Error{strprintf(_("The transactions removal process can only be executed within a db txn"))};

    // Check for transaction existence and remove entries from disk
    using TxIterator = std::unordered_map<uint256, CVaultTx, SaltedTxidHasher>::const_iterator;
    std::vector<TxIterator> erased_txs;
    bilingual_str str_err;
    for (const uint256& hash : txs_to_remove) {
        auto it_wtx = mapVault.find(hash);
        if (it_wtx == mapVault.end()) {
            return util::Error{strprintf(_("Transaction %s does not belong to this vault"), hash.GetHex())};
        }
        if (!batch.EraseTx(hash)) {
            return util::Error{strprintf(_("Failure removing transaction: %s"), hash.GetHex())};
        }
        erased_txs.emplace_back(it_wtx);
    }

    // Register callback to update the memory state only when the db txn is actually dumped to disk
    batch.RegisterTxnListener({.on_commit = [&, erased_txs]() EXCLUSIVE_LOCKS_REQUIRED(cs_vault) {
                                   // Update the in-memory state and notify upper layers about the removals
                                   for (const auto& it : erased_txs) {
                                       const uint256 hash{it->first};
                                       wtxOrdered.erase(it->second.m_it_wtxOrdered);
                                       for (const auto& txin : it->second.tx->vin)
                                           mapTxSpends.erase(txin.prevout);
                                       mapVault.erase(it);
                                       NotifyTransactionChanged(hash, CT_DELETED);
                                   }

                                   MarkDirty();
                               },
                               .on_abort = {}});

    return {};
}

bool CVault::SetAddressBookWithDB(VaultBatch& batch, const CTxDestination& address, const std::string& strName, const std::optional<AddressPurpose>& new_purpose)
{
    bool fUpdated = false;
    bool is_mine;
    std::optional<AddressPurpose> purpose;
    {
        LOCK(cs_vault);
        std::map<CTxDestination, CAddressBookData>::iterator mi = m_address_book.find(address);
        fUpdated = mi != m_address_book.end() && !mi->second.IsChange();

        CAddressBookData& record = mi != m_address_book.end() ? mi->second : m_address_book[address];
        record.SetLabel(strName);
        is_mine = IsMine(address) != ISMINE_NO;
        if (new_purpose) { /* update purpose only if requested */
            record.purpose = new_purpose;
        } else if (!record.purpose) {
            // A caller passing no purpose means "leave the recorded one alone", but a
            // vault archive can restore an address that never had one (archive.h
            // encodes that as purpose 0), so there may be nothing to leave alone.
            // Derive it exactly as a load does rather than leave a labelled entry the
            // address book cannot classify.
            record.purpose = is_mine ? AddressPurpose::RECEIVE : AddressPurpose::SEND;
        }
        purpose = record.purpose;
    }

    const std::string& encoded_dest = EncodeDestination(address);
    if (new_purpose && !batch.WritePurpose(encoded_dest, PurposeToString(*new_purpose))) {
        VaultLogPrintf("saved ok=0 stage=address-book-purpose");
        return false;
    }
    if (!batch.WriteName(encoded_dest, strName)) {
        VaultLogPrintf("saved ok=0 stage=address-book-name");
        return false;
    }

    CHECK_NONFATAL(purpose.has_value());
    NotifyAddressBookChanged(address, strName, is_mine,
                             *purpose,
                             (fUpdated ? CT_UPDATED : CT_NEW));
    return true;
}

bool CVault::SetAddressBook(const CTxDestination& address, const std::string& strName, const std::optional<AddressPurpose>& purpose)
{
    VaultBatch batch(GetDatabase());
    return SetAddressBookWithDB(batch, address, strName, purpose);
}

bool CVault::DelAddressBook(const CTxDestination& address)
{
    return RunWithinTxn(GetDatabase(), /*process_desc=*/"address book entry removal", [&](VaultBatch& batch) {
        return DelAddressBookWithDB(batch, address);
    });
}

bool CVault::DelAddressBookWithDB(VaultBatch& batch, const CTxDestination& address)
{
    const std::string& dest = EncodeDestination(address);
    {
        LOCK(cs_vault);
        // If we want to delete receiving addresses, we should avoid calling EraseAddressData because it will delete the previously_spent value. Could instead just erase the label so it becomes a change address, and keep the data.
        // NOTE: This isn't a problem for sending addresses because they don't have any data that needs to be kept.
        // When adding new address data, it should be considered here whether to retain or delete it.
        if (IsMine(address)) {
            VaultLogPrintf("rejected reason=ismine-address-not-supported bug_report=%s", CLIENT_BUGREPORT);
            return false;
        }
        // Delete data rows associated with this address
        if (!batch.EraseAddressData(address)) {
            VaultLogPrintf("saved ok=0 stage=address-book-erase-data");
            return false;
        }

        // Delete purpose entry
        if (!batch.ErasePurpose(dest)) {
            VaultLogPrintf("saved ok=0 stage=address-book-erase-purpose");
            return false;
        }

        // Delete name entry
        if (!batch.EraseName(dest)) {
            VaultLogPrintf("saved ok=0 stage=address-book-erase-name");
            return false;
        }

        // finally, remove it from the map
        m_address_book.erase(address);
    }

    // All good, signal changes
    NotifyAddressBookChanged(address, "", /*is_mine=*/false, AddressPurpose::SEND, CT_DELETED);
    return true;
}

size_t CVault::KeypoolCountExternalKeys() const
{
    AssertLockHeld(cs_vault);

    unsigned int count = 0;
    for (auto spk_man : m_external_spk_managers) {
        count += spk_man.second->GetKeyPoolSize();
    }

    return count;
}

unsigned int CVault::GetKeyPoolSize() const
{
    AssertLockHeld(cs_vault);

    unsigned int count = 0;
    for (auto spk_man : GetActiveScriptPubKeyMans()) {
        count += spk_man->GetKeyPoolSize();
    }
    return count;
}

bool CVault::TopUpKeyPool(unsigned int kpSize)
{
    LOCK(cs_vault);
    bool res = true;
    for (auto spk_man : GetActiveScriptPubKeyMans()) {
        res &= spk_man->TopUp(kpSize);
    }
    return res;
}

util::Result<CTxDestination> CVault::GetNewDestination(const OutputType type, const std::string label)
{
    LOCK(cs_vault);
    auto spk_man = GetScriptPubKeyMan(type, /*internal=*/false);
    if (!spk_man) {
        return util::Error{strprintf(_("Error: No %s addresses available."), FormatOutputType(type))};
    }

    auto op_dest = spk_man->GetNewDestination(type);
    if (op_dest) {
        SetAddressBook(*op_dest, label, AddressPurpose::RECEIVE);
    }

    return op_dest;
}

util::Result<CTxDestination> CVault::GetNewChangeDestination(const OutputType type)
{
    LOCK(cs_vault);

    ReserveDestination reservedest(this, type);
    auto op_dest = reservedest.GetReservedDestination(true);
    if (op_dest) reservedest.KeepDestination();

    return op_dest;
}

std::optional<int64_t> CVault::GetOldestKeyPoolTime() const
{
    LOCK(cs_vault);
    if (m_spk_managers.empty()) {
        return std::nullopt;
    }

    std::optional<int64_t> oldest_key{std::numeric_limits<int64_t>::max()};
    for (const auto& spk_man_pair : m_spk_managers) {
        oldest_key = std::min(oldest_key, spk_man_pair.second->GetOldestKeyPoolTime());
    }
    return oldest_key;
}

void CVault::MarkDestinationsDirty(const std::set<CTxDestination>& destinations)
{
    for (auto& entry : mapVault) {
        CVaultTx& wtx = entry.second;
        if (wtx.m_is_cache_empty) continue;
        for (unsigned int i = 0; i < wtx.tx->vout.size(); i++) {
            CTxDestination dst;
            if (ExtractDestination(wtx.tx->vout[i].scriptPubKey, dst) && destinations.count(dst)) {
                wtx.MarkDirty();
                break;
            }
        }
    }
}

void CVault::ForEachAddrBookEntry(const ListAddrBookFunc& func) const
{
    AssertLockHeld(cs_vault);
    for (const std::pair<const CTxDestination, CAddressBookData>& item : m_address_book) {
        const auto& entry = item.second;
        func(item.first, entry.GetLabel(), entry.IsChange(), entry.purpose);
    }
}

std::vector<CTxDestination> CVault::ListAddrBookAddresses(const std::optional<AddrBookFilter>& _filter) const
{
    AssertLockHeld(cs_vault);
    std::vector<CTxDestination> result;
    AddrBookFilter filter = _filter ? *_filter : AddrBookFilter();
    ForEachAddrBookEntry([&result, &filter](const CTxDestination& dest, const std::string& label, bool is_change, const std::optional<AddressPurpose>& purpose) {
        // Filter by change
        if (filter.ignore_change && is_change) return;
        // Filter by label
        if (filter.m_op_label && *filter.m_op_label != label) return;
        // All good
        result.emplace_back(dest);
    });
    return result;
}

std::set<std::string> CVault::ListAddrBookLabels(const std::optional<AddressPurpose> purpose) const
{
    AssertLockHeld(cs_vault);
    std::set<std::string> label_set;
    ForEachAddrBookEntry([&](const CTxDestination& _dest, const std::string& _label,
                             bool _is_change, const std::optional<AddressPurpose>& _purpose) {
        if (_is_change) return;
        if (!purpose || purpose == _purpose) {
            label_set.insert(_label);
        }
    });
    return label_set;
}

util::Result<CTxDestination> ReserveDestination::GetReservedDestination(bool internal)
{
    m_spk_man = pvault->GetScriptPubKeyMan(type, internal);
    if (!m_spk_man) {
        return util::Error{strprintf(_("Error: No %s addresses available."), FormatOutputType(type))};
    }

    if (nIndex == -1) {
        int64_t index;
        auto op_address = m_spk_man->GetReservedDestination(type, internal, index);
        if (!op_address) return op_address;
        nIndex = index;
        address = *op_address;
        fInternal = internal;
    }
    return address;
}

void ReserveDestination::KeepDestination()
{
    if (nIndex != -1) {
        m_spk_man->KeepDestination(nIndex, type);
    }
    nIndex = -1;
    address = CNoDestination();
}

void ReserveDestination::ReturnDestination()
{
    if (nIndex != -1) {
        m_spk_man->ReturnDestination(nIndex, fInternal, address);
    }
    nIndex = -1;
    address = CNoDestination();
}

util::Result<void> CVault::DisplayAddress(const CTxDestination& dest)
{
    CScript scriptPubKey = GetScriptForDestination(dest);
    for (const auto& spk_man : GetScriptPubKeyMans(scriptPubKey)) {
        auto signer_spk_man = dynamic_cast<ExternalSignerScriptPubKeyMan*>(spk_man);
        if (signer_spk_man == nullptr) {
            continue;
        }
        ExternalSigner signer = ExternalSignerScriptPubKeyMan::GetExternalSigner();
        return signer_spk_man->DisplayAddress(dest, signer);
    }
    return util::Error{_("There is no ScriptPubKeyManager for this address")};
}

bool CVault::LockCoin(const COutPoint& output, VaultBatch* batch)
{
    AssertLockHeld(cs_vault);
    setLockedCoins.insert(output);
    if (batch) {
        return batch->WriteLockedUTXO(output);
    }
    return true;
}

bool CVault::UnlockCoin(const COutPoint& output, VaultBatch* batch)
{
    AssertLockHeld(cs_vault);
    bool was_locked = setLockedCoins.erase(output);
    if (batch && was_locked) {
        return batch->EraseLockedUTXO(output);
    }
    return true;
}

bool CVault::UnlockAllCoins()
{
    AssertLockHeld(cs_vault);
    bool success = true;
    VaultBatch batch(GetDatabase());
    for (auto it = setLockedCoins.begin(); it != setLockedCoins.end(); ++it) {
        success &= batch.EraseLockedUTXO(*it);
    }
    setLockedCoins.clear();
    return success;
}

bool CVault::IsLockedCoin(const COutPoint& output) const
{
    AssertLockHeld(cs_vault);
    return setLockedCoins.count(output) > 0;
}

void CVault::ListLockedCoins(std::vector<COutPoint>& vOutpts) const
{
    AssertLockHeld(cs_vault);
    for (std::set<COutPoint>::iterator it = setLockedCoins.begin();
         it != setLockedCoins.end(); it++) {
        COutPoint outpt = (*it);
        vOutpts.push_back(outpt);
    }
}

/** @} */ // end of Actions

/**
 * Compute smart timestamp for a transaction being added to the vault.
 *
 * Logic:
 * - If sending a transaction, assign its timestamp to the current time.
 * - If receiving a transaction outside a block, assign its timestamp to the
 *   current time.
 * - If receiving a transaction during a rescanning process, assign all its
 *   (not already known) transactions' timestamps to the block time.
 * - If receiving a block with a future timestamp, assign all its (not already
 *   known) transactions' timestamps to the current time.
 * - If receiving a block with a past timestamp, before the most recent known
 *   transaction (that we care about), assign all its (not already known)
 *   transactions' timestamps to the same timestamp as that most-recent-known
 *   transaction.
 * - If receiving a block with a past timestamp, but after the most recent known
 *   transaction, assign all its (not already known) transactions' timestamps to
 *   the block time.
 *
 */
unsigned int CVault::ComputeTimeSmart(const CVaultTx& wtx, bool rescanning_old_block) const
{
    std::optional<uint256> block_hash;
    if (auto* conf = wtx.state<TxStateConfirmed>()) {
        block_hash = conf->confirmed_block_hash;
    } else if (auto* conf = wtx.state<TxStateBlockConflicted>()) {
        block_hash = conf->conflicting_block_hash;
    }

    unsigned int nTimeSmart = wtx.nTimeReceived;
    if (block_hash) {
        int64_t blocktime;
        int64_t block_max_time;
        if (chain().findBlock(*block_hash, FoundBlock().time(blocktime).maxTime(block_max_time))) {
            if (rescanning_old_block) {
                nTimeSmart = block_max_time;
            } else {
                int64_t latestNow = wtx.nTimeReceived;
                int64_t latestEntry = 0;

                // Tolerate times up to the last timestamp in the vault not more than 5 minutes into the future
                int64_t latestTolerated = latestNow + 300;
                const TxItems& txOrdered = wtxOrdered;
                for (auto it = txOrdered.rbegin(); it != txOrdered.rend(); ++it) {
                    CVaultTx* const pwtx = it->second;
                    if (pwtx == &wtx) {
                        continue;
                    }
                    int64_t nSmartTime;
                    nSmartTime = pwtx->nTimeSmart;
                    if (!nSmartTime) {
                        nSmartTime = pwtx->nTimeReceived;
                    }
                    if (nSmartTime <= latestTolerated) {
                        latestEntry = nSmartTime;
                        if (nSmartTime > latestNow) {
                            latestNow = nSmartTime;
                        }
                        break;
                    }
                }

                nTimeSmart = std::max(latestEntry, std::min(blocktime, latestNow));
            }
        } else {
            VaultLogPrintf("rejected reason=block-not-in-index tx=%s block=%s", wtx.GetHash().ToString(), block_hash->ToString());
        }
    }
    return nTimeSmart;
}

bool CVault::SetAddressPreviouslySpent(VaultBatch& batch, const CTxDestination& dest, bool used)
{
    if (std::get_if<CNoDestination>(&dest))
        return false;

    if (!used) {
        if (auto* data{common::FindKey(m_address_book, dest)}) data->previously_spent = false;
        return batch.WriteAddressPreviouslySpent(dest, false);
    }

    LoadAddressPreviouslySpent(dest);
    return batch.WriteAddressPreviouslySpent(dest, true);
}

void CVault::LoadAddressPreviouslySpent(const CTxDestination& dest)
{
    m_address_book[dest].previously_spent = true;
}

void CVault::LoadAddressReceiveRequest(const CTxDestination& dest, const std::string& id, const std::string& request)
{
    m_address_book[dest].receive_requests[id] = request;
}

bool CVault::IsAddressPreviouslySpent(const CTxDestination& dest) const
{
    if (auto* data{common::FindKey(m_address_book, dest)}) return data->previously_spent;
    return false;
}

std::vector<std::string> CVault::GetAddressReceiveRequests() const
{
    std::vector<std::string> values;
    for (const auto& [dest, entry] : m_address_book) {
        for (const auto& [id, request] : entry.receive_requests) {
            values.emplace_back(request);
        }
    }
    return values;
}

bool CVault::SetAddressReceiveRequest(VaultBatch& batch, const CTxDestination& dest, const std::string& id, const std::string& value)
{
    if (!batch.WriteAddressReceiveRequest(dest, id, value)) return false;
    m_address_book[dest].receive_requests[id] = value;
    return true;
}

bool CVault::EraseAddressReceiveRequest(VaultBatch& batch, const CTxDestination& dest, const std::string& id)
{
    if (!batch.EraseAddressReceiveRequest(dest, id)) return false;
    m_address_book[dest].receive_requests.erase(id);
    return true;
}

static util::Result<fs::path> GetVaultPath(const std::string& name)
{
    // Do some checking on vault path. It should be either a:
    //
    // 1. Path where a directory can be created.
    // 2. Path to an existing directory.
    // 3. Path to a symlink to a directory.
    // 4. The name of an existing data file in -vaultdir.
    const fs::path vault_path = fsbridge::AbsPathJoin(GetVaultDir(), fs::PathFromString(name));
    fs::file_type path_type = fs::symlink_status(vault_path).type();
    if (!(path_type == fs::file_type::not_found || path_type == fs::file_type::directory ||
          (path_type == fs::file_type::symlink && fs::is_directory(vault_path)) ||
          (path_type == fs::file_type::regular && fs::PathFromString(name).filename() == fs::PathFromString(name)))) {
        return util::Error{Untranslated(strprintf(
            "Invalid -vault path '%s'. -vault path should point to a directory where vault.dat can be stored, "
            "or a location where such a directory could be created (%s)",
            name, fs::quoted(fs::PathToString(GetVaultDir()))))};
    }
    return vault_path;
}

std::unique_ptr<VaultDatabase> MakeVaultDatabase(const std::string& name, const DatabaseOptions& options, DatabaseStatus& status, bilingual_str& error_string)
{
    const auto& vault_path = GetVaultPath(name);
    if (!vault_path) {
        error_string = util::ErrorString(vault_path);
        status = DatabaseStatus::FAILED_BAD_PATH;
        return nullptr;
    }
    return MakeDatabase(*vault_path, options, status, error_string);
}

std::shared_ptr<CVault> CVault::Create(VaultContext& context, const std::string& name, std::unique_ptr<VaultDatabase> database, uint64_t vault_creation_flags, bilingual_str& error, std::vector<bilingual_str>& warnings)
{
    interfaces::Chain* chain = context.chain;
    ArgsManager& args = *Assert(context.args);
    const std::string& vaultFile = database->Filename();

    const auto start{SteadyClock::now()};
    // TODO: Can't use std::make_shared because we need a custom deleter but
    // should be possible to use std::allocate_shared.
    std::shared_ptr<CVault> vaultInstance(new CVault(chain, name, std::move(database)), FlushAndDeleteVault);
    vaultInstance->m_keypool_size = std::max(args.GetIntArg("-keypool", DEFAULT_KEYPOOL_SIZE), int64_t{1});
    vaultInstance->m_notify_tx_changed_script = args.GetArg("-vaultnotify", "");

    // Load vault
    bool rescan_required = false;
    DBErrors nLoadVaultRet = vaultInstance->LoadVault();
    if (nLoadVaultRet != DBErrors::LOAD_OK) {
        if (nLoadVaultRet == DBErrors::CORRUPT) {
            error = strprintf(_("Error loading %s: Vault corrupted"), vaultFile);
            return nullptr;
        } else if (nLoadVaultRet == DBErrors::NONCRITICAL_ERROR) {
            warnings.push_back(strprintf(_("Error reading %s! All keys read correctly, but transaction data"
                                           " or address metadata may be missing or incorrect."),
                                         vaultFile));
        } else if (nLoadVaultRet == DBErrors::TOO_NEW) {
            error = strprintf(_("Error loading %s: Vault requires newer version of %s"), vaultFile, CLIENT_NAME);
            return nullptr;
        } else if (nLoadVaultRet == DBErrors::EXTERNAL_SIGNER_SUPPORT_REQUIRED) {
            error = strprintf(_("Error loading %s: External signer vault being loaded without external signer support compiled"), vaultFile);
            return nullptr;
        } else if (nLoadVaultRet == DBErrors::NEED_RESCAN) {
            warnings.push_back(strprintf(_("Error reading %s! Transaction data may be missing or incorrect."
                                           " Rescanning vault."),
                                         vaultFile));
            rescan_required = true;
        } else if (nLoadVaultRet == DBErrors::UNKNOWN_DESCRIPTOR) {
            error = strprintf(_("Unrecognized descriptor found. Loading vault %s\n\n"
                                "The vault might had been created on a newer version.\n"
                                "Please try running the latest software version.\n"),
                              vaultFile);
            return nullptr;
        } else {
            error = strprintf(_("Error loading %s"), vaultFile);
            return nullptr;
        }
    }

    // This vault is in its first run if there are no ScriptPubKeyMans and it isn't blank or no privkeys
    const bool fFirstRun = vaultInstance->m_spk_managers.empty() &&
                           !vaultInstance->IsVaultFlagSet(VAULT_FLAG_DISABLE_PRIVATE_KEYS) &&
                           !vaultInstance->IsVaultFlagSet(VAULT_FLAG_BLANK_VAULT);
    if (fFirstRun) {
        vaultInstance->SetMinVersion();

        if ((vaultInstance->m_vault_flags & vault_creation_flags) != vault_creation_flags) {
            if (vaultInstance->m_vault_flags == 0) {
                vaultInstance->InitVaultFlags(vault_creation_flags);
            } else {
                vaultInstance->SetVaultFlag(vault_creation_flags);
            }
        }

        if ((vault_creation_flags & VAULT_FLAG_EXTERNAL_SIGNER) || !(vault_creation_flags & (VAULT_FLAG_DISABLE_PRIVATE_KEYS | VAULT_FLAG_BLANK_VAULT))) {
            LOCK(vaultInstance->cs_vault);
            vaultInstance->SetupDescriptorScriptPubKeyMans();
        }

        if (chain && chain->hasChainstate()) {
            FlushSyncPointDuringLoad(*vaultInstance, chain->getTipLocator());
        }
    } else if (vault_creation_flags & VAULT_FLAG_DISABLE_PRIVATE_KEYS) {
        // Make it impossible to disable private keys after creation
        error = strprintf(_("Error loading %s: Private keys can only be disabled during creation"), vaultFile);
        return nullptr;
    } else if (vaultInstance->IsVaultFlagSet(VAULT_FLAG_DISABLE_PRIVATE_KEYS)) {
        for (auto spk_man : vaultInstance->GetActiveScriptPubKeyMans()) {
            if (spk_man->HavePrivateKeys()) {
                warnings.push_back(strprintf(_("Warning: Private keys detected in vault {%s} with disabled private keys"), vaultFile));
                break;
            }
        }
    }

    if (!args.GetArg("-addresstype", "").empty()) {
        std::optional<OutputType> parsed = ParseOutputType(args.GetArg("-addresstype", ""));
        if (!parsed) {
            error = strprintf(_("Unknown address type '%s'"), args.GetArg("-addresstype", ""));
            return nullptr;
        }
        vaultInstance->m_default_address_type = parsed.value();
    }

    if (!args.GetArg("-changetype", "").empty()) {
        std::optional<OutputType> parsed = ParseOutputType(args.GetArg("-changetype", ""));
        if (!parsed) {
            error = strprintf(_("Unknown change type '%s'"), args.GetArg("-changetype", ""));
            return nullptr;
        }
        vaultInstance->m_default_change_type = parsed.value();
    }

    vaultInstance->m_spend_zero_conf_change = args.GetBoolArg("-spendzeroconfchange", DEFAULT_SPEND_ZEROCONF_CHANGE);

    vaultInstance->VaultLogPrintf("loaded elapsed_ms=%d", Ticks<std::chrono::milliseconds>(SteadyClock::now() - start));

    // Try to top up keypool. No-op if the vault is locked.
    vaultInstance->TopUpKeyPool();

    // Cache the first key time
    std::optional<int64_t> time_first_key;
    for (auto spk_man : vaultInstance->GetAllScriptPubKeyMans()) {
        int64_t time = spk_man->GetTimeFirstKey();
        if (!time_first_key || time < *time_first_key) time_first_key = time;
    }
    if (time_first_key) vaultInstance->MaybeUpdateBirthTime(*time_first_key);

    if (chain && chain->hasChainstate() && !AttachChain(vaultInstance, *chain, rescan_required, error, warnings)) {
        vaultInstance->m_chain_notifications_handler.reset(); // Reset this pointer so that the vault will actually be unloaded
        return nullptr;
    }

    if (!chain || !chain->hasChainstate()) {
        std::optional<HeaderTip> header_tip;
        {
            LOCK(context.vaults_mutex);
            header_tip = context.header_tip;
        }
        if (header_tip) {
            LOCK(vaultInstance->cs_vault);
            vaultInstance->ApplyHeaderTip(header_tip->height, header_tip->hash, header_tip->block_time);
        }
    }

    {
        LOCK(vaultInstance->cs_vault);
        vaultInstance->SetBroadcastTransactions(args.GetBoolArg("-vaultbroadcast", DEFAULT_VAULTBROADCAST));
        vaultInstance->VaultLogPrintf("loaded keypool_size=%u", vaultInstance->GetKeyPoolSize());
        vaultInstance->VaultLogPrintf("loaded tx_count=%u", vaultInstance->mapVault.size());
        vaultInstance->VaultLogPrintf("loaded address_book_size=%u", vaultInstance->m_address_book.size());
    }

    return vaultInstance;
}

void CVault::FlushSyncPointDuringLoad(CVault& vault, const CBlockLocator& locator)
{
    vault.chainStateFlushed(locator);
}

bool CVault::AttachChain(const std::shared_ptr<CVault>& vaultInstance, interfaces::Chain& chain, const bool rescan_required, bilingual_str& error, std::vector<bilingual_str>& warnings)
{
    LOCK(vaultInstance->cs_vault);
    // allow setting the chain if it hasn't been set already but prevent changing it
    assert(!vaultInstance->m_chain || vaultInstance->m_chain == &chain);
    vaultInstance->m_chain = &chain;

    // Unless allowed, ensure vault files are not reused across chains:
    if (!gArgs.GetBoolArg("-vaultcrosschain", DEFAULT_VAULTCROSSCHAIN)) {
        VaultBatch batch(vaultInstance->GetDatabase());
        CBlockLocator locator;
        if (batch.ReadBestBlock(locator) && locator.vHave.size() > 0 && chain.getHeight()) {
            // Vault is assumed to be from another chain, if genesis block in the active
            // chain differs from the genesis block known to the vault.
            if (chain.getBlockHash(0) != locator.vHave.back()) {
                error = Untranslated("Vault files should not be reused across chains. Restart quicksilver-daemon with -vaultcrosschain to override.");
                return false;
            }
        }
    }

    // Register vault with validationinterface. It's done before rescan to avoid
    // missing block connections during the rescan.
    // Because of the vault lock being held, block connection notifications are going to
    // be pending on the validation-side until lock release. Blocks that are connected while the
    // rescan is ongoing will not be processed in the rescan but with the block connected notifications,
    // so the vault will only be completeley synced after the notifications delivery.
    // chainStateFlushed notifications are ignored until the rescan is finished
    // so that in case of a shutdown event, the rescan will be repeated at the next start.
    // This is temporary until rescan and notifications delivery are unified under same
    // interface.
    vaultInstance->m_attaching_chain = true; // ignores chainStateFlushed notifications
    vaultInstance->m_chain_notifications_handler = vaultInstance->chain().handleNotifications(vaultInstance);

    // If rescan_required = true, rescan_height remains equal to 0
    int rescan_height = 0;
    if (!rescan_required) {
        VaultBatch batch(vaultInstance->GetDatabase());
        CBlockLocator locator;
        if (batch.ReadBestBlock(locator)) {
            if (const std::optional<int> fork_height = chain.findLocatorFork(locator)) {
                rescan_height = *fork_height;
            }
        }
    }

    const std::optional<int> tip_height = chain.getHeight();
    if (tip_height) {
        vaultInstance->m_last_block_processed = chain.getBlockHash(*tip_height);
        vaultInstance->m_last_block_processed_height = *tip_height;
        chain.findBlock(vaultInstance->m_last_block_processed, FoundBlock().time(vaultInstance->m_last_block_processed_time));
    } else {
        vaultInstance->m_last_block_processed.SetNull();
        vaultInstance->m_last_block_processed_height = -1;
        vaultInstance->m_last_block_processed_time = -1;
    }

    if (tip_height && *tip_height != rescan_height) {
        // No need to read and scan block if block was created before
        // our vault birthday (as adjusted for block time variability)
        std::optional<int64_t> time_first_key = vaultInstance->m_birth_time.load();
        if (time_first_key) {
            FoundBlock found = FoundBlock().height(rescan_height);
            chain.findFirstBlockWithTimeAndHeight(*time_first_key - TIMESTAMP_WINDOW, rescan_height, found);
            if (!found.found) {
                // We were unable to find a block that had a time more recent than our earliest timestamp
                // or a height higher than the vault was synced to, indicating that the vault is newer than the
                // current chain tip. Skip rescanning in this case.
                rescan_height = *tip_height;
            }
        }

        // Technically we could execute the code below in any case, but performing the
        // `while` loop below can make startup very slow, so only check blocks on disk
        // if necessary.
        if (chain.havePruned()) {
            int block_height = *tip_height;
            while (block_height > 0 && chain.haveBlockOnDisk(block_height - 1) && rescan_height != block_height) {
                --block_height;
            }

            if (rescan_height != block_height) {
                // We can't rescan beyond blocks we don't have data for, stop and throw an error.
                // This might happen if a user uses an old vault within a pruned node
                // or if they ran -disablevault for a longer time, then decided to re-enable
                // Exit early and print an error.
                // If a block is pruned after this check, we will load the vault,
                // but fail the rescan with a generic error.
                error = _("Prune: last vault synchronisation goes beyond pruned data. You need to -reindex (download the whole blockchain again in case of pruned node)");
                return false;
            }
        }

        chain.initMessage(_("Rescanning…"));
        vaultInstance->VaultLogPrintf("rescanning blocks=%i start_height=%i", *tip_height - rescan_height, rescan_height);

        {
            VaultRescanReserver reserver(*vaultInstance);
            if (!reserver.reserve() || (ScanResult::SUCCESS != vaultInstance->ScanForVaultTransactions(chain.getBlockHash(rescan_height), rescan_height, /*max_height=*/{}, reserver, /*fUpdate=*/true, /*save_progress=*/true).status)) {
                error = _("Failed to rescan the vault during initialization");
                return false;
            }
        }
        vaultInstance->m_attaching_chain = false;
        FlushSyncPointDuringLoad(*vaultInstance, chain.getTipLocator());
        vaultInstance->GetDatabase().IncrementUpdateCounter();
    }
    vaultInstance->m_attaching_chain = false;

    return true;
}

const CAddressBookData* CVault::FindAddressBookEntry(const CTxDestination& dest, bool allow_change) const
{
    const auto& address_book_it = m_address_book.find(dest);
    if (address_book_it == m_address_book.end()) return nullptr;
    if ((!allow_change) && address_book_it->second.IsChange()) {
        return nullptr;
    }
    return &address_book_it->second;
}

void CVault::postInitProcess()
{
    // Add vault transactions that aren't already in a block to relaypool
    // Do this here as relaypool requires genesis block to be loaded
    ResubmitVaultTransactions(/*relay=*/false, /*force=*/true);

    // Update vault transactions with current relaypool transactions.
    WITH_LOCK(cs_vault, chain().requestRelayPoolTransactions(*this));
}

bool CVault::BackupVault(const std::string& strDest) const
{
    const bool was_recorded = IsBackupRecorded();
    if (!was_recorded && !SetBackupRecorded(true)) {
        return false;
    }

    if (m_chain) {
        CBlockLocator loc;
        WITH_LOCK(cs_vault, chain().findBlock(m_last_block_processed, FoundBlock().locator(loc)));
        if (!loc.IsNull()) {
            VaultBatch batch(GetDatabase());
            batch.WriteBestBlock(loc);
        }
    }
    const bool backed_up = GetDatabase().Backup(strDest);
    if (!backed_up && !was_recorded) {
        SetBackupRecorded(false);
    }
    return backed_up;
}

bool CVault::IsBackupRecorded() const
{
    LOCK(cs_vault);
    bool recorded{false};
    VaultBatch batch(GetDatabase());
    batch.ReadBackupRecorded(recorded);
    return recorded;
}

bool CVault::SetBackupRecorded(bool recorded) const
{
    LOCK(cs_vault);
    VaultBatch batch(GetDatabase());
    return batch.WriteBackupRecorded(recorded);
}

util::Result<AgentAllotmentRecord> CVault::RecordAgentAllotmentSetup(const std::string& label, CAmount funding_limit, CAmount daily_limit)
{
    const std::string trimmed_label{util::TrimString(label)};
    if (trimmed_label.empty()) {
        return util::Error{_("Agent allotment setup requires a label.")};
    }
    if (funding_limit <= 0 || !MoneyRange(funding_limit)) {
        return util::Error{_("Agent allotment setup requires a positive funding amount.")};
    }
    if (!MoneyRange(daily_limit)) {
        return util::Error{_("Agent allotment setup has an invalid daily guardrail amount.")};
    }

    const std::string address_label{strprintf("agent:%s", trimmed_label)};
    auto funding_dest = GetNewDestination(DEFAULT_ADDRESS_TYPE, address_label);
    if (!funding_dest) {
        return util::Error{util::ErrorString(funding_dest)};
    }

    LOCK(cs_vault);
    VaultBatch batch(GetDatabase());
    std::vector<AgentAllotmentRecord> records;
    batch.ReadAgentAllotmentRecords(records);

    unsigned int next_index{static_cast<unsigned int>(records.size() + 1)};
    auto id_exists = [&](const std::string& id) {
        return std::any_of(records.begin(), records.end(), [&](const AgentAllotmentRecord& record) {
            return record.id == id;
        });
    };

    AgentAllotmentRecord record;
    do {
        record.id = strprintf("agent-%u", next_index++);
    } while (id_exists(record.id));
    record.label = trimmed_label;
    record.funding_limit = funding_limit;
    record.daily_limit = daily_limit;
    record.risk_accepted_time = GetTime();
    record.backend_created = false;
    record.funding_address = EncodeDestination(*funding_dest);
    record.policy_status = AgentAllotmentPolicyStatus::PendingIntegration;

    records.push_back(record);
    if (!batch.WriteAgentAllotmentRecords(records)) {
        return util::Error{_("Agent allotment setup could not be saved to this vault.")};
    }
    return record;
}

std::vector<AgentAllotmentRecord> CVault::ListAgentAllotmentRecords() const
{
    LOCK(cs_vault);
    VaultBatch batch(GetDatabase());
    std::vector<AgentAllotmentRecord> records;
    batch.ReadAgentAllotmentRecords(records);
    return records;
}

std::string CVault::AgentAllotmentPolicyRequest(const AgentAllotmentRecord& record, CAmount funding_available) const
{
    UniValue policy{UniValue::VOBJ};
    policy.pushKV("type", "quicksilver.agent_allotment_policy_request");
    policy.pushKV("version", 1);
    policy.pushKV("chain", Params().GetChainTypeString());
    policy.pushKV("genesis_hash", Params().GenesisBlock().GetHash().ToString());
    policy.pushKV("id", record.id);
    policy.pushKV("label", record.label);
    policy.pushKV("funding_address", record.funding_address);
    // Amounts and times cross a machine boundary as JSON strings, not numbers: cinnabar
    // is atomic and MAX_MONEY exceeds 2^53, so a JSON number would lose precision in any
    // reader backed by a double. DecodeAllotmentPolicyRequest requires the string form.
    policy.pushKV("funding_limit_cinnabar", util::ToString(record.funding_limit));
    policy.pushKV("funding_available_cinnabar", util::ToString(funding_available));
    policy.pushKV("daily_limit_cinnabar", util::ToString(record.daily_limit));
    policy.pushKV("risk_accepted_time", util::ToString(record.risk_accepted_time));
    policy.pushKV("request_created_time", util::ToString(GetTime()));
    policy.pushKV("policy_status", record.policy_status == AgentAllotmentPolicyStatus::Enforced ? "enforced" : "pending_integration");
    policy.pushKV("backend_created", record.backend_created);
    return policy.write();
}

util::Result<AgentAllotmentPolicyRequestMetadata> CVault::ValidateAgentAllotmentPolicyRequest(const std::string& request_json) const
{
    auto artifact{agent::DecodeAllotmentPolicyRequest(
        request_json,
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash().ToString())};
    if (!artifact) return util::Error{util::ErrorString(artifact)};

    if (!IsValidDestination(DecodeDestination(artifact->funding_address))) {
        return util::Error{Untranslated("Agent allotment policy request funding address is not valid for this chain.")};
    }

    LOCK(cs_vault);
    VaultBatch batch(GetDatabase());
    std::vector<AgentAllotmentRecord> records;
    batch.ReadAgentAllotmentRecords(records);
    const auto record_it{std::find_if(records.begin(), records.end(), [&](const AgentAllotmentRecord& record) {
        return record.id == artifact->id;
    })};
    if (record_it == records.end()) {
        return util::Error{Untranslated("Agent allotment policy request is not recorded in this vault.")};
    }
    const AgentAllotmentRecord& record{*record_it};
    const AgentAllotmentPolicyStatus policy_status{artifact->policy_status == "enforced" ? AgentAllotmentPolicyStatus::Enforced : AgentAllotmentPolicyStatus::PendingIntegration};
    if (record.label != artifact->label ||
        record.funding_address != artifact->funding_address ||
        record.funding_limit != artifact->policy.funding_limit ||
        record.daily_limit != artifact->policy.daily_limit ||
        record.risk_accepted_time != artifact->risk_accepted_time ||
        record.backend_created != artifact->backend_created ||
        record.policy_status != policy_status) {
        return util::Error{Untranslated("Agent allotment policy request no longer matches this vault's recorded setup.")};
    }

    AgentAllotmentPolicyRequestMetadata request;
    request.id = artifact->id;
    request.label = artifact->label;
    request.funding_address = artifact->funding_address;
    request.funding_limit = artifact->policy.funding_limit;
    request.funding_available = artifact->funding_available;
    request.daily_limit = artifact->policy.daily_limit;
    request.risk_accepted_time = artifact->risk_accepted_time;
    request.request_created_time = artifact->request_created_time;
    request.backend_created = artifact->backend_created;
    request.policy_status = policy_status;
    return request;
}

int CVault::GetTxDepthInMainChain(const CVaultTx& wtx) const
{
    AssertLockHeld(cs_vault);
    if (auto* conf = wtx.state<TxStateConfirmed>()) {
        assert(conf->confirmed_block_height >= 0);
        return GetLastBlockHeight() - conf->confirmed_block_height + 1;
    } else if (auto* conf = wtx.state<TxStateBlockConflicted>()) {
        assert(conf->conflicting_block_height >= 0);
        return -1 * (GetLastBlockHeight() - conf->conflicting_block_height + 1);
    } else {
        return 0;
    }
}

int CVault::GetTxBlocksToMaturity(const CVaultTx& wtx) const
{
    AssertLockHeld(cs_vault);

    if (!wtx.IsCoinBase()) {
        return 0;
    }
    int chain_depth = GetTxDepthInMainChain(wtx);
    assert(chain_depth >= 0); // coinbase tx should not be conflicted
    return std::max(0, (COINBASE_MATURITY + 1) - chain_depth);
}

bool CVault::IsTxImmatureCoinBase(const CVaultTx& wtx) const
{
    AssertLockHeld(cs_vault);

    // note GetBlocksToMaturity is 0 for non-coinbase tx
    return GetTxBlocksToMaturity(wtx) > 0;
}

bool CVault::IsCrypted() const
{
    return HasEncryptionKeys();
}

bool CVault::IsLocked() const
{
    if (!IsCrypted()) {
        return false;
    }
    LOCK(cs_vault);
    return vMasterKey.empty();
}

bool CVault::Lock()
{
    if (!IsCrypted())
        return false;

    {
        LOCK2(m_relock_mutex, cs_vault);
        if (!vMasterKey.empty()) {
            memory_cleanse(vMasterKey.data(), vMasterKey.size() * sizeof(decltype(vMasterKey)::value_type));
            vMasterKey.clear();
        }
    }

    NotifyStatusChanged(this);
    return true;
}

bool CVault::Unlock(const CKeyingMaterial& vMasterKeyIn)
{
    {
        LOCK(cs_vault);
        for (const auto& spk_man_pair : m_spk_managers) {
            if (!spk_man_pair.second->CheckDecryptionKey(vMasterKeyIn)) {
                return false;
            }
        }
        vMasterKey = vMasterKeyIn;
    }
    NotifyStatusChanged(this);
    return true;
}

std::set<ScriptPubKeyMan*> CVault::GetActiveScriptPubKeyMans() const
{
    std::set<ScriptPubKeyMan*> spk_mans;
    for (bool internal : {false, true}) {
        for (OutputType t : OUTPUT_TYPES) {
            auto spk_man = GetScriptPubKeyMan(t, internal);
            if (spk_man) {
                spk_mans.insert(spk_man);
            }
        }
    }
    return spk_mans;
}

bool CVault::IsActiveScriptPubKeyMan(const ScriptPubKeyMan& spkm) const
{
    for (const auto& [_, ext_spkm] : m_external_spk_managers) {
        if (ext_spkm == &spkm) return true;
    }
    for (const auto& [_, int_spkm] : m_internal_spk_managers) {
        if (int_spkm == &spkm) return true;
    }
    return false;
}

std::set<ScriptPubKeyMan*> CVault::GetAllScriptPubKeyMans() const
{
    std::set<ScriptPubKeyMan*> spk_mans;
    for (const auto& spk_man_pair : m_spk_managers) {
        spk_mans.insert(spk_man_pair.second.get());
    }
    return spk_mans;
}

ScriptPubKeyMan* CVault::GetScriptPubKeyMan(const OutputType& type, bool internal) const
{
    const std::map<OutputType, ScriptPubKeyMan*>& spk_managers = internal ? m_internal_spk_managers : m_external_spk_managers;
    std::map<OutputType, ScriptPubKeyMan*>::const_iterator it = spk_managers.find(type);
    if (it == spk_managers.end()) {
        return nullptr;
    }
    return it->second;
}

std::set<ScriptPubKeyMan*> CVault::GetScriptPubKeyMans(const CScript& script) const
{
    std::set<ScriptPubKeyMan*> spk_mans;

    // Search the cache for relevant SPKMs instead of iterating m_spk_managers
    const auto& it = m_cached_spks.find(script);
    if (it != m_cached_spks.end()) {
        spk_mans.insert(it->second.begin(), it->second.end());
    }
    SignatureData sigdata;
    Assume(std::all_of(spk_mans.begin(), spk_mans.end(), [&script, &sigdata](ScriptPubKeyMan* spkm) { return spkm->CanProvide(script, sigdata); }));

    return spk_mans;
}

ScriptPubKeyMan* CVault::GetScriptPubKeyMan(const uint256& id) const
{
    if (m_spk_managers.count(id) > 0) {
        return m_spk_managers.at(id).get();
    }
    return nullptr;
}

std::unique_ptr<SigningProvider> CVault::GetSolvingProvider(const CScript& script) const
{
    SignatureData sigdata;
    return GetSolvingProvider(script, sigdata);
}

std::unique_ptr<SigningProvider> CVault::GetSolvingProvider(const CScript& script, SignatureData& sigdata) const
{
    // Search the cache for relevant SPKMs instead of iterating m_spk_managers
    const auto& it = m_cached_spks.find(script);
    if (it != m_cached_spks.end()) {
        // All spkms for a given script must already be able to make a SigningProvider for the script, so just return the first one.
        Assume(it->second.at(0)->CanProvide(script, sigdata));
        return it->second.at(0)->GetSolvingProvider(script);
    }

    return nullptr;
}

std::vector<VaultDescriptor> CVault::GetVaultDescriptors(const CScript& script) const
{
    std::vector<VaultDescriptor> descs;
    for (const auto spk_man : GetScriptPubKeyMans(script)) {
        if (const auto desc_spk_man = dynamic_cast<DescriptorScriptPubKeyMan*>(spk_man)) {
            LOCK(desc_spk_man->cs_desc_man);
            descs.push_back(desc_spk_man->GetVaultDescriptor());
        }
    }
    return descs;
}

void CVault::AddScriptPubKeyMan(const uint256& id, std::unique_ptr<ScriptPubKeyMan> spkm_man)
{
    // Add spkm_man to m_spk_managers before calling any method
    // that might access it.
    const auto& spkm = m_spk_managers[id] = std::move(spkm_man);

    // Update birth time if needed
    MaybeUpdateBirthTime(spkm->GetTimeFirstKey());
}

bool CVault::WithEncryptionKey(std::function<bool(const CKeyingMaterial&)> cb) const
{
    LOCK(cs_vault);
    return cb(vMasterKey);
}

bool CVault::HasEncryptionKeys() const
{
    return !mapMasterKeys.empty();
}

void CVault::ConnectScriptPubKeyManNotifiers()
{
    for (const auto& spk_man : GetActiveScriptPubKeyMans()) {
        spk_man->NotifyCanGetAddressesChanged.connect(NotifyCanGetAddressesChanged);
        spk_man->NotifyFirstKeyTimeChanged.connect(std::bind(&CVault::MaybeUpdateBirthTime, this, std::placeholders::_2));
    }
}

DescriptorScriptPubKeyMan& CVault::LoadDescriptorScriptPubKeyMan(uint256 id, VaultDescriptor& desc)
{
    DescriptorScriptPubKeyMan* spk_manager;
    if (IsVaultFlagSet(VAULT_FLAG_EXTERNAL_SIGNER)) {
        spk_manager = new ExternalSignerScriptPubKeyMan(*this, desc, m_keypool_size);
    } else {
        spk_manager = new DescriptorScriptPubKeyMan(*this, desc, m_keypool_size);
    }
    AddScriptPubKeyMan(id, std::unique_ptr<ScriptPubKeyMan>(spk_manager));
    return *spk_manager;
}

DescriptorScriptPubKeyMan& CVault::SetupDescriptorScriptPubKeyMan(VaultBatch& batch, const CExtKey& master_key, const OutputType& output_type, bool internal)
{
    AssertLockHeld(cs_vault);
    auto spk_manager = std::unique_ptr<DescriptorScriptPubKeyMan>(new DescriptorScriptPubKeyMan(*this, m_keypool_size));
    if (IsCrypted()) {
        if (IsLocked()) {
            throw std::runtime_error(std::string(__func__) + ": Vault is locked, cannot setup new descriptors");
        }
        if (!spk_manager->CheckDecryptionKey(vMasterKey) && !spk_manager->Encrypt(vMasterKey, &batch)) {
            throw std::runtime_error(std::string(__func__) + ": Could not encrypt new descriptors");
        }
    }
    spk_manager->SetupDescriptorGeneration(batch, master_key, output_type, internal);
    DescriptorScriptPubKeyMan* out = spk_manager.get();
    uint256 id = spk_manager->GetID();
    AddScriptPubKeyMan(id, std::move(spk_manager));
    AddActiveScriptPubKeyManWithDb(batch, id, output_type, internal);
    return *out;
}

void CVault::SetupDescriptorScriptPubKeyMans(VaultBatch& batch, const CExtKey& master_key)
{
    AssertLockHeld(cs_vault);
    for (bool internal : {false, true}) {
        for (OutputType t : OUTPUT_TYPES) {
            SetupDescriptorScriptPubKeyMan(batch, master_key, t, internal);
        }
    }
}

void CVault::SetupOwnDescriptorScriptPubKeyMans(VaultBatch& batch)
{
    AssertLockHeld(cs_vault);
    assert(!IsVaultFlagSet(VAULT_FLAG_EXTERNAL_SIGNER));
    // Make a seed
    CKey seed_key = GenerateRandomKey();
    CPubKey seed = seed_key.GetPubKey();
    assert(seed_key.VerifyPubKey(seed));

    // Get the extended key
    CExtKey master_key;
    master_key.SetSeed(seed_key);

    SetupDescriptorScriptPubKeyMans(batch, master_key);
}

void CVault::SetupDescriptorScriptPubKeyMans()
{
    AssertLockHeld(cs_vault);

    if (!IsVaultFlagSet(VAULT_FLAG_EXTERNAL_SIGNER)) {
        if (!RunWithinTxn(GetDatabase(), /*process_desc=*/"setup descriptors", [&](VaultBatch& batch) EXCLUSIVE_LOCKS_REQUIRED(cs_vault) {
                SetupOwnDescriptorScriptPubKeyMans(batch);
                return true;
            })) throw std::runtime_error("Error: cannot process db transaction for descriptors setup");
    } else {
        ExternalSigner signer = ExternalSignerScriptPubKeyMan::GetExternalSigner();

        // TODO: add account parameter
        int account = 0;
        UniValue signer_res = signer.GetDescriptors(account);

        if (!signer_res.isObject()) throw std::runtime_error(std::string(__func__) + ": Unexpected result");

        VaultBatch batch(GetDatabase());
        if (!batch.TxnBegin()) throw std::runtime_error("Error: cannot create db transaction for descriptors import");

        // One active ScriptPubKeyMan per (output type, chain). A throw
        // here is rolled back by SQLiteBatch::Close aborting the txn.
        std::map<std::pair<OutputType, bool>, std::string> imported;
        for (bool internal : {false, true}) {
            const UniValue& descriptor_vals = signer_res.find_value(internal ? "internal" : "receive");
            if (!descriptor_vals.isArray()) throw std::runtime_error(std::string(__func__) + ": Unexpected result");
            for (const UniValue& desc_val : descriptor_vals.get_array().getValues()) {
                const std::string& desc_str = desc_val.getValStr();
                FlatSigningProvider keys;
                std::string desc_error;
                auto descs = Parse(desc_str, keys, desc_error, false);
                if (descs.empty()) {
                    throw std::runtime_error(std::string(__func__) + ": Invalid descriptor \"" + desc_str + "\" (" + desc_error + ")");
                }
                auto& desc = descs.at(0);
                if (!desc->GetOutputType()) {
                    throw std::runtime_error(std::string(__func__) + ": Descriptor \"" + desc_str + "\" has no address type");
                }
                OutputType t = *desc->GetOutputType();
                const auto key = std::make_pair(t, internal);
                auto it = imported.find(key);
                if (it != imported.end()) {
                    throw std::runtime_error(std::string(__func__) + ": Multiple descriptors for output type " + FormatOutputType(t) + " (" + (internal ? "internal" : "receive") + "): \"" + it->second + "\" and \"" + desc_str + "\"");
                }
                auto spk_manager = std::unique_ptr<ExternalSignerScriptPubKeyMan>(new ExternalSignerScriptPubKeyMan(*this, m_keypool_size));
                spk_manager->SetupDescriptor(batch, std::move(desc));
                uint256 id = spk_manager->GetID();
                AddScriptPubKeyMan(id, std::move(spk_manager));
                AddActiveScriptPubKeyManWithDb(batch, id, t, internal);
                imported.emplace(key, desc_str);
            }
        }

        for (bool internal : {false, true}) {
            for (OutputType t : OUTPUT_TYPES) {
                if (!imported.contains(std::make_pair(t, internal))) {
                    VaultLogPrintf("signer missing type=%s internal=%d", FormatOutputType(t), internal);
                }
            }
        }

        // Ensure imported descriptors are committed to disk
        if (!batch.TxnCommit()) throw std::runtime_error("Error: cannot commit db transaction for descriptors import");
    }
}

void CVault::AddActiveScriptPubKeyMan(uint256 id, OutputType type, bool internal)
{
    VaultBatch batch(GetDatabase());
    return AddActiveScriptPubKeyManWithDb(batch, id, type, internal);
}

void CVault::AddActiveScriptPubKeyManWithDb(VaultBatch& batch, uint256 id, OutputType type, bool internal)
{
    if (!batch.WriteActiveScriptPubKeyMan(static_cast<uint8_t>(type), id, internal)) {
        throw std::runtime_error(std::string(__func__) + ": writing active ScriptPubKeyMan id failed");
    }
    LoadActiveScriptPubKeyMan(id, type, internal);
}

void CVault::LoadActiveScriptPubKeyMan(uint256 id, OutputType type, bool internal)
{
    Assert(IsVaultFlagSet(VAULT_FLAG_DESCRIPTORS));

    VaultLogPrintf("ready spkman=%s type=%s internal=%d active=1", id.ToString(), FormatOutputType(type), internal);
    auto& spk_mans = internal ? m_internal_spk_managers : m_external_spk_managers;
    auto& spk_mans_other = internal ? m_external_spk_managers : m_internal_spk_managers;
    auto spk_man = m_spk_managers.at(id).get();
    spk_mans[type] = spk_man;

    const auto it = spk_mans_other.find(type);
    if (it != spk_mans_other.end() && it->second == spk_man) {
        spk_mans_other.erase(type);
    }

    NotifyCanGetAddressesChanged();
}

void CVault::DeactivateScriptPubKeyMan(uint256 id, OutputType type, bool internal)
{
    auto spk_man = GetScriptPubKeyMan(type, internal);
    if (spk_man != nullptr && spk_man->GetID() == id) {
        VaultLogPrintf("ready spkman=%s type=%s internal=%d active=0", id.ToString(), FormatOutputType(type), internal);
        VaultBatch batch(GetDatabase());
        if (!batch.EraseActiveScriptPubKeyMan(static_cast<uint8_t>(type), internal)) {
            throw std::runtime_error(std::string(__func__) + ": erasing active ScriptPubKeyMan id failed");
        }

        auto& spk_mans = internal ? m_internal_spk_managers : m_external_spk_managers;
        spk_mans.erase(type);
    }

    NotifyCanGetAddressesChanged();
}

DescriptorScriptPubKeyMan* CVault::GetDescriptorScriptPubKeyMan(const VaultDescriptor& desc) const
{
    for (auto& spk_man_pair : m_spk_managers) {
        // Try to downcast to DescriptorScriptPubKeyMan then check if the descriptors match
        DescriptorScriptPubKeyMan* spk_manager = dynamic_cast<DescriptorScriptPubKeyMan*>(spk_man_pair.second.get());
        if (spk_manager != nullptr && spk_manager->HasVaultDescriptor(desc)) {
            return spk_manager;
        }
    }

    return nullptr;
}

std::optional<bool> CVault::IsInternalScriptPubKeyMan(ScriptPubKeyMan* spk_man) const
{
    // only active ScriptPubKeyMan can be internal
    if (!GetActiveScriptPubKeyMans().count(spk_man)) {
        return std::nullopt;
    }

    const auto desc_spk_man = dynamic_cast<DescriptorScriptPubKeyMan*>(spk_man);
    if (!desc_spk_man) {
        throw std::runtime_error(std::string(__func__) + ": unexpected ScriptPubKeyMan type.");
    }

    LOCK(desc_spk_man->cs_desc_man);
    const auto& type = desc_spk_man->GetVaultDescriptor().descriptor->GetOutputType();
    assert(type.has_value());

    return GetScriptPubKeyMan(*type, /* internal= */ true) == desc_spk_man;
}

ScriptPubKeyMan* CVault::AddVaultDescriptor(VaultDescriptor& desc, const FlatSigningProvider& signing_provider, const std::string& label, bool internal)
{
    AssertLockHeld(cs_vault);

    Assert(IsVaultFlagSet(VAULT_FLAG_DESCRIPTORS));

    auto spk_man = GetDescriptorScriptPubKeyMan(desc);
    if (spk_man) {
        VaultLogPrintf("saved descriptor=%s existing=1", desc.descriptor->ToString());
        spk_man->UpdateVaultDescriptor(desc);
    } else {
        auto new_spk_man = std::unique_ptr<DescriptorScriptPubKeyMan>(new DescriptorScriptPubKeyMan(*this, desc, m_keypool_size));
        spk_man = new_spk_man.get();

        // Save the descriptor to memory
        uint256 id = new_spk_man->GetID();
        AddScriptPubKeyMan(id, std::move(new_spk_man));
    }

    // Add the private keys to the descriptor
    for (const auto& entry : signing_provider.keys) {
        const CKey& key = entry.second;
        spk_man->AddDescriptorKey(key, key.GetPubKey());
    }

    // Top up key pool, the manager will generate new scriptPubKeys internally
    if (!spk_man->TopUp()) {
        VaultLogPrintf("rejected reason=spk-topup-failed");
        return nullptr;
    }

    // Apply the label if necessary
    // Note: we disable labels for ranged descriptors
    if (!desc.descriptor->IsRange()) {
        auto script_pub_keys = spk_man->GetScriptPubKeys();
        if (script_pub_keys.empty()) {
            VaultLogPrintf("rejected reason=spk-generate-failed detail=cache-empty");
            return nullptr;
        }

        if (!internal) {
            for (const auto& script : script_pub_keys) {
                CTxDestination dest;
                if (ExtractDestination(script, dest)) {
                    SetAddressBook(dest, label, AddressPurpose::RECEIVE);
                }
            }
        }
    }

    // Save the descriptor to DB
    spk_man->WriteDescriptor();

    return spk_man;
}

bool CVault::CanGrindR() const
{
    return !IsVaultFlagSet(VAULT_FLAG_EXTERNAL_SIGNER);
}

void CVault::CacheNewScriptPubKeys(const std::set<CScript>& spks, ScriptPubKeyMan* spkm)
{
    for (const auto& script : spks) {
        m_cached_spks[script].push_back(spkm);
    }
}

void CVault::TopUpCallback(const std::set<CScript>& spks, ScriptPubKeyMan* spkm)
{
    // Update scriptPubKey cache
    CacheNewScriptPubKeys(spks, spkm);
}

std::set<CExtPubKey> CVault::GetActiveHDPubKeys() const
{
    AssertLockHeld(cs_vault);

    Assert(IsVaultFlagSet(VAULT_FLAG_DESCRIPTORS));

    std::set<CExtPubKey> active_xpubs;
    for (const auto& spkm : GetActiveScriptPubKeyMans()) {
        const DescriptorScriptPubKeyMan* desc_spkm = dynamic_cast<DescriptorScriptPubKeyMan*>(spkm);
        assert(desc_spkm);
        LOCK(desc_spkm->cs_desc_man);
        VaultDescriptor w_desc = desc_spkm->GetVaultDescriptor();

        std::set<CPubKey> desc_pubkeys;
        std::set<CExtPubKey> desc_xpubs;
        w_desc.descriptor->GetPubKeys(desc_pubkeys, desc_xpubs);
        active_xpubs.merge(std::move(desc_xpubs));
    }
    return active_xpubs;
}

std::optional<CKey> CVault::GetKey(const CKeyID& keyid) const
{
    Assert(IsVaultFlagSet(VAULT_FLAG_DESCRIPTORS));

    for (const auto& spkm : GetAllScriptPubKeyMans()) {
        const DescriptorScriptPubKeyMan* desc_spkm = dynamic_cast<DescriptorScriptPubKeyMan*>(spkm);
        assert(desc_spkm);
        LOCK(desc_spkm->cs_desc_man);
        if (std::optional<CKey> key = desc_spkm->GetKey(keyid)) {
            return key;
        }
    }
    return std::nullopt;
}
} // namespace vault
