// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_AGENTALLOTMENTPAGE_H
#define QUICKSILVER_QT_AGENTALLOTMENTPAGE_H

#include <agent/peertransport.h>
#include <consensus/amount.h>
#include <qt/quicksilverunits.h>

#include <QWidget>

#include <chrono>
#include <functional>
#include <vector>

class QuicksilverAmountField;

QT_BEGIN_NAMESPACE
class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QVBoxLayout;
QT_END_NAMESPACE

class VaultModel;

class AgentAllotmentPage : public QWidget
{
    Q_OBJECT

public:
    using PeerRelayFunction = std::function<agent::PeerTransactionRelayResult(const CService&,
                                                                              const CSerializedNetMsg&,
                                                                              std::chrono::milliseconds)>;

    explicit AgentAllotmentPage(QWidget* parent = nullptr);
    AgentAllotmentPage(QWidget* parent, PeerRelayFunction peer_relay);

    void setModel(VaultModel* model);
    void setDisplayUnit(QuicksilverUnit unit);
    void refresh();

Q_SIGNALS:
    void fundAgentAllotmentSetupRequested(const QString& address, const QString& label, CAmount amount);

private:
    void recordAgentAllotmentSetup();
    void reviewAgentAllotmentPolicyRequest();
    void reviewPaymentReceipt();
    void savePaymentReceiptToAgentInbox();
    void copyAgentSpendSignCommand();
    void signAgentSpendLocally();
    void reviewSignedAgentSpend();
    void copySignedSpendRelayPayloads();
    void copyRelayPeerAddCommand();
    void copyRelayPeerDiscoveryCommand();
    void importNodePeersToAgentStore();
    void copyNodeAddressImportCommand();
    void copyHeaderPeerSyncCommand();
    void copySignedSpendPeerRelayCommand();
    void copySignedSpendStoredPeerRelayCommand();
    void relaySignedSpendToConfiguredPeers();
    void finishPeerRelay(std::vector<agent::PeerTransactionRelayResult> results, quint64 generation);
    void submitSignedAgentSpend();
    void updateRecordedSetups();
    void updateCreateState();
    void updatePolicyReviewState();
    void updatePaymentReceiptReviewState();
    bool refreshStoredAgentUtxos(bool requested = false);
    void updateAgentSpendCommandState();
    void updateSignedSpendReviewState();
    void updatePeerRelayCommandState();

    VaultModel* m_model{nullptr};
    QuicksilverUnit m_display_unit{QuicksilverUnit::HG};
    QLineEdit* m_name_edit{nullptr};
    QuicksilverAmountField* m_funding_limit{nullptr};
    QuicksilverAmountField* m_daily_limit{nullptr};
    QCheckBox* m_acceptance{nullptr};
    QLabel* m_records_label{nullptr};
    QVBoxLayout* m_records_list{nullptr};
    QLabel* m_state_label{nullptr};
    QPushButton* m_create_button{nullptr};
    QPlainTextEdit* m_policy_request_edit{nullptr};
    QLabel* m_policy_review_state{nullptr};
    QPushButton* m_policy_review_button{nullptr};
    QPlainTextEdit* m_payment_receipt_edit{nullptr};
    QLabel* m_payment_receipt_state{nullptr};
    QPushButton* m_payment_receipt_review_button{nullptr};
    QPushButton* m_payment_receipt_save_button{nullptr};
    QLabel* m_agent_utxo_state{nullptr};
    QPushButton* m_agent_utxo_refresh_button{nullptr};
    QPlainTextEdit* m_spend_bundle_edit{nullptr};
    QLineEdit* m_spend_destination_edit{nullptr};
    QuicksilverAmountField* m_spend_amount{nullptr};
    QuicksilverAmountField* m_spent_today{nullptr};
    QLabel* m_spend_command_state{nullptr};
    QPushButton* m_copy_spend_command_button{nullptr};
    QPushButton* m_sign_spend_button{nullptr};
    QPlainTextEdit* m_signed_spend_edit{nullptr};
    QLineEdit* m_relay_peer_edit{nullptr};
    QLabel* m_signed_spend_state{nullptr};
    QPushButton* m_signed_spend_review_button{nullptr};
    QPushButton* m_signed_spend_copy_relay_button{nullptr};
    QPushButton* m_copy_add_peer_command_button{nullptr};
    QPushButton* m_copy_discover_peers_command_button{nullptr};
    QPushButton* m_import_node_peers_button{nullptr};
    QPushButton* m_copy_node_address_import_command_button{nullptr};
    QPushButton* m_copy_sync_headers_command_button{nullptr};
    QPushButton* m_signed_spend_copy_peer_command_button{nullptr};
    QPushButton* m_signed_spend_copy_stored_peer_command_button{nullptr};
    QPushButton* m_signed_spend_relay_peer_button{nullptr};
    QPushButton* m_signed_spend_submit_button{nullptr};
    PeerRelayFunction m_peer_relay;
    bool m_signed_spend_review_valid{false};
    bool m_peer_relay_in_flight{false};
    quint64 m_peer_relay_generation{0};
};

#endif // QUICKSILVER_QT_AGENTALLOTMENTPAGE_H
