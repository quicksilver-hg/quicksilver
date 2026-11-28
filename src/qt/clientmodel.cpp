// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/clientmodel.h>

#include <qt/bantablemodel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/peertablemodel.h>
#include <qt/peertablesortproxy.h>

#include <clientversion.h>
#include <common/args.h>
#include <common/system.h>
#include <interfaces/handler.h>
#include <interfaces/node.h>
#include <net.h>
#include <netbase.h>
#include <util/threadnames.h>
#include <util/time.h>
#include <validation.h>

#include <stdint.h>

#include <QCoreApplication>
#include <QDebug>
#include <QMetaObject>
#include <QThread>
#include <QTimer>

ClientModel::ClientModel(interfaces::Node& node, OptionsModel *_optionsModel, QObject *parent) :
    QObject(parent),
    m_node(node),
    optionsModel(_optionsModel),
    m_thread(new QThread(this))
{
    peerTableModel = new PeerTableModel(m_node, this);
    m_peer_table_sort_proxy = new PeerTableSortProxy(this);
    m_peer_table_sort_proxy->setSourceModel(peerTableModel);

    banTableModel = new BanTableModel(m_node, this);

    QTimer* timer = new QTimer;
    timer->setInterval(MODEL_UPDATE_DELAY);
    connect(timer, &QTimer::timeout, [this] {
        // no locking required at this point
        // the following calls will acquire the required lock
        Q_EMIT relaypoolSizeChanged(m_node.getRelayPoolSize(), m_node.getRelayPoolDynamicUsage(), m_node.getRelayPoolMaxUsage());
        Q_EMIT bytesChanged(m_node.getTotalBytesRecv(), m_node.getTotalBytesSent());
    });
    connect(m_thread, &QThread::finished, timer, &QObject::deleteLater);
    connect(m_thread, &QThread::started, [timer] { timer->start(); });
    // move timer to thread so that polling doesn't disturb main event loop
    timer->moveToThread(m_thread);
    m_thread->start();
    QTimer::singleShot(0, timer, []() {
        util::ThreadRename("qt-clientmodl");
    });

    subscribeToCoreSignals();
}

void ClientModel::stop()
{
    unsubscribeFromCoreSignals();

    m_thread->quit();
    m_thread->wait();
}

ClientModel::~ClientModel()
{
    stop();
}

int ClientModel::getNumConnections(unsigned int flags) const
{
    ConnectionDirection connections = ConnectionDirection::None;

    if(flags == CONNECTIONS_IN)
        connections = ConnectionDirection::In;
    else if (flags == CONNECTIONS_OUT)
        connections = ConnectionDirection::Out;
    else if (flags == CONNECTIONS_ALL)
        connections = ConnectionDirection::Both;

    return m_node.getNodeCount(connections);
}


std::map<CNetAddr, LocalServiceInfo> ClientModel::getNetLocalAddresses() const
{
    return m_node.getNetLocalAddresses();
}


uint256 ClientModel::getBestBlockHash()
{
    uint256 tip{WITH_LOCK(m_cached_tip_mutex, return m_cached_tip_blocks)};

    if (!tip.IsNull()) {
        return tip;
    }

    // Lock order must be: first `cs_main`, then `m_cached_tip_mutex`.
    // The following will lock `cs_main` (and release it), so we must not
    // own `m_cached_tip_mutex` here.
    tip = m_node.getBestBlockHash();

    LOCK(m_cached_tip_mutex);
    // We checked that `m_cached_tip_blocks` is not null above, but then we
    // released the mutex `m_cached_tip_mutex`, so it could have changed in the
    // meantime. Thus, check again.
    if (m_cached_tip_blocks.IsNull()) {
        m_cached_tip_blocks = tip;
    }
    return m_cached_tip_blocks;
}

QString ClientModel::getStatusBarWarnings() const
{
    return QString::fromStdString(m_node.getWarnings().translated);
}

OptionsModel *ClientModel::getOptionsModel()
{
    return optionsModel;
}

PeerTableModel *ClientModel::getPeerTableModel()
{
    return peerTableModel;
}

PeerTableSortProxy* ClientModel::peerTableSortProxy()
{
    return m_peer_table_sort_proxy;
}

BanTableModel *ClientModel::getBanTableModel()
{
    return banTableModel;
}

QString ClientModel::formatFullVersion() const
{
    return QString::fromStdString(FormatFullVersion());
}

QString ClientModel::formatSubVersion() const
{
    return QString::fromStdString(strSubVersion);
}

QString ClientModel::formatClientStartupTime() const
{
    return QDateTime::fromSecsSinceEpoch(GetStartupTime()).toString();
}

QString ClientModel::dataDir() const
{
    return GUIUtil::PathToQString(gArgs.GetDataDirNet());
}

QString ClientModel::blocksDir() const
{
    return GUIUtil::PathToQString(gArgs.GetBlocksDirPath());
}

void ClientModel::TipChanged(SynchronizationState sync_state, interfaces::BlockTip tip, double verification_progress, SyncType synctype)
{
    if (synctype == SyncType::BLOCK_SYNC) {
        WITH_LOCK(m_cached_tip_mutex, m_cached_tip_blocks = tip.block_hash;);
    }

    // Throttle GUI notifications about (a) blocks during initial sync, and (b) both blocks and headers during reindex.
    const bool throttle = (sync_state != SynchronizationState::POST_INIT && synctype == SyncType::BLOCK_SYNC) || sync_state == SynchronizationState::INIT_REINDEX;
    const auto now{throttle ? SteadyClock::now() : SteadyClock::time_point{}};
    {
        LOCK(m_tip_notification_mutex);
        auto& last_notification = synctype == SyncType::BLOCK_SYNC ? m_last_block_notification : m_last_header_notification;
        if (throttle && now < last_notification + MODEL_UPDATE_DELAY) return;
        last_notification = now;
    }

    Q_EMIT numBlocksChanged(tip.block_height, QDateTime::fromSecsSinceEpoch(tip.block_time), verification_progress, synctype, sync_state);
}

ClientModel::SyncState ClientModel::syncState(int blocks, int peers, int answered, bool behind_known_headers)
{
    if (blocks < 0) return SyncState::Connecting;
    // A node that has heard from no peer cannot know it is in sync.
    if (peers == 0) return SyncState::WaitingForPeers;
    if (peers > 0 && answered == 0) return SyncState::Connecting;
    return behind_known_headers ? SyncState::CatchingUp : SyncState::Synchronized;
}

QString ClientModel::syncStateText(SyncState state)
{
    switch (state) {
    case SyncState::Connecting:
        return QCoreApplication::translate("QuicksilverGUI", "Connecting");
    case SyncState::WaitingForPeers:
        return QCoreApplication::translate("QuicksilverGUI", "Waiting for peers");
    case SyncState::CatchingUp:
        return QCoreApplication::translate("QuicksilverGUI", "Catching up");
    case SyncState::Synchronized:
        return QCoreApplication::translate("QuicksilverGUI", "Synchronized");
    }
    assert(false);
    return {};
}

void ClientModel::subscribeToCoreSignals()
{
    m_event_handlers.emplace_back(m_node.handleShowProgress(
        [this](const std::string& title, int progress, [[maybe_unused]] bool resume_possible) {
            Q_EMIT showProgress(QString::fromStdString(title), progress);
        }));
    m_event_handlers.emplace_back(m_node.handleNotifyNumConnectionsChanged(
        [this](int new_num_connections) {
            Q_EMIT numConnectionsChanged(new_num_connections);
        }));
    m_event_handlers.emplace_back(m_node.handleNotifyHeadersAnsweredPeersChanged(
        [this](int count) {
            Q_EMIT headersAnsweredPeersChanged(count);
        }));
    m_event_handlers.emplace_back(m_node.handleNotifyNetworkActiveChanged(
        [this](bool network_active) {
            Q_EMIT networkActiveChanged(network_active);
        }));
    m_event_handlers.emplace_back(m_node.handleNotifyAlertChanged(
        [this]() {
            qDebug() << "ClientModel: NotifyAlertChanged";
            Q_EMIT alertsChanged(getStatusBarWarnings());
        }));
    m_event_handlers.emplace_back(m_node.handleBannedListChanged(
        [this]() {
            qDebug() << "ClientModel: Requesting update for peer banlist";
            // Queued even on the GUI thread: refresh() reads the list through
            // getBanned, which can send this notification again while the table
            // is in the middle of its layout change (S0008-A-01).
            QMetaObject::invokeMethod(banTableModel, [this] { banTableModel->refresh(); }, Qt::QueuedConnection);
        }));
    m_event_handlers.emplace_back(m_node.handleNotifyBlockTip(
        [this](SynchronizationState sync_state, interfaces::BlockTip tip, double verification_progress) {
            TipChanged(sync_state, tip, verification_progress, SyncType::BLOCK_SYNC);
        }));
    m_event_handlers.emplace_back(m_node.handleNotifyHeaderTip(
        [this](SynchronizationState sync_state, interfaces::BlockTip tip, bool presync) {
            TipChanged(sync_state, tip, /*verification_progress=*/0.0, presync ? SyncType::HEADER_PRESYNC : SyncType::HEADER_SYNC);
        }));
}

void ClientModel::unsubscribeFromCoreSignals()
{
    // This disconnects future node notifications. The timer thread is joined
    // by stop(); Signals2 has no equivalent join for a slot already in flight.
    m_event_handlers.clear();
}

bool ClientModel::getProxyInfo(std::string& ip_port) const
{
    Proxy ipv4, ipv6;
    if (m_node.getProxy(NET_IPV4, ipv4) && m_node.getProxy(NET_IPV6, ipv6)) {
      ip_port = ipv4.proxy.ToStringAddrPort();
      return true;
    }
    return false;
}
