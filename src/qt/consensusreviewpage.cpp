// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/consensusreviewpage.h>

#include <qt/benchpanel.h>
#include <qt/guiconstants.h>

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QVariant>
#include <QVBoxLayout>

namespace {
void SetActionClass(QPushButton* button, const QString& class_name)
{
    if (button->property("class").toString() == class_name) return;
    button->setProperty("class", class_name);
    button->style()->unpolish(button);
    button->style()->polish(button);
}
} // namespace

ConsensusReviewPage::ConsensusReviewPage(uint64_t blockchain_size_gb, uint64_t chain_state_size_gb, QWidget* parent)
    : QWidget(parent),
      m_blockchain_size_gb(blockchain_size_gb),
      m_chain_state_size_gb(chain_state_size_gb)
{
    setObjectName(QStringLiteral("consensusReviewPage"));
    setProperty("class", QStringLiteral("quicksilverPage"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(14);

    // What consensus costs, as one panel of label/value rows under the
    // statement of where this desktop stands.
    const BenchPanel::Parts review = BenchPanel::Make(QStringLiteral("consensusReviewHeader"), tr("Consensus"), this);
    m_title = new QLabel(review.frame);
    m_title->setObjectName(QStringLiteral("consensusReviewTitle"));
    m_title->setProperty("class", QStringLiteral("benchValue"));
    review.body->addWidget(m_title);
    m_intro = new QLabel(review.frame);
    m_intro->setObjectName(QStringLiteral("consensusReviewIntro"));
    m_intro->setProperty("class", QStringLiteral("benchNote"));
    m_intro->setWordWrap(true);
    review.body->addWidget(m_intro);
    auto* rows_host = new QWidget(review.frame);
    QGridLayout* rows = BenchPanel::MakeRows(rows_host);
    const auto add_cost = [&](const char* name, const QString& key, const QString& value) {
        QLabel* label = BenchPanel::AddRow(rows, key, QString::fromLatin1(name) + QStringLiteral("Value"), rows_host);
        label->setText(value);
        label->setWordWrap(true);
    };
    add_cost("consensusStorageCost", tr("Storage"), tr("Default pruning keeps a %1 GB recent-block window while chain state grows by about %2 GB per year. Keeping full history grows total storage by about %3 GB per year instead.")
                                                      .arg(DEFAULT_PRUNE_TARGET_GB)
                                                      .arg(m_chain_state_size_gb)
                                                      .arg(m_blockchain_size_gb + m_chain_state_size_gb));
    add_cost("consensusBackgroundCost", tr("Background work"), tr("Keeps a verifying process online and syncs with peers."));
    add_cost("consensusVaultCost", tr("Vault requirement"), tr("Not required for holding, requesting, or transferring quicksilver."));
    review.body->addWidget(rows_host);
    root->addWidget(review.frame);

    const BenchPanel::Parts choice = BenchPanel::Make(QStringLiteral("consensusChoicePanel"), tr("Choose without pressure"), this);
    m_choice_copy = new QLabel(choice.frame);
    m_choice_copy->setObjectName(QStringLiteral("consensusChoiceCopy"));
    m_choice_copy->setProperty("class", QStringLiteral("benchNote"));
    m_choice_copy->setWordWrap(true);
    choice.body->addWidget(m_choice_copy);

    auto* button_row = new QHBoxLayout();
    button_row->setContentsMargins(0, 4, 0, 0);
    button_row->setSpacing(10);
    m_decline_button = new QPushButton(choice.frame);
    m_decline_button->setObjectName(QStringLiteral("consensusDeclineButton"));
    connect(m_decline_button, &QPushButton::clicked, this, &ConsensusReviewPage::declined);
    button_row->addWidget(m_decline_button);

    m_continue_button = new QPushButton(choice.frame);
    m_continue_button->setObjectName(QStringLiteral("consensusContinueButton"));
    connect(m_continue_button, &QPushButton::clicked, this, &ConsensusReviewPage::continueRequested);
    button_row->addWidget(m_continue_button);
    button_row->addStretch();
    choice.body->addLayout(button_row);

    root->addWidget(choice.frame);
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
