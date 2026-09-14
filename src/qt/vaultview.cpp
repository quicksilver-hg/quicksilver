// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/vaultview.h>

#include <qt/addressbookpage.h>
#include <qt/agentallotmentpage.h>
#include <qt/askpassphrasedialog.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/minemintpage.h>
#include <qt/miningmodel.h>
#include <qt/networkpage.h>
#include <qt/optionsmodel.h>
#include <qt/overviewpage.h>
#include <qt/platformstyle.h>
#include <qt/quicksilverstyle.h>
#include <qt/receivecoinsdialog.h>
#include <qt/sendcoinsdialog.h>
#include <qt/sendcoinsrecipient.h>
#include <qt/signverifymessagedialog.h>
#include <qt/transactiontablemodel.h>
#include <qt/transactionview.h>
#include <qt/vaultmodel.h>

#include <interfaces/node.h>
#include <node/interface_ui.h>
#include <util/strencodings.h>

#include <QAction>
#include <QHBoxLayout>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QVBoxLayout>

VaultView::VaultView(VaultModel* vault_model, const PlatformStyle* _platformStyle, QWidget* parent)
    : PageStack(parent),
      vaultModel(vault_model),
      platformStyle(_platformStyle)
{
    assert(vaultModel);

    // Create tabs
    overviewPage = new OverviewPage(platformStyle);
    overviewPage->setVaultModel(vaultModel);

    transactionsPage = new QWidget(this);
    QVBoxLayout *vbox = new QVBoxLayout();
    QHBoxLayout *hbox_buttons = new QHBoxLayout();
    transactionView = new TransactionView(platformStyle, this);
    transactionView->setModel(vaultModel);

    vbox->addWidget(transactionView);
    QPushButton *exportButton = new QPushButton(tr("&Export"), this);
    exportButton->setToolTip(tr("Export the data in the current tab to a file"));
    if (platformStyle->getImagesOnButtons()) {
        exportButton->setIcon(platformStyle->ColorIcon(":/icons/export", QuicksilverStyle::Color(QuicksilverStyle::Token::Amber)));
    }
    hbox_buttons->addStretch();
    hbox_buttons->addWidget(exportButton);
    vbox->addLayout(hbox_buttons);
    transactionsPage->setLayout(vbox);

    receiveCoinsPage = new ReceiveCoinsDialog(platformStyle);
    receiveCoinsPage->setModel(vaultModel);

    sendCoinsPage = new SendCoinsDialog(platformStyle);
    sendCoinsPage->setModel(vaultModel);

    agentAllotmentPage = new AgentAllotmentPage(this);
    agentAllotmentPage->setModel(vaultModel);
    agentAllotmentPage->setDisplayUnit(vaultModel->getOptionsModel()->getDisplayUnit());
    connect(vaultModel->getOptionsModel(), &OptionsModel::displayUnitChanged, agentAllotmentPage, &AgentAllotmentPage::setDisplayUnit);
    connect(agentAllotmentPage, &AgentAllotmentPage::fundAgentAllotmentSetupRequested, this, [this](const QString& address, const QString& label, CAmount amount) {
        SendCoinsRecipient recipient(address, label, amount, QString());
        setCurrentWidget(sendCoinsPage);
        sendCoinsPage->pasteEntry(recipient);
    });

    mineMintPage = new MineMintPage(this);
    networkPage = new NetworkPage(this);

    usedSendingAddressesPage = new AddressBookPage(platformStyle, AddressBookPage::ForEditing, AddressBookPage::SendingTab, this);
    usedSendingAddressesPage->setModel(vaultModel->getAddressTableModel());

    usedReceivingAddressesPage = new AddressBookPage(platformStyle, AddressBookPage::ForEditing, AddressBookPage::ReceivingTab, this);
    usedReceivingAddressesPage->setModel(vaultModel->getAddressTableModel());

    addWidget(overviewPage);
    addWidget(transactionsPage);
    addWidget(receiveCoinsPage);
    addWidget(sendCoinsPage);
    addWidget(agentAllotmentPage);
    addWidget(mineMintPage);
    addWidget(networkPage);

    connect(overviewPage, &OverviewPage::transactionClicked, this, &VaultView::transactionClicked);
    // Clicking on a transaction on the overview pre-selects the transaction on the transaction history page
    connect(overviewPage, &OverviewPage::transactionClicked, transactionView, qOverload<const QModelIndex&>(&TransactionView::focusTransaction));

    connect(overviewPage, &OverviewPage::outOfSyncWarningClicked, this, &VaultView::outOfSyncWarningClicked);
    connect(overviewPage, &OverviewPage::backupRequested, this, &VaultView::backupRequested);

    connect(sendCoinsPage, &SendCoinsDialog::coinsSent, this, &VaultView::coinsSent);
    connect(sendCoinsPage, &SendCoinsDialog::solverSettingsRequested, this, &VaultView::solverSettingsRequested);
    connect(mineMintPage, &MineMintPage::solverSettingsRequested, this, &VaultView::solverSettingsRequested);
    connect(mineMintPage, &MineMintPage::networkHelpRequested, this, &VaultView::gotoNetworkPage);
    connect(networkPage, &NetworkPage::addPeerRequested, this, [this](const QString& address) {
        if (!clientModel) {
            networkPage->setAddPeerResult(tr("Consensus is not running, so there is nothing to connect."));
            return;
        }
        // Queued for retrying, not connected: saying otherwise would repeat the
        // false-success this panel exists to end.
        const bool added = clientModel->node().addNode(address.toStdString());
        networkPage->setAddPeerResult(added
            ? tr("Added %1. If it answers, the peer count changes within a minute or two.").arg(address)
            : tr("%1 is already on the list of peers this node tries.").arg(address));
    });
    // Highlight transaction after send
    connect(sendCoinsPage, &SendCoinsDialog::coinsSent, transactionView, qOverload<const uint256&>(&TransactionView::focusTransaction));

    // Clicking on "Export" allows to export the transaction list
    connect(exportButton, &QPushButton::clicked, transactionView, &TransactionView::exportClicked);

    // Pass through messages from sendCoinsPage
    connect(sendCoinsPage, &SendCoinsDialog::message, this, &VaultView::message);
    // Pass through messages from transactionView
    connect(transactionView, &TransactionView::message, this, &VaultView::message);

    connect(this, &VaultView::setPrivacy, overviewPage, &OverviewPage::setPrivacy);
    connect(this, &VaultView::setPrivacy, this, &VaultView::disableTransactionView);

    // Receive and pass through messages from vault model
    connect(vaultModel, &VaultModel::message, this, &VaultView::message);

    // Handle changes in encryption status
    connect(vaultModel, &VaultModel::encryptionStatusChanged, this, &VaultView::encryptionStatusChanged);

    // Balloon pop-up for new transaction
    connect(vaultModel->getTransactionTableModel(), &TransactionTableModel::rowsInserted, this, &VaultView::processNewTransaction);

    // Ask for passphrase if needed
    connect(vaultModel, &VaultModel::requireUnlock, this, &VaultView::unlockVault);

    // Show progress dialog
    connect(vaultModel, &VaultModel::showProgress, this, &VaultView::showProgress);
}

VaultView::~VaultView() = default;

void VaultView::setClientModel(ClientModel *_clientModel)
{
    this->clientModel = _clientModel;

    overviewPage->setClientModel(_clientModel);
    sendCoinsPage->setClientModel(_clientModel);
    vaultModel->setClientModel(_clientModel);

    if (!_clientModel && miningModel) {
        // Same reason as VaultFrame: the model polls the node handle this client model
        // provided, and must not outlive it.
        delete miningModel;
        miningModel = nullptr;
    }

    if (_clientModel && vaultModel && !miningModel) {
        miningModel = new MiningModel(_clientModel->node(), vaultModel, this);
        connect(mineMintPage, &MineMintPage::startRequested, miningModel, &MiningModel::start);
        connect(mineMintPage, &MineMintPage::stopRequested, miningModel, &MiningModel::stop);
        connect(mineMintPage, &MineMintPage::newAddressRequested, this, [this]{
            if (!miningModel) return;
            mineMintPage->setPayoutAddress(miningModel->freshPayoutAddress());
        });
        connect(miningModel, &MiningModel::statusUpdated, mineMintPage, &MineMintPage::setStatus);
        connect(miningModel, &MiningModel::miningError, this, [this](const QString& m){
            Q_EMIT message(tr("Mining"), m, CClientUIInterface::MSG_ERROR);
        });
        mineMintPage->setPayoutAddress(miningModel->freshPayoutAddress()); // prefill
        miningModel->refresh();
    }

    if (_clientModel) {
        auto refresh_network = [this]{
            const int peers = clientModel->getNumConnections(CONNECTIONS_ALL);
            const double progress = clientModel->node().getVerificationProgress();
            networkPage->updateStatus(peers, progress, !clientModel->node().isInitialBlockDownload());
            mineMintPage->setPeerCount(peers);
        };
        connect(clientModel, &ClientModel::numBlocksChanged, this,
                [refresh_network](int, const QDateTime&, double, SyncType, SynchronizationState){ refresh_network(); });
        connect(clientModel, &ClientModel::numConnectionsChanged, this,
                [refresh_network](int){ refresh_network(); });
        refresh_network();
    }
}

void VaultView::processNewTransaction(const QModelIndex& parent, int start, int /*end*/)
{
    // Prevent balloon-spam when initial block download is in progress
    if (!clientModel || clientModel->node().isInitialBlockDownload()) {
        return;
    }

    TransactionTableModel *ttm = vaultModel->getTransactionTableModel();
    if (!ttm || ttm->processingQueuedTransactions())
        return;

    QString date = ttm->index(start, TransactionTableModel::Date, parent).data().toString();
    qint64 amount = ttm->index(start, TransactionTableModel::Amount, parent).data(Qt::EditRole).toLongLong();
    QString type = ttm->index(start, TransactionTableModel::Type, parent).data().toString();
    QModelIndex index = ttm->index(start, 0, parent);
    QString address = ttm->data(index, TransactionTableModel::AddressRole).toString();
    QString label = GUIUtil::HtmlEscape(ttm->data(index, TransactionTableModel::LabelRole).toString());

    Q_EMIT incomingTransaction(date, vaultModel->getOptionsModel()->getDisplayUnit(), amount, type, address, label, GUIUtil::HtmlEscape(vaultModel->getVaultName()));
}

void VaultView::gotoOverviewPage()
{
    setCurrentWidget(overviewPage);
}

void VaultView::gotoHistoryPage()
{
    setCurrentWidget(transactionsPage);
}

void VaultView::gotoMineMintPage()
{
    setCurrentWidget(mineMintPage);
}

void VaultView::gotoAgentAllotmentPage()
{
    agentAllotmentPage->refresh();
    setCurrentWidget(agentAllotmentPage);
}

void VaultView::gotoNetworkPage()
{
    setCurrentWidget(networkPage);
}

void VaultView::gotoReceiveCoinsPage()
{
    setCurrentWidget(receiveCoinsPage);
}

void VaultView::gotoSendCoinsPage(QString addr)
{
    setCurrentWidget(sendCoinsPage);

    if (!addr.isEmpty())
        sendCoinsPage->setAddress(addr);
}

void VaultView::gotoSignMessageTab(QString addr)
{
    // calls show() in showTab_SM()
    SignVerifyMessageDialog *signVerifyMessageDialog = new SignVerifyMessageDialog(platformStyle, this);
    signVerifyMessageDialog->setAttribute(Qt::WA_DeleteOnClose);
    signVerifyMessageDialog->setModel(vaultModel);
    signVerifyMessageDialog->showTab_SM(true);

    if (!addr.isEmpty())
        signVerifyMessageDialog->setAddress_SM(addr);
}

void VaultView::gotoVerifyMessageTab(QString addr)
{
    // calls show() in showTab_VM()
    SignVerifyMessageDialog *signVerifyMessageDialog = new SignVerifyMessageDialog(platformStyle, this);
    signVerifyMessageDialog->setAttribute(Qt::WA_DeleteOnClose);
    signVerifyMessageDialog->setModel(vaultModel);
    signVerifyMessageDialog->showTab_VM(true);

    if (!addr.isEmpty())
        signVerifyMessageDialog->setAddress_VM(addr);
}

bool VaultView::handlePaymentRequest(const SendCoinsRecipient& recipient)
{
    return sendCoinsPage->handlePaymentRequest(recipient);
}

void VaultView::showOutOfSyncWarning(bool fShow)
{
    overviewPage->showOutOfSyncWarning(fShow);
}

void VaultView::setBackupState(bool backup_done)
{
    overviewPage->setBackupState(backup_done);
}

void VaultView::encryptVault()
{
    auto dlg = new AskPassphraseDialog(AskPassphraseDialog::Encrypt, this);
    dlg->setModel(vaultModel);
    connect(dlg, &QDialog::finished, this, &VaultView::encryptionStatusChanged);
    GUIUtil::ShowModalDialogAsynchronously(dlg);
}

void VaultView::backupVault(std::function<void(bool)> done)
{
    QPointer<VaultView> self{this};
    GUIUtil::getSaveFileName(this,
        tr("Backup Vault"), QString(),
        //: Name of the vault data file format.
        tr("Vault Data") + QLatin1String(" (*.dat)"),
        [self, done = std::move(done)](const QString& filename) {
            if (!self || filename.isEmpty() || !self->vaultModel) {
                if (done) done(false);
                return;
            }
            if (!self->vaultModel->vault().backupVault(filename.toLocal8Bit().data())) {
                Q_EMIT self->message(self->tr("Backup Failed"), self->tr("There was an error trying to save the vault data to %1.").arg(filename),
                    CClientUIInterface::MSG_ERROR);
                if (done) done(false);
                return;
            }
            Q_EMIT self->message(self->tr("Backup Successful"), self->tr("The vault data was successfully saved to %1.").arg(filename),
                CClientUIInterface::MSG_INFORMATION);
            if (done) done(true);
        });
}

void VaultView::changePassphrase()
{
    auto dlg = new AskPassphraseDialog(AskPassphraseDialog::ChangePass, this);
    dlg->setModel(vaultModel);
    GUIUtil::ShowModalDialogAsynchronously(dlg);
}

void VaultView::unlockVault()
{
    if (vaultModel->getEncryptionStatus() != VaultModel::Locked) {
        return;
    }
    if (findChild<AskPassphraseDialog*>(QStringLiteral("unlockVaultDialog"))) {
        return;
    }
    vaultModel->notifyUnlockDialogShown();
    auto dlg = new AskPassphraseDialog(AskPassphraseDialog::Unlock, this);
    dlg->setObjectName(QStringLiteral("unlockVaultDialog"));
    dlg->setModel(vaultModel);
    QObject::connect(dlg, &QDialog::finished, vaultModel, &VaultModel::completePendingUnlock, Qt::QueuedConnection);
    GUIUtil::ShowModalDialogAsynchronously(dlg);
}

void VaultView::usedSendingAddresses()
{
    GUIUtil::bringToFront(usedSendingAddressesPage);
}

void VaultView::usedReceivingAddresses()
{
    GUIUtil::bringToFront(usedReceivingAddressesPage);
}

void VaultView::showProgress(const QString &title, int nProgress)
{
    if (nProgress == 0) {
        progressDialog = new QProgressDialog(title, tr("Cancel"), 0, 100);
        GUIUtil::PolishProgressDialog(progressDialog);
        progressDialog->setWindowModality(Qt::ApplicationModal);
        progressDialog->setAutoClose(false);
        progressDialog->setValue(0);
    } else if (nProgress == 100) {
        if (progressDialog) {
            progressDialog->close();
            progressDialog->deleteLater();
            progressDialog = nullptr;
        }
    } else if (progressDialog) {
        if (progressDialog->wasCanceled()) {
            getVaultModel()->vault().abortRescan();
        } else {
            progressDialog->setValue(nProgress);
        }
    }
}

void VaultView::disableTransactionView(bool disable)
{
    transactionView->setDisabled(disable);
}
