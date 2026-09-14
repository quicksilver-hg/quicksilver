// Copyright (c) 2019-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_VAULT_SCRIPTPUBKEYMAN_H
#define QUICKSILVER_VAULT_SCRIPTPUBKEYMAN_H

#include <addresstype.h>
#include <common/messages.h>
#include <common/signmessage.h>
#include <common/types.h>
#include <logging.h>
#include <node/types.h>
#include <psqt.h>
#include <script/descriptor.h>
#include <script/script.h>
#include <script/signingprovider.h>
#include <util/result.h>
#include <util/time.h>
#include <vault/crypter.h>
#include <vault/types.h>
#include <vault/vaultdb.h>
#include <vault/vaultutil.h>

#include <boost/signals2/signal.hpp>

#include <functional>
#include <optional>
#include <unordered_map>

enum class OutputType;

namespace vault {
class ScriptPubKeyMan;

// Vault storage things that ScriptPubKeyMans need in order to be able to store things to the vault database.
// It provides access to things that are part of the entire vault and not specific to a ScriptPubKeyMan such as
// vault flags, vault version, encryption keys, encryption status, and the database itself. This allows a
// ScriptPubKeyMan to have callbacks into CVault without causing a circular dependency.
// VaultStorage should be the same for all ScriptPubKeyMans of a vault.
class VaultStorage
{
public:
    virtual ~VaultStorage() = default;
    virtual std::string GetDisplayName() const = 0;
    virtual VaultDatabase& GetDatabase() const = 0;
    virtual bool IsVaultFlagSet(uint64_t) const = 0;
    virtual void UnsetBlankVaultFlag(VaultBatch&) = 0;
    //! Pass the encryption key to cb().
    virtual bool WithEncryptionKey(std::function<bool(const CKeyingMaterial&)> cb) const = 0;
    virtual bool HasEncryptionKeys() const = 0;
    virtual bool IsLocked() const = 0;
    //! Callback function for after TopUp completes containing any scripts that were added by a SPKMan
    virtual void TopUpCallback(const std::set<CScript>&, ScriptPubKeyMan*) = 0;
};

//! Constant representing an unknown spkm creation time
static constexpr int64_t UNKNOWN_TIME = std::numeric_limits<int64_t>::max();

//! Default for -keypool
static const unsigned int DEFAULT_KEYPOOL_SIZE = 1000;

struct VaultDestination {
    CTxDestination dest;
    std::optional<bool> internal;
};

/*
 * A class implementing ScriptPubKeyMan manages some (or all) scriptPubKeys used in a vault.
 * It contains the scripts and keys related to the scriptPubKeys it manages.
 * A ScriptPubKeyMan will be able to give out scriptPubKeys to be used, as well as marking
 * when a scriptPubKey has been used. It also handles when and how to store a scriptPubKey
 * and its related scripts and keys, including encryption.
 */
class ScriptPubKeyMan
{
protected:
    VaultStorage& m_storage;

public:
    explicit ScriptPubKeyMan(VaultStorage& storage) : m_storage(storage) {}
    virtual ~ScriptPubKeyMan() = default;
    virtual util::Result<CTxDestination> GetNewDestination(const OutputType type) { return util::Error{Untranslated("Not supported")}; }
    virtual isminetype IsMine(const CScript& script) const { return ISMINE_NO; }

    //! Check that the given decryption key is valid for this ScriptPubKeyMan, i.e. it decrypts all of the keys handled by it.
    virtual bool CheckDecryptionKey(const CKeyingMaterial& master_key) { return false; }
    virtual bool Encrypt(const CKeyingMaterial& master_key, VaultBatch* batch) { return false; }

    virtual util::Result<CTxDestination> GetReservedDestination(const OutputType type, bool internal, int64_t& index) { return util::Error{Untranslated("Not supported")}; }
    virtual void KeepDestination(int64_t index, const OutputType& type) {}
    virtual void ReturnDestination(int64_t index, bool internal, const CTxDestination& addr) {}

    /** Fills internal address pool. Use within ScriptPubKeyMan implementations should be used sparingly and only
     * when something from the address pool is removed, excluding GetNewDestination and GetReservedDestination.
     * External vault code is primarily responsible for topping up prior to fetching new addresses
     */
    virtual bool TopUp(unsigned int size = 0) { return false; }

    /** Mark unused addresses as being used
     * Affects all keys up to and including the one determined by provided script.
     *
     * @param script determines the last key to mark as used
     *
     * @return All of the addresses affected
     */
    virtual std::vector<VaultDestination> MarkUnusedAddresses(const CScript& script) { return {}; }

    /* Returns true if HD is enabled */
    virtual bool IsHDEnabled() const { return false; }

    /* Returns true if the vault can give out new addresses. This means it has keys in the keypool or can generate new keys */
    virtual bool CanGetAddresses(bool internal = false) const { return false; }

    virtual bool HavePrivateKeys() const { return false; }

    virtual std::optional<int64_t> GetOldestKeyPoolTime() const { return GetTime(); }

    virtual unsigned int GetKeyPoolSize() const { return 0; }

    virtual int64_t GetTimeFirstKey() const { return 0; }

    virtual std::unique_ptr<CKeyMetadata> GetMetadata(const CTxDestination& dest) const { return nullptr; }

    virtual std::unique_ptr<SigningProvider> GetSolvingProvider(const CScript& script) const { return nullptr; }

    /** Whether this ScriptPubKeyMan can provide a SigningProvider (via GetSolvingProvider) that, combined with
     * sigdata, can produce solving data.
     */
    virtual bool CanProvide(const CScript& script, SignatureData& sigdata) { return false; }

    /** Creates new signatures and adds them to the transaction. Returns whether all inputs were signed */
    virtual bool SignTransaction(CMutableTransaction& tx, const std::map<COutPoint, Coin>& coins, int sighash, std::map<int, bilingual_str>& input_errors) const { return false; }
    /** Sign a message with the given script */
    virtual SigningResult SignMessage(const std::string& message, const PKHash& pkhash, std::string& str_sig) const { return SigningResult::SIGNING_FAILED; };
    /** Adds script and derivation path information to a PSQT, and optionally signs it. */
    virtual std::optional<common::PSQTError> FillPSQT(PartiallySignedQuicksilverTransaction& psqt, const PrecomputedTransactionData& txdata, int sighash_type = SIGHASH_DEFAULT, bool sign = true, bool bip32derivs = false, int* n_signed = nullptr, bool finalize = true) const { return common::PSQTError::UNSUPPORTED; }

    virtual uint256 GetID() const { return uint256(); }

    /** Returns a set of all the scriptPubKeys that this ScriptPubKeyMan watches */
    virtual std::unordered_set<CScript, SaltedSipHasher> GetScriptPubKeys() const { return {}; };

    /** Tags vault log output with the vault name to ease debugging in multi-vault use cases */
    template <typename... Params>
    void VaultLogPrintf(util::ConstevalFormatString<sizeof...(Params)> vault_fmt, const Params&... params) const
    {
        std::string msg{tfm::format(vault_fmt, params...)};
        while (msg.ends_with('\n'))
            msg.pop_back();
        LogInfo(HgLog::VAULT, "%s vault=%s", msg, m_storage.GetDisplayName());
    };

    /** Keypool has new keys */
    boost::signals2::signal<void()> NotifyCanGetAddressesChanged;

    /** Birth time changed */
    boost::signals2::signal<void(const ScriptPubKeyMan* spkm, int64_t new_birth_time)> NotifyFirstKeyTimeChanged;
};

class DescriptorScriptPubKeyMan : public ScriptPubKeyMan
{
private:
    using ScriptPubKeyMap = std::map<CScript, int32_t>; // Map of scripts to descriptor range index
    using PubKeyMap = std::map<CPubKey, int32_t>;       // Map of pubkeys involved in scripts to descriptor range index
    using CryptedKeyMap = std::map<CKeyID, std::pair<CPubKey, std::vector<unsigned char>>>;
    using KeyMap = std::map<CKeyID, CKey>;

    ScriptPubKeyMap m_map_script_pub_keys GUARDED_BY(cs_desc_man);
    PubKeyMap m_map_pubkeys GUARDED_BY(cs_desc_man);
    int32_t m_max_cached_index = -1;

    KeyMap m_map_keys GUARDED_BY(cs_desc_man);
    CryptedKeyMap m_map_crypted_keys GUARDED_BY(cs_desc_man);

    //! keeps track of whether Unlock has run a thorough check before
    bool m_decryption_thoroughly_checked = false;

    //! Number of pre-generated keys/scripts (part of the look-ahead process, used to detect payments)
    int64_t m_keypool_size GUARDED_BY(cs_desc_man){DEFAULT_KEYPOOL_SIZE};

    bool AddDescriptorKeyWithDB(VaultBatch& batch, const CKey& key, const CPubKey& pubkey) EXCLUSIVE_LOCKS_REQUIRED(cs_desc_man);

    KeyMap GetKeys() const EXCLUSIVE_LOCKS_REQUIRED(cs_desc_man);

    // Cached FlatSigningProviders to avoid regenerating them each time they are needed.
    mutable std::map<int32_t, FlatSigningProvider> m_map_signing_providers;
    // Fetch the SigningProvider for the given script and optionally include private keys
    std::unique_ptr<FlatSigningProvider> GetSigningProvider(const CScript& script, bool include_private = false) const;
    // Fetch the SigningProvider for a given index and optionally include private keys. Called by the above functions.
    std::unique_ptr<FlatSigningProvider> GetSigningProvider(int32_t index, bool include_private = false) const EXCLUSIVE_LOCKS_REQUIRED(cs_desc_man);

protected:
    VaultDescriptor m_vault_descriptor GUARDED_BY(cs_desc_man);

    //! Same as 'TopUp' but designed for use within a batch transaction context
    bool TopUpWithDB(VaultBatch& batch, unsigned int size = 0);

public:
    DescriptorScriptPubKeyMan(VaultStorage& storage, VaultDescriptor& descriptor, int64_t keypool_size)
        : ScriptPubKeyMan(storage),
          m_keypool_size(keypool_size),
          m_vault_descriptor(descriptor)
    {
    }
    DescriptorScriptPubKeyMan(VaultStorage& storage, int64_t keypool_size)
        : ScriptPubKeyMan(storage),
          m_keypool_size(keypool_size)
    {
    }

    mutable RecursiveMutex cs_desc_man;

    util::Result<CTxDestination> GetNewDestination(const OutputType type) override;
    isminetype IsMine(const CScript& script) const override;

    bool CheckDecryptionKey(const CKeyingMaterial& master_key) override;
    bool Encrypt(const CKeyingMaterial& master_key, VaultBatch* batch) override;

    util::Result<CTxDestination> GetReservedDestination(const OutputType type, bool internal, int64_t& index) override;
    void ReturnDestination(int64_t index, bool internal, const CTxDestination& addr) override;

    // Tops up the descriptor cache and m_map_script_pub_keys. The cache is stored in the vault file
    // and is used to expand the descriptor in GetNewDestination. DescriptorScriptPubKeyMan relies
    // more on ephemeral data than earlier keypool-based vaults. For vaults using unhardened derivation
    // (with or without private keys), the "keypool" is a single xpub.
    bool TopUp(unsigned int size = 0) override;

    std::vector<VaultDestination> MarkUnusedAddresses(const CScript& script) override;

    bool IsHDEnabled() const override;

    //! Setup descriptors based on the given CExtkey
    bool SetupDescriptorGeneration(VaultBatch& batch, const CExtKey& master_key, OutputType addr_type, bool internal);

    bool HavePrivateKeys() const override;
    bool HasPrivKey(const CKeyID& keyid) const EXCLUSIVE_LOCKS_REQUIRED(cs_desc_man);
    //! Retrieve the particular key if it is available. Returns nullopt if the key is not in the vault, or if the vault is locked.
    std::optional<CKey> GetKey(const CKeyID& keyid) const EXCLUSIVE_LOCKS_REQUIRED(cs_desc_man);

    std::optional<int64_t> GetOldestKeyPoolTime() const override;
    unsigned int GetKeyPoolSize() const override;

    int64_t GetTimeFirstKey() const override;

    std::unique_ptr<CKeyMetadata> GetMetadata(const CTxDestination& dest) const override;

    bool CanGetAddresses(bool internal = false) const override;

    std::unique_ptr<SigningProvider> GetSolvingProvider(const CScript& script) const override;

    bool CanProvide(const CScript& script, SignatureData& sigdata) override;

    // Fetch the SigningProvider for the given pubkey and always include private keys. This should only be called by signing code.
    std::unique_ptr<FlatSigningProvider> GetSigningProvider(const CPubKey& pubkey) const;
    // Fetch the SigningProvider for a vault-owned script and include private keys for explicit key handoff paths.
    std::unique_ptr<FlatSigningProvider> GetPrivateSigningProvider(const CScript& script) const;

    bool SignTransaction(CMutableTransaction& tx, const std::map<COutPoint, Coin>& coins, int sighash, std::map<int, bilingual_str>& input_errors) const override;
    SigningResult SignMessage(const std::string& message, const PKHash& pkhash, std::string& str_sig) const override;
    std::optional<common::PSQTError> FillPSQT(PartiallySignedQuicksilverTransaction& psqt, const PrecomputedTransactionData& txdata, int sighash_type = SIGHASH_DEFAULT, bool sign = true, bool bip32derivs = false, int* n_signed = nullptr, bool finalize = true) const override;

    uint256 GetID() const override;

    void SetCache(const DescriptorCache& cache);

    bool AddKey(const CKeyID& key_id, const CKey& key);
    bool AddCryptedKey(const CKeyID& key_id, const CPubKey& pubkey, const std::vector<unsigned char>& crypted_key);

    bool HasVaultDescriptor(const VaultDescriptor& desc) const;
    void UpdateVaultDescriptor(VaultDescriptor& descriptor);
    bool CanUpdateToVaultDescriptor(const VaultDescriptor& descriptor, std::string& error);
    void AddDescriptorKey(const CKey& key, const CPubKey& pubkey);
    void WriteDescriptor();

    VaultDescriptor GetVaultDescriptor() const EXCLUSIVE_LOCKS_REQUIRED(cs_desc_man);
    std::unordered_set<CScript, SaltedSipHasher> GetScriptPubKeys() const override;
    std::unordered_set<CScript, SaltedSipHasher> GetScriptPubKeys(int32_t minimum_index) const;
    int32_t GetEndRange() const;

    [[nodiscard]] bool GetDescriptorString(std::string& out, const bool priv) const;

};


} // namespace vault

#endif // QUICKSILVER_VAULT_SCRIPTPUBKEYMAN_H
