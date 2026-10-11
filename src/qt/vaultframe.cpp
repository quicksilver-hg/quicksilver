// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/vaultframe.h>

#include <chainparams.h>
#include <node/interface_ui.h>
#include <psqt.h>
#include <qt/benchpanel.h>
#include <qt/clientmodel.h>
#include <qt/consensusreviewpage.h>
#include <qt/desktoplaunchpage.h>
#include <qt/guiutil.h>
#include <qt/minemintpage.h>
#include <qt/miningmodel.h>
#include <qt/networkpage.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/psqtoperationsdialog.h>
#include <qt/transactiontablemodel.h>
#include <qt/vaultmodel.h>
#include <qt/vaultview.h>
#include <interfaces/node.h>
#include <util/fs.h>
#include <util/fs_helpers.h>

#include <cassert>
#include <fstream>
#include <string>

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QModelIndex>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QStringList>
#include <QVBoxLayout>

namespace {
const char* CONSENSUS_ENABLED_SETTING = "Desktop/ConsensusEnabled";
} // namespace

VaultFrame::VaultFrame(const PlatformStyle* _platformStyle, QWidget* parent)
    : QFrame(parent),
      platformStyle(_platformStyle)
{
    setObjectName(QStringLiteral("vaultFrame"));

    QHBoxLayout* vaultFrameLayout = new QHBoxLayout(this);
    setContentsMargins(0,0,0,0);
    vaultStack = new PageStack(this);
    vaultStack->setObjectName(QStringLiteral("vaultFrameStack"));
    vaultFrameLayout->setContentsMargins(0,0,0,0);
    vaultFrameLayout->addWidget(vaultStack);

    m_launch_page = new DesktopLaunchPage(platformStyle, Params().AssumedBlockchainSize(), Params().AssumedChainStateSize(), vaultStack);
    connect(m_launch_page, &DesktopLaunchPage::vaultRequested, this, &VaultFrame::gotoVaultPage);
    connect(m_launch_page, &DesktopLaunchPage::backupRequested, this, &VaultFrame::backupVault);
    connect(m_launch_page, &DesktopLaunchPage::consensusRequested, this, &VaultFrame::gotoNetworkPage);
    connect(m_launch_page, &DesktopLaunchPage::miningRequested, this, &VaultFrame::gotoMineMintPage);
    connect(m_launch_page, &DesktopLaunchPage::privacyRequested, this, &VaultFrame::privacyRequested);
    connect(m_launch_page, &DesktopLaunchPage::ledgerRequested, this, &VaultFrame::ledgerRequested);
    vaultStack->addWidget(m_launch_page);
    // Home is the page a vault opens on, so the frame asks for Home's room.
    m_size_hint = m_launch_page->sizeHint();

    m_consensus_review_page = new ConsensusReviewPage(Params().AssumedBlockchainSize(), Params().AssumedChainStateSize(), vaultStack);
    connect(m_consensus_review_page, &ConsensusReviewPage::continueRequested, this, &VaultFrame::acceptConsensusAndShowStatus);
    connect(m_consensus_review_page, &ConsensusReviewPage::declined, this, [this] {
        if (!m_consensus_enabled) setConsensusEnabled(false);
        gotoLaunchPage();
    });
    vaultStack->addWidget(m_consensus_review_page);

    m_network_page = new NetworkPage(vaultStack);
    connect(m_network_page, &NetworkPage::restartRequested, this, &VaultFrame::networkRestartRequested);
    connect(m_network_page, &NetworkPage::addPeerRequested, this, &VaultFrame::addPeer);
    vaultStack->addWidget(m_network_page);

    m_mining_page = new MineMintPage(vaultStack);
    m_mining_page->setAddressHelperAvailable(false);
    connect(m_mining_page, &MineMintPage::networkHelpRequested, this, &VaultFrame::gotoNetworkPage);
    vaultStack->addWidget(m_mining_page);

    m_no_vault_page = new QWidget(vaultStack);
    m_no_vault_page->setObjectName(QStringLiteral("noVaultPage"));
    m_no_vault_page->setProperty("class", QStringLiteral("quicksilverPage"));
    // The page grammar, as on the consensus review: one panel across the top of
    // the page saying where the desktop stands, with the two ways forward.
    auto* no_vault_page_layout = new QVBoxLayout(m_no_vault_page);
    no_vault_page_layout->setContentsMargins(14, 14, 14, 14);
    no_vault_page_layout->setSpacing(14);
    const BenchPanel::Parts no_vault = BenchPanel::Make(QStringLiteral("noVaultState"), tr("Vault access"), m_no_vault_page);

    auto* title = new QLabel(tr("No vault is open"), no_vault.frame);
    title->setObjectName(QStringLiteral("emptyVaultTitle"));
    title->setProperty("class", QStringLiteral("benchValue"));
    no_vault.body->addWidget(title);

    m_no_vault_body = new QLabel(no_vault.frame);
    m_no_vault_body->setObjectName(QStringLiteral("emptyVaultBody"));
    m_no_vault_body->setProperty("class", QStringLiteral("benchNote"));
    m_no_vault_body->setWordWrap(true);
    no_vault.body->addWidget(m_no_vault_body);

    auto* button_row = new QHBoxLayout();
    button_row->setContentsMargins(0, 4, 0, 0);
    button_row->setSpacing(10);

    m_no_vault_create_button = new QPushButton(tr("Create vault"), no_vault.frame);
    m_no_vault_create_button->setObjectName(QStringLiteral("emptyVaultCreateButton"));
    m_no_vault_create_button->setProperty("class", QStringLiteral("primaryActionButton"));
    connect(m_no_vault_create_button, &QPushButton::clicked, this, &VaultFrame::createVaultButtonClicked);

    m_no_vault_open_button = new QPushButton(tr("Open existing"), no_vault.frame);
    m_no_vault_open_button->setObjectName(QStringLiteral("emptyVaultOpenButton"));
    m_no_vault_open_button->setProperty("class", QStringLiteral("secondaryActionButton"));
    connect(m_no_vault_open_button, &QPushButton::clicked, this, &VaultFrame::openVaultButtonClicked);

    button_row->addWidget(m_no_vault_create_button);
    button_row->addWidget(m_no_vault_open_button);
    button_row->addStretch();
    no_vault.body->addLayout(button_row);

    no_vault_page_layout->addWidget(no_vault.frame);
    no_vault_page_layout->addStretch();
    vaultStack->addWidget(m_no_vault_page);
    m_consensus_enabled = QSettings().value(QLatin1String(CONSENSUS_ENABLED_SETTING), false).toBool();
    setVaultRuntimeAvailable(false);
    updateConsensusState();
    vaultStack->setCurrentWidget(m_launch_page);
}

VaultFrame::~VaultFrame() = default;

// The frame is the stack plus nothing: its layout holds only vaultStack, with no
// margins of its own, so every sizing answer is the stack's answer.
QSize VaultFrame::minimumSizeHint() const
{
    return vaultStack ? vaultStack->minimumSizeHint() : QFrame::minimumSizeHint();
}

bool VaultFrame::hasHeightForWidth() const
{
    return vaultStack ? vaultStack->hasHeightForWidth() : QFrame::hasHeightForWidth();
}

int VaultFrame::heightForWidth(int width) const
{
    return vaultStack ? vaultStack->heightForWidth(width) : QFrame::heightForWidth(width);
}

void VaultFrame::setClientModel(ClientModel *_clientModel)
{
    this->clientModel = _clientModel;

    for (auto i = mapVaultViews.constBegin(); i != mapVaultViews.constEnd(); ++i) {
        i.value()->setClientModel(_clientModel);
    }

    if (!clientModel && m_mining_model) {
        // The mining model wraps the node handle this client model handed over and
        // polls it once a second for as long as it exists. Shutdown clears the client
        // model first and tears the node down afterwards, so a poll that outlives the
        // client model is a poll into a node that is going away -- it survived only
        // because the status call happens to null-check one member first.
        delete m_mining_model;
        m_mining_model = nullptr;
        if (m_launch_page) m_launch_page->clearMiningStatus();
    }

    if (clientModel) {
        if (!m_mining_model) {
            m_mining_model = new MiningModel(clientModel->node(), nullptr, this);
            connect(m_mining_page, &MineMintPage::startRequested, m_mining_model, &MiningModel::start);
            connect(m_mining_page, &MineMintPage::stopRequested, m_mining_model, &MiningModel::stop);
            connect(m_mining_page, &MineMintPage::newAddressRequested, this, [this] {
                if (!m_mining_model) return;
                refreshTopLevelMiningAddressHelper();
                m_mining_page->setPayoutAddress(m_mining_model->freshPayoutAddress());
            });
            connect(m_mining_model, &MiningModel::statusUpdated, m_mining_page, &MineMintPage::setStatus);
            connect(m_mining_model, &MiningModel::statusUpdated, this, &VaultFrame::setMiningStatus);
            connect(m_mining_model, &MiningModel::miningError, this, [this](const QString& m){
                Q_EMIT message(tr("Mining"), m, CClientUIInterface::MSG_ERROR);
            });
            m_mining_model->refresh();
        }
        // The node's warnings, such as a build that is not a release, read on Home.
        connect(clientModel, &ClientModel::alertsChanged, m_launch_page, &DesktopLaunchPage::setAlerts, Qt::UniqueConnection);
        m_launch_page->setAlerts(clientModel->getStatusBarWarnings());
        connect(clientModel, &ClientModel::numBlocksChanged, this,
                [this](int, const QDateTime&, double, SyncType, SynchronizationState) { refreshNetworkPage(); });
        connect(clientModel, &ClientModel::numConnectionsChanged, this,
                [this](int) { refreshNetworkPage(); });
    }
    refreshNetworkPage();
    refreshTopLevelMiningAddressHelper();
}

void VaultFrame::setMiningStatus(const interfaces::MiningStatus& status)
{
    if (m_launch_page) m_launch_page->setMiningStatus(status);
    Q_EMIT miningStatusUpdated(status);
}

void VaultFrame::setSyncState(bool synced, double progress)
{
    if (m_launch_page) m_launch_page->setSyncState(synced, progress);
}

void VaultFrame::setVaultRuntimeAvailable(bool available)
{
    m_vault_runtime_available = available;
    m_launch_page->setVaultRuntimeAvailable(available);
    m_no_vault_body->setText(available
        ? tr("Create a new vault or open an existing one from this desktop.")
        : tr("Vault access will be available after startup attaches the vault runtime."));
    m_no_vault_create_button->setEnabled(available);
    m_no_vault_open_button->setEnabled(available);
    m_no_vault_create_button->setToolTip(available
        ? tr("Create a new vault.")
        : tr("Vault access becomes available after startup attaches the vault runtime."));
    m_no_vault_open_button->setToolTip(available
        ? tr("Open an existing vault.")
        : tr("Vault access becomes available after startup attaches the vault runtime."));
}

bool VaultFrame::addView(VaultView* vaultView)
{
    if (mapVaultViews.count(vaultView->getVaultModel()) > 0) return false;

    connect(vaultView, &VaultView::miningStatusUpdated, this, &VaultFrame::setMiningStatus);
    if (clientModel) vaultView->setClientModel(clientModel);

    vaultStack->addWidget(vaultView);
    mapVaultViews[vaultView->getVaultModel()] = vaultView;
    connectLaunchSummarySignals(vaultView->getVaultModel());
    if (!m_current_vault_view) {
        m_current_vault_view = vaultView;
    }
    updateLaunchSummary();
    refreshTopLevelMiningAddressHelper();

    return true;
}

void VaultFrame::setCurrentVault(VaultModel* vault_model)
{
    if (mapVaultViews.count(vault_model) == 0) return;

    VaultView *vaultView = mapVaultViews.value(vault_model);
    assert(vaultView);

    // A vault choice keeps the page the rail names. On one of a vault's own pages
    // the chosen vault shows the same page; Home, and the no-vault page a vault was
    // just opened from, become this vault's Home; the node's pages (Network, the
    // consensus review, a launch refused for want of Tor) stay as they are.
    QWidget* const showing = vaultStack->currentWidget();
    if (auto* previous = qobject_cast<VaultView*>(showing)) {
        if (previous != vaultView) vaultView->setCurrentIndex(previous->currentIndex());
        vaultStack->setCurrentWidget(vaultView);
    } else if (showing == m_no_vault_page || showing == m_launch_page) {
        vaultStack->setCurrentWidget(m_launch_page);
    } else if (showing == m_mining_page) {
        vaultView->gotoMineMintPage();
        vaultStack->setCurrentWidget(vaultView);
    }
    m_current_vault_view = vaultView;
    updateLaunchSummary();
    refreshTopLevelMiningAddressHelper();

    Q_EMIT currentVaultSet();
}

void VaultFrame::removeVault(VaultModel* vault_model)
{
    if (mapVaultViews.count(vault_model) == 0) return;

    VaultView *vaultView = mapVaultViews.take(vault_model);
    const bool was_current_widget = vaultStack->currentWidget() == vaultView;
    vaultStack->removeWidget(vaultView);
    if (m_current_vault_view == vaultView) {
        m_current_vault_view = mapVaultViews.isEmpty() ? nullptr : mapVaultViews.constBegin().value();
    }
    // The next vault keeps the page the rail names, as a vault choice does.
    if (was_current_widget && m_current_vault_view) {
        m_current_vault_view->setCurrentIndex(vaultView->currentIndex());
        vaultStack->setCurrentWidget(m_current_vault_view);
    } else if (was_current_widget || !m_current_vault_view) {
        gotoLaunchPage();
    }
    delete vaultView;
    updateLaunchSummary();
    refreshTopLevelMiningAddressHelper();
}

void VaultFrame::removeAllVaults()
{
    QMap<VaultModel*, VaultView*>::const_iterator i;
    for (i = mapVaultViews.constBegin(); i != mapVaultViews.constEnd(); ++i)
        vaultStack->removeWidget(i.value());
    mapVaultViews.clear();
    m_current_vault_view = nullptr;
    gotoLaunchPage();
    updateLaunchSummary();
    refreshTopLevelMiningAddressHelper();
}

bool VaultFrame::handlePaymentRequest(const SendCoinsRecipient &recipient)
{
    VaultView *vaultView = currentVaultView();
    if (!vaultView)
        return false;

    return vaultView->handlePaymentRequest(recipient);
}

void VaultFrame::markConsensusInitializationFailed()
{
    m_consensus_start_failed = true;
    setConsensusEnabled(false);
    m_network_page->showStartFailed();
    vaultStack->setCurrentWidget(m_network_page);
}

void VaultFrame::markConsensusTorMissing()
{
    m_consensus_tor_missing = true;
    setConsensusEnabled(false);
    m_network_page->showTorMissing();
    vaultStack->setCurrentWidget(m_network_page);
}

void VaultFrame::gotoLaunchPage()
{
    vaultStack->setCurrentWidget(m_launch_page);
}

void VaultFrame::gotoVaultPage()
{
    if (!m_current_vault_view) {
        vaultStack->setCurrentWidget(m_no_vault_page);
        return;
    }
    gotoLaunchPage();
}

void VaultFrame::gotoHistoryPage()
{
    VaultView* vaultView = currentVaultView();
    if (!vaultView) {
        gotoVaultPage();
        return;
    }
    QMap<VaultModel*, VaultView*>::const_iterator i;
    for (i = mapVaultViews.constBegin(); i != mapVaultViews.constEnd(); ++i)
        i.value()->gotoHistoryPage();
    vaultStack->setCurrentWidget(vaultView);
}

void VaultFrame::gotoMineMintPage()
{
    if (!m_consensus_enabled) {
        gotoNetworkPage();
        return;
    }

    VaultView* vaultView = currentVaultView();
    if (!vaultView) {
        refreshTopLevelMiningAddressHelper();
        vaultStack->setCurrentWidget(m_mining_page);
        return;
    }
    QMap<VaultModel*, VaultView*>::const_iterator i;
    for (i = mapVaultViews.constBegin(); i != mapVaultViews.constEnd(); ++i)
        i.value()->gotoMineMintPage();
    vaultStack->setCurrentWidget(vaultView);
}

void VaultFrame::gotoAgentAllotmentPage()
{
    VaultView* vaultView = currentVaultView();
    if (!vaultView) {
        gotoVaultPage();
        return;
    }
    QMap<VaultModel*, VaultView*>::const_iterator i;
    for (i = mapVaultViews.constBegin(); i != mapVaultViews.constEnd(); ++i)
        i.value()->gotoAgentAllotmentPage();
    vaultStack->setCurrentWidget(vaultView);
}

void VaultFrame::gotoNetworkPage()
{
    if (m_consensus_enabled) {
        gotoNetworkStatusPage();
        return;
    }
    vaultStack->setCurrentWidget(m_consensus_review_page);
}

void VaultFrame::gotoNetworkStatusPage()
{
    refreshNetworkPage();
    vaultStack->setCurrentWidget(m_network_page);
}

void VaultFrame::gotoReceiveCoinsPage()
{
    VaultView* vaultView = currentVaultView();
    if (!vaultView) {
        gotoVaultPage();
        return;
    }
    QMap<VaultModel*, VaultView*>::const_iterator i;
    for (i = mapVaultViews.constBegin(); i != mapVaultViews.constEnd(); ++i)
        i.value()->gotoReceiveCoinsPage();
    vaultStack->setCurrentWidget(vaultView);
}

void VaultFrame::gotoSendCoinsPage(QString addr)
{
    VaultView* vaultView = currentVaultView();
    if (!vaultView) {
        gotoVaultPage();
        return;
    }
    QMap<VaultModel*, VaultView*>::const_iterator i;
    for (i = mapVaultViews.constBegin(); i != mapVaultViews.constEnd(); ++i)
        i.value()->gotoSendCoinsPage(addr);
    vaultStack->setCurrentWidget(vaultView);
}

void VaultFrame::setPrivacy(bool privacy)
{
    if (m_privacy == privacy) return;
    m_privacy = privacy;
    if (m_launch_page) m_launch_page->setPrivacy(m_privacy);
    updateLaunchSummary();
}

void VaultFrame::gotoSignMessageTab(QString addr)
{
    VaultView *vaultView = currentVaultView();
    if (vaultView)
        vaultView->gotoSignMessageTab(addr);
}

void VaultFrame::gotoVerifyMessageTab(QString addr)
{
    VaultView *vaultView = currentVaultView();
    if (vaultView)
        vaultView->gotoVerifyMessageTab(addr);
}

void VaultFrame::gotoLoadPSQT(bool from_clipboard)
{
    std::vector<unsigned char> data;

    if (from_clipboard) {
        std::string raw = QApplication::clipboard()->text().toStdString();
        auto result = DecodeBase64(raw);
        if (!result) {
            Q_EMIT message(tr("Error"), tr("Unable to decode PSQT from clipboard (invalid base64)"), CClientUIInterface::MSG_ERROR);
            return;
        }
        data = std::move(*result);
    } else {
        QPointer<VaultFrame> self{this};
        GUIUtil::getOpenFileName(this,
            tr("Load Transaction Data"), QString(),
            tr("Partially Signed Transaction (*.psqt)"),
            [self](const QString& filename) {
                if (!self || filename.isEmpty()) return;
                if (GetFileSize(filename.toLocal8Bit().data(), MAX_FILE_SIZE_PSQT) == MAX_FILE_SIZE_PSQT) {
                    Q_EMIT self->message(self->tr("Error"), self->tr("PSQT file must be smaller than 100 MiB"), CClientUIInterface::MSG_ERROR);
                    return;
                }
                std::ifstream in{filename.toLocal8Bit().data(), std::ios::binary};
                std::vector<unsigned char> file_data{std::istreambuf_iterator<char>{in}, {}};

                // Some psqt files may be base64 strings in the file rather than binary data
                std::string b64_str{file_data.begin(), file_data.end()};
                b64_str.erase(b64_str.find_last_not_of(" \t\n\r\f\v") + 1); // Trim trailing whitespace
                auto b64_dec = DecodeBase64(b64_str);
                if (b64_dec.has_value()) {
                    file_data = b64_dec.value();
                }

                std::string error;
                PartiallySignedQuicksilverTransaction psqtx;
                if (!DecodeRawPSQT(psqtx, MakeByteSpan(file_data), error)) {
                    Q_EMIT self->message(self->tr("Error"), self->tr("Unable to decode PSQT") + "\n" + QString::fromStdString(error), CClientUIInterface::MSG_ERROR);
                    return;
                }

                auto dlg = new PSQTOperationsDialog(self, self->currentVaultModel(), self->clientModel);
                dlg->openWithPSQT(psqtx);
                GUIUtil::ShowModalDialogAsynchronously(dlg);
            });
        return;
    }

    std::string error;
    PartiallySignedQuicksilverTransaction psqtx;
    if (!DecodeRawPSQT(psqtx, MakeByteSpan(data), error)) {
        Q_EMIT message(tr("Error"), tr("Unable to decode PSQT") + "\n" + QString::fromStdString(error), CClientUIInterface::MSG_ERROR);
        return;
    }

    auto dlg = new PSQTOperationsDialog(this, currentVaultModel(), clientModel);
    dlg->openWithPSQT(psqtx);
    GUIUtil::ShowModalDialogAsynchronously(dlg);
}

void VaultFrame::encryptVault()
{
    VaultView *vaultView = currentVaultView();
    if (vaultView)
        vaultView->encryptVault();
}

void VaultFrame::backupVault()
{
    VaultView *vaultView = currentVaultView();
    if (!vaultView) return;
    QPointer<VaultFrame> self{this};
    vaultView->backupVault([self](bool ok) {
        if (self && ok) self->setCurrentVaultBackupRecorded(true);
    });
}

void VaultFrame::changePassphrase()
{
    VaultView *vaultView = currentVaultView();
    if (vaultView)
        vaultView->changePassphrase();
}

void VaultFrame::unlockVault()
{
    VaultView *vaultView = currentVaultView();
    if (vaultView)
        vaultView->unlockVault();
}

void VaultFrame::usedSendingAddresses()
{
    VaultView *vaultView = currentVaultView();
    if (vaultView)
        vaultView->usedSendingAddresses();
}

void VaultFrame::usedReceivingAddresses()
{
    VaultView *vaultView = currentVaultView();
    if (vaultView)
        vaultView->usedReceivingAddresses();
}

VaultView* VaultFrame::currentVaultView() const
{
    if (VaultView* current_view = qobject_cast<VaultView*>(vaultStack->currentWidget())) {
        return current_view;
    }
    return m_current_vault_view;
}

VaultModel* VaultFrame::currentVaultModel() const
{
    VaultView* vault_view = currentVaultView();
    return vault_view ? vault_view->getVaultModel() : nullptr;
}

void VaultFrame::updateLaunchSummary()
{
    VaultModel* vault_model = currentVaultModel();
    if (!vault_model) {
        m_launch_page->setVaultSummary(false, QString());
        m_launch_page->setBackupState(false, false);
        m_launch_page->setVaultModel(nullptr);
        return;
    }

    const auto agent_records = vault_model->listAgentAllotmentRecords();
    size_t funded_agent_setups{0};
    for (const auto& record : agent_records) {
        if (record.funding_limit > 0 && vault_model->agentAllotmentFundingAvailable(record) >= record.funding_limit) {
            ++funded_agent_setups;
        }
    }
    const size_t agent_setups = agent_records.size();
    const QString agent_summary = agent_setups == 0
        ? QString()
        : tr("Funding %1 of %2 confirmed").arg(funded_agent_setups).arg(agent_setups);
    m_launch_page->setVaultSummary(true, vault_model->getDisplayName(), agent_summary);
    m_launch_page->setVaultModel(vault_model);
    const bool backup_recorded = currentVaultBackupRecorded();
    m_launch_page->setBackupState(true, backup_recorded);
}

void VaultFrame::connectLaunchSummarySignals(VaultModel* vault_model)
{
    if (!vault_model) return;

    connect(vault_model, &VaultModel::balanceChanged, this, &VaultFrame::updateLaunchSummary, Qt::UniqueConnection);
    connect(vault_model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &VaultFrame::updateLaunchSummary, Qt::UniqueConnection);

    TransactionTableModel* tx_model = vault_model->getTransactionTableModel();
    if (!tx_model) return;
    connect(tx_model, &QAbstractItemModel::rowsInserted, this, &VaultFrame::updateLaunchSummary, Qt::UniqueConnection);
    connect(tx_model, &QAbstractItemModel::rowsRemoved, this, &VaultFrame::updateLaunchSummary, Qt::UniqueConnection);
    connect(tx_model, &QAbstractItemModel::dataChanged, this, &VaultFrame::updateLaunchSummary, Qt::UniqueConnection);
    connect(tx_model, &QAbstractItemModel::modelReset, this, &VaultFrame::updateLaunchSummary, Qt::UniqueConnection);
}

bool VaultFrame::currentVaultBackupRecorded() const
{
    const VaultModel* vault_model = currentVaultModel();
    return vault_model && vault_model->backupRecorded();
}

void VaultFrame::setCurrentVaultBackupRecorded(bool backed_up)
{
    VaultModel* vault_model = currentVaultModel();
    if (!vault_model) return;

    vault_model->setBackupRecorded(backed_up);
    updateLaunchSummary();
    // The status strip reads this label. currentVaultSet is the notification
    // the window already uses to repaint vault chrome.
    Q_EMIT currentVaultSet();
}

void VaultFrame::updateConsensusState()
{
    m_launch_page->setConsensusEnabled(m_consensus_enabled);
    m_launch_page->setConsensusStartupFailed(m_consensus_start_failed);
    m_consensus_review_page->setConsensusEnabled(m_consensus_enabled);
    m_consensus_review_page->setStartupFailed(m_consensus_start_failed);
}

void VaultFrame::setConsensusEnabled(bool enabled)
{
    const bool changed = m_consensus_enabled != enabled;
    m_consensus_enabled = enabled;
    QSettings().setValue(QLatin1String(CONSENSUS_ENABLED_SETTING), enabled);
    updateConsensusState();
    if (changed) Q_EMIT consensusStateChanged(enabled);
}

void VaultFrame::acceptConsensusAndShowStatus()
{
    if (m_consensus_start_failed) return;

    m_consensus_tor_missing = false;
    setConsensusEnabled(true);
    // The state-change signal may show the vault-handover dialog. "Not now"
    // reverts the opt-in; if that happens before this function continues, do
    // not overwrite that truthful state with the consensus-starting screen.
    if (m_consensus_enabled) {
        gotoNetworkStatusPage();
    } else if (m_consensus_tor_missing) {
        // Refused for want of Tor: markConsensusTorMissing() already shows why.
        gotoNetworkStatusPage();
    } else {
        gotoLaunchPage();
    }
}

void VaultFrame::refreshNetworkPage()
{
    if (!m_network_page) return;

    if (m_consensus_start_failed) {
        m_network_page->showStartFailed();
        setPeerCount(-1);
        return;
    }

    if (m_consensus_tor_missing && !clientModel) {
        m_network_page->showTorMissing();
        setPeerCount(-1);
        return;
    }

    if (!clientModel) {
        m_network_page->showInitializing();
        setPeerCount(-1);
        return;
    }

    const int peers = clientModel->getNumConnections(CONNECTIONS_ALL);
    const double progress = clientModel->node().getVerificationProgress();
    m_network_page->updateStatus(peers, progress, !clientModel->node().isInitialBlockDownload());
    setPeerCount(peers);
}

void VaultFrame::setChainTip(int height, const QDateTime& block_time)
{
    if (m_launch_page) m_launch_page->setChainTip(height, block_time);
}

void VaultFrame::setPeerCount(int peers)
{
    // The launch screen and the mining page are where a first run actually looks,
    // and neither could tell an isolated node from a connected one before this.
    if (m_launch_page) m_launch_page->setPeerCount(peers);
    if (m_mining_page) m_mining_page->setPeerCount(peers);
}

void VaultFrame::addPeer(const QString& address)
{
    if (!m_network_page) return;
    if (!clientModel) {
        m_network_page->setAddPeerResult(tr("Consensus is not running, so there is nothing to connect."));
        return;
    }
    // Accepted means queued for retrying, not connected: the connection manager dials
    // added peers on its own schedule. Saying "connected" here would repeat the very
    // mistake this panel exists to fix.
    const bool added = clientModel->node().addNode(address.toStdString());
    m_network_page->setAddPeerResult(added
        ? tr("Added %1. If it answers, the peer count changes within a minute or two.").arg(address)
        : tr("%1 is already on the list of peers this node tries.").arg(address));
}

void VaultFrame::refreshTopLevelMiningAddressHelper()
{
    if (!m_mining_page) return;
    VaultModel* vault_model = currentVaultModel();
    if (m_mining_model) m_mining_model->setVaultModel(vault_model);
    m_mining_page->setAddressHelperAvailable(vault_model != nullptr);
}
