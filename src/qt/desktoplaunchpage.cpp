// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/desktoplaunchpage.h>

#include <qt/guiconstants.h>
#include <qt/platformstyle.h>
#include <qt/quicksilverstyle.h>

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QSize>
#include <QStringList>
#include <QVariant>
#include <QVBoxLayout>

namespace {
QFrame* MakeCapabilityCard(const QString& object_name, const QIcon& icon, const QString& title, const QString& state, const QString& body, QPushButton*& button, QLabel*& state_label, QWidget* parent)
{
    auto* card = new QFrame(parent);
    card->setObjectName(object_name);
    card->setProperty("class", QStringLiteral("launchCapabilityCard"));

    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(10);

    auto* top_row = new QHBoxLayout();
    top_row->setSpacing(10);

    auto* icon_label = new QLabel(card);
    icon_label->setObjectName(object_name + QStringLiteral("Icon"));
    icon_label->setPixmap(icon.pixmap(QSize(24, 24)));
    icon_label->setFixedSize(QSize(30, 30));
    top_row->addWidget(icon_label);

    auto* title_label = new QLabel(title, card);
    title_label->setObjectName(object_name + QStringLiteral("Title"));
    title_label->setProperty("class", QStringLiteral("launchCapabilityTitle"));
    top_row->addWidget(title_label, 1);

    state_label = new QLabel(state, card);
    state_label->setObjectName(object_name + QStringLiteral("State"));
    state_label->setProperty("class", QStringLiteral("launchCapabilityState"));
    top_row->addWidget(state_label);
    layout->addLayout(top_row);

    auto* body_label = new QLabel(body, card);
    body_label->setObjectName(object_name + QStringLiteral("Body"));
    body_label->setProperty("class", QStringLiteral("muted"));
    body_label->setWordWrap(true);
    layout->addWidget(body_label, 1);

    button = new QPushButton(card);
    button->setObjectName(object_name + QStringLiteral("Button"));
    layout->addWidget(button, 0, Qt::AlignLeft);

    return card;
}
} // namespace

DesktopLaunchPage::DesktopLaunchPage(const PlatformStyle* platform_style, uint64_t blockchain_size_gb, uint64_t chain_state_size_gb, QWidget* parent)
    : QWidget(parent),
      m_platform_style(platform_style),
      m_blockchain_size_gb(blockchain_size_gb),
      m_chain_state_size_gb(chain_state_size_gb)
{
    setObjectName(QStringLiteral("desktopLaunchPage"));
    setProperty("class", QStringLiteral("quicksilverPage"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 28, 28, 28);
    root->setSpacing(22);

    auto* header = new QFrame(this);
    header->setObjectName(QStringLiteral("desktopLaunchHeader"));
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(0, 0, 0, 0);
    header_layout->setSpacing(16);

    auto* mark = new QLabel(header);
    mark->setObjectName(QStringLiteral("desktopLaunchMark"));
    mark->setPixmap(platform_style->TextColorIcon(QIcon(QStringLiteral(":/icons/quicksilver"))).pixmap(QSize(42, 42)));
    mark->setFixedSize(QSize(48, 48));
    mark->setAlignment(Qt::AlignCenter);
    header_layout->addWidget(mark);

    auto* title_block = new QVBoxLayout();
    title_block->setContentsMargins(0, 0, 0, 0);
    title_block->setSpacing(4);
    auto* title = new QLabel(tr("Quicksilver"), header);
    title->setObjectName(QStringLiteral("desktopLaunchTitle"));
    title->setProperty("class", QStringLiteral("pageTitle"));
    title_block->addWidget(title);
    auto* subtitle = new QLabel(tr("Vault first. Consensus only when you choose it."), header);
    subtitle->setObjectName(QStringLiteral("desktopLaunchSubtitle"));
    subtitle->setProperty("class", QStringLiteral("muted"));
    subtitle->setWordWrap(true);
    title_block->addWidget(subtitle);
    header_layout->addLayout(title_block, 1);
    root->addWidget(header);

    auto* capability_grid = new QGridLayout();
    capability_grid->setContentsMargins(0, 0, 0, 0);
    capability_grid->setHorizontalSpacing(14);
    capability_grid->setVerticalSpacing(14);

    QFrame* vault_card = MakeCapabilityCard(
        QStringLiteral("launchVaultCard"),
        platform_style->ColorIcon(QStringLiteral(":/icons/overview"), QuicksilverStyle::Color(QuicksilverStyle::Token::CinnabarBright)),
        tr("Vault"),
        tr("Create or open"),
        tr("Hold quicksilver, transfer it, request it, and review recent activity."),
        m_vault_button,
        m_vault_state,
        this);
    m_vault_button->setProperty("class", QStringLiteral("launchPrimaryButton"));
    connect(m_vault_button, &QPushButton::clicked, this, &DesktopLaunchPage::vaultRequested);
    m_vault_summary = vault_card->findChild<QLabel*>(QStringLiteral("launchVaultCardBody"));
    m_vault_privacy_button = new QPushButton(vault_card);
    m_vault_privacy_button->setObjectName(QStringLiteral("launchVaultBalancePrivacyButton"));
    m_vault_privacy_button->setProperty("class", QStringLiteral("secondaryActionButton"));
    connect(m_vault_privacy_button, &QPushButton::clicked, this, [this] {
        Q_EMIT privacyRequested(!m_privacy);
    });
    vault_card->layout()->addWidget(m_vault_privacy_button);

    QFrame* consensus_card = MakeCapabilityCard(
        QStringLiteral("launchConsensusCard"),
        platform_style->ColorIcon(QStringLiteral(":/icons/connect_4"), QuicksilverStyle::Color(QuicksilverStyle::Token::Teal)),
        tr("Consensus"),
        tr("Optional"),
        tr("Run a verifying node when the storage cost is an intentional choice."),
        m_consensus_button,
        m_consensus_state,
        this);
    m_consensus_button->setText(tr("Review cost"));
    connect(m_consensus_button, &QPushButton::clicked, this, &DesktopLaunchPage::consensusRequested);
    m_consensus_summary = consensus_card->findChild<QLabel*>(QStringLiteral("launchConsensusCardBody"));

    QFrame* mining_card = MakeCapabilityCard(
        QStringLiteral("launchMiningCard"),
        platform_style->ColorIcon(QStringLiteral(":/icons/lock_closed"), QuicksilverStyle::Color(QuicksilverStyle::Token::Amber)),
        tr("Mining"),
        tr("Locked"),
        tr("Mining becomes available after consensus is enabled."),
        m_mining_button,
        m_mining_state,
        this);
    m_mining_button->setText(tr("Locked"));
    m_mining_button->setEnabled(false);
    connect(m_mining_button, &QPushButton::clicked, this, &DesktopLaunchPage::miningRequested);
    m_mining_icon = mining_card->findChild<QLabel*>(QStringLiteral("launchMiningCardIcon"));
    m_mining_summary = mining_card->findChild<QLabel*>(QStringLiteral("launchMiningCardBody"));

    capability_grid->addWidget(vault_card, 0, 0);
    capability_grid->addWidget(consensus_card, 0, 1);
    capability_grid->addWidget(mining_card, 0, 2);
    capability_grid->setColumnStretch(0, 1);
    capability_grid->setColumnStretch(1, 1);
    capability_grid->setColumnStretch(2, 1);
    root->addLayout(capability_grid);

    m_backup_panel = new QFrame(this);
    m_backup_panel->setObjectName(QStringLiteral("desktopLaunchBackupPanel"));
    auto* backup_layout = new QVBoxLayout(m_backup_panel);
    backup_layout->setContentsMargins(18, 16, 18, 16);
    backup_layout->setSpacing(8);

    auto* backup_top = new QHBoxLayout();
    backup_top->setSpacing(10);
    auto* backup_title = new QLabel(tr("Backup"), m_backup_panel);
    backup_title->setObjectName(QStringLiteral("desktopLaunchBackupTitle"));
    backup_title->setProperty("class", QStringLiteral("hudHeading"));
    backup_top->addWidget(backup_title, 1);
    m_backup_state = new QLabel(m_backup_panel);
    m_backup_state->setObjectName(QStringLiteral("desktopLaunchBackupState"));
    m_backup_state->setProperty("class", QStringLiteral("launchCapabilityState"));
    backup_top->addWidget(m_backup_state);
    backup_layout->addLayout(backup_top);

    m_backup_summary = new QLabel(m_backup_panel);
    m_backup_summary->setObjectName(QStringLiteral("desktopLaunchBackupSummary"));
    m_backup_summary->setProperty("class", QStringLiteral("muted"));
    m_backup_summary->setWordWrap(true);
    backup_layout->addWidget(m_backup_summary);

    m_backup_button = new QPushButton(m_backup_panel);
    m_backup_button->setObjectName(QStringLiteral("desktopLaunchBackupButton"));
    m_backup_button->setProperty("class", QStringLiteral("primaryActionButton"));
    connect(m_backup_button, &QPushButton::clicked, this, &DesktopLaunchPage::backupRequested);
    backup_layout->addWidget(m_backup_button, 0, Qt::AlignLeft);
    root->addWidget(m_backup_panel);

    auto* cost_panel = new QFrame(this);
    cost_panel->setObjectName(QStringLiteral("desktopLaunchCostPanel"));
    auto* cost_layout = new QVBoxLayout(cost_panel);
    cost_layout->setContentsMargins(18, 16, 18, 16);
    cost_layout->setSpacing(6);
    auto* cost_title = new QLabel(tr("Costs appear before commitment"), cost_panel);
    cost_title->setObjectName(QStringLiteral("desktopLaunchCostTitle"));
    cost_title->setProperty("class", QStringLiteral("hudHeading"));
    cost_layout->addWidget(cost_title);
    // Storage figures are derived, never written out: the archival total is the sum
    // chainparams already publishes, so these two sentences cannot drift from the
    // intro dialog's own arithmetic. See doc/design/chain-storage.md.
    auto* cost_copy = new QLabel(tr("Transfers spend proof-of-work. With default pruning, consensus keeps a %1 GB recent-block window and grows the chain state by about %2 GB per year; keeping full history grows total storage by about %3 GB per year. Consensus is never required just to use the vault.")
                                     .arg(DEFAULT_PRUNE_TARGET_GB)
                                     .arg(m_chain_state_size_gb)
                                     .arg(m_blockchain_size_gb + m_chain_state_size_gb),
                                 cost_panel);
    cost_copy->setObjectName(QStringLiteral("desktopLaunchCostCopy"));
    cost_copy->setProperty("class", QStringLiteral("muted"));
    cost_copy->setWordWrap(true);
    cost_layout->addWidget(cost_copy);
    root->addWidget(cost_panel);
    root->addStretch();

    setVaultSummary(false, QString());
    setBackupState(false, false);
    setConsensusEnabled(false);
}

void DesktopLaunchPage::setVaultSummary(bool has_vault, const QString& vault_name, const QStringList& holdings, const QString& last_activity, const QString& agent_summary)
{
    m_has_vault = has_vault;
    m_vault_state->setText(has_vault ? tr("Ready") : (m_vault_runtime_available ? tr("Create or open") : tr("Starting")));
    if (!has_vault) {
        m_vault_button->setText(m_vault_runtime_available ? tr("Create or open vault") : tr("Vault runtime starting"));
        m_vault_button->setEnabled(m_vault_runtime_available);
        m_vault_button->setToolTip(m_vault_runtime_available
            ? tr("Create a new vault or open an existing one.")
            : tr("Vault access becomes available after startup attaches the vault runtime."));
        m_vault_privacy_button->setVisible(false);
        m_vault_summary->setText(m_vault_runtime_available
            ? tr("Create or open a vault to hold quicksilver, transfer it, request it, and review recent activity.")
            : tr("Vault access will be available after startup attaches the vault runtime. Consensus remains optional."));
        return;
    }

    m_vault_button->setText(tr("Open vault"));
    m_vault_button->setEnabled(m_vault_runtime_available);
    m_vault_button->setToolTip(m_vault_runtime_available
        ? tr("Open the active vault.")
        : tr("Vault access becomes available after startup attaches the vault runtime."));
    m_vault_privacy_button->setVisible(true);
    m_vault_privacy_button->setText(m_privacy ? tr("Show balance") : tr("Hide balance"));
    m_vault_privacy_button->setToolTip(m_privacy
        ? tr("Show this vault balance on the launch screen.")
        : tr("Hide this vault balance on the launch screen."));
    QStringList summary;
    summary << holdings;
    summary << (agent_summary.isEmpty() ? tr("Agent setups: none") : agent_summary);
    summary << tr("Backup: check required");
    summary << (last_activity.isEmpty() ? tr("Recent activity: none yet") : tr("Recent activity: %1").arg(last_activity));
    m_vault_summary->setText(tr("%1 is available. %2.").arg(vault_name, summary.join(QStringLiteral(" | "))));
}

void DesktopLaunchPage::setVaultRuntimeAvailable(bool available)
{
    if (m_vault_runtime_available == available) return;
    m_vault_runtime_available = available;
    if (!m_has_vault) {
        setVaultSummary(false, QString());
    } else {
        m_vault_button->setEnabled(available);
        m_vault_button->setToolTip(available
            ? tr("Open the active vault.")
            : tr("Vault access becomes available after startup attaches the vault runtime."));
    }
}

void DesktopLaunchPage::setBackupState(bool has_vault, bool backup_done)
{
    m_backup_panel->setVisible(has_vault);
    if (!has_vault) return;

    m_backup_state->setText(backup_done ? tr("Done") : tr("Needed"));
    m_backup_summary->setText(backup_done
        ? tr("This vault records a completed backup. Make another backup after meaningful activity, because a backup only holds the history that existed when it was made.")
        : tr("Back up the vault file before relying on this desktop. This app does not issue a recovery phrase; only a current vault-file backup restores both spending keys and transaction history."));
    m_backup_button->setText(backup_done ? tr("Back up again") : tr("Back up vault"));
}

void DesktopLaunchPage::setConsensusEnabled(bool enabled)
{
    m_consensus_enabled = enabled;
    // Mirrors what the old pair of calls did: setConsensusEnabled overwrote every
    // label unconditionally and setConsensusStartupFailed(true) then overwrote them
    // again, so an enable always cleared a previous failure's text.
    m_consensus_failed = false;
    renderConsensusCards();
}

void DesktopLaunchPage::setPeerCount(int peers)
{
    m_peers = peers;
    renderConsensusCards();
}

void DesktopLaunchPage::renderConsensusCards()
{
    if (m_consensus_failed) {
        m_consensus_state->setText(tr("Restart needed"));
        m_consensus_summary->setText(tr("Consensus did not start. The saved opt-in was cleared; restart after fixing node settings to try again."));
        m_consensus_button->setText(tr("View issue"));

        m_mining_state->setText(tr("Locked"));
        m_mining_summary->setText(tr("Mining stays locked because consensus is not running."));
        m_mining_button->setText(tr("Locked"));
        m_mining_button->setEnabled(false);
        renderMiningIcon(/*locked=*/true);
        return;
    }

    const bool enabled = m_consensus_enabled;
    // Zero is the state that matters and -1 is not zero: a launch screen shown before
    // any client model exists must not accuse the user of being offline.
    const bool isolated = enabled && m_peers == 0;

    // The chip still answers only "is consensus on" -- that is what the opt-in tests
    // pin, and it is a different question from whether the node found anyone.
    m_consensus_state->setText(enabled ? tr("Enabled") : tr("Optional"));
    if (!enabled) {
        m_consensus_summary->setText(tr("Run a verifying node when the storage cost is an intentional choice."));
    } else if (isolated) {
        m_consensus_summary->setText(tr("Running, but connected to no peer. Until it finds one this node cannot see the network's chain. Open status to find out why."));
    } else {
        m_consensus_summary->setText(tr("Independent verification is enabled. Open status to review peers and sync progress."));
    }
    m_consensus_button->setText(enabled ? tr("View status") : tr("Review cost"));

    m_mining_state->setText(enabled ? tr("Available") : tr("Locked"));
    if (!enabled) {
        m_mining_summary->setText(tr("Mining becomes available after consensus is enabled."));
    } else if (isolated) {
        // Not locked: the first node on a new chain has no peers either, and
        // doc/bootstrapping.md makes that a supported way to start.
        m_mining_summary->setText(tr("Mining is available, but with no peers anything mined would be on a chain of this computer's own."));
    } else {
        m_mining_summary->setText(tr("Mining can now be configured because consensus has been accepted."));
    }
    m_mining_button->setText(enabled ? tr("Open mining") : tr("Locked"));
    m_mining_button->setEnabled(enabled);
    renderMiningIcon(/*locked=*/!enabled);
}

void DesktopLaunchPage::renderMiningIcon(bool locked)
{
    if (!m_mining_icon) return;
    const QString icon_name = locked ? QStringLiteral(":/icons/lock_closed") : QStringLiteral(":/icons/tx_mined");
    m_mining_icon->setPixmap(m_platform_style->ColorIcon(icon_name, QuicksilverStyle::Color(QuicksilverStyle::Token::Amber)).pixmap(QSize(24, 24)));
}

void DesktopLaunchPage::setPrivacy(bool privacy)
{
    m_privacy = privacy;
    if (m_vault_privacy_button && m_vault_privacy_button->isVisible()) {
        m_vault_privacy_button->setText(m_privacy ? tr("Show balance") : tr("Hide balance"));
        m_vault_privacy_button->setToolTip(m_privacy
            ? tr("Show this vault balance on the launch screen.")
            : tr("Hide this vault balance on the launch screen."));
    }
}

void DesktopLaunchPage::setConsensusStartupFailed(bool failed)
{
    if (!failed) return;

    m_consensus_failed = true;
    renderConsensusCards();
}
