// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_DESKTOPLAUNCHPAGE_H
#define QUICKSILVER_QT_DESKTOPLAUNCHPAGE_H

#include <QStringList>
#include <QWidget>

#include <cstdint>

class PlatformStyle;

QT_BEGIN_NAMESPACE
class QFrame;
class QLabel;
class QPushButton;
QT_END_NAMESPACE

class DesktopLaunchPage : public QWidget
{
    Q_OBJECT

public:
    //! Storage sizes are injected rather than read from Params() so this screen can be
    //! tested at the figures a mainnet user actually sees; the Qt suite runs on sandbox,
    //! where both are zero. Same reason Intro takes them. See doc/design/chain-storage.md.
    explicit DesktopLaunchPage(const PlatformStyle* platform_style, uint64_t blockchain_size_gb, uint64_t chain_state_size_gb, QWidget* parent);

    void setVaultSummary(bool has_vault, const QString& vault_name, const QStringList& holdings = QStringList(), const QString& last_activity = QString(), const QString& agent_summary = QString());
    void setVaultRuntimeAvailable(bool available);
    void setBackupState(bool has_vault, bool backup_done);
    void setConsensusEnabled(bool enabled);
    void setConsensusStartupFailed(bool failed);
    //! Connection count, or -1 while it is not known yet.
    //!
    //! This is the first screen, and it previously said "Enabled" over a node with no
    //! peers exactly as it did over a synced one. The state chip still answers only
    //! "is consensus on" -- the connectivity goes in the card body, where the sentence
    //! people actually read is.
    void setPeerCount(int peers);
    void setPrivacy(bool privacy);

Q_SIGNALS:
    void vaultRequested();
    void backupRequested();
    void consensusRequested();
    void miningRequested();
    void privacyRequested(bool privacy);

private:
    void renderConsensusCards();
    void renderMiningIcon(bool locked);

    const PlatformStyle* m_platform_style;
    const uint64_t m_blockchain_size_gb;
    const uint64_t m_chain_state_size_gb;
    QLabel* m_vault_state{nullptr};
    QLabel* m_vault_summary{nullptr};
    QPushButton* m_vault_button{nullptr};
    QPushButton* m_vault_privacy_button{nullptr};
    QFrame* m_backup_panel{nullptr};
    QLabel* m_backup_state{nullptr};
    QLabel* m_backup_summary{nullptr};
    QPushButton* m_backup_button{nullptr};
    QLabel* m_consensus_state{nullptr};
    QLabel* m_consensus_summary{nullptr};
    QPushButton* m_consensus_button{nullptr};
    QLabel* m_mining_state{nullptr};
    QLabel* m_mining_summary{nullptr};
    QLabel* m_mining_icon{nullptr};
    QPushButton* m_mining_button{nullptr};
    bool m_privacy{false};
    bool m_has_vault{false};
    bool m_vault_runtime_available{false};
    bool m_consensus_enabled{false};
    bool m_consensus_failed{false};
    int m_peers{-1};
};

#endif // QUICKSILVER_QT_DESKTOPLAUNCHPAGE_H
