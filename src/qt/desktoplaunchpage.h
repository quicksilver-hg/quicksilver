// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_DESKTOPLAUNCHPAGE_H
#define QUICKSILVER_QT_DESKTOPLAUNCHPAGE_H

#include <QPointer>
#include <QString>
#include <QWidget>

#include <cstdint>

class PlatformStyle;
class TransactionFilterProxy;
class VaultModel;
namespace interfaces { struct MiningStatus; }

QT_BEGIN_NAMESPACE
class QDateTime;
class QEvent;
class QFrame;
class QLabel;
class QProgressBar;
class QPushButton;
class QSortFilterProxyModel;
class QTableView;
class QWidget;
QT_END_NAMESPACE

class LedgerRows;

//! Home: the newest entries of the vault's ledger beside the node, mining and
//! vault instruments.
class DesktopLaunchPage : public QWidget
{
    Q_OBJECT

public:
    //! Storage sizes are injected rather than read from Params() so this screen can be
    //! tested at the figures a mainnet user actually sees; the Qt suite runs on sandbox,
    //! where both are zero. Same reason Intro takes them. See doc/design/chain-storage.md.
    explicit DesktopLaunchPage(const PlatformStyle* platform_style, uint64_t blockchain_size_gb, uint64_t chain_state_size_gb, QWidget* parent);

    //! The vault the instruments describe. agent_summary is empty when the vault has
    //! no agent setups.
    void setVaultSummary(bool has_vault, const QString& vault_name, const QString& agent_summary = QString());
    void setVaultRuntimeAvailable(bool available);
    void setBackupState(bool has_vault, bool backup_done);
    void setConsensusEnabled(bool enabled);
    void setConsensusStartupFailed(bool failed);
    //! Connection count, or -1 while it is not known yet.
    //!
    //! This is the first screen, and it previously said "Enabled" over a node with no
    //! peers exactly as it did over a synced one. The Consensus row still answers only
    //! "is consensus on"; the connectivity goes in the Peers row and, when there is no
    //! peer at all, in the note under the rows.
    void setPeerCount(int peers);
    void setPrivacy(bool privacy);
    //! The Home ledger reads this vault. Null hides the table and says no vault is open.
    void setVaultModel(VaultModel* model);
    //! Block tip for the Node panel. A negative height means the tip is not known yet.
    void setChainTip(int height, const QDateTime& block_time);
    //! Shared sync inputs and GUI-computed progress (0..1) for the Node panel.
    void setSyncState(int blocks, int answered, bool behind_known_headers, double progress);
    //! The miner's status. Until the first report the Mining panel says only whether
    //! mining is available.
    void setMiningStatus(const interfaces::MiningStatus& status);
    void clearMiningStatus();
    //! How many times the ledger columns were measured. A burst of inserts
    //! should add a handful, not one measurement per row.
    int ledgerColumnFitsForTesting() const { return m_ledger_column_fits; }

public Q_SLOTS:
    //! The node's warnings, such as a build that is not a release. Empty hides them.
    void setAlerts(const QString& warnings);

Q_SIGNALS:
    void vaultRequested();
    void backupRequested();
    void consensusRequested();
    void miningRequested();
    void privacyRequested(bool privacy);
    //! The Ledger page, with every entry and its filters.
    void ledgerRequested();

private:
    void renderConsensusCards();
    void renderVaultRows();
    //! When the vault's maturing rewards can be spent, above the ledger.
    void renderMaturing();
    void refreshLedgerCount();
    //! One refilter, recount, and column fit for every insert queued this turn.
    void scheduleLedgerRefresh();
    //! Column order and widths, once the rows have columns to arrange.
    void arrangeLedgerColumns();
    void fitLedgerColumns();
    //! Below WIDE_WINDOW, narrow the instruments column by what the label
    //! still lacks. Runs after the page's layout is done.
    void fitInstruments();
    //! The instruments column's width, and the notes' text width with it.
    void setInstrumentWidth(int width);
    bool eventFilter(QObject* watched, QEvent* event) override;

    const PlatformStyle* m_platform_style;
    const uint64_t m_blockchain_size_gb;
    const uint64_t m_chain_state_size_gb;

    // Ledger panel.
    QFrame* m_ledger{nullptr};
    QLabel* m_ledger_count{nullptr};
    QLabel* m_ledger_empty{nullptr};
    QPushButton* m_ledger_open_full{nullptr};
    QTableView* m_ledger_table{nullptr};
    TransactionFilterProxy* m_ledger_filter{nullptr};
    QSortFilterProxyModel* m_first_rows{nullptr};
    LedgerRows* m_ledger_rows{nullptr};
    QPointer<VaultModel> m_ledger_model;

    QWidget* m_instruments{nullptr};

    // Node panel.
    QLabel* m_consensus_state{nullptr};
    QLabel* m_consensus_summary{nullptr};
    QLabel* m_node_alerts{nullptr};
    QWidget* m_node_live_rows{nullptr};
    QLabel* m_node_state{nullptr};
    QLabel* m_node_height{nullptr};
    QLabel* m_node_peers{nullptr};
    QLabel* m_node_last_block{nullptr};
    QProgressBar* m_node_sync{nullptr};
    QWidget* m_cost_panel{nullptr};
    QPushButton* m_consensus_button{nullptr};

    // Mining panel.
    QLabel* m_mining_state{nullptr};
    QLabel* m_mining_solver{nullptr};
    QLabel* m_mining_rate{nullptr};
    QLabel* m_mining_summary{nullptr};
    QPushButton* m_mining_button{nullptr};

    // Vault panel.
    QLabel* m_vault_name{nullptr};
    QLabel* m_vault_encryption{nullptr};
    QLabel* m_vault_agents{nullptr};
    QLabel* m_vault_agents_key{nullptr};
    QLabel* m_vault_summary{nullptr};
    QLabel* m_vault_maturing{nullptr};
    QPushButton* m_vault_button{nullptr};
    QPushButton* m_vault_privacy_button{nullptr};
    QWidget* m_backup_panel{nullptr};
    QLabel* m_backup_state{nullptr};
    QLabel* m_backup_summary{nullptr};
    QPushButton* m_backup_button{nullptr};

    QString m_vault_display_name;
    QString m_agent_summary;
    bool m_privacy{false};
    bool m_has_vault{false};
    bool m_vault_runtime_available{false};
    bool m_consensus_enabled{false};
    bool m_consensus_failed{false};
    bool m_have_sync{false};
    int m_sync_blocks{-1};
    int m_sync_answered{-1};
    bool m_sync_behind_known_headers{false};
    bool m_have_mining_status{false};
    bool m_mining_active{false};
    QString m_mining_word;
    int m_peers{-1};
    //! Set while fitLedgerColumns is writing widths, so the resize it causes
    //! does not re-enter.
    bool m_fitting_ledger{false};
    //! A coalesced ledger refresh is waiting for this turn's inserts to finish.
    bool m_ledger_refresh_pending{false};
    //! Set while that refresh runs, so the signals it emits do not arm another.
    bool m_refreshing_ledger{false};
    //! Entries to fitLedgerColumns that measured. Early returns do not count.
    int m_ledger_column_fits{0};
    //! Set while a fitInstruments call is queued.
    bool m_fitting_instruments{false};
};

#endif // QUICKSILVER_QT_DESKTOPLAUNCHPAGE_H
