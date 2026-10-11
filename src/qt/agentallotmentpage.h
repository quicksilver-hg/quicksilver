// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_AGENTALLOTMENTPAGE_H
#define QUICKSILVER_QT_AGENTALLOTMENTPAGE_H

#include <consensus/amount.h>
#include <qt/quicksilverunits.h>

#include <QWidget>

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
namespace vault { struct AgentAllotmentRecord; }

class AgentAllotmentPage : public QWidget
{
    Q_OBJECT

public:
    explicit AgentAllotmentPage(QWidget* parent = nullptr);

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
    void copyAgentBundle(const vault::AgentAllotmentRecord& record, CAmount funding_available);
    void reviewAgentSpendRequest();
    void cosignAgentSpendRequest();
    void updateCosignState();
    void updateRecordedSetups();
    void updateCreateState();
    void updatePolicyReviewState();
    void updatePaymentReceiptReviewState();
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
    QPlainTextEdit* m_cosign_edit{nullptr};
    QLabel* m_cosign_state{nullptr};
    QPushButton* m_cosign_review_button{nullptr};
    QPushButton* m_cosign_button{nullptr};
    QPushButton* m_cosign_refuse_button{nullptr};
    QString m_reviewed_request;
    //! Advanced by every review, edit, refusal and model change, so an unlock
    //! started for one review cannot co-sign after that review is gone.
    quint64 m_cosign_generation{0};
};

#endif // QUICKSILVER_QT_AGENTALLOTMENTPAGE_H
