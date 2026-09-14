// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_NETWORKPAGE_H
#define QUICKSILVER_QT_NETWORKPAGE_H

#include <QWidget>

class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

class NetworkPage : public QWidget
{
    Q_OBJECT

public:
    explicit NetworkPage(QWidget* parent = nullptr);

    //! Why a node with zero peers has nothing to dial. Zero peers is not by itself
    //! a fault -- a node that has just started has none either -- so the page has to
    //! separate "not yet" from "there is no route at all", which is the state a
    //! Windows first run lands in and which nothing previously reported.
    enum class BootstrapObstacle {
        None,              //!< something is dialable; zero peers is still "not yet"
        NoSeeds,           //!< this network ships no seed addresses (sandbox)
        OnionSeedsNeedTor, //!< every seed is an onion and no Tor proxy is configured
    };

    //! Pure, and public for the tests: the running binary can only ever exercise one
    //! chain's combination, and the case that matters is the one this box is not.
    static BootstrapObstacle diagnoseBootstrap(bool has_fixed_seeds, bool onion_only_seeds, bool onion_proxy_configured);

    void showInitializing();
    void showStartFailed();
    void updateStatus(int peers, double verification_progress, bool synced);
    //! Outcome of the last manual peer addition, reported by the container that owns
    //! the node handle.
    void setAddPeerResult(const QString& message);
    //! Show the manual Tor recipe only when there is no onion proxy to use. The fact is
    //! a parameter rather than a read of netbase because SetProxy() refuses an invalid
    //! Proxy: a test that installed one could never remove it, and would silently change
    //! the answer for every case that ran after it in the same binary.
    void applyTorSetupAdvice(bool onion_proxy_configured);

Q_SIGNALS:
    void restartRequested(const QString& chain_token);
    //! A peer address typed by the user, for the container to hand to the node. The
    //! page holds no node handle, so it cannot add the peer itself.
    void addPeerRequested(const QString& address);

private:
    void buildDeveloperNetworkSection(QVBoxLayout* root);
    void buildAddPeerSection(QVBoxLayout* root);
    void setNodeControlsEnabled(bool enabled);
    void refreshBootstrapDiagnosis(int peers);

    QLabel* m_intro{nullptr};
    QLabel* m_developer_banner{nullptr};
    QLabel* m_current_network_value{nullptr};
    QLabel* m_status_value{nullptr};
    QLabel* m_peers_value{nullptr};
    QLabel* m_sync_value{nullptr};
    QLabel* m_bootstrap_diagnosis{nullptr};
    QGroupBox* m_tor_setup_panel{nullptr};
    QLabel* m_tor_setup_warning{nullptr};
    QLabel* m_tor_setup_steps{nullptr};
    QLabel* m_tor_setup_working{nullptr};
    QLineEdit* m_add_peer_edit{nullptr};
    QPushButton* m_add_peer_button{nullptr};
    QLabel* m_add_peer_result{nullptr};
};

#endif // QUICKSILVER_QT_NETWORKPAGE_H
