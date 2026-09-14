// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/consensusreviewpage.h>

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
#include <QStyle>
#include <QVariant>
#include <QVBoxLayout>

namespace {
QFrame* MakeCostRow(const QString& object_name, const QString& label, const QString& value, QWidget* parent)
{
    auto* row = new QFrame(parent);
    row->setObjectName(object_name);
    row->setProperty("class", QStringLiteral("consensusCostRow"));

    auto* layout = new QVBoxLayout(row);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(5);

    auto* label_widget = new QLabel(label, row);
    label_widget->setObjectName(object_name + QStringLiteral("Label"));
    label_widget->setProperty("class", QStringLiteral("hudHeading"));
    layout->addWidget(label_widget);

    auto* value_widget = new QLabel(value, row);
    value_widget->setObjectName(object_name + QStringLiteral("Value"));
    value_widget->setProperty("class", QStringLiteral("sectionValue"));
    value_widget->setWordWrap(true);
    layout->addWidget(value_widget);

    return row;
}

void SetActionClass(QPushButton* button, const QString& class_name)
{
    if (button->property("class").toString() == class_name) return;
    button->setProperty("class", class_name);
    button->style()->unpolish(button);
    button->style()->polish(button);
}
} // namespace

ConsensusReviewPage::ConsensusReviewPage(const PlatformStyle* platform_style, uint64_t blockchain_size_gb, uint64_t chain_state_size_gb, QWidget* parent)
    : QWidget(parent),
      m_blockchain_size_gb(blockchain_size_gb),
      m_chain_state_size_gb(chain_state_size_gb)
{
    setObjectName(QStringLiteral("consensusReviewPage"));
    setProperty("class", QStringLiteral("quicksilverPage"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 28, 28, 28);
    root->setSpacing(18);

    auto* header = new QFrame(this);
    header->setObjectName(QStringLiteral("consensusReviewHeader"));
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(0, 0, 0, 0);
    header_layout->setSpacing(14);

    auto* mark = new QLabel(header);
    mark->setObjectName(QStringLiteral("consensusReviewMark"));
    mark->setPixmap(platform_style->ColorIcon(QStringLiteral(":/icons/connect_4"), QuicksilverStyle::Color(QuicksilverStyle::Token::Teal)).pixmap(QSize(34, 34)));
    mark->setAlignment(Qt::AlignCenter);
    mark->setFixedSize(QSize(46, 46));
    header_layout->addWidget(mark);

    auto* title_block = new QVBoxLayout();
    title_block->setContentsMargins(0, 0, 0, 0);
    title_block->setSpacing(4);
    m_title = new QLabel(tr("Consensus is optional"), header);
    m_title->setObjectName(QStringLiteral("consensusReviewTitle"));
    m_title->setProperty("class", QStringLiteral("pageTitle"));
    title_block->addWidget(m_title);

    m_intro = new QLabel(tr("Run independent verification only when the storage and background activity are an intentional choice. The vault works without it."), header);
    m_intro->setObjectName(QStringLiteral("consensusReviewIntro"));
    m_intro->setProperty("class", QStringLiteral("muted"));
    m_intro->setWordWrap(true);
    title_block->addWidget(m_intro);
    header_layout->addLayout(title_block, 1);
    root->addWidget(header);

    auto* cost_grid = new QGridLayout();
    cost_grid->setContentsMargins(0, 0, 0, 0);
    cost_grid->setHorizontalSpacing(12);
    cost_grid->setVerticalSpacing(12);
    cost_grid->addWidget(MakeCostRow(QStringLiteral("consensusStorageCost"), tr("Storage"), tr("Default pruning keeps a %1 GB recent-block window while chain state grows by about %2 GB per year. Keeping full history grows total storage by about %3 GB per year instead.")
                                             .arg(DEFAULT_PRUNE_TARGET_GB)
                                             .arg(m_chain_state_size_gb)
                                             .arg(m_blockchain_size_gb + m_chain_state_size_gb), this), 0, 0);
    cost_grid->addWidget(MakeCostRow(QStringLiteral("consensusBackgroundCost"), tr("Background work"), tr("Keeps a verifying process online and syncs with peers."), this), 0, 1);
    cost_grid->addWidget(MakeCostRow(QStringLiteral("consensusVaultCost"), tr("Vault requirement"), tr("Not required for holding, requesting, or transferring quicksilver."), this), 0, 2);
    cost_grid->setColumnStretch(0, 1);
    cost_grid->setColumnStretch(1, 1);
    cost_grid->setColumnStretch(2, 1);
    root->addLayout(cost_grid);

    auto* choice_panel = new QFrame(this);
    choice_panel->setObjectName(QStringLiteral("consensusChoicePanel"));
    auto* choice_layout = new QVBoxLayout(choice_panel);
    choice_layout->setContentsMargins(18, 16, 18, 16);
    choice_layout->setSpacing(9);

    auto* choice_title = new QLabel(tr("Choose without pressure"), choice_panel);
    choice_title->setObjectName(QStringLiteral("consensusChoiceTitle"));
    choice_title->setProperty("class", QStringLiteral("hudHeading"));
    choice_layout->addWidget(choice_title);

    m_choice_copy = new QLabel(tr("Independent verification is valuable, but most vault-only users should decline until they specifically want to run consensus."), choice_panel);
    m_choice_copy->setObjectName(QStringLiteral("consensusChoiceCopy"));
    m_choice_copy->setProperty("class", QStringLiteral("muted"));
    m_choice_copy->setWordWrap(true);
    choice_layout->addWidget(m_choice_copy);

    auto* button_row = new QHBoxLayout();
    button_row->setContentsMargins(0, 4, 0, 0);
    button_row->setSpacing(10);
    m_decline_button = new QPushButton(tr("Decline for now"), choice_panel);
    m_decline_button->setObjectName(QStringLiteral("consensusDeclineButton"));
    m_decline_button->setProperty("class", QStringLiteral("primaryActionButton"));
    connect(m_decline_button, &QPushButton::clicked, this, &ConsensusReviewPage::declined);
    button_row->addWidget(m_decline_button);

    m_continue_button = new QPushButton(tr("Enable consensus"), choice_panel);
    m_continue_button->setObjectName(QStringLiteral("consensusContinueButton"));
    m_continue_button->setProperty("class", QStringLiteral("secondaryActionButton"));
    connect(m_continue_button, &QPushButton::clicked, this, &ConsensusReviewPage::continueRequested);
    button_row->addWidget(m_continue_button);
    button_row->addStretch();
    choice_layout->addLayout(button_row);

    root->addWidget(choice_panel);
    root->addStretch();
    setConsensusEnabled(false);
}

void ConsensusReviewPage::setConsensusEnabled(bool enabled)
{
    m_consensus_enabled = enabled;
    refreshState();
}

void ConsensusReviewPage::setStartupFailed(bool failed)
{
    m_startup_failed = failed;
    refreshState();
}

void ConsensusReviewPage::refreshState()
{
    if (m_startup_failed) {
        m_title->setText(tr("Consensus restart required"));
        m_intro->setText(tr("Consensus did not start. The vault-first shell can stay open, but consensus needs a fresh application start after node settings are fixed."));
        m_choice_copy->setText(tr("The saved opt-in was cleared so the next launch returns to the vault-first path."));
        m_decline_button->setText(tr("Back to home"));
        m_continue_button->setText(tr("Restart required"));
        m_continue_button->setEnabled(false);
        SetActionClass(m_decline_button, QStringLiteral("primaryActionButton"));
        SetActionClass(m_continue_button, QStringLiteral("secondaryActionButton"));
        return;
    }

    m_title->setText(m_consensus_enabled ? tr("Consensus is enabled") : tr("Consensus is optional"));
    m_intro->setText(m_consensus_enabled
        ? tr("Independent verification has been accepted on this desktop. Node status shows the current peer and sync state.")
        : tr("Run independent verification only when the storage and background activity are an intentional choice. The vault works without it."));
    m_choice_copy->setText(m_consensus_enabled
        ? tr("You can review live status now. Disabling the provisioned node is a separate startup setting and is not changed here.")
        : tr("Independent verification is valuable, but most vault-only users should decline until they specifically want to run consensus."));
    m_decline_button->setText(m_consensus_enabled ? tr("Back to home") : tr("Decline for now"));
    m_continue_button->setText(m_consensus_enabled ? tr("Show node status") : tr("Enable consensus"));
    m_continue_button->setEnabled(true);
    // When consensus is optional, the visual hierarchy follows the copy's
    // recommendation to decline. Once it is running, live status becomes the
    // primary next action.
    SetActionClass(m_decline_button, m_consensus_enabled ? QStringLiteral("secondaryActionButton") : QStringLiteral("primaryActionButton"));
    SetActionClass(m_continue_button, m_consensus_enabled ? QStringLiteral("primaryActionButton") : QStringLiteral("secondaryActionButton"));
}
