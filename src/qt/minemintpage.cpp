// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/minemintpage.h>

#include <interfaces/node.h>
#include <qt/guiutil.h>
#include <qt/quicksilverunits.h>

#include <QFormLayout>
#include <QGroupBox>
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
    label->setProperty("class", QStringLiteral("sectionValue"));
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
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(14);

    auto* title = new QLabel(tr("Mine / Mint"), this);
    title->setObjectName(QStringLiteral("mineMintTitle"));
    title->setProperty("class", QStringLiteral("pageTitle"));
    root->addWidget(title);

    auto* intro = new QLabel(tr("Mine into a vault address to mint new coins. Mining is opt-in and off by default."), this);
    intro->setObjectName(QStringLiteral("mineMintEmptyState"));
    intro->setProperty("class", QStringLiteral("muted"));
    intro->setWordWrap(true);
    root->addWidget(intro);

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
    isolation_button->setProperty("class", QStringLiteral("secondaryActionButton"));
    isolation_layout->addWidget(m_isolation_banner, 1);
    isolation_layout->addWidget(isolation_button, 0, Qt::AlignTop);
    m_isolation_panel->setVisible(false);
    root->addWidget(m_isolation_panel);

    auto* payout_panel = new QFrame(this);
    payout_panel->setObjectName(QStringLiteral("mineMintPayoutPanel"));
    auto* payout_row = new QHBoxLayout(payout_panel);
    payout_row->setContentsMargins(14, 12, 14, 12);
    payout_row->setSpacing(10);
    auto* payout_label = new QLabel(tr("Payout address"), payout_panel);
    payout_label->setProperty("class", QStringLiteral("hudHeading"));
    m_payout_edit = new QLineEdit(payout_panel);
    m_payout_edit->setObjectName(QStringLiteral("payoutEdit"));
    m_payout_edit->setPlaceholderText(tr("Address that receives mined coins"));
    m_new_address_button = new QPushButton(tr("Use new"), payout_panel);
    m_new_address_button->setObjectName(QStringLiteral("newAddressButton"));
    payout_row->addWidget(payout_label);
    payout_row->addWidget(m_payout_edit, 1);
    payout_row->addWidget(m_new_address_button);
    root->addWidget(payout_panel);

    auto* group = new QGroupBox(tr("Bootstrap mining state"), this);
    auto* form = new QFormLayout(group);
    m_status_value = MakeValueLabel(QStringLiteral("miningStatusValue"));
    m_payout_value = MakeValueLabel(QStringLiteral("payoutTargetValue"));
    m_solver_value = MakeValueLabel(QStringLiteral("solverStatusValue"));
    m_minted_value = MakeValueLabel(QStringLiteral("sessionMintedValue"));
    m_blocks_value = MakeValueLabel(QStringLiteral("blocksFoundValue"));
    m_rate_value = MakeValueLabel(QStringLiteral("attemptsRateValue"));
    m_congestion_value = MakeValueLabel(QStringLiteral("congestionValue"));
    form->addRow(tr("Block mining"), m_status_value);
    form->addRow(tr("Payout target"), m_payout_value);
    form->addRow(tr("Solver"), m_solver_value);
    m_solver_health_value = MakeValueLabel(QStringLiteral("solverHealthValue"));
    // Fault sentences are long -- the Windows device-fault one names a registry key,
    // a value and an event ID -- and an unwrapped label stretches the whole page.
    m_solver_health_value->setWordWrap(true);
    // Of every state this row can report, exactly one is fixable from inside the app,
    // and the setting for it is two clicks away in Options. Put the control here
    // rather than a sentence describing where the control lives.
    m_configure_solver_button = new QPushButton(tr("Choose solver…"), group);
    m_configure_solver_button->setObjectName(QStringLiteral("configureSolverButton"));
    m_configure_solver_button->setProperty("class", QStringLiteral("secondaryActionButton"));
    m_configure_solver_button->setVisible(false);
    auto* health_row = new QWidget(group);
    auto* health_layout = new QHBoxLayout(health_row);
    health_layout->setContentsMargins(0, 0, 0, 0);
    health_layout->setSpacing(8);
    health_layout->addWidget(m_solver_health_value, 1);
    health_layout->addWidget(m_configure_solver_button, 0, Qt::AlignTop);
    form->addRow(tr("Solver health"), health_row);
    form->addRow(tr("Session minted"), m_minted_value);
    form->addRow(tr("Blocks found"), m_blocks_value);
    form->addRow(tr("Recent attempts / sec"), m_rate_value);
    form->addRow(tr("Congestion"), m_congestion_value);
    root->addWidget(group);

    auto* controls = new QHBoxLayout();
    m_start_button = new QPushButton(tr("Start"), this);
    m_start_button->setObjectName(QStringLiteral("startMiningButton"));
    m_start_button->setProperty("class", QStringLiteral("primaryActionButton"));
    m_stop_button = new QPushButton(tr("Stop"), this);
    m_stop_button->setObjectName(QStringLiteral("stopMiningButton"));
    m_stop_button->setProperty("class", QStringLiteral("secondaryActionButton"));
    m_stop_button->setEnabled(false);
    controls->addWidget(m_start_button);
    controls->addWidget(m_stop_button);
    controls->addStretch();
    root->addLayout(controls);
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

void MineMintPage::setStatus(const interfaces::MiningStatus& s)
{
    m_status_value->setText(s.active ? tr("Active") : tr("Idle"));
    m_payout_value->setText(s.address.empty() ? tr("None") : QString::fromStdString(s.address));
    m_solver_value->setText(s.gpu_solver ? tr("GPU bridge") : tr("CPU"));
    m_minted_value->setText(QuicksilverUnits::formatWithUnit(
        QuicksilverUnits::Unit::HG, s.coins_minted_session));
    m_blocks_found = s.blocks_found;
    refreshIsolationState();
    // No rate yet is not a rate of zero: printing 0.000 here is what made an
    // armed miner look identical to a dead one for the whole of its first graph.
    const QString rate = s.attempts_per_second
        ? QString::number(*s.attempts_per_second, 'f', 3)
        : (s.active ? tr("Warming up") : tr("None"));
    // Singular and plural as separate strings rather than the "%n graph(s)" idiom, for
    // the reason set out in qt/maturity.cpp: no app catalogue is installed, so %n is
    // never resolved and the literal "(s)" ships.
    const qlonglong graphs = static_cast<qlonglong>(s.graphs_attempted);
    const QString graph_count = graphs == 1 ? tr("1 graph attempted")
                                            : tr("%1 graphs attempted").arg(graphs);
    m_rate_value->setText(rate + QStringLiteral(" (") + graph_count + QStringLiteral(")"));
    // The three-way branch is the whole point: "armed but nothing finished yet"
    // must not read the same as "armed with a dead card".
    if (!s.active) {
        m_solver_health_value->setText(tr("Not running"));
    } else if (s.solver_ok) {
        m_solver_health_value->setText(s.graphs_attempted == 0
            ? tr("Working — no graph finished yet")
            : tr("Working"));
    } else if (s.solver_missing) {
        // The core message names -cuckatoosolver, which is right for quicksilverd and
        // useless here: this window owns the setting. Sending a desktop user after a
        // command-line flag pointed them away from a control two clicks away.
        m_solver_health_value->setText(
            tr("No GPU solver is configured. Choose one in Controls > Options > Main."));
    } else {
        m_solver_health_value->setText(QString::fromStdString(s.last_solver_error));
    }
    m_configure_solver_button->setVisible(s.active && !s.solver_ok && s.solver_missing);
    m_congestion_value->setText(QString::number(s.congestion_multiplier, 'f', 2) + QStringLiteral("×"));
    m_stop_button->setEnabled(s.active);
    m_start_button->setEnabled(!s.active);
}

QString MineMintPage::payoutAddress() const { return m_payout_edit->text().trimmed(); }
void MineMintPage::setPayoutAddress(const QString& a) { m_payout_edit->setText(a); }

void MineMintPage::setAddressHelperAvailable(bool available)
{
    m_new_address_button->setEnabled(available);
    m_new_address_button->setToolTip(available ? QString() : tr("Open a vault to create a fresh payout address."));
}
