// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/networkpage.h>

#include <chainparams.h>
#include <chainparamsbase.h>
#include <common/args.h>
#include <netaddress.h>
#include <netbase.h>
#include <protocol.h>
#include <qt/benchpanel.h>
#include <qt/guiutil.h>
#include <streams.h>
#include <tor/bundled_tor.h>
#include <util/chaintype.h>
#include <util/fs.h>

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSizePolicy>
#include <QString>
#include <QVariant>
#include <QVBoxLayout>
#include <QtGlobal>

#include <cassert>
#include <exception>

#include <QCoreApplication>

namespace {
QString Tr(const char* text)
{
    return QCoreApplication::translate("NetworkPage", text);
}

struct DeveloperNetworkContext {
    ChainType chain;
    const char* object_name;
    QString formal_name;
    QString purpose;
};

//! True when every fixed seed this network ships is an onion address, so nothing
//! can be dialled at all without a Tor proxy.
//!
//! Decoded from the seed bytes rather than assumed. Onion-only is a current fact
//! about Quicksilver, not a law: adding one clearnet seed must retire this warning
//! by itself, because a hardcoded answer would keep telling users to install Tor
//! long after they no longer need it, and nothing would fail to catch that.
bool FixedSeedsAreOnionOnly()
{
    const std::vector<uint8_t>& seeds = Params().FixedSeeds();
    if (seeds.empty()) return false;
    try {
        ParamsStream stream{DataStream{seeds}, CAddress::V2_NETWORK};
        while (!stream.eof()) {
            CService endpoint;
            stream >> endpoint;
            if (endpoint.GetNetwork() != NET_ONION) return false;
        }
    } catch (const std::exception&) {
        // Undecodable seeds are not evidence that Tor is the obstacle.
        return false;
    }
    return true;
}

//! True when a proxy that can carry onion addresses is configured (-onion, or -proxy
//! standing in for it). Read from the running netbase state rather than from the
//! argument list, so a proxy arriving by any route counts.
bool OnionProxyConfigured()
{
    Proxy proxy;
    return GetProxy(NET_ONION, proxy);
}

QString ChainToken(ChainType chain)
{
    return QString::fromStdString(ChainTypeToString(chain));
}

QString DeveloperDataDir(ChainType chain)
{
    fs::path data_dir = gArgs.GetDataDirBase();
    const std::string chain_data_dir = CreateBaseChainParams(chain)->DataDir();
    if (!chain_data_dir.empty()) data_dir /= fs::PathFromString(chain_data_dir);
    return GUIUtil::PathToQString(data_dir);
}

QString DeveloperNetworkLabel(ChainType chain)
{
    switch (chain) {
    case ChainType::MAIN:
        return Tr("Quicksilver Distributed Ledger System");
    case ChainType::PUBLIC_TEST:
        return Tr("Quicksilver Public Test Network");
    case ChainType::SANDBOX:
        return Tr("Quicksilver Sandbox");
    }
    assert(false);
}

QString DeveloperNetworkPurpose(ChainType chain)
{
    switch (chain) {
    case ChainType::MAIN:
        return Tr("Live vault balances and production verification.");
    case ChainType::PUBLIC_TEST:
        return Tr("Shared public rehearsal network for development.");
    case ChainType::SANDBOX:
        return Tr("Private local network for tests, tools, and demonstrations.");
    }
    assert(false);
}

QWidget* MakeNetworkContextCard(const DeveloperNetworkContext& context, ChainType active_chain, NetworkPage* owner, QWidget* parent)
{
    // One row of the Networks panel: the network, whether this window uses
    // it, and the command; its purpose, token and data directory under it.
    auto* card = new QWidget(parent);
    card->setObjectName(QString::fromLatin1(context.object_name));
    card->setProperty("class", QStringLiteral("networkContextRow"));

    auto* layout = new QGridLayout(card);
    layout->setContentsMargins(0, 6, 0, 6);
    layout->setHorizontalSpacing(12);
    layout->setVerticalSpacing(3);
    layout->setColumnStretch(0, 1);

    auto* name = new QLabel(context.formal_name, card);
    name->setObjectName(card->objectName() + QStringLiteral("Name"));
    name->setProperty("class", QStringLiteral("sectionValue"));
    layout->addWidget(name, 0, 0);

    // One state/action column: "Current" on this window's network, the
    // Restart command on the others, at the same right edge.
    const bool active = context.chain == active_chain;
    if (active) {
        auto* state = new QLabel(Tr("Current"), card);
        state->setObjectName(card->objectName() + QStringLiteral("State"));
        state->setProperty("class", QStringLiteral("benchValue"));
        state->setProperty("benchTone", QStringLiteral("good"));
        layout->addWidget(state, 0, 2, 3, 1, Qt::AlignRight | Qt::AlignVCenter);
    } else {
        auto* action = new QPushButton(Tr("Restart"), card);
        action->setObjectName(card->objectName() + QStringLiteral("Button"));
        action->setProperty("class", QStringLiteral("benchQuiet"));
        action->setToolTip(Tr("Restart Quicksilver with this network context."));
        QObject::connect(action, &QPushButton::clicked, owner, [owner, token = ChainToken(context.chain)] {
            Q_EMIT owner->restartRequested(token);
        });
        layout->addWidget(action, 0, 2, 3, 1, Qt::AlignRight | Qt::AlignVCenter);
    }

    auto* details = new QHBoxLayout;
    details->setSpacing(14);
    auto* purpose = new QLabel(context.purpose, card);
    purpose->setObjectName(card->objectName() + QStringLiteral("Purpose"));
    purpose->setProperty("class", QStringLiteral("benchNote"));
    details->addWidget(purpose);

    auto* token = new QLabel(Tr("Token: %1").arg(ChainToken(context.chain)), card);
    token->setObjectName(card->objectName() + QStringLiteral("Token"));
    token->setProperty("class", QStringLiteral("benchNote"));
    token->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details->addWidget(token);

    auto* datadir = new QLabel(Tr("Datadir: %1").arg(DeveloperDataDir(context.chain)), card);
    datadir->setObjectName(card->objectName() + QStringLiteral("Datadir"));
    datadir->setProperty("class", QStringLiteral("benchNote"));
    datadir->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // Paths are long; this line wraps rather than widen the page.
    datadir->setWordWrap(true);
    datadir->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    details->addStretch(1);
    layout->addLayout(details, 1, 0, 1, 2);
    layout->addWidget(datadir, 2, 0, 1, 2);

    return card;
}
} // namespace

NetworkPage::NetworkPage(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("networkPage"));
    setProperty("class", QStringLiteral("quicksilverPage"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(14);

    m_intro = new QLabel(this);
    m_intro->setObjectName(QStringLiteral("networkIntro"));
    m_intro->setProperty("class", QStringLiteral("benchNote"));
    m_intro->setWordWrap(true);

    buildDeveloperNetworkSection(root);

    const BenchPanel::Parts tor = BenchPanel::Make(QStringLiteral("torSetupPanel"), tr("Tor setup"), this);
    m_tor_setup_panel = tor.frame;
    QFrame* tor_group = m_tor_setup_panel;
    QVBoxLayout* tor_layout = tor.body;

    // Platform-conditional, and it has to be: /etc/tor/torrc, Debian packaging and Unix
    // service groups do not exist on Windows, so the Linux text was not merely unhelpful
    // there -- it was the only in-app route to the one transport that reaches the fixed
    // seed, described in terms of files the reader does not have.
    m_tor_setup_warning = new QLabel(
#ifdef Q_OS_WIN
        tr("Installing Tor does not finish setup. Tor leaves its control port disabled by default, and Quicksilver must be told which SOCKS5 proxy reaches onion peers."),
#else
        tr("Installing Tor does not finish setup. Stock Debian and Ubuntu packages normally leave the control port disabled, and Quicksilver must be told which SOCKS5 proxy reaches onion peers."),
#endif
        tor_group);
    m_tor_setup_warning->setObjectName(QStringLiteral("torSetupWarning"));
    m_tor_setup_warning->setProperty("class", QStringLiteral("benchNote"));
    m_tor_setup_warning->setWordWrap(true);
    tor_layout->addWidget(m_tor_setup_warning);

    m_tor_setup_steps = new QLabel(
#ifdef Q_OS_WIN
        tr("1. Install the Tor Expert Bundle and run tor.exe under the same Windows account as Quicksilver, so the authentication cookie is readable.\n"
           "2. In that Tor's torrc (the Expert Bundle keeps one at Data\\Tor\\torrc inside the folder you unpacked) enable ControlPort 9051 and CookieAuthentication 1; then restart tor.exe.\n"
           "3. In Settings > Options > Network enable the separate Tor SOCKS5 proxy at 127.0.0.1:9050 (the -onion setting), then restart Quicksilver. Tor Browser can be used instead, but its proxy is on 127.0.0.1:9150. See doc/tor.md for exact checks."),
#else
        tr("1. In /etc/tor/torrc enable ControlPort 9051, CookieAuthentication 1, and CookieAuthFileGroupReadable 1; then restart Tor.\n"
           "2. In Settings > Options > Network enable the separate Tor SOCKS5 proxy at 127.0.0.1:9050 (the -onion setting), then restart Quicksilver.\n"
           "3. If cookie authentication is denied, give the Quicksilver user read access through the Tor service group. See doc/tor.md for exact checks."),
#endif
        tor_group);
    m_tor_setup_steps->setObjectName(QStringLiteral("torSetupSteps"));
    m_tor_setup_steps->setProperty("class", QStringLiteral("benchNote"));
    m_tor_setup_steps->setWordWrap(true);
    m_tor_setup_steps->setTextInteractionFlags(Qt::TextSelectableByMouse);
    tor_layout->addWidget(m_tor_setup_steps);
    // Not "Quicksilver's Tor": the proxy may equally be one the operator configured with
    // -onion or -proxy, and claiming ownership of someone else's Tor would be a new lie
    // in place of the one this removes.
    m_tor_setup_working = new QLabel(
        tr("A Tor proxy is configured and carrying onion addresses. No manual setup is needed. On the desktop this is normally the Tor that Quicksilver starts and supervises itself."),
        tor_group);
    m_tor_setup_working->setObjectName(QStringLiteral("torSetupWorking"));
    m_tor_setup_working->setProperty("class", QStringLiteral("benchNote"));
    m_tor_setup_working->setWordWrap(true);
    tor_layout->addWidget(m_tor_setup_working);
    root->addWidget(tor_group);
    applyTorSetupAdvice(OnionProxyConfigured());

    const BenchPanel::Parts connectivity = BenchPanel::Make(QStringLiteral("networkConnectivityPanel"), tr("Node connectivity"), this);
    QFrame* group = connectivity.frame;
    QVBoxLayout* group_layout = connectivity.body;
    // Whether independent verification is running is this panel's subject:
    // the sentence heads it rather than floating above the page.
    m_intro->setParent(group);
    group_layout->addWidget(m_intro);
    auto* rows_host = new QWidget(group);
    QGridLayout* rows = BenchPanel::MakeRows(rows_host);
    m_status_value = BenchPanel::AddRow(rows, tr("Status"), QStringLiteral("networkStatusValue"), rows_host);
    m_peers_value = BenchPanel::AddRow(rows, tr("Peers"), QStringLiteral("peerCountValue"), rows_host);
    m_sync_value = BenchPanel::AddRow(rows, tr("Sync"), QStringLiteral("syncStatusValue"), rows_host);
    group_layout->addWidget(rows_host);

    // "Peers: 0" is a reading, not an explanation, and the two states behind it are
    // not alike: a node that started ten seconds ago has no peers either. This says
    // which one it is, and it is only shown when the count is zero.
    m_bootstrap_diagnosis = new QLabel(group);
    m_bootstrap_diagnosis->setObjectName(QStringLiteral("bootstrapDiagnosis"));
    m_bootstrap_diagnosis->setProperty("class", QStringLiteral("isolationBanner"));
    m_bootstrap_diagnosis->setWordWrap(true);
    m_bootstrap_diagnosis->setVisible(false);
    group_layout->addWidget(m_bootstrap_diagnosis);
    root->addWidget(group);

    buildAddPeerSection(root);
    root->addStretch();
    showInitializing();
}

void NetworkPage::buildAddPeerSection(QVBoxLayout* root)
{
    const BenchPanel::Parts panel = BenchPanel::Make(QStringLiteral("addPeerPanel"), tr("Connect to a peer"), this);
    QFrame* group = panel.frame;
    QVBoxLayout* layout = panel.body;

    // doc/bootstrapping.md has documented -addnode as the fallback since the seed
    // design was written, and it was reachable only by editing a config file or
    // typing an RPC. That is not a fallback a desktop user has.
    auto* copy = new QLabel(tr("If you were given a peer address, name it here. It is retried until it answers, and once connected this node learns other peers from it. This is the same as the -addnode setting."), group);
    copy->setObjectName(QStringLiteral("addPeerCopy"));
    copy->setProperty("class", QStringLiteral("benchNote"));
    copy->setWordWrap(true);
    layout->addWidget(copy);

    auto* row = new QHBoxLayout();
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    m_add_peer_edit = new QLineEdit(group);
    m_add_peer_edit->setObjectName(QStringLiteral("addPeerEdit"));
    m_add_peer_edit->setPlaceholderText(tr("host:%1, or an address ending in .onion").arg(static_cast<int>(Params().GetDefaultPort())));
    m_add_peer_button = new QPushButton(tr("Add peer"), group);
    m_add_peer_button->setObjectName(QStringLiteral("addPeerButton"));
    m_add_peer_button->setProperty("class", QStringLiteral("benchQuiet"));
    row->addWidget(m_add_peer_edit, 1);
    row->addWidget(m_add_peer_button);
    layout->addLayout(row);

    m_add_peer_result = new QLabel(group);
    m_add_peer_result->setObjectName(QStringLiteral("addPeerResult"));
    m_add_peer_result->setProperty("class", QStringLiteral("benchNote"));
    m_add_peer_result->setWordWrap(true);
    layout->addWidget(m_add_peer_result);

    auto submit = [this] {
        const QString address = m_add_peer_edit->text().trimmed();
        // Rejected here rather than passed on: an empty or whitespace-bearing entry
        // is accepted by the connection manager and then simply never resolves, which
        // is indistinguishable from a peer that is merely down.
        if (address.isEmpty() || address.contains(QChar::Space)) {
            setAddPeerResult(tr("Enter a peer address, with no spaces."));
            return;
        }
        Q_EMIT addPeerRequested(address);
    };
    connect(m_add_peer_button, &QPushButton::clicked, this, submit);
    connect(m_add_peer_edit, &QLineEdit::returnPressed, this, submit);

    root->addWidget(group);
}

void NetworkPage::setAddPeerResult(const QString& message)
{
    m_add_peer_result->setText(message);
}

void NetworkPage::setNodeControlsEnabled(bool enabled)
{
    m_add_peer_edit->setEnabled(enabled);
    m_add_peer_button->setEnabled(enabled);
}

NetworkPage::BootstrapObstacle NetworkPage::diagnoseBootstrap(bool has_fixed_seeds, bool onion_only_seeds, bool onion_proxy_configured)
{
    if (!has_fixed_seeds) return BootstrapObstacle::NoSeeds;
    if (onion_only_seeds && !onion_proxy_configured) return BootstrapObstacle::OnionSeedsNeedTor;
    return BootstrapObstacle::None;
}

void NetworkPage::refreshBootstrapDiagnosis(int peers)
{
    if (peers > 0) {
        m_bootstrap_diagnosis->setVisible(false);
        return;
    }
    switch (diagnoseBootstrap(!Params().FixedSeeds().empty(), FixedSeedsAreOnionOnly(), OnionProxyConfigured())) {
    case BootstrapObstacle::NoSeeds:
        m_bootstrap_diagnosis->setText(tr("This network ships no seed addresses, so this node has nothing to look for on its own. Name a peer below to connect to one."));
        break;
    case BootstrapObstacle::OnionSeedsNeedTor:
        m_bootstrap_diagnosis->setText(tr("Every seed address this network ships is reachable only through Tor, and no Tor proxy is configured — so this node has nothing it can dial. Set up Tor above, or name a peer below. Until one of those happens it will stay at zero peers, and anything it mines will be on a chain of its own."));
        break;
    case BootstrapObstacle::None:
        m_bootstrap_diagnosis->setText(tr("No peers yet. This node is still trying the seed addresses it ships with."));
        break;
    }
    m_bootstrap_diagnosis->setVisible(true);
}

void NetworkPage::applyTorSetupAdvice(bool onion_proxy_configured)
{
    m_tor_setup_warning->setVisible(!onion_proxy_configured);
    m_tor_setup_steps->setVisible(!onion_proxy_configured);
    m_tor_setup_working->setVisible(onion_proxy_configured);
}

void NetworkPage::buildDeveloperNetworkSection(QVBoxLayout* root)
{
    const ChainType active_chain = Params().GetChainType();

    m_developer_banner = new QLabel(this);
    m_developer_banner->setObjectName(QStringLiteral("developerNetworkBanner"));
    m_developer_banner->setProperty("class", QStringLiteral("developerNetworkBanner"));
    m_developer_banner->setWordWrap(true);
    m_developer_banner->setVisible(active_chain != ChainType::MAIN);
    m_developer_banner->setText(tr("Developer network active: this window is not using the live ledger. The live vault is unaffected by this context."));

    const BenchPanel::Parts panel = BenchPanel::Make(QStringLiteral("developerNetworkContext"), tr("Networks"), this);
    QFrame* group = panel.frame;
    QVBoxLayout* layout = panel.body;
    // The banner says which network this window uses: it heads the Networks panel.
    m_developer_banner->setParent(group);
    layout->addWidget(m_developer_banner);

    auto* restart_copy = new QLabel(tr("Network selection binds chain parameters, ports, and datadir at startup. Switch by restarting the application with the desired -chain token."), group);
    restart_copy->setObjectName(QStringLiteral("developerNetworkRestartCopy"));
    restart_copy->setProperty("class", QStringLiteral("benchNote"));
    restart_copy->setWordWrap(true);
    layout->addWidget(restart_copy);

    const DeveloperNetworkContext contexts[] = {
        {ChainType::MAIN, "developerNetworkMainCard", DeveloperNetworkLabel(ChainType::MAIN), DeveloperNetworkPurpose(ChainType::MAIN)},
        {ChainType::PUBLIC_TEST, "developerNetworkPublicTestCard", DeveloperNetworkLabel(ChainType::PUBLIC_TEST), DeveloperNetworkPurpose(ChainType::PUBLIC_TEST)},
        {ChainType::SANDBOX, "developerNetworkSandboxCard", DeveloperNetworkLabel(ChainType::SANDBOX), DeveloperNetworkPurpose(ChainType::SANDBOX)},
    };

    for (const auto& context : contexts) {
        auto* rule = new QFrame(group);
        rule->setObjectName(QStringLiteral("benchRule"));
        rule->setFixedHeight(1);
        layout->addWidget(rule);
        layout->addWidget(MakeNetworkContextCard(context, active_chain, this, group));
    }

    root->addWidget(group);
}

void NetworkPage::showInitializing()
{
    m_intro->setText(tr("Consensus is starting. Peer and sync data will appear when independent verification is ready."));
    m_status_value->setText(tr("Starting consensus"));
    m_peers_value->setText(tr("Waiting"));
    m_sync_value->setText(tr("Waiting for consensus"));
    m_bootstrap_diagnosis->setVisible(false);
    setNodeControlsEnabled(false);
}

void NetworkPage::showStartFailed()
{
    m_intro->setText(tr("Consensus did not start. The saved opt-in was cleared; restart after fixing node settings to try again."));
    m_status_value->setText(tr("Startup failed"));
    m_peers_value->setText(tr("Unavailable"));
    m_sync_value->setText(tr("Not running"));
    m_bootstrap_diagnosis->setVisible(false);
    setNodeControlsEnabled(false);
}

void NetworkPage::showTorMissing()
{
    // The node's own sentence names the remedies; this adds which of them take
    // effect without a restart (the settings and PATH are read at launch).
    m_intro->setText(tr("Consensus did not start. %1 A tor placed beside the Quicksilver executable is found when "
                        "you enable consensus again; a change to PATH or quicksilver.conf needs a restart of "
                        "Quicksilver first. Your vault is unaffected.")
                         .arg(QString::fromStdString(tor::MissingTorMessage().translated)));
    m_status_value->setText(tr("Startup failed"));
    m_peers_value->setText(tr("Unavailable"));
    m_sync_value->setText(tr("Not running"));
    m_bootstrap_diagnosis->setVisible(false);
    setNodeControlsEnabled(false);
}

void NetworkPage::updateStatus(int peers, double verification_progress, bool synced)
{
    m_intro->setText(tr("Independent verification is running on this desktop."));
    m_status_value->setText(peers > 0 ? tr("Online") : tr("Offline"));
    m_peers_value->setText(QString::number(peers));
    m_sync_value->setText(synced
        ? tr("Synced")
        : tr("Syncing %1%").arg(QString::number(verification_progress * 100.0, 'f', 1)));
    // Read every poll, not once at construction: the bundled Tor took 7 s to bootstrap on
    // the L1-6 walk box, so the proxy does not exist yet when this page is built.
    applyTorSetupAdvice(OnionProxyConfigured());
    refreshBootstrapDiagnosis(peers);
    setNodeControlsEnabled(true);
}
