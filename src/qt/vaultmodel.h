// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_VAULTMODEL_H
#define QUICKSILVER_QT_VAULTMODEL_H

#include <key.h>

#include <qt/vaultmodeltransaction.h>

#include <interfaces/vault.h>
#include <primitives/transaction.h>
#include <support/allocators/secure.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include <QObject>

enum class OutputType;

class AddressTableModel;
class ClientModel;
class OptionsModel;
class PlatformStyle;
class RecentRequestsTableModel;
class SendCoinsRecipient;
class TransactionTableModel;
class VaultModelTransaction;

class CKeyID;
class COutPoint;
class CPubKey;
class uint256;

namespace interfaces {
class Node;
} // namespace interfaces
namespace vault {
class CCoinControl;
} // namespace vault

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

/** Interface to Quicksilver vault from Qt view code. */
class VaultModel : public QObject
{
    Q_OBJECT

public:
    explicit VaultModel(std::unique_ptr<interfaces::Vault> vault, ClientModel& client_model, const PlatformStyle* platformStyle, QObject* parent = nullptr);
    explicit VaultModel(std::unique_ptr<interfaces::Vault> vault, interfaces::Node& node, OptionsModel* options_model, const PlatformStyle* platformStyle, QObject* parent = nullptr);
    ~VaultModel();

    enum StatusCode // Returned by sendCoins
    {
        OK,
        InvalidAmount,
        InvalidAddress,
        AmountExceedsBalance,
        DuplicateAddress,
        VaultSyncUnavailable,
        TransactionCreationFailed,
        TransactionCommitFailed,
    };

    enum EncryptionStatus {
        NoKeys,      // vault->IsVaultFlagSet(VAULT_FLAG_DISABLE_PRIVATE_KEYS)
        Unencrypted, // !vault->IsCrypted()
        Locked,      // vault->IsCrypted() && vault->IsLocked()
        Unlocked     // vault->IsCrypted() && !vault->IsLocked()
    };

    OptionsModel* getOptionsModel() const;
    AddressTableModel* getAddressTableModel() const;
    TransactionTableModel* getTransactionTableModel() const;
    RecentRequestsTableModel* getRecentRequestsTableModel() const;

    EncryptionStatus getEncryptionStatus() const;

    // Check address for validity
    bool validateAddress(const QString& address) const;

    // Return status record for SendCoins, contains error id + information
    struct SendCoinsReturn {
        SendCoinsReturn(StatusCode _status = OK, QString _reason = "", bool _solver_configuration_required = false)
            : status(_status),
              reason(_reason),
              solver_configuration_required(_solver_configuration_required)
        {
        }
        StatusCode status;
        //! What went wrong, in the words of whichever layer knew. Carried back
        //! rather than shown from here, so one failure raises one dialog: the
        //! model used to put this on screen itself and then return a status the
        //! caller announced a second time.
        QString reason;
        bool solver_configuration_required{false};
    };

    struct AgentSignedSpendBroadcastResult {
        bool accepted{false};
        QString txid;
        QString error;
    };

    // Prepare transaction state before broadcasting a transfer.
    SendCoinsReturn prepareTransaction(VaultModelTransaction& transaction, const vault::CCoinControl& coinControl, const std::function<void(uint32_t nonce)>& tx_proof_progress = {});

    /** Proof-of-work grinds running on worker threads.
     *
     * A transfer's grind does not run on the GUI thread -- SendCoinsDialog hands it
     * to a detached worker so the window stays usable -- and nothing joins that
     * thread. The worker executes inside the CVault this model owns the last
     * reference to, holding cs_vault for however long the grind takes, which on a
     * live target is minutes. So every path that would unload the vault, including
     * `Close vault` and enabling consensus, has to ask first: deleting the model
     * under a running grind destroys the vault the worker is standing in.
     *
     * A cancel request is a request to the grinds running *now*. It is cleared when
     * the last one ends, so it cannot leak into the next transfer.
     */
    bool proofOfWorkInFlight() const { return m_proofs_in_flight.load() > 0; }
    void requestProofOfWorkCancel() { m_proof_cancel_requested = true; }
    bool proofOfWorkCancelRequested() const { return m_proof_cancel_requested.load(); }

    //! Marks a grind as running for as long as it exists. Held on the worker thread.
    class ProofOfWorkScope
    {
    public:
        explicit ProofOfWorkScope(VaultModel& model);
        ~ProofOfWorkScope();

        ProofOfWorkScope(const ProofOfWorkScope&) = delete;
        ProofOfWorkScope(ProofOfWorkScope&&) = delete;
        ProofOfWorkScope& operator=(const ProofOfWorkScope&) = delete;
        ProofOfWorkScope& operator=(ProofOfWorkScope&&) = delete;

    private:
        VaultModel& m_model;
    };

    // Broadcast a transfer to a list of recipients.
    SendCoinsReturn sendCoins(VaultModelTransaction& transaction);

    // Vault encryption
    bool setVaultEncrypted(const SecureString& passphrase);
    // Passphrase only needed when unlocking
    bool setVaultLocked(bool locked, const SecureString& passPhrase = SecureString());
    bool changePassphrase(const SecureString& oldPass, const SecureString& newPass);

    // RAII object for unlocking vault, passed to requestUnlock's continuation.
    // Relocks on destruction when the vault was locked and unlock succeeded.
    class UnlockContext
    {
    public:
        UnlockContext(VaultModel* vault, bool valid, bool relock);
        ~UnlockContext();

        bool isValid() const { return valid; }

        // Disable unused copy/move constructors/assignments explicitly.
        UnlockContext(const UnlockContext&) = delete;
        UnlockContext(UnlockContext&&) = delete;
        UnlockContext& operator=(const UnlockContext&) = delete;
        UnlockContext& operator=(UnlockContext&&) = delete;

    private:
        VaultModel* vault;
        const bool valid;
        const bool relock;
    };

    void requestUnlock(std::function<void(std::shared_ptr<UnlockContext>)> done);
    void notifyUnlockDialogShown();

    void displayAddress(std::string sAddress) const;

    static bool isVaultEnabled();

    interfaces::Node& node() const { return m_node; }
    interfaces::Vault& vault() const { return *m_vault; }
    ClientModel& clientModel() const;
    void setClientModel(ClientModel* client_model);

    QString getVaultName() const;
    QString getDisplayName() const;
    bool backupRecorded() const;
    bool setBackupRecorded(bool recorded);
    util::Result<vault::AgentAllotmentRecord> recordAgentAllotmentSetup(const QString& label, CAmount funding_limit, CAmount daily_limit);
    std::vector<vault::AgentAllotmentRecord> listAgentAllotmentRecords() const;
    QString agentAllotmentPolicyRequest(const vault::AgentAllotmentRecord& record, CAmount funding_available) const;
    util::Result<vault::AgentAllotmentPolicyRequestMetadata> validateAgentAllotmentPolicyRequest(const QString& request_json) const;
    util::Result<vault::AgentAllotmentPolicyBundle> agentAllotmentPolicyBundle(const QString& request_json) const;
    CAmount agentAllotmentFundingAvailable(const vault::AgentAllotmentRecord& record) const;
    AgentSignedSpendBroadcastResult broadcastAgentAllotmentSignedSpend(CTransactionRef tx) const;
    bool tryListCoins(interfaces::Vault::CoinsList& coins) const;
    bool tryGetCoins(const std::vector<COutPoint>& outputs, std::vector<interfaces::VaultTxOut>& coins) const;

    bool isMultivault() const;

    void refresh(bool pk_hash_only = false);

    uint256 getLastBlockProcessed() const;

    // Retrieve the cached vault balance
    interfaces::VaultBalances getCachedBalance() const;

    // If coin control has selected outputs, searches the total amount inside the vault.
    // Otherwise, uses the vault's cached available balance.
    CAmount getAvailableBalance(const vault::CCoinControl* control);

private:
    bool canCreateTransactionsNow();

    std::unique_ptr<interfaces::Vault> m_vault;
    std::unique_ptr<interfaces::Handler> m_handler_unload;
    std::unique_ptr<interfaces::Handler> m_handler_status_changed;
    std::unique_ptr<interfaces::Handler> m_handler_address_book_changed;
    std::unique_ptr<interfaces::Handler> m_handler_transaction_changed;
    std::unique_ptr<interfaces::Handler> m_handler_show_progress;
    std::unique_ptr<interfaces::Handler> m_handler_can_get_addrs_changed;
    ClientModel* m_client_model;
    interfaces::Node& m_node;

    // Written on worker threads, read on the GUI thread. See ProofOfWorkScope.
    std::atomic<int> m_proofs_in_flight{0};
    std::atomic<bool> m_proof_cancel_requested{false};

    bool fForceCheckBalanceChanged{true};

    // Vault has an options model for vault-specific options
    OptionsModel* optionsModel;

    AddressTableModel* addressTableModel{nullptr};
    TransactionTableModel* transactionTableModel{nullptr};
    RecentRequestsTableModel* recentRequestsTableModel{nullptr};

    // Cache some values to be able to detect changes
    interfaces::VaultBalances m_cached_balances;
    EncryptionStatus cachedEncryptionStatus{Unencrypted};
    QTimer* timer;

    // Block hash denoting when the last balance update was done.
    uint256 m_cached_last_update_tip{};

    std::function<void(std::shared_ptr<UnlockContext>)> m_pending_unlock;
    bool m_unlock_dialog_shown{false};
    bool m_unlock_was_locked{false};

    void subscribeToCoreSignals();
    void unsubscribeFromCoreSignals();
    void checkBalanceChanged(const interfaces::VaultBalances& new_balances);

Q_SIGNALS:
    // Signal that balance in vault changed
    void balanceChanged(const interfaces::VaultBalances& balances);

    // Encryption status of vault changed
    void encryptionStatusChanged();

    // Signal emitted when vault needs to be unlocked
    // It is valid behaviour for listeners to keep the vault locked after this signal;
    // this means that the unlocking failed or was cancelled.
    void requireUnlock();

    // Fired when a message should be reported to the user
    void message(const QString& title, const QString& message, unsigned int style);

    // Coins sent: from vault, to recipient, in (serialized) transaction:
    void coinsSent(VaultModel* vault, SendCoinsRecipient recipient, QByteArray transaction);

    // Show progress dialog e.g. for rescan
    void showProgress(const QString& title, int nProgress);

    // Signal that vault is about to be removed
    void unload();

    // Notify that there are now keys in the keypool
    void canGetAddressesChanged();

    void timerTimeout();

public Q_SLOTS:
    void completePendingUnlock();
    /* Starts a timer to periodically update the balance */
    void startPollBalance();

    /* Vault status might have changed */
    void updateStatus();
    /* New transaction, or transaction changed status */
    void updateTransaction();
    /* New, updated or removed address book entry */
    void updateAddressBook(const QString& address, const QString& label, bool isMine, vault::AddressPurpose purpose, int status);
    /* Current, immature or unconfirmed balance might have changed - emit 'balanceChanged' if so */
    void pollBalanceChanged();
};

#endif // QUICKSILVER_QT_VAULTMODEL_H
