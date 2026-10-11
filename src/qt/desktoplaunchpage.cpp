// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/desktoplaunchpage.h>

#include <chainparams.h>
#include <interfaces/node.h>
#include <interfaces/vault.h>
#include <qt/benchpanel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/ledgerrows.h>
#include <qt/maturity.h>
#include <qt/minemintpage.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/quicksilverstyle.h>
#include <qt/quicksilverunits.h>
#include <qt/transactiondescdialog.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactionrecord.h>
#include <qt/transactiontablemodel.h>
#include <qt/vaultmodel.h>

#include <QAbstractItemView>
#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSizePolicy>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QVBoxLayout>

#include <vector>

namespace {
constexpr int INSTRUMENT_WIDTH{320};
//! How many of the newest rows Home's ledger shows (F-451). The Ledger page
//! shows them all.
constexpr int LEDGER_SNAPSHOT_ROWS{10};

//! The first `count` rows of a model that is already sorted, newest first:
//! Home's snapshot of the ledger. Whether a row is in depends on where it
//! sits, so every change in the source re-asks every row.
class FirstRows : public QSortFilterProxyModel
{
public:
    FirstRows(int count, QObject* parent) : QSortFilterProxyModel(parent), m_count(count) {}

    void setSourceModel(QAbstractItemModel* source) override
    {
        for (const QMetaObject::Connection& connection : m_source_connections) disconnect(connection);
        m_source_connections.clear();
        QSortFilterProxyModel::setSourceModel(source);
        if (!source) return;
        const auto refilter = [this] { invalidateFilter(); };
        m_source_connections = {
            connect(source, &QAbstractItemModel::rowsInserted, this, refilter),
            connect(source, &QAbstractItemModel::rowsRemoved, this, refilter),
            connect(source, &QAbstractItemModel::rowsMoved, this, refilter),
            connect(source, &QAbstractItemModel::layoutChanged, this, refilter),
            connect(source, &QAbstractItemModel::modelReset, this, refilter),
        };
    }

protected:
    bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override
    {
        return !source_parent.isValid() && source_row < m_count;
    }

private:
    const int m_count;
    std::vector<QMetaObject::Connection> m_source_connections;
};
//! The text width inside an instrument panel: the column less the panel's
//! border and body margins.
constexpr int NOTE_WIDTH{INSTRUMENT_WIDTH - 2 - 24};

//! A wrapped note in the instrument column. A word-wrapped QLabel reports the
//! height of a narrow golden-ratio box as its minimum, which made the column
//! taller than the window; the column's width is fixed, so the note answers
//! for that width instead.
class NoteLabel : public QLabel
{
public:
    explicit NoteLabel(QWidget* parent) : QLabel(parent)
    {
        setWordWrap(true);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    }
    QSize sizeHint() const override { return {NOTE_WIDTH, heightForWidth(NOTE_WIDTH)}; }
    QSize minimumSizeHint() const override { return sizeHint(); }
};

//! A note under a panel's rows: muted, or the warning tone when it says
//! something is wrong.
QLabel* MakeNote(const QString& object_name, QWidget* parent)
{
    auto* label = new NoteLabel(parent);
    label->setObjectName(object_name);
    label->setProperty("class", QStringLiteral("benchNote"));
    return label;
}

void SetTone(QLabel* label, const char* tone)
{
    if (label->property("benchTone").toString() == QLatin1String(tone)) return;
    label->setProperty("benchTone", QString::fromLatin1(tone));
    label->style()->unpolish(label);
    label->style()->polish(label);
}

QPushButton* MakeQuietButton(const QString& object_name, QWidget* parent)
{
    auto* button = new QPushButton(parent);
    button->setObjectName(object_name);
    button->setProperty("class", QStringLiteral("benchQuiet"));
    return button;
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

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(12);

    // LEDGER: the newest rows, with how many there are in all and a link to the
    // Ledger page in the head. The Ledger page holds every row and the filters.
    const BenchPanel::Parts ledger = BenchPanel::Make(QStringLiteral("homeLedger"), tr("Ledger"), this);
    m_ledger = ledger.frame;
    m_ledger->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_ledger_count = new QLabel(m_ledger);
    m_ledger_count->setObjectName(QStringLiteral("homeLedgerCount"));
    m_ledger_count->setProperty("class", QStringLiteral("benchNote"));
    ledger.head->addWidget(m_ledger_count);
    m_ledger_open_full = MakeQuietButton(QStringLiteral("homeLedgerOpenFull"), m_ledger);
    m_ledger_open_full->setText(tr("Full ledger"));
    m_ledger_open_full->setToolTip(tr("Open the Ledger page, with every entry and its filters"));
    connect(m_ledger_open_full, &QPushButton::clicked, this, &DesktopLaunchPage::ledgerRequested);
    ledger.head->addWidget(m_ledger_open_full);
    ledger.body->setContentsMargins(0, 0, 0, 0);
    ledger.body->setSpacing(0);


    m_ledger_empty = new QLabel(tr("No vault is open"), m_ledger);
    m_ledger_empty->setObjectName(QStringLiteral("homeLedgerEmpty"));
    m_ledger_empty->setProperty("class", QStringLiteral("benchNote"));
    m_ledger_empty->setContentsMargins(12, 12, 12, 12);
    m_ledger_empty->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    ledger.body->addWidget(m_ledger_empty);

    m_ledger_filter = new TransactionFilterProxy(this);
    m_ledger_filter->setDynamicSortFilter(true);
    m_ledger_filter->setSortRole(Qt::EditRole);
    auto* newest = new FirstRows(LEDGER_SNAPSHOT_ROWS, this);
    newest->setSourceModel(m_ledger_filter);
    m_ledger_rows = new LedgerRows(LedgerRows::Tooltips::LabelOnly, this);
    m_ledger_rows->setSourceModel(newest);
    // The head counts every row, so it follows the full list as well as the snapshot.
    for (QAbstractItemModel* rows : {static_cast<QAbstractItemModel*>(m_ledger_filter), static_cast<QAbstractItemModel*>(m_ledger_rows)}) {
        for (const auto& signal : {&QAbstractItemModel::rowsInserted, &QAbstractItemModel::rowsRemoved}) {
            connect(rows, signal, this, &DesktopLaunchPage::refreshLedgerCount);
        }
        connect(rows, &QAbstractItemModel::modelReset, this, &DesktopLaunchPage::refreshLedgerCount);
        connect(rows, &QAbstractItemModel::layoutChanged, this, &DesktopLaunchPage::refreshLedgerCount);
    }

    m_ledger_table = new QTableView(m_ledger);
    m_ledger_table->setObjectName(QStringLiteral("homeLedgerTable"));
    m_ledger_table->setProperty("class", QStringLiteral("benchTable"));
    m_ledger_table->setModel(m_ledger_rows);
    for (const auto& signal : {&QAbstractItemModel::rowsInserted, &QAbstractItemModel::rowsRemoved}) {
        connect(m_ledger_rows, signal, this, &DesktopLaunchPage::fitLedgerColumns);
    }
    connect(m_ledger_rows, &QAbstractItemModel::modelReset, this, &DesktopLaunchPage::fitLedgerColumns);
    connect(m_ledger_rows, &QAbstractItemModel::dataChanged, this, &DesktopLaunchPage::fitLedgerColumns);
    connect(m_ledger_rows, &QAbstractItemModel::layoutChanged, this, &DesktopLaunchPage::fitLedgerColumns);
    m_ledger_table->setFrameShape(QFrame::NoFrame);
    m_ledger_table->setShowGrid(false);
    m_ledger_table->setAlternatingRowColors(true);
    m_ledger_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_ledger_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_ledger_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_ledger_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_ledger_table->setWordWrap(false);
    m_ledger_table->setTextElideMode(Qt::ElideMiddle);
    m_ledger_table->setIconSize(QSize(14, 14));
    m_ledger_table->verticalHeader()->hide();
    m_ledger_table->verticalHeader()->setDefaultSectionSize(30);
    QHeaderView* header = m_ledger_table->horizontalHeader();
    header->setHighlightSections(false);
    header->setSectionsClickable(false);
    header->setMinimumSectionSize(40);
    connect(m_ledger_rows, &QAbstractItemModel::modelReset, this, &DesktopLaunchPage::arrangeLedgerColumns);
    connect(m_ledger_rows, &QAbstractItemModel::columnsInserted, this, &DesktopLaunchPage::arrangeLedgerColumns);
    connect(m_ledger_table, &QTableView::doubleClicked, this, [](const QModelIndex& index) {
        auto* dialog = new TransactionDescDialog(index);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    });
    ledger.body->addWidget(m_ledger_table, 1);
    // The node's warnings, a missing backup and the maturity countdown head the
    // ledger column: that column gives up rows to make room, where the fixed
    // instruments column would overflow a 1200x800 window.
    // The maturity countdown describes the ledger's maturing rows, so it heads
    // the Ledger panel, inside it.
    m_vault_maturing = MakeNote(QStringLiteral("homeVaultMaturing"), m_ledger);
    m_vault_maturing->setContentsMargins(12, 10, 12, 0);
    m_vault_maturing->setVisible(false);
    ledger.body->insertWidget(0, m_vault_maturing);
    // A missing backup's warning follows it. The Backup row and its command are
    // the Vault panel's; the sentence is too long for the fixed instruments
    // column, which would overflow a 1200x800 window, while the ledger gives
    // up rows to make room.
    m_backup_panel = new QWidget(m_ledger);
    m_backup_panel->setObjectName(QStringLiteral("desktopLaunchBackupPanel"));
    auto* backup_layout = new QVBoxLayout(m_backup_panel);
    backup_layout->setContentsMargins(12, 10, 12, 0);
    m_backup_summary = MakeNote(QStringLiteral("desktopLaunchBackupSummary"), m_backup_panel);
    backup_layout->addWidget(m_backup_summary);
    ledger.body->insertWidget(1, m_backup_panel);
    root->addWidget(m_ledger, 1);

    // The instruments: a fixed narrow column of label/value panels.
    auto* instruments = new QWidget(this);
    instruments->setObjectName(QStringLiteral("homeInstruments"));
    instruments->setFixedWidth(INSTRUMENT_WIDTH);
    auto* instruments_layout = new QVBoxLayout(instruments);
    instruments_layout->setContentsMargins(0, 0, 0, 0);
    instruments_layout->setSpacing(12);

    // NODE.
    const BenchPanel::Parts node = BenchPanel::Make(QStringLiteral("launchConsensusCard"), tr("Node"), instruments);
    // The node's warnings (this build is not a release) say something about
    // the node, so they head its panel.
    m_node_alerts = MakeNote(QStringLiteral("homeNodeAlerts"), node.frame);
    SetTone(m_node_alerts, "warn");
    m_node_alerts->setVisible(false);
    node.body->addWidget(m_node_alerts);
    auto* consensus_rows = new QWidget(node.frame);
    m_consensus_state = BenchPanel::AddRow(BenchPanel::MakeRows(consensus_rows), tr("Consensus"), QStringLiteral("launchConsensusCardState"), consensus_rows);
    node.body->addWidget(consensus_rows);
    m_node_live_rows = new QWidget(node.frame);
    auto* live = BenchPanel::MakeRows(m_node_live_rows);
    m_node_state = BenchPanel::AddRow(live, tr("State"), QStringLiteral("homeNodeState"), m_node_live_rows);
    m_node_height = BenchPanel::AddRow(live, tr("Height"), QStringLiteral("homeNodeHeight"), m_node_live_rows);
    m_node_peers = BenchPanel::AddRow(live, tr("Peers"), QStringLiteral("homeNodePeers"), m_node_live_rows);
    m_node_last_block = BenchPanel::AddRow(live, tr("Last block"), QStringLiteral("homeNodeLastBlock"), m_node_live_rows);
    m_node_sync = new QProgressBar(m_node_live_rows);
    m_node_sync->setObjectName(QStringLiteral("homeNodeSync"));
    m_node_sync->setTextVisible(false);
    m_node_sync->setRange(0, 1000);
    live->addWidget(m_node_sync, live->rowCount(), 0, 1, 2);
    node.body->addWidget(m_node_live_rows);
    m_consensus_summary = MakeNote(QStringLiteral("launchConsensusCardBody"), node.frame);
    node.body->addWidget(m_consensus_summary);

    m_cost_panel = new QWidget(node.frame);
    m_cost_panel->setObjectName(QStringLiteral("desktopLaunchCostPanel"));
    auto* cost_layout = new QVBoxLayout(m_cost_panel);
    cost_layout->setContentsMargins(0, 0, 0, 0);
    // Storage figures are derived, never written out: the archival total is the sum
    // chainparams already publishes, so these two sentences cannot drift from the
    // intro dialog's own arithmetic. See doc/design/chain-storage.md.
    auto* cost_copy = MakeNote(QStringLiteral("desktopLaunchCostCopy"), m_cost_panel);
    cost_copy->setText(tr("Transfers spend proof-of-work. With default pruning, consensus keeps a %1 GB recent-block window and grows the chain state by about %2 GB per year; keeping full history grows total storage by about %3 GB per year. Consensus is never required just to use the vault.")
                           .arg(DEFAULT_PRUNE_TARGET_GB)
                           .arg(m_chain_state_size_gb)
                           .arg(m_blockchain_size_gb + m_chain_state_size_gb));
    cost_layout->addWidget(cost_copy);
    node.body->addWidget(m_cost_panel);
    m_consensus_button = MakeQuietButton(QStringLiteral("launchConsensusCardButton"), node.frame);
    connect(m_consensus_button, &QPushButton::clicked, this, &DesktopLaunchPage::consensusRequested);
    node.body->addWidget(m_consensus_button, 0, Qt::AlignLeft);
    instruments_layout->addWidget(node.frame);

    // MINING. No interval chart: no model keeps block intervals.
    const BenchPanel::Parts mining = BenchPanel::Make(QStringLiteral("launchMiningCard"), tr("Mining"), instruments);
    auto* mining_rows = new QWidget(mining.frame);
    auto* mining_grid = BenchPanel::MakeRows(mining_rows);
    m_mining_state = BenchPanel::AddRow(mining_grid, tr("State"), QStringLiteral("launchMiningCardState"), mining_rows);
    m_mining_solver = BenchPanel::AddRow(mining_grid, tr("Solver"), QStringLiteral("homeMiningSolver"), mining_rows);
    m_mining_rate = BenchPanel::AddRow(mining_grid, tr("Rate"), QStringLiteral("homeMiningRate"), mining_rows);
    mining.body->addWidget(mining_rows);
    m_mining_summary = MakeNote(QStringLiteral("launchMiningCardBody"), mining.frame);
    mining.body->addWidget(m_mining_summary);
    m_mining_button = MakeQuietButton(QStringLiteral("launchMiningCardButton"), mining.frame);
    connect(m_mining_button, &QPushButton::clicked, this, &DesktopLaunchPage::miningRequested);
    mining.body->addWidget(m_mining_button, 0, Qt::AlignLeft);
    instruments_layout->addWidget(mining.frame);

    // VAULT.
    const BenchPanel::Parts vault = BenchPanel::Make(QStringLiteral("launchVaultCard"), tr("Vault"), instruments);
    auto* vault_rows = new QWidget(vault.frame);
    auto* vault_grid = BenchPanel::MakeRows(vault_rows);
    m_vault_name = BenchPanel::AddRow(vault_grid, tr("Vault"), QStringLiteral("launchVaultCardState"), vault_rows);
    m_vault_encryption = BenchPanel::AddRow(vault_grid, tr("Encryption"), QStringLiteral("homeVaultEncryption"), vault_rows);
    m_backup_state = BenchPanel::AddRow(vault_grid, tr("Backup"), QStringLiteral("desktopLaunchBackupState"), vault_rows);
    m_vault_agents = BenchPanel::AddRow(vault_grid, tr("Agents"), QStringLiteral("homeVaultAgents"), vault_rows, &m_vault_agents_key);
    vault.body->addWidget(vault_rows);
    m_vault_summary = MakeNote(QStringLiteral("launchVaultCardBody"), vault.frame);
    vault.body->addWidget(m_vault_summary);


    auto* vault_commands = new QHBoxLayout;
    vault_commands->setSpacing(6);
    m_vault_button = MakeQuietButton(QStringLiteral("launchVaultCardButton"), vault.frame);
    connect(m_vault_button, &QPushButton::clicked, this, &DesktopLaunchPage::vaultRequested);
    vault_commands->addWidget(m_vault_button);
    m_vault_privacy_button = MakeQuietButton(QStringLiteral("launchVaultBalancePrivacyButton"), vault.frame);
    connect(m_vault_privacy_button, &QPushButton::clicked, this, [this] {
        Q_EMIT privacyRequested(!m_privacy);
    });
    vault_commands->addWidget(m_vault_privacy_button);
    m_backup_button = MakeQuietButton(QStringLiteral("desktopLaunchBackupButton"), vault.frame);
    connect(m_backup_button, &QPushButton::clicked, this, &DesktopLaunchPage::backupRequested);
    vault_commands->addWidget(m_backup_button);
    vault_commands->addStretch();
    vault.body->addLayout(vault_commands);
    instruments_layout->addWidget(vault.frame);
    instruments_layout->addStretch();
    root->addWidget(instruments);

    setVaultSummary(false, QString());
    setBackupState(false, false);
    setConsensusEnabled(false);
    setPeerCount(-1);
    setChainTip(-1, QDateTime());
    setVaultModel(nullptr);
}

void DesktopLaunchPage::setVaultSummary(bool has_vault, const QString& vault_name, const QString& agent_summary)
{
    m_has_vault = has_vault;
    m_vault_display_name = vault_name;
    m_agent_summary = agent_summary;
    renderVaultRows();
}

void DesktopLaunchPage::renderVaultRows()
{
    if (!m_has_vault) {
        m_vault_name->setText(m_vault_runtime_available ? tr("None open") : tr("Starting"));
        m_vault_button->setText(m_vault_runtime_available ? tr("Create or open vault") : tr("Vault runtime starting"));
        m_vault_button->setEnabled(m_vault_runtime_available);
        m_vault_button->setToolTip(m_vault_runtime_available
            ? tr("Create a new vault or open an existing one.")
            : tr("Vault access becomes available after startup attaches the vault runtime."));
        m_vault_privacy_button->setVisible(false);
        m_vault_summary->setText(m_vault_runtime_available
            ? tr("Create or open a vault to hold quicksilver, transfer it, request it, and review recent activity.")
            : tr("Vault access will be available after startup attaches the vault runtime. Consensus remains optional."));
        m_vault_summary->setVisible(true);
        for (QLabel* label : {m_vault_encryption, m_vault_agents, m_vault_agents_key}) label->setVisible(false);
        m_vault_encryption->setText(QString());
        m_vault_button->setVisible(true);
        m_vault_maturing->setVisible(false);
        return;
    }

    m_vault_name->setText(m_vault_display_name);
    // Home is the open vault; there is nothing further for the command to open.
    m_vault_button->setVisible(false);
    renderMaturing();
    m_vault_privacy_button->setVisible(true);
    m_vault_privacy_button->setText(m_privacy ? tr("Show balance") : tr("Hide balance"));
    m_vault_privacy_button->setToolTip(m_privacy
        ? tr("Show this vault's balances.")
        : tr("Mask this vault's balances."));
    m_vault_summary->clear();
    m_vault_summary->setVisible(false);

    QString encryption = tr("Unknown");
    if (m_ledger_model) {
        switch (m_ledger_model->getEncryptionStatus()) {
        case VaultModel::Locked: encryption = tr("Locked"); break;
        case VaultModel::Unlocked: encryption = tr("Unlocked"); break;
        case VaultModel::Unencrypted: encryption = tr("Not encrypted"); break;
        case VaultModel::NoKeys: encryption = tr("No keys"); break;
        }
    }
    m_vault_encryption->setText(encryption);
    m_vault_encryption->setVisible(true);
    const bool agents = !m_agent_summary.isEmpty();
    m_vault_agents->setText(m_agent_summary);
    m_vault_agents->setVisible(agents);
    m_vault_agents_key->setVisible(agents);
}

void DesktopLaunchPage::setVaultRuntimeAvailable(bool available)
{
    if (m_vault_runtime_available == available) return;
    m_vault_runtime_available = available;
    renderVaultRows();
}

void DesktopLaunchPage::setBackupState(bool has_vault, bool backup_done)
{
    m_backup_panel->setVisible(has_vault);
    m_backup_button->setVisible(has_vault);
    if (!has_vault) {
        m_backup_state->setText(tr("No vault"));
        SetTone(m_backup_state, "plain");
        return;
    }

    m_backup_state->setText(backup_done ? tr("Done") : tr("Needed"));
    SetTone(m_backup_state, backup_done ? "good" : "warn");
    // Only a missing backup needs the sentence; a recorded one is said by the row.
    m_backup_summary->setText(backup_done
        ? tr("This vault records a completed backup. Make another backup after meaningful activity, because a backup only holds the history that existed when it was made.")
        : tr("Back up the vault file before relying on this desktop. This app does not issue a recovery phrase; only a current vault-file backup restores both spending keys and transaction history."));
    m_backup_summary->setVisible(!backup_done);
    m_backup_panel->setVisible(!backup_done);
    SetTone(m_backup_summary, "warn");
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
    m_node_peers->setText(peers < 0 ? QStringLiteral("—") : QString::number(peers));
    renderConsensusCards();
}

void DesktopLaunchPage::setVaultModel(VaultModel* model)
{
    const bool changed = model != m_ledger_model;
    if (changed) {
        if (m_ledger_model) disconnect(m_ledger_model, nullptr, this, nullptr);
        m_ledger_model = model;
        m_ledger_filter->setSourceModel(model ? model->getTransactionTableModel() : nullptr);
        arrangeLedgerColumns();
        if (model) {
            m_ledger_filter->sort(TransactionTableModel::Date, Qt::DescendingOrder);
            connect(model, &VaultModel::encryptionStatusChanged, this, &DesktopLaunchPage::renderVaultRows);
            if (OptionsModel* options = model->getOptionsModel()) {
                connect(options, &OptionsModel::displayUnitChanged, this, [this, options] {
                    m_ledger_rows->setDisplay(options->getDisplayUnit(), m_privacy);
                });
            }
        }
    }
    const QuicksilverUnit unit = model && model->getOptionsModel() ? model->getOptionsModel()->getDisplayUnit() : QuicksilverUnit::HG;
    m_ledger_rows->setDisplay(unit, m_privacy);
    m_ledger_table->setVisible(model != nullptr);
    m_ledger_open_full->setVisible(model != nullptr);
    m_ledger_count->setVisible(model != nullptr);
    m_ledger_empty->setVisible(model == nullptr);
    refreshLedgerCount();
    renderVaultRows();
}

void DesktopLaunchPage::arrangeLedgerColumns()
{
    QHeaderView* header = m_ledger_table->horizontalHeader();
    if (header->count() <= TransactionTableModel::Amount) return;
    // STATE reads last, after the amount.
    if (header->visualIndex(TransactionTableModel::Status) != TransactionTableModel::Amount) {
        header->moveSection(header->visualIndex(TransactionTableModel::Status), TransactionTableModel::Amount);
    }
    header->setSectionResizeMode(QHeaderView::Fixed);
    header->setSectionResizeMode(TransactionTableModel::ToAddress, QHeaderView::Stretch);
    fitLedgerColumns();
}

void DesktopLaunchPage::fitLedgerColumns()
{
    QHeaderView* header = m_ledger_table->horizontalHeader();
    if (header->count() <= TransactionTableModel::Amount) return;
    // Every column but Label is exactly as wide as its content, so the label
    // takes all the slack, as on the Ledger page.
    // QTableView narrows the public QAbstractItemView call to protected.
    QAbstractItemView* cells = m_ledger_table;
    for (int column : {TransactionTableModel::Date, TransactionTableModel::Type, TransactionTableModel::Amount, TransactionTableModel::Status}) {
        m_ledger_table->setColumnWidth(column, std::max(cells->sizeHintForColumn(column), header->sectionSizeHint(column)));
    }
}

void DesktopLaunchPage::refreshLedgerCount()
{
    const int shown = m_ledger_rows->rowCount();
    const int all = m_ledger_filter->rowCount();
    if (shown < all) {
        m_ledger_count->setText(tr("%1 of %2 entries").arg(shown).arg(all));
    } else {
        m_ledger_count->setText(all == 1 ? tr("1 entry") : tr("%1 entries").arg(all));
    }
}

void DesktopLaunchPage::setChainTip(int height, const QDateTime& block_time)
{
    const QString none = QStringLiteral("—");
    if (height < 0) {
        m_node_height->setText(none);
        m_node_last_block->setText(none);
        return;
    }
    m_node_height->setText(QString::number(height));
    if (!block_time.isValid()) {
        m_node_last_block->setText(none);
        return;
    }
    const qint64 secs = block_time.secsTo(QDateTime::currentDateTime());
    const qint64 age = secs < 0 ? 0 : secs;
    m_node_last_block->setText(tr("%1 ago").arg(GUIUtil::formatNiceTimeOffset(age)));
}

void DesktopLaunchPage::setSyncState(bool synced, double progress)
{
    m_have_sync = true;
    m_synced = synced;
    m_node_sync->setValue(static_cast<int>(qBound(0.0, progress, 1.0) * 1000.0 + 0.5));
    renderConsensusCards();
}

void DesktopLaunchPage::setMiningStatus(const interfaces::MiningStatus& status)
{
    const MineMintPage::StatusText rows = MineMintPage::statusText(status);
    m_have_mining_status = true;
    m_mining_active = status.active;
    m_mining_word = rows.block_mining;
    m_mining_solver->setText(rows.solver);
    m_mining_rate->setText(rows.attempts);
    renderConsensusCards();
}

void DesktopLaunchPage::clearMiningStatus()
{
    m_have_mining_status = false;
    m_mining_active = false;
    m_mining_word.clear();
    renderConsensusCards();
}

void DesktopLaunchPage::renderConsensusCards()
{
    const QString none = QStringLiteral("—");
    if (m_consensus_failed) {
        m_consensus_state->setText(tr("Restart needed"));
        SetTone(m_consensus_state, "warn");
        m_consensus_summary->setText(tr("Consensus did not start. The saved opt-in was cleared; restart after fixing node settings to try again."));
        m_consensus_summary->setVisible(true);
        SetTone(m_consensus_summary, "warn");
        m_node_live_rows->setVisible(false);
        m_cost_panel->setVisible(false);
        m_consensus_button->setText(tr("View issue"));

        m_mining_state->setText(tr("Locked"));
        m_mining_solver->setText(none);
        m_mining_rate->setText(none);
        m_mining_summary->setText(tr("Mining stays locked because consensus is not running."));
        m_mining_summary->setVisible(true);
        SetTone(m_mining_summary, "plain");
        m_mining_button->setText(tr("Locked"));
        m_mining_button->setEnabled(false);
        return;
    }

    const bool enabled = m_consensus_enabled;
    // Zero is the state that matters and -1 is not zero: a launch screen shown before
    // any client model exists must not accuse the user of being offline.
    const bool isolated = enabled && m_peers == 0;

    // The Consensus row answers only "is consensus on" -- that is what the opt-in
    // tests pin, and it is a different question from whether the node found anyone.
    m_consensus_state->setText(enabled ? tr("Enabled") : tr("Optional"));
    SetTone(m_consensus_state, "plain");
    m_node_live_rows->setVisible(enabled);
    m_cost_panel->setVisible(!enabled);
    if (!enabled) {
        m_consensus_summary->setText(tr("Run a verifying node when the storage cost is an intentional choice."));
    } else if (isolated) {
        m_consensus_summary->setText(tr("Running, but connected to no peer. Until it finds one this node cannot see the network's chain."));
    } else {
        m_consensus_summary->setText(tr("Independent verification is enabled. Open status to review peers and sync progress."));
    }
    // The rows say the ordinary cases; the note is for the one that needs a sentence.
    m_consensus_summary->setVisible(isolated);
    SetTone(m_consensus_summary, "warn");
    m_consensus_button->setText(enabled ? tr("View status") : tr("Review cost"));

    QString node_state = none;
    if (m_peers == 0) {
        node_state = tr("No peers");
    } else if (m_have_sync) {
        node_state = m_synced ? tr("Synchronized") : tr("Catching up");
    } else {
        node_state = tr("Connecting");
    }
    m_node_state->setText(node_state);
    SetTone(m_node_state, m_have_sync && m_synced && m_peers != 0 ? "good" : "plain");

    // Once the miner reports, the row uses the strip's Idle / Active / Halted.
    m_mining_state->setText(!enabled ? tr("Locked") : (m_have_mining_status ? m_mining_word : tr("Available")));
    if (!enabled || !m_have_mining_status) {
        m_mining_solver->setText(none);
        m_mining_rate->setText(none);
    }
    if (!enabled) {
        m_mining_summary->setText(tr("Mining becomes available after consensus is enabled."));
    } else if (isolated) {
        // Not locked: the first node on a new chain has no peers either, and
        // doc/bootstrapping.md makes that a supported way to start.
        m_mining_summary->setText(tr("Mining is available, but with no peers anything mined would be on a chain of this computer's own."));
    } else {
        m_mining_summary->setText(tr("Mining can now be configured because consensus has been accepted."));
    }
    // The Node note already says there is no peer, and Mine / Mint carries its
    // own isolation banner; Home repeats it here only while blocks are being
    // mined onto that private chain.
    m_mining_summary->setVisible(isolated && m_have_mining_status && m_mining_active);
    SetTone(m_mining_summary, "warn");
    m_mining_button->setText(enabled ? tr("Open mining") : tr("Locked"));
    m_mining_button->setEnabled(enabled);
}

void DesktopLaunchPage::setPrivacy(bool privacy)
{
    m_privacy = privacy;
    const QuicksilverUnit unit = m_ledger_model && m_ledger_model->getOptionsModel() ? m_ledger_model->getOptionsModel()->getDisplayUnit() : QuicksilverUnit::HG;
    m_ledger_rows->setDisplay(unit, m_privacy);
    if (m_has_vault) renderVaultRows();
}

void DesktopLaunchPage::renderMaturing()
{
    // Every immature row counts: the soonest says when anything can be spent, the
    // latest when all of it can, and the rows at the soonest height say how much (F-432).
    std::vector<std::pair<int, CAmount>> maturing;
    const TransactionTableModel* rows = m_ledger_model ? m_ledger_model->getTransactionTableModel() : nullptr;
    if (rows) {
        for (int row = 0; row < rows->rowCount(QModelIndex()); ++row) {
            const QModelIndex index = rows->index(row, 0);
            if (index.data(TransactionTableModel::StatusRole).toInt() != TransactionStatus::Immature) continue;
            maturing.emplace_back(index.data(TransactionTableModel::MaturesInRole).toInt(),
                                  index.data(TransactionTableModel::AmountRole).toLongLong());
        }
    }
    const qsmaturity::MaturingSummary summary{qsmaturity::SummarizeMaturing(maturing)};
    const QuicksilverUnit unit = m_ledger_model && m_ledger_model->getOptionsModel() ? m_ledger_model->getOptionsModel()->getDisplayUnit() : QuicksilverUnit::HG;
    const QString hint = qsmaturity::FormatMaturingHint(summary, unit, QuicksilverUnits::SeparatorStyle::ALWAYS, m_privacy,
                                                        Params().GetConsensus().nPowTargetSpacing);
    m_vault_maturing->setText(hint.isEmpty() ? QString() : tr("Maturing: %1").arg(hint));
    m_vault_maturing->setVisible(!hint.isEmpty());
}

void DesktopLaunchPage::setAlerts(const QString& warnings)
{
    m_node_alerts->setText(warnings);
    m_node_alerts->setVisible(!warnings.isEmpty());
}

void DesktopLaunchPage::setConsensusStartupFailed(bool failed)
{
    if (!failed) return;

    m_consensus_failed = true;
    renderConsensusCards();
}
