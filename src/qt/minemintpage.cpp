// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/minemintpage.h>

#include <interfaces/node.h>
#include <qt/benchpanel.h>
#include <qt/guiutil.h>
#include <qt/quicksilverunits.h>

#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVariant>
#include <QVBoxLayout>

namespace {
QLabel* MakeValueLabel(const QString& object_name)
{
    auto* label = new QLabel(QStringLiteral("Not connected"));
    label->setObjectName(object_name);
    label->setProperty("class", QStringLiteral("benchValue"));
    label->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}
} // namespace

MineMintPage::MineMintPage(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("mineMintPage"));
    setProperty("class", QStringLiteral("quicksilverPage"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(14);

    auto* intro = new QLabel(tr("Mine into a vault address to mint new coins. Mining is opt-in: it runs only after you start it here, or after the configuration file enables it."), this);
    intro->setObjectName(QStringLiteral("mineMintEmptyState"));
    intro->setProperty("class", QStringLiteral("benchNote"));
    intro->setWordWrap(true);


    // A miner with no peers is not a miner that is doing badly, it is a miner
    // building a chain nobody else will ever accept -- and every other reading on
    // this page looks identical either way: solver working, graphs climbing, blocks
    // found. Nothing here can distinguish the two, so the peer count has to be shown
    // where the mining is, not only on a page the user has no reason to open.
    m_isolation_panel = new QWidget(this);
    m_isolation_panel->setObjectName(QStringLiteral("mineMintIsolationPanel"));
    auto* isolation_layout = new QHBoxLayout(m_isolation_panel);
    isolation_layout->setContentsMargins(0, 0, 0, 0);
    isolation_layout->setSpacing(8);
    m_isolation_banner = new QLabel(
        tr("Not connected to any peer. Anything mined now extends a separate chain belonging to this computer alone; when this node reaches the network, the network's chain replaces it and those coins are gone."),
        m_isolation_panel);
    m_isolation_banner->setObjectName(QStringLiteral("mineMintIsolationBanner"));
    m_isolation_banner->setProperty("class", QStringLiteral("isolationBanner"));
    m_isolation_banner->setWordWrap(true);
    auto* isolation_button = new QPushButton(tr("Open Network"), m_isolation_panel);
    isolation_button->setObjectName(QStringLiteral("mineMintIsolationNetworkButton"));
    isolation_button->setProperty("class", QStringLiteral("benchQuiet"));
    isolation_layout->addWidget(m_isolation_banner, 1);
    isolation_layout->addWidget(isolation_button, 0, Qt::AlignTop);
    m_isolation_panel->setVisible(false);
    root->addWidget(m_isolation_panel);

    const BenchPanel::Parts payout = BenchPanel::Make(QStringLiteral("mineMintPayoutPanel"), tr("Payout address"), this);
    QFrame* payout_panel = payout.frame;
    payout.body->addWidget(intro);
    auto* payout_row = new QHBoxLayout;
    payout_row->setSpacing(8);
    m_payout_edit = new QLineEdit(payout_panel);
    m_payout_edit->setObjectName(QStringLiteral("payoutEdit"));
    m_payout_edit->setPlaceholderText(tr("Address to use when mining starts"));
    m_payout_edit->setToolTip(tr("Press Start to use this address for newly mined coins."));
    m_payout_edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    connect(m_payout_edit, &QLineEdit::textChanged, this, [this] { if (m_payout_value) renderPayout(); });
    m_new_address_button = new QPushButton(tr("Use new"), payout_panel);
    m_new_address_button->setObjectName(QStringLiteral("newAddressButton"));
    m_new_address_button->setProperty("class", QStringLiteral("benchQuiet"));
    payout_row->addWidget(m_payout_edit, 1);
    payout_row->addWidget(m_new_address_button);
    payout.body->addLayout(payout_row);
    root->addWidget(payout_panel);

    // The miner's state as label/value rows, with its commands under them.
    const BenchPanel::Parts state = BenchPanel::Make(QStringLiteral("mineMintStatePanel"), tr("Mining state"), this);
    QFrame* group = state.frame;
    auto* rows_host = new QWidget(group);
    QGridLayout* rows = BenchPanel::MakeRows(rows_host);
    const auto add_row = [rows, rows_host](const QString& key, QWidget* value) {
        const int row = rows->rowCount();
        auto* key_label = new QLabel(key, rows_host);
        key_label->setProperty("class", QStringLiteral("benchKey"));
        rows->addWidget(key_label, row, 0, Qt::AlignTop);
        rows->addWidget(value, row, 1);
    };
    m_status_value = MakeValueLabel(QStringLiteral("miningStatusValue"));
    m_payout_value = MakeValueLabel(QStringLiteral("payoutTargetValue"));
    m_reported_payout = statusText(interfaces::MiningStatus{}).payout;
    m_solver_value = MakeValueLabel(QStringLiteral("solverStatusValue"));
    m_minted_value = MakeValueLabel(QStringLiteral("sessionMintedValue"));
    m_blocks_value = MakeValueLabel(QStringLiteral("blocksFoundValue"));
    m_rate_value = MakeValueLabel(QStringLiteral("attemptsRateValue"));
    m_congestion_value = MakeValueLabel(QStringLiteral("congestionValue"));
    add_row(tr("Block mining"), m_status_value);
    add_row(tr("Mining payout"), m_payout_value);
    m_payout_value->setToolTip(tr("The payout reported by the miner. The address above is applied when you press Start."));
    add_row(tr("Solver"), m_solver_value);
    m_solver_health_value = new QLabel(group);
    m_solver_health_value->setObjectName(QStringLiteral("solverHealthValue"));
    m_solver_health_value->setProperty("class", QStringLiteral("benchNote"));
    m_solver_health_value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // Fault sentences are long -- the Windows device-fault one names a registry key,
    // a value and an event ID -- and an unwrapped label stretches the whole page.
    m_solver_health_value->setWordWrap(true);
    m_solver_health_value->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    // The halted state is fixable from Options: a solver path, or processor
    // block mining. One button opens that screen; the health sentence names both.
    m_configure_solver_button = new QPushButton(tr("Choose solver…"), group);
    m_configure_solver_button->setObjectName(QStringLiteral("configureSolverButton"));
    m_configure_solver_button->setProperty("class", QStringLiteral("benchQuiet"));
    m_configure_solver_button->setVisible(false);
    auto* health_row = new QWidget(group);
    auto* health_layout = new QHBoxLayout(health_row);
    health_layout->setContentsMargins(0, 0, 0, 0);
    health_layout->setSpacing(8);
    health_layout->addWidget(m_solver_health_value, 1);
    health_layout->addWidget(m_configure_solver_button, 0, Qt::AlignTop);

    add_row(tr("Session minted"), m_minted_value);
    add_row(tr("Blocks found"), m_blocks_value);
    add_row(tr("Recent attempts / sec"), m_rate_value);
    add_row(tr("Congestion"), m_congestion_value);
    state.body->addWidget(rows_host);
    state.body->addWidget(health_row);

    auto* controls = new QHBoxLayout();
    m_start_button = new QPushButton(tr("Start"), this);
    m_start_button->setObjectName(QStringLiteral("startMiningButton"));
    m_start_button->setProperty("class", QStringLiteral("primaryActionButton"));
    m_stop_button = new QPushButton(tr("Stop"), this);
    m_stop_button->setObjectName(QStringLiteral("stopMiningButton"));
    m_stop_button->setProperty("class", QStringLiteral("benchQuiet"));
    m_stop_button->setEnabled(false);
    controls->addWidget(m_start_button);
    controls->addWidget(m_stop_button);
    controls->addStretch();
    state.body->addLayout(controls);
    root->addWidget(group);
    root->addStretch();

    connect(m_start_button, &QPushButton::clicked, this, &MineMintPage::handleStartClicked);
    connect(m_stop_button, &QPushButton::clicked, this, &MineMintPage::stopRequested);
    connect(m_new_address_button, &QPushButton::clicked, this, &MineMintPage::newAddressRequested);
    connect(m_configure_solver_button, &QPushButton::clicked, this, &MineMintPage::solverSettingsRequested);
    connect(isolation_button, &QPushButton::clicked, this, &MineMintPage::networkHelpRequested);
}

void MineMintPage::handleStartClicked()
{
    // Not a refusal. doc/bootstrapping.md makes zero-peer mining the documented way
    // the first node on a new chain starts, and startmining is deliberately not gated
    // on peer count -- so the only defensible behaviour is to make the user assert it
    // rather than to block it or, as before, to do it silently.
    if (m_peers != 0) {
        Q_EMIT startRequested(payoutAddress());
        return;
    }
    auto* box = new QMessageBox{QMessageBox::Warning, tr("Mine with no peers?"),
                    tr("This computer is not connected to any Quicksilver peer."),
                    QMessageBox::NoButton, this};
    box->setObjectName(QStringLiteral("isolatedMiningWarning"));
    box->setInformativeText(tr(
        "Without a peer this node cannot see the network's chain, so it will mine onto a "
        "chain of its own. Coins mined that way appear in the ledger and then disappear "
        "as soon as this node connects and the network's chain replaces them.\n\n"
        "Connect to the network first, unless you are deliberately starting one."));
    QPushButton* cancel_button{box->addButton(QMessageBox::Cancel)};
    QPushButton* proceed_button{box->addButton(tr("Mine anyway"), QMessageBox::AcceptRole)};
    proceed_button->setObjectName(QStringLiteral("isolatedMiningProceedButton"));
    box->setDefaultButton(cancel_button);
    box->setEscapeButton(cancel_button);
    connect(box, &QMessageBox::finished, this, [this, box, proceed_button] {
        if (box->clickedButton() == proceed_button) Q_EMIT startRequested(payoutAddress());
    });
    GUIUtil::ShowModalDialogAsynchronously(box);
}

void MineMintPage::setPeerCount(int peers)
{
    m_peers = peers;
    refreshIsolationState();
}

void MineMintPage::refreshIsolationState()
{
    m_isolation_panel->setVisible(m_peers == 0);
    // Isolation is the whole meaning of the count while it is zero: a bare "1" here
    // is the sentence the walk read as success, and it is the one number on this page
    // that a private fork makes false.
    m_blocks_value->setText(m_peers == 0 && m_blocks_found > 0
        ? tr("%1 — on this computer's own chain, not the network's").arg(m_blocks_found)
        : QString::number(m_blocks_found));
}

MineMintPage::StatusText MineMintPage::statusText(const interfaces::MiningStatus& s)
{
    // Armed with nothing permitted to run is not "warming up" and not "CPU".
    // Idle with nothing permitted is not "CPU" either: the permit is a fact
    // about the configuration, and it is known before anyone presses Start.
    // That row stays Idle. Nothing was refused, because nothing was armed.
    const bool halted{s.active && !s.block_solving_possible};
    const bool idle_unconfigured{!s.active && !s.gpu_solver && !s.block_solving_possible};
    StatusText rows;
    rows.block_mining = !s.active ? tr("Idle") : (halted ? tr("Halted") : tr("Active"));
    if (!s.address.empty()) {
        rows.payout = QString::fromStdString(s.address);
    } else if (s.payout_script.empty()) {
        rows.payout = tr("Not set");
    } else if (s.payout_script.starts_with("6a")) {
        rows.payout = tr("Raw script (provably unspendable)");
    } else {
        rows.payout = tr("Raw script %1").arg(QString::fromStdString(s.payout_script));
    }
    if (s.gpu_solver) {
        rows.solver = tr("GPU bridge");
    } else if (halted || idle_unconfigured) {
        rows.solver = tr("None");
    } else {
        rows.solver = tr("CPU");
    }
    // No rate yet is not a rate of zero: printing 0.000 here is what made an
    // armed miner look identical to a dead one for the whole of its first graph.
    // Halted is the terminal case of that same trap: a graph is never attempted.
    // Idle reads as the Block mining row does, never as a rate of "None": an
    // idle miner, an armed one with no rate yet and a halted one are three
    // different words, and none of them is a number.
    const QString rate{!s.active ? tr("Idle")
        : s.attempts_per_second ? QString::number(*s.attempts_per_second, 'f', 3)
        : (halted ? tr("Not solving") : tr("Warming up"))};
    // Singular and plural as separate strings rather than the "%n graph(s)" idiom, for
    // the reason set out in qt/maturity.cpp: no app catalogue is installed, so %n is
    // never resolved and the literal "(s)" ships. A count of zero says nothing the
    // word before it has not.
    const qlonglong graphs{static_cast<qlonglong>(s.graphs_attempted)};
    rows.attempts = rate;
    if (graphs > 0) {
        const QString graph_count{graphs == 1 ? tr("1 graph attempted")
                                              : tr("%1 graphs attempted").arg(graphs)};
        rows.attempts += QStringLiteral(" (") + graph_count + QStringLiteral(")");
    }
    if (!s.active && !idle_unconfigured) {
        rows.health = tr("Not running");
    } else if (halted || idle_unconfigured) {
        // The same sentence the halted row already uses. It describes the
        // configuration, so it is true before arming as well as after a refusal.
        rows.health = tr("No GPU solver is configured, and processor block mining is off. Choose a solver, or allow processor block mining, in Settings > Options > Main.");
        rows.show_configure_solver = true;
    } else if (s.solver_ok) {
        // "armed but nothing finished yet" must not read the same as "armed
        // with a dead card". Halted was split out above; it is not this branch.
        rows.health = s.graphs_attempted == 0
            ? tr("Working — no graph finished yet")
            : tr("Working");
    } else if (s.solver_missing) {
        // The core message names -cuckatoosolver, which is right for quicksilver-daemon and
        // useless here: this window owns the setting. Sending a desktop user after a
        // command-line flag pointed them away from a control two clicks away.
        rows.health = tr("No GPU solver is configured. Choose one in Settings > Options > Main.");
        rows.show_configure_solver = true;
    } else {
        rows.health = QString::fromStdString(s.last_solver_error);
    }
    return rows;
}

MineMintPage::StatusText MineMintPage::statusTextForTesting(const interfaces::MiningStatus& status)
{
    return statusText(status);
}

void MineMintPage::setStatus(const interfaces::MiningStatus& s)
{
    const StatusText rows{statusText(s)};
    m_status_value->setText(rows.block_mining);
    m_active = s.active;
    m_reported_payout = rows.payout;
    renderPayout();
    m_solver_value->setText(rows.solver);
    m_minted_value->setText(QuicksilverUnits::formatWithUnit(
        QuicksilverUnits::Unit::HG, s.coins_minted_session));
    m_blocks_found = s.blocks_found;
    refreshIsolationState();
    m_rate_value->setText(rows.attempts);
    m_solver_health_value->setText(rows.health);
    m_configure_solver_button->setVisible(rows.show_configure_solver);
    m_congestion_value->setText(QString::number(s.congestion_multiplier, 'f', 2) + QStringLiteral("×"));
    m_stop_button->setEnabled(s.active);
    m_start_button->setEnabled(!s.active);
}

//! While idle, an entered address is the one Start will use, so the row names
//! it; otherwise the row names what the miner reports.
void MineMintPage::renderPayout()
{
    const QString draft{payoutAddress()};
    m_payout_value->setText(!m_active && !draft.isEmpty() ? tr("%1 (applied on Start)").arg(draft) : m_reported_payout);
}

QString MineMintPage::payoutAddress() const { return m_payout_edit->text().trimmed(); }
void MineMintPage::setPayoutAddress(const QString& a) { m_payout_edit->setText(a); }

void MineMintPage::setAddressHelperAvailable(bool available)
{
    m_new_address_button->setEnabled(available);
    m_new_address_button->setToolTip(available ? QString() : tr("Open a vault to create a fresh payout address."));
}
