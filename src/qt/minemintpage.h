// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_MINEMINTPAGE_H
#define QUICKSILVER_QT_MINEMINTPAGE_H

#include <qt/quicksilverunits.h>

#include <QWidget>

#include <cstdint>

namespace interfaces { struct MiningStatus; }
class DeveloperOptionsToggle;
class QLabel;
class QValidatedLineEdit;
class QLineEdit;
class QPushButton;

class MineMintPage : public QWidget
{
    Q_OBJECT

public:
    explicit MineMintPage(QWidget* parent = nullptr);

    struct StatusText {
        QString block_mining;
        QString payout;
        QString solver;
        QString attempts;
        QString health;
        bool show_configure_solver{false};
    };
    //! Pure formatting for the Mine/Mint status readings.
    //! Pure: no widget. setStatus applies this and nothing else decides those rows.
    static StatusText statusText(const interfaces::MiningStatus& status);
    static StatusText statusTextForTesting(const interfaces::MiningStatus& status);

    void setStatus(const interfaces::MiningStatus& status);
    void setDisplayUnit(QuicksilverUnit unit);
    //! Connection count from the client model, or -1 while it is not yet known.
    //!
    //! Mining with zero peers is not a degraded mode, it is a different activity:
    //! a node that cannot see the network's chain extends one of its own. The page
    //! has to know the count to be able to say so, because nothing in MiningStatus
    //! can -- the miner works exactly as well on a private fork as on the network.
    void setPeerCount(int peers);
    QString payoutAddress() const;
    void setPayoutAddress(const QString& address);
    //! The draft belongs to the named vault. Restoring it never writes settings.
    void setPayoutVault(const QString& vault_name);
    void setSavedPayoutAddress(const QString& address);
    //! The conf's -minescript, hex; empty when unset. Offered by the script's Use saved.
    void setSavedPayoutScript(const QString& script_hex);
    void setAddressHelperAvailable(bool available);
    //! The custom mining script Start will use, lowercase hex; empty unless the
    //! script field holds a valid script.
    QString payoutScript() const;

Q_SIGNALS:
    void startRequested(const QString& address);
    //! Start on the custom mining script (F-556): it wins over the address field.
    void startScriptRequested(const QString& script_hex);
    void stopRequested();
    void newAddressRequested();
    //! The one health state the user can fix from this page: open the solver setting.
    void solverSettingsRequested();
    //! Raised from the isolation banner: the fix for no peers lives on the Network page.
    void networkHelpRequested();

private:
    void handleStartClicked();
    void emitStart();
    //! Puts an address-in-disguise script in the payout field (G1 5h, option A).
    void convertAddressScript();
    void refreshDeveloperOptions();
    //! G1 5g or 5i for a valid script, lowercase hex.
    QString scriptLabel(const QString& script_hex) const;
    void refreshIsolationState();
    void renderMinted();
    void renderPayout();
    void refreshStartAvailability();

    QWidget* m_isolation_panel{nullptr};
    QLabel* m_isolation_banner{nullptr};
    QLabel* m_status_value{nullptr};
    bool m_active{false};
    QString m_reported_payout;
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
    QPushButton* m_saved_address_button{nullptr};
    //! Fills the field with the miner's retained address. Only a press fills it.
    QPushButton* m_last_address_button{nullptr};
    QString m_saved_payout_address;
    //! The retained payout address while mining is stopped; empty while mining.
    QString m_last_address;
    //! The miner retains a target (address or script) that Start will not reuse.
    bool m_has_retained_target{false};
    bool m_address_helper_available{false};
    DeveloperOptionsToggle* m_developer_toggle{nullptr};
    QWidget* m_developer_section{nullptr};
    QValidatedLineEdit* m_script_edit{nullptr};
    QPushButton* m_saved_script_button{nullptr};
    QLabel* m_script_note{nullptr};
    QLabel* m_script_warning{nullptr};
    QLabel* m_script_wins_note{nullptr};
    QString m_saved_payout_script;
    //! 5h's text after a conversion; kept until the next edit of either field.
    QString m_converted_note;
    //! The miner is running on a raw script, so the section must stay open.
    bool m_active_on_script{false};
    QString m_payout_setting_key;
    QPushButton* m_start_button{nullptr};
    QLabel* m_start_reason{nullptr};
    QPushButton* m_stop_button{nullptr};
    //! -1 until a client model reports one, so a page with no node behind it does
    //! not accuse the user of being offline.
    int m_peers{-1};
    int64_t m_blocks_found{0};
    int64_t m_coins_minted_session{0};
    QuicksilverUnit m_display_unit{QuicksilverUnit::HGS};
};

#endif // QUICKSILVER_QT_MINEMINTPAGE_H
