// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_CONSENSUSREVIEWPAGE_H
#define QUICKSILVER_QT_CONSENSUSREVIEWPAGE_H

#include <QWidget>

#include <cstdint>

class PlatformStyle;

QT_BEGIN_NAMESPACE
class QLabel;
class QPushButton;
QT_END_NAMESPACE

class ConsensusReviewPage : public QWidget
{
    Q_OBJECT

public:
    //! Storage sizes are injected rather than read from Params(); see DesktopLaunchPage.
    explicit ConsensusReviewPage(const PlatformStyle* platform_style, uint64_t blockchain_size_gb, uint64_t chain_state_size_gb, QWidget* parent = nullptr);

    void setConsensusEnabled(bool enabled);
    void setStartupFailed(bool failed);

Q_SIGNALS:
    void continueRequested();
    void declined();

private:
    const uint64_t m_blockchain_size_gb;
    const uint64_t m_chain_state_size_gb;
    QLabel* m_title{nullptr};
    QLabel* m_intro{nullptr};
    QLabel* m_choice_copy{nullptr};
    QPushButton* m_decline_button{nullptr};
    QPushButton* m_continue_button{nullptr};
    bool m_consensus_enabled{false};
    bool m_startup_failed{false};

    void refreshState();
};

#endif // QUICKSILVER_QT_CONSENSUSREVIEWPAGE_H
