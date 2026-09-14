// Copyright (c) 2018-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_INTERFACES_VAULT_H
#define QUICKSILVER_INTERFACES_VAULT_H

#include <addresstype.h>
#include <common/signmessage.h>
#include <consensus/amount.h>
#include <interfaces/chain.h>
#include <pubkey.h>
#include <script/script.h>
#include <support/allocators/secure.h>
#include <util/fs.h>
#include <util/result.h>
#include <util/ui_change_type.h>
#include <vault/types.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

class CKey;
enum class OutputType;
struct PartiallySignedQuicksilverTransaction;
struct bilingual_str;
namespace common {
enum class PSQTError;
} // namespace common
namespace node {
enum class TransactionError;
} // namespace node
namespace vault {
class CCoinControl;
class CVault;
enum class AddressPurpose;
enum isminetype : unsigned int;
struct CRecipient;
struct VaultContext;
using isminefilter = std::underlying_type<isminetype>::type;
} // namespace vault

namespace interfaces {

class Handler;
struct VaultAddress;
struct VaultBalances;
struct VaultTx;
struct VaultTxOut;
struct VaultTxStatus;

using VaultOrderForm = std::vector<std::pair<std::string, std::string>>;
using VaultValueMap = std::map<std::string, std::string>;

//! Interface for accessing a vault.
class Vault
{
public:
    virtual ~Vault() = default;

    //! Encrypt vault.
    virtual bool encryptVault(const SecureString& vault_passphrase) = 0;

    //! Return whether vault is encrypted.
    virtual bool isCrypted() = 0;

    //! Lock vault.
    virtual bool lock() = 0;

    //! Unlock vault.
    virtual bool unlock(const SecureString& vault_passphrase) = 0;

    //! Return whether vault is locked.
    virtual bool isLocked() = 0;

    //! Change vault passphrase.
    virtual bool changeVaultPassphrase(const SecureString& old_vault_passphrase,
                                       const SecureString& new_vault_passphrase) = 0;

    //! Abort a rescan.
    virtual void abortRescan() = 0;

    //! Back up vault.
    virtual bool backupVault(const std::string& filename) = 0;

    //! Return whether this vault records a completed backup.
    virtual bool isBackupRecorded() = 0;

    //! Record whether this vault has a completed backup.
    virtual bool setBackupRecorded(bool recorded) = 0;

    //! Record an accepted shared-key agent setup intent and reserve its funding address in the vault.
    virtual util::Result<vault::AgentAllotmentRecord> recordAgentAllotmentSetup(const std::string& label, CAmount funding_limit, CAmount daily_limit) = 0;

    //! Return accepted shared-key agent setup records stored in the vault.
    virtual std::vector<vault::AgentAllotmentRecord> listAgentAllotmentRecords() = 0;

    //! Build the JSON handoff request for an accepted shared-key agent setup.
    virtual std::string agentAllotmentPolicyRequest(const vault::AgentAllotmentRecord& record, CAmount funding_available) = 0;

    //! Parse and validate an Agent Allotment Gateway policy handoff request without enforcing it.
    virtual util::Result<vault::AgentAllotmentPolicyRequestMetadata> validateAgentAllotmentPolicyRequest(const std::string& request_json) = 0;

    //! Export a validated Agent Allotment Gateway policy bundle with the reserved funding key.
    virtual util::Result<vault::AgentAllotmentPolicyBundle> agentAllotmentPolicyBundle(const std::string& request_json) = 0;

    //! Get vault name.
    virtual std::string getVaultName() = 0;

    // Get a new address.
    virtual util::Result<CTxDestination> getNewDestination(const OutputType type, const std::string& label) = 0;

    //! Get public key.
    virtual bool getPubKey(const CScript& script, const CKeyID& address, CPubKey& pub_key) = 0;

    //! Sign message
    virtual SigningResult signMessage(const std::string& message, const PKHash& pkhash, std::string& str_sig) = 0;

    //! Return whether vault has private key.
    virtual bool isSpendable(const CTxDestination& dest) = 0;

    //! Add or update address.
    virtual bool setAddressBook(const CTxDestination& dest, const std::string& name, const std::optional<vault::AddressPurpose>& purpose) = 0;

    // Remove address.
    virtual bool delAddressBook(const CTxDestination& dest) = 0;

    //! Look up address in vault, return whether exists.
    virtual bool getAddress(const CTxDestination& dest,
                            std::string* name,
                            vault::isminetype* is_mine,
                            vault::AddressPurpose* purpose) = 0;

    //! Get vault address list.
    virtual std::vector<VaultAddress> getAddresses() = 0;

    //! Get receive requests.
    virtual std::vector<std::string> getAddressReceiveRequests() = 0;

    //! Save or remove receive request.
    virtual bool setAddressReceiveRequest(const CTxDestination& dest, const std::string& id, const std::string& value) = 0;

    //! Display address on external signer
    virtual util::Result<void> displayAddress(const CTxDestination& dest) = 0;

    //! Lock coin.
    virtual bool lockCoin(const COutPoint& output, const bool write_to_db) = 0;

    //! Unlock coin.
    virtual bool unlockCoin(const COutPoint& output) = 0;

    //! Return whether coin is locked.
    virtual bool isLockedCoin(const COutPoint& output) = 0;

    //! List locked coins.
    virtual void listLockedCoins(std::vector<COutPoint>& outputs) = 0;

    //! Return whether transaction creation can proceed without blocking on missing chain/header state.
    virtual bool canCreateTransactionsNow() = 0;

    //! Authoritatively check transaction-creation state, waiting for transient vault
    //! lock contention. Intended for preparation that already runs off the GUI thread.
    virtual bool canCreateTransactions() = 0;

    //! Create transaction. `tx_proof_cancel` is polled from inside the proof-of-work
    //! grind; returning true abandons it. Without one a grind cannot be stopped
    //! before it finishes, and it holds the vault for its whole duration.
    virtual util::Result<CTransactionRef> createTransaction(const std::vector<vault::CRecipient>& recipients,
                                                            const vault::CCoinControl& coin_control,
                                                            bool sign,
                                                            int& change_pos,
                                                            const std::function<void(uint32_t nonce)>& tx_proof_progress = {},
                                                            const std::function<bool()>& tx_proof_cancel = {}) = 0;

    //! Commit transaction.
    virtual util::Result<void> commitTransaction(CTransactionRef tx,
                                                 VaultValueMap value_map,
                                                 VaultOrderForm order_form) = 0;

    //! Return whether transaction can be abandoned.
    virtual bool transactionCanBeAbandoned(const uint256& txid) = 0;

    //! Abandon transaction.
    virtual bool abandonTransaction(const uint256& txid) = 0;

    //! Get a transaction.
    virtual CTransactionRef getTx(const uint256& txid) = 0;

    //! Get transaction information.
    virtual VaultTx getVaultTx(const uint256& txid) = 0;

    //! Get list of all vault transactions.
    virtual std::set<VaultTx> getVaultTxs() = 0;

    //! Try to get updated status for a particular transaction, if possible without blocking.
    virtual bool tryGetTxStatus(const uint256& txid,
                                VaultTxStatus& tx_status,
                                int& num_blocks,
                                int64_t& block_time) = 0;

    //! Try to get transaction details, if possible without blocking and with an initialized vault tip.
    virtual bool tryGetVaultTxDetails(const uint256& txid,
                                      VaultTx& tx,
                                      VaultTxStatus& tx_status,
                                      VaultOrderForm& order_form,
                                      bool& in_relaypool,
                                      int& num_blocks) = 0;

    //! Fill PSQT.
    virtual std::optional<common::PSQTError> fillPSQT(int sighash_type,
                                                      bool sign,
                                                      bool bip32derivs,
                                                      size_t* n_signed,
                                                      PartiallySignedQuicksilverTransaction& psqtx,
                                                      bool& complete) = 0;

    //! Get balances.
    virtual VaultBalances getBalances() = 0;

    //! Get balances if possible without blocking.
    virtual bool tryGetBalances(VaultBalances& balances, uint256& block_hash) = 0;

    //! Get balance refresh marker if possible without blocking.
    virtual bool tryGetBalanceUpdateBlockHash(uint256& block_hash) = 0;

    //! Get available balance.
    virtual CAmount getAvailableBalance(const vault::CCoinControl& coin_control) = 0;

    //! Return whether transaction input belongs to vault.
    virtual vault::isminetype txinIsMine(const CTxIn& txin) = 0;

    //! Return whether transaction output belongs to vault.
    virtual vault::isminetype txoutIsMine(const CTxOut& txout) = 0;

    //! Return debit amount if transaction input belongs to vault.
    virtual CAmount getDebit(const CTxIn& txin, vault::isminefilter filter) = 0;

    //! Return credit amount if transaction input belongs to vault.
    virtual CAmount getCredit(const CTxOut& txout, vault::isminefilter filter) = 0;

    //! Return AvailableCoins + LockedCoins grouped by vault address.
    //! (put change in one group with vault address)
    using CoinsList = std::map<CTxDestination, std::vector<std::tuple<COutPoint, VaultTxOut>>>;
    //! Try to list spendable outputs if possible without blocking and with an initialized vault tip.
    virtual bool tryListCoins(CoinsList& coins) = 0;

    //! Return vault transaction output information.
    //! Try to return output information if possible without blocking and with an initialized vault tip.
    virtual bool tryGetCoins(const std::vector<COutPoint>& outputs, std::vector<VaultTxOut>& coins) = 0;

    // Return whether HD enabled.
    virtual bool hdEnabled() = 0;

    // Return whether the vault is blank.
    virtual bool canGetAddresses() = 0;

    // Return whether private keys enabled.
    virtual bool privateKeysDisabled() = 0;

    // Return whether the vault contains a Taproot scriptPubKeyMan
    virtual bool taprootEnabled() = 0;

    // Return whether vault uses an external signer.
    virtual bool hasExternalSigner() = 0;

    // Get default address type.
    virtual OutputType getDefaultAddressType() = 0;

    //! Unload the vault. `load_on_start` is written straight to the startup vault
    //! list: false unlists it, which is what `Close vault` means, and std::nullopt
    //! leaves the list untouched -- for an unload the application intends to reverse
    //! itself, such as handing the vault over to node initialisation when consensus
    //! is enabled mid-session.
    virtual void remove(std::optional<bool> load_on_start = false) = 0;

    //! Register handler for unload message.
    using UnloadFn = std::function<void()>;
    virtual std::unique_ptr<Handler> handleUnload(UnloadFn fn) = 0;

    //! Register handler for show progress messages.
    using ShowProgressFn = std::function<void(const std::string& title, int progress)>;
    virtual std::unique_ptr<Handler> handleShowProgress(ShowProgressFn fn) = 0;

    //! Register handler for status changed messages.
    using StatusChangedFn = std::function<void()>;
    virtual std::unique_ptr<Handler> handleStatusChanged(StatusChangedFn fn) = 0;

    //! Register handler for address book changed messages.
    using AddressBookChangedFn = std::function<void(const CTxDestination& address,
                                                    const std::string& label,
                                                    bool is_mine,
                                                    vault::AddressPurpose purpose,
                                                    ChangeType status)>;
    virtual std::unique_ptr<Handler> handleAddressBookChanged(AddressBookChangedFn fn) = 0;

    //! Register handler for transaction changed messages.
    using TransactionChangedFn = std::function<void(const uint256& txid, ChangeType status)>;
    virtual std::unique_ptr<Handler> handleTransactionChanged(TransactionChangedFn fn) = 0;

    //! Register handler for keypool changed messages.
    using CanGetAddressesChangedFn = std::function<void()>;
    virtual std::unique_ptr<Handler> handleCanGetAddressesChanged(CanGetAddressesChangedFn fn) = 0;

    //! Return pointer to internal vault class, useful for testing.
    virtual vault::CVault* vault() { return nullptr; }
};

//! Vault chain client that in addition to having chain client methods for
//! starting up, shutting down, and registering RPCs, also has additional
//! methods (called by the GUI) to load and create vaults.
class VaultLoader : public ChainClient
{
public:
    /** Apply a tip from a separately validated header chain.
     *
     * This gives detached vaults a height/hash clock for balance refreshes and
     * confirmation depth without claiming that transaction bodies were scanned.
     * A live consensus chain remains authoritative when one is attached.
     */
    virtual bool setHeaderTip(int height, const uint256& hash, int64_t block_time) = 0;

    //! Create new vault.
    virtual util::Result<std::unique_ptr<Vault>> createVault(const std::string& name, const SecureString& passphrase, uint64_t vault_creation_flags, std::vector<bilingual_str>& warnings) = 0;

    //! Load existing vault.
    virtual util::Result<std::unique_ptr<Vault>> loadVault(const std::string& name, std::vector<bilingual_str>& warnings) = 0;

    //! Restore backup vault
    virtual util::Result<std::unique_ptr<Vault>> restoreVault(const fs::path& backup_file, const std::string& vault_name, std::vector<bilingual_str>& warnings) = 0;

    //! Return available vaults in vault directory.
    virtual std::vector<std::pair<std::string, std::string>> listVaultDir() = 0;

    //! Return interfaces for accessing vaults (if any).
    virtual std::vector<std::unique_ptr<Vault>> getVaults() = 0;

    //! Register handler for load vault messages. This callback is triggered by
    //! createVault and loadVault above, and also triggered when vaults are
    //! loaded at startup or by RPC.
    using LoadVaultFn = std::function<void(std::unique_ptr<Vault> vault)>;
    virtual std::unique_ptr<Handler> handleLoadVault(LoadVaultFn fn) = 0;

    //! Return pointer to internal context, useful for testing.
    virtual vault::VaultContext* context() { return nullptr; }
};

//! Information about one vault address.
struct VaultAddress {
    CTxDestination dest;
    vault::isminetype is_mine;
    vault::AddressPurpose purpose;
    std::string name;

    VaultAddress(CTxDestination dest, vault::isminetype is_mine, vault::AddressPurpose purpose, std::string name)
        : dest(std::move(dest)), is_mine(is_mine), purpose(std::move(purpose)), name(std::move(name))
    {
    }
};

//! Collection of vault balances.
struct VaultBalances {
    CAmount balance = 0;
    CAmount unconfirmed_balance = 0;
    CAmount immature_balance = 0;
    //! Coins handed to an agent: owned, but locked out of coin selection. Held apart from
    //! `balance` so the spendable figure is one a transfer can honour.
    CAmount delegated_balance = 0;

    bool balanceChanged(const VaultBalances& prev) const
    {
        return balance != prev.balance || unconfirmed_balance != prev.unconfirmed_balance ||
               immature_balance != prev.immature_balance || delegated_balance != prev.delegated_balance;
    }
};

// Vault transaction information.
struct VaultTx {
    CTransactionRef tx;
    std::vector<vault::isminetype> txin_is_mine;
    std::vector<vault::isminetype> txout_is_mine;
    std::vector<bool> txout_is_change;
    std::vector<CTxDestination> txout_address;
    std::vector<vault::isminetype> txout_address_is_mine;
    CAmount credit;
    CAmount debit;
    CAmount change;
    int64_t time;
    std::map<std::string, std::string> value_map;
    bool is_coinbase;

    bool operator<(const VaultTx& a) const { return tx->GetHash() < a.tx->GetHash(); }
};

//! Updated transaction status.
struct VaultTxStatus {
    int block_height;
    int blocks_to_maturity;
    int depth_in_main_chain;
    unsigned int time_received;
    uint32_t lock_time;
    bool is_trusted;
    bool is_abandoned;
    bool is_coinbase;
    bool is_in_main_chain;
};

//! Vault transaction output.
struct VaultTxOut {
    CTxOut txout;
    int64_t time;
    int depth_in_main_chain = -1;
    bool is_spent = false;
};

//! Return implementation of Vault interface. This function is defined in
//! dummyvault.cpp and throws if the vault component is not compiled.
std::unique_ptr<Vault> MakeVault(vault::VaultContext& context, const std::shared_ptr<vault::CVault>& vault);

//! Return implementation of ChainClient interface for a vault loader. This
//! function will be undefined in builds where ENABLE_VAULT is false.
std::unique_ptr<VaultLoader> MakeVaultLoader(Chain& chain, ArgsManager& args);

} // namespace interfaces

#endif // QUICKSILVER_INTERFACES_VAULT_H
