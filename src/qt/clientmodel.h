// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_CLIENTMODEL_H
#define QUICKSILVER_QT_CLIENTMODEL_H

#include <QObject>
#include <QDateTime>

#include <memory>
#include <sync.h>
#include <uint256.h>
#include <util/time.h>

#include <netaddress.h>

class BanTableModel;
class OptionsModel;
class PeerTableModel;
class PeerTableSortProxy;
enum class SynchronizationState;
struct LocalServiceInfo;

namespace interfaces {
class Handler;
class Node;
struct BlockTip;
}

QT_BEGIN_NAMESPACE
QT_END_NAMESPACE

enum class SyncType {
    HEADER_PRESYNC,
    HEADER_SYNC,
    BLOCK_SYNC
};

enum NumConnections {
    CONNECTIONS_IN   = (1U << 0),
    CONNECTIONS_OUT  = (1U << 1),
    CONNECTIONS_ALL  = (CONNECTIONS_IN | CONNECTIONS_OUT),
};

/** Model for Quicksilver network client. */
class ClientModel : public QObject
{
    Q_OBJECT

public:
    enum class SyncState {
        Connecting,
        WaitingForPeers,
        CatchingUp,
        Synchronized,
    };

    explicit ClientModel(interfaces::Node& node, OptionsModel *optionsModel, QObject *parent = nullptr);
    ~ClientModel();

    static SyncState syncState(int blocks, int peers, int answered, bool behind_known_headers);
    static QString syncStateText(SyncState state);

    void stop();

    interfaces::Node& node() const { return m_node; }
    OptionsModel *getOptionsModel();
    PeerTableModel *getPeerTableModel();
    PeerTableSortProxy* peerTableSortProxy();
    BanTableModel *getBanTableModel();

    //! Return number of connections, default is in- and outbound (total)
    int getNumConnections(unsigned int flags = CONNECTIONS_ALL) const;
    std::map<CNetAddr, LocalServiceInfo> getNetLocalAddresses() const;
    uint256 getBestBlockHash() EXCLUSIVE_LOCKS_REQUIRED(!m_cached_tip_mutex);

    //! Return warnings to be displayed in status bar
    QString getStatusBarWarnings() const;

    QString formatFullVersion() const;
    QString formatSubVersion() const;
    QString formatClientStartupTime() const;
    QString dataDir() const;
    QString blocksDir() const;

    bool getProxyInfo(std::string& ip_port) const;

    // caches for the best header: hash, number of blocks and block time

private:
    Mutex m_tip_notification_mutex;
    SteadyClock::time_point m_last_block_notification GUARDED_BY(m_tip_notification_mutex){};
    SteadyClock::time_point m_last_header_notification GUARDED_BY(m_tip_notification_mutex){};
    Mutex m_cached_tip_mutex;
    uint256 m_cached_tip_blocks GUARDED_BY(m_cached_tip_mutex){};

    interfaces::Node& m_node;
    std::vector<std::unique_ptr<interfaces::Handler>> m_event_handlers;
    OptionsModel *optionsModel;
    PeerTableModel* peerTableModel{nullptr};
    PeerTableSortProxy* m_peer_table_sort_proxy{nullptr};
    BanTableModel* banTableModel{nullptr};

    //! A thread to interact with m_node asynchronously
    QThread* const m_thread;

    void TipChanged(SynchronizationState sync_state, interfaces::BlockTip tip, double verification_progress, SyncType synctype)
        EXCLUSIVE_LOCKS_REQUIRED(!m_cached_tip_mutex, !m_tip_notification_mutex);
    void subscribeToCoreSignals();
    void unsubscribeFromCoreSignals();

Q_SIGNALS:
    void numConnectionsChanged(int count);
    void headersAnsweredPeersChanged(int count);
    void numBlocksChanged(int count, const QDateTime& blockDate, double nVerificationProgress, SyncType header, SynchronizationState sync_state);
    void relaypoolSizeChanged(long count, size_t relaypoolSizeInBytes, size_t relaypoolMaxSizeInBytes);
    void networkActiveChanged(bool networkActive);
    void alertsChanged(const QString &warnings);
    void bytesChanged(quint64 totalBytesIn, quint64 totalBytesOut);

    // Show progress dialog e.g. for verifychain
    void showProgress(const QString &title, int nProgress);
};

#endif // QUICKSILVER_QT_CLIENTMODEL_H
