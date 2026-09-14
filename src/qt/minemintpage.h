// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_MINEMINTPAGE_H
#define QUICKSILVER_QT_MINEMINTPAGE_H

#include <QWidget>

#include <cstdint>

namespace interfaces { struct MiningStatus; }
class QLabel;
class QLineEdit;
class QPushButton;

class MineMintPage : public QWidget
{
    Q_OBJECT

public:
    explicit MineMintPage(QWidget* parent = nullptr);

    void setStatus(const interfaces::MiningStatus& status);
    //! Connection count from the client model, or -1 while it is not yet known.
    //!
    //! Mining with zero peers is not a degraded mode, it is a different activity:
    //! a node that cannot see the network's chain extends one of its own. The page
    //! has to know the count to be able to say so, because nothing in MiningStatus
    //! can -- the miner works exactly as well on a private fork as on the network.
    void setPeerCount(int peers);
    QString payoutAddress() const;
    void setPayoutAddress(const QString& address);
    void setAddressHelperAvailable(bool available);

Q_SIGNALS:
    void startRequested(const QString& address);
    void stopRequested();
    void newAddressRequested();
    //! The one health state the user can fix from this page: open the solver setting.
    void solverSettingsRequested();
    //! Raised from the isolation banner: the fix for no peers lives on the Network page.
    void networkHelpRequested();

private:
    void handleStartClicked();
    void refreshIsolationState();

    QWidget* m_isolation_panel{nullptr};
    QLabel* m_isolation_banner{nullptr};
    QLabel* m_status_value{nullptr};
    QLabel* m_payout_value{nullptr};
    QLabel* m_solver_value{nullptr};
    QLabel* m_solver_health_value{nullptr};
    QPushButton* m_configure_solver_button{nullptr};
    QLabel* m_minted_value{nullptr};
    QLabel* m_blocks_value{nullptr};
    QLabel* m_rate_value{nullptr};
    QLabel* m_congestion_value{nullptr};
    QLineEdit* m_payout_edit{nullptr};
    QPushButton* m_new_address_button{nullptr};
    QPushButton* m_start_button{nullptr};
    QPushButton* m_stop_button{nullptr};
    //! -1 until a client model reports one, so a page with no node behind it does
    //! not accuse the user of being offline.
    int m_peers{-1};
    int64_t m_blocks_found{0};
};

#endif // QUICKSILVER_QT_MINEMINTPAGE_H
