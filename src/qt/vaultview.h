// Copyright (c) 2011-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_VAULTVIEW_H
#define QUICKSILVER_QT_VAULTVIEW_H

#include <consensus/amount.h>
#include <qt/pagestack.h>
#include <qt/quicksilverunits.h>

#include <functional>

class ClientModel;
class MineMintPage;
class MiningModel;
class NetworkPage;
class OverviewPage;
class PlatformStyle;
class ReceiveCoinsDialog;
class SendCoinsDialog;
class SendCoinsRecipient;
class TransactionView;
class VaultModel;
class AddressBookPage;
class AgentAllotmentPage;

QT_BEGIN_NAMESPACE
class QModelIndex;
class QProgressDialog;
QT_END_NAMESPACE

/*
  VaultView class. This class represents the view to a single vault.
  It was added to support multiple vault functionality. Each vault gets its own VaultView instance.
  It communicates with both the client and the vault models to give the user an up-to-date view of the
  current core state.
*/
class VaultView : public PageStack
{
    Q_OBJECT

public:
    explicit VaultView(VaultModel* vault_model, const PlatformStyle* platformStyle, QWidget* parent);
    ~VaultView();

    /** Set the client model.
        The client model represents the part of the core that communicates with the P2P network, and is vault-agnostic.
    */
    void setClientModel(ClientModel *clientModel);
    VaultModel* getVaultModel() const noexcept { return vaultModel; }
    void setBackupState(bool backup_done);

    bool handlePaymentRequest(const SendCoinsRecipient& recipient);

    void showOutOfSyncWarning(bool fShow);

private:
    ClientModel* clientModel{nullptr};

    //!
    //! The vault model represents a Quicksilver vault, and offers access to
    //! the list of transactions, address book and sending functionality.
    //!
    VaultModel* const vaultModel;

    OverviewPage *overviewPage;
    AgentAllotmentPage* agentAllotmentPage{nullptr};
    MineMintPage* mineMintPage{nullptr};
    MiningModel* miningModel{nullptr};
    NetworkPage* networkPage{nullptr};
    QWidget *transactionsPage;
    ReceiveCoinsDialog *receiveCoinsPage;
    SendCoinsDialog *sendCoinsPage;
    AddressBookPage *usedSendingAddressesPage;
    AddressBookPage *usedReceivingAddressesPage;

    TransactionView *transactionView;

    QProgressDialog* progressDialog{nullptr};
    const PlatformStyle *platformStyle;

public Q_SLOTS:
    /** Switch to overview (home) page */
    void gotoOverviewPage();
    /** Switch to history (transactions) page */
    void gotoHistoryPage();
    /** Switch to mine/mint page */
    void gotoMineMintPage();
    /** Switch to agent allotments page */
    void gotoAgentAllotmentPage();
    /** Switch to network page */
    void gotoNetworkPage();
    /** Switch to request page */
    void gotoReceiveCoinsPage();
    /** Switch to transfer page */
    void gotoSendCoinsPage(QString addr = "");

    /** Show Sign/Verify Message dialog and switch to sign message tab */
    void gotoSignMessageTab(QString addr = "");
    /** Show Sign/Verify Message dialog and switch to verify message tab */
    void gotoVerifyMessageTab(QString addr = "");

    /** Show incoming transaction notification for new transactions.

        The new items are those between start and end inclusive, under the given parent item.
    */
    void processNewTransaction(const QModelIndex& parent, int start, int /*end*/);
    /** Encrypt the vault */
    void encryptVault();
    /** Backup the vault */
    void backupVault(std::function<void(bool)> done = {});
    /** Change encrypted vault passphrase */
    void changePassphrase();
    /** Ask for passphrase to unlock vault temporarily */
    void unlockVault();

    /** Show used sending addresses */
    void usedSendingAddresses();
    /** Show used receiving addresses */
    void usedReceivingAddresses();

    /** Show progress dialog e.g. for rescan */
    void showProgress(const QString &title, int nProgress);

private Q_SLOTS:
    void disableTransactionView(bool disable);

Q_SIGNALS:
    void setPrivacy(bool privacy);
    void transactionClicked();
    void coinsSent();
    void solverSettingsRequested();
    /**  Fired when a message should be reported to the user */
    void message(const QString &title, const QString &message, unsigned int style);
    /** Encryption status of vault changed */
    void encryptionStatusChanged();
    /** Notify that a new transaction appeared */
    void incomingTransaction(const QString& date, QuicksilverUnit unit, const CAmount& amount, const QString& type, const QString& address, const QString& label, const QString& vaultName);
    /** Notify that the out of sync warning icon has been pressed */
    void outOfSyncWarningClicked();
    /** Request backup for this vault */
    void backupRequested();
};

#endif // QUICKSILVER_QT_VAULTVIEW_H
