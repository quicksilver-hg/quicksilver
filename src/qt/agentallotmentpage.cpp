// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/agentallotmentpage.h>

#include <qt/benchpanel.h>
#include <qt/guiutil.h>
#include <qt/quicksilveramountfield.h>
#include <qt/quicksilverunits.h>
#include <qt/vaultmodel.h>

#include <agent/allotmentpolicy.h>
#include <chainparams.h>
#include <common/args.h>
#include <consensus/params.h>
#include <core_io.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <node/context.h>
#include <psqt.h>
#include <script/solver.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/readwritefile.h>
#include <util/result.h>
#include <util/strencodings.h>
#include <univalue.h>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QStringList>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <string>

namespace {
QLabel* MakeMutedLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setProperty("class", QStringLiteral("muted"));
    label->setWordWrap(true);
    return label;
}

void SetLabelClass(QLabel* label, const QString& class_name)
{
    if (!label) return;
    label->setProperty("class", class_name);
    label->style()->unpolish(label);
    label->style()->polish(label);
}

bool AmountFieldEmpty(const QuicksilverAmountField* field)
{
    const QLineEdit* line_edit = field->findChild<QLineEdit*>();
    return !line_edit || line_edit->text().trimmed().isEmpty();
}

QString ShellQuote(QString value)
{
    value.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QStringLiteral("'%1'").arg(value);
}

fs::path AgentPaymentReceiptInboxDirectory()
{
    const fs::path configured_path{gArgs.GetPathArg("-paymentreceiptdir", fs::path{"agent"} / "payment-receipts.d")};
    return fsbridge::AbsPathJoin(gArgs.GetDataDirNet(), configured_path);
}

QString AgentPaymentReceiptScanCommand(const fs::path& receipt_dir)
{
    return QStringLiteral("quicksilver-agent -chain=%1 -paymentreceiptdir=%2 scanreceipts")
        .arg(QString::fromStdString(Params().GetChainTypeString()),
             ShellQuote(QString::fromStdString(fs::PathToString(receipt_dir))));
}

fs::path AgentPaymentReceiptInboxPath(const agent::AllotmentPaymentReceiptArtifact& receipt)
{
    // util::ToString, not std::to_string: this names a file on disk, so a locale that
    // formatted the index differently would write a receipt the next run cannot find.
    const std::string filename{receipt.funding_output.txid + "-" + util::ToString(receipt.funding_output.vout) + ".json"};
    return AgentPaymentReceiptInboxDirectory() /
           fs::PathFromString(filename);
}

util::Result<fs::path> SaveAgentPaymentReceiptToInbox(const agent::AllotmentPaymentReceiptArtifact& receipt, const QString& receipt_json)
{
    const fs::path receipt_dir{AgentPaymentReceiptInboxDirectory()};
    if (!fs::is_directory(receipt_dir) && !TryCreateDirectories(receipt_dir)) {
        return util::Error{Untranslated(strprintf("Could not create agent receipt inbox %s.", fs::PathToString(receipt_dir)))};
    }

    const fs::path receipt_path{AgentPaymentReceiptInboxPath(receipt)};
    if (!WriteBinaryFile(receipt_path, receipt_json.toStdString() + "\n")) {
        return util::Error{Untranslated(strprintf("Could not write payment receipt to %s.", fs::PathToString(receipt_path)))};
    }
    return receipt_path;
}

agent::AllotmentPaymentReceiptArtifact AgentPaymentReceiptFromFundingOutput(const vault::AgentAllotmentPolicyBundle& bundle,
                                                                              const vault::AgentAllotmentFundingOutput& output,
                                                                              int64_t received_time)
{
    return agent::AllotmentPaymentReceiptArtifact{
        .chain = Params().GetChainTypeString(),
        .genesis_hash = Params().GenesisBlock().GetHash().ToString(),
        .funding_address = bundle.metadata.funding_address,
        .funding_output = {
            .txid = output.txid,
            .vout = output.vout,
            .amount = output.amount,
        },
        .received_time = received_time,
        .payment_id = bundle.metadata.id + ":" + output.txid + ":" + util::ToString(output.vout),
        .label = bundle.metadata.label,
        .memo = "agent setup funding",
        .payer = "desktop vault",
    };
}

QString AgentPaymentReceiptJson(const agent::AllotmentPaymentReceiptArtifact& receipt)
{
    UniValue json{UniValue::VOBJ};
    json.pushKV("type", "quicksilver.agent_payment_receipt");
    json.pushKV("version", 1);
    json.pushKV("chain", receipt.chain);
    json.pushKV("genesis_hash", receipt.genesis_hash);
    json.pushKV("funding_address", receipt.funding_address);
    json.pushKV("txid", receipt.funding_output.txid);
    json.pushKV("vout", static_cast<uint64_t>(receipt.funding_output.vout));
    json.pushKV("amount_cinnabar", util::ToString(receipt.funding_output.amount));
    json.pushKV("received_time", util::ToString(receipt.received_time));
    if (!receipt.payment_id.empty()) json.pushKV("payment_id", receipt.payment_id);
    if (!receipt.label.empty()) json.pushKV("label", receipt.label);
    if (!receipt.memo.empty()) json.pushKV("memo", receipt.memo);
    if (!receipt.payer.empty()) json.pushKV("payer", receipt.payer);
    return QString::fromStdString(json.write());
}

QString ScantxoutsetRecoveryCommand(const QString& address)
{
    const QString descriptor = QStringLiteral("addr(%1)").arg(address);
    const QString scan_objects = QStringLiteral("[\"%1\"]").arg(descriptor);
    return QStringLiteral("quicksilver-cli -chain=%1 scantxoutset start %2 | quicksilver-agent -chain=%1 -fundingaddress=%3 -scantxoutset=- importrecovery")
        .arg(QString::fromStdString(Params().GetChainTypeString()),
             ShellQuote(scan_objects),
             ShellQuote(address));
}

//! The base64 PSQT from a pasted spend request: the value of its psqt= line, or
//! the whole paste when there is no such line.
QString PastedSpendRequest(const QString& pasted)
{
    for (const QString& line : pasted.split(QLatin1Char('\n'))) {
        if (line.trimmed().startsWith(QStringLiteral("psqt="))) return line.trimmed().mid(5).trimmed();
    }
    return pasted.trimmed();
}

void ClearLayout(QLayout* layout)
{
    while (QLayoutItem* item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
}

} // namespace

AgentAllotmentPage::AgentAllotmentPage(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("agentAllotmentPage"));
    setProperty("class", QStringLiteral("quicksilverPage"));

    auto* page_layout = new QVBoxLayout(this);
    page_layout->setContentsMargins(0, 0, 0, 0);

    // VaultView is a stacked widget, so even a hidden page contributes its minimum
    // size. Keep this long workflow behind a viewport instead of forcing every vault
    // page -- including Home -- to be as tall as all of these panels combined.
    auto* scroll_area = new QScrollArea(this);
    scroll_area->setObjectName(QStringLiteral("agentAllotmentScrollArea"));
    scroll_area->setFrameShape(QFrame::NoFrame);
    scroll_area->setWidgetResizable(true);
    page_layout->addWidget(scroll_area);

    auto* contents = new QWidget(scroll_area);
    contents->setObjectName(QStringLiteral("agentAllotmentScrollContents"));
    auto* root = new QVBoxLayout(contents);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(14);

    auto* intro = MakeMutedLabel(
        tr("Create an allotment, fund its address, and review every spend request from the agent."),
        contents);
    intro->setObjectName(QStringLiteral("agentAllotmentIntro"));

    auto* risk_panel = new QFrame(contents);
    risk_panel->setObjectName(QStringLiteral("agentAllotmentRiskPanel"));
    QVBoxLayout* risk_layout = BenchPanel::Install(risk_panel, tr("How an agent allotment works")).body;
    risk_layout->setSpacing(8);
    // The page's one-line purpose leads into the risks it names.
    intro->setParent(risk_panel);
    risk_layout->addWidget(intro);

    auto* dishonest_agent = MakeMutedLabel(tr("The agent holds one key and this vault holds the other. The agent cannot spend without this vault's signature."), risk_panel);
    dishonest_agent->setObjectName(QStringLiteral("agentAllotmentDishonestAgentRisk"));
    risk_layout->addWidget(dishonest_agent);

    auto* compromised_host = MakeMutedLabel(tr("Stop an allotment and this vault refuses every later request. A request the vault has already signed and broadcast still confirms."), risk_panel);
    compromised_host->setObjectName(QStringLiteral("agentAllotmentCompromisedHostRisk"));
    risk_layout->addWidget(compromised_host);

    auto* guarantee = MakeMutedLabel(tr("Spending limits are the agent's own check. This vault does not enforce them, and no limit is a guarantee."), risk_panel);
    guarantee->setObjectName(QStringLiteral("agentAllotmentGuaranteeRisk"));
    risk_layout->addWidget(guarantee);

    root->addWidget(risk_panel);

    // Agent funding: the setup fields, the risk acceptance and the one
    // primary command of the page, in one panel.
    const BenchPanel::Parts setup = BenchPanel::Make(QStringLiteral("agentAllotmentFundingGroup"), tr("Agent funding"), contents);
    QFrame* setup_group = setup.frame;
    QVBoxLayout* setup_layout = setup.body;
    setup_layout->setSpacing(9);

    auto add_row = [setup_group, setup_layout](const QString& label_text, QWidget* field) {
        auto* row = new QHBoxLayout();
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(10);

        auto* label = new QLabel(label_text, setup_group);
        label->setProperty("class", QStringLiteral("benchKey"));
        label->setMinimumWidth(120);
        label->setBuddy(field);
        row->addWidget(label);
        row->addWidget(field, 1);
        setup_layout->addLayout(row);
    };

    m_name_edit = new QLineEdit(setup_group);
    m_name_edit->setObjectName(QStringLiteral("agentAllotmentNameEdit"));
    m_name_edit->setPlaceholderText(tr("Agent label"));
    add_row(tr("Name"), m_name_edit);

    m_funding_limit = new QuicksilverAmountField(setup_group);
    m_funding_limit->setObjectName(QStringLiteral("agentAllotmentFundingLimit"));
    m_funding_limit->SetMinValue(0);
    m_funding_limit->SetAllowEmpty(true);
    add_row(tr("Funding amount"), m_funding_limit);

    m_daily_limit = new QuicksilverAmountField(setup_group);
    m_daily_limit->setObjectName(QStringLiteral("agentAllotmentDailyLimit"));
    m_daily_limit->SetMinValue(0);
    m_daily_limit->SetAllowEmpty(true);
    add_row(tr("Daily guardrail"), m_daily_limit);

    auto* acceptance_panel = new QFrame(setup_group);
    acceptance_panel->setObjectName(QStringLiteral("agentAllotmentAcceptancePanel"));
    acceptance_panel->setProperty("benchBody", true);
    auto* acceptance_layout = new QVBoxLayout(acceptance_panel);
    acceptance_layout->setContentsMargins(0, 6, 0, 0);
    acceptance_layout->setSpacing(10);

    m_acceptance = new QCheckBox(tr("I understand that every spend by this agent needs this vault to co-sign it,\nand that I must back up this vault again after creating it."), acceptance_panel);
    m_acceptance->setObjectName(QStringLiteral("agentAllotmentAcceptanceCheck"));
    acceptance_layout->addWidget(m_acceptance);

    auto* action_row = new QHBoxLayout();
    action_row->setContentsMargins(0, 0, 0, 0);
    action_row->setSpacing(10);

    m_create_button = new QPushButton(tr("Create agent allotment"), acceptance_panel);
    m_create_button->setObjectName(QStringLiteral("agentAllotmentCreateButton"));
    m_create_button->setProperty("class", QStringLiteral("primaryActionButton"));
    m_create_button->setToolTip(tr("Creates an allotment with separate agent and vault keys and reserves its funding address."));
    action_row->addWidget(m_create_button);

    m_state_label = new QLabel(acceptance_panel);
    m_state_label->setObjectName(QStringLiteral("agentAllotmentBackendState"));
    m_state_label->setProperty("class", QStringLiteral("muted"));
    m_state_label->setWordWrap(true);
    action_row->addWidget(m_state_label, 1);
    acceptance_layout->addLayout(action_row);
    setup_layout->addWidget(acceptance_panel);
    root->addWidget(setup_group);

    auto* records_panel = new QFrame(contents);
    records_panel->setObjectName(QStringLiteral("agentAllotmentRecordsPanel"));
    QVBoxLayout* records_layout = BenchPanel::Install(records_panel, tr("Agent allotments")).body;
    records_layout->setSpacing(8);

    m_records_label = MakeMutedLabel(QString(), records_panel);
    m_records_label->setObjectName(QStringLiteral("agentAllotmentRecordedSetups"));
    records_layout->addWidget(m_records_label);

    m_records_list = new QVBoxLayout();
    m_records_list->setContentsMargins(0, 0, 0, 0);
    m_records_list->setSpacing(8);
    records_layout->addLayout(m_records_list);
    root->addWidget(records_panel);

    auto* review_panel = new QFrame(contents);
    review_panel->setObjectName(QStringLiteral("agentAllotmentPolicyReviewPanel"));
    QVBoxLayout* review_layout = BenchPanel::Install(review_panel, tr("Policy request review")).body;
    review_layout->setSpacing(8);

    m_policy_request_edit = new QPlainTextEdit(review_panel);
    m_policy_request_edit->setObjectName(QStringLiteral("agentAllotmentPolicyRequestEdit"));
    m_policy_request_edit->setPlaceholderText(tr("Paste policy request JSON"));
    m_policy_request_edit->setMinimumHeight(84);
    m_policy_request_edit->setTabChangesFocus(true);
    review_layout->addWidget(m_policy_request_edit);

    auto* review_action_row = new QHBoxLayout();
    review_action_row->setContentsMargins(0, 0, 0, 0);
    review_action_row->setSpacing(10);

    m_policy_review_button = new QPushButton(tr("Review request"), review_panel);
    m_policy_review_button->setObjectName(QStringLiteral("agentAllotmentReviewPolicyButton"));
    m_policy_review_button->setProperty("class", QStringLiteral("benchQuiet"));
    m_policy_review_button->setToolTip(tr("Checks policy request JSON against this vault's active chain."));
    review_action_row->addWidget(m_policy_review_button);

    m_policy_review_state = new QLabel(review_panel);
    m_policy_review_state->setObjectName(QStringLiteral("agentAllotmentPolicyReviewState"));
    m_policy_review_state->setProperty("class", QStringLiteral("muted"));
    m_policy_review_state->setWordWrap(true);
    review_action_row->addWidget(m_policy_review_state, 1);
    review_layout->addLayout(review_action_row);
    root->addWidget(review_panel);

    auto* receipt_panel = new QFrame(contents);
    receipt_panel->setObjectName(QStringLiteral("agentAllotmentPaymentReceiptPanel"));
    QVBoxLayout* receipt_layout = BenchPanel::Install(receipt_panel, tr("Payment receipt review")).body;
    receipt_layout->setSpacing(8);

    m_payment_receipt_edit = new QPlainTextEdit(receipt_panel);
    m_payment_receipt_edit->setObjectName(QStringLiteral("agentAllotmentPaymentReceiptEdit"));
    m_payment_receipt_edit->setPlaceholderText(tr("Paste payment receipt JSON"));
    m_payment_receipt_edit->setMinimumHeight(84);
    m_payment_receipt_edit->setTabChangesFocus(true);
    receipt_layout->addWidget(m_payment_receipt_edit);

    auto* receipt_action_row = new QHBoxLayout();
    receipt_action_row->setContentsMargins(0, 0, 0, 0);
    receipt_action_row->setSpacing(10);

    m_payment_receipt_review_button = new QPushButton(tr("Review receipt"), receipt_panel);
    m_payment_receipt_review_button->setObjectName(QStringLiteral("agentAllotmentReviewPaymentReceiptButton"));
    m_payment_receipt_review_button->setProperty("class", QStringLiteral("benchQuiet"));
    m_payment_receipt_review_button->setToolTip(tr("Checks a pasted agent payment receipt against this desktop's active chain and shows its funding metadata."));
    receipt_action_row->addWidget(m_payment_receipt_review_button);

    m_payment_receipt_save_button = new QPushButton(tr("Save to agent inbox"), receipt_panel);
    m_payment_receipt_save_button->setObjectName(QStringLiteral("agentAllotmentSavePaymentReceiptButton"));
    m_payment_receipt_save_button->setProperty("class", QStringLiteral("benchQuiet"));
    m_payment_receipt_save_button->setToolTip(tr("Writes a valid payment receipt into the local quicksilver-agent scanreceipts inbox and copies the scan command."));
    receipt_action_row->addWidget(m_payment_receipt_save_button);

    m_payment_receipt_state = new QLabel(receipt_panel);
    m_payment_receipt_state->setObjectName(QStringLiteral("agentAllotmentPaymentReceiptState"));
    m_payment_receipt_state->setProperty("class", QStringLiteral("muted"));
    m_payment_receipt_state->setWordWrap(true);
    receipt_action_row->addWidget(m_payment_receipt_state, 1);
    receipt_layout->addLayout(receipt_action_row);
    root->addWidget(receipt_panel);

    auto* cosign_panel = new QFrame(contents);
    cosign_panel->setObjectName(QStringLiteral("agentAllotmentCosignPanel"));
    QVBoxLayout* cosign_layout = BenchPanel::Install(cosign_panel, tr("Agent spend request")).body;
    cosign_layout->setSpacing(8);
    m_cosign_edit = new QPlainTextEdit(cosign_panel);
    m_cosign_edit->setObjectName(QStringLiteral("agentAllotmentCosignEdit"));
    m_cosign_edit->setPlaceholderText(tr("Paste the agent's spend request (psqt=…)"));
    m_cosign_edit->setMinimumHeight(84);
    m_cosign_edit->setTabChangesFocus(true);
    cosign_layout->addWidget(m_cosign_edit);
    auto* cosign_actions = new QHBoxLayout();
    cosign_actions->setContentsMargins(0, 0, 0, 0);
    cosign_actions->setSpacing(10);
    m_cosign_review_button = new QPushButton(tr("Review"), cosign_panel);
    m_cosign_review_button->setObjectName(QStringLiteral("agentAllotmentReviewCosignButton"));
    m_cosign_review_button->setProperty("class", QStringLiteral("benchQuiet"));
    cosign_actions->addWidget(m_cosign_review_button);
    m_cosign_button = new QPushButton(tr("Co-sign and broadcast"), cosign_panel);
    // DU's unchanged viewport guard locates the final action by this object name.
    m_cosign_button->setObjectName(QStringLiteral("agentAllotmentSubmitSignedSpendButton"));
    m_cosign_button->setProperty("class", QStringLiteral("benchQuiet"));
    cosign_actions->addWidget(m_cosign_button);
    m_cosign_refuse_button = new QPushButton(tr("Refuse"), cosign_panel);
    m_cosign_refuse_button->setObjectName(QStringLiteral("agentAllotmentRefuseButton"));
    m_cosign_refuse_button->setProperty("class", QStringLiteral("benchQuiet"));
    cosign_actions->addWidget(m_cosign_refuse_button);
    cosign_actions->addStretch(1);
    cosign_layout->addLayout(cosign_actions);
    m_cosign_state = MakeMutedLabel(QString(), cosign_panel);
    m_cosign_state->setObjectName(QStringLiteral("agentAllotmentCosignState"));
    m_cosign_state->setTextFormat(Qt::PlainText);
    m_cosign_state->setText(tr("Paste a spend request and review it before co-signing."));
    cosign_layout->addWidget(m_cosign_state);
    root->addWidget(cosign_panel);
    root->addStretch();
    scroll_area->setWidget(contents);

    connect(m_name_edit, &QLineEdit::textChanged, this, &AgentAllotmentPage::updateCreateState);
    connect(m_acceptance, &QCheckBox::toggled, this, &AgentAllotmentPage::updateCreateState);
    connect(m_funding_limit, &QuicksilverAmountField::valueChanged, this, &AgentAllotmentPage::updateCreateState);
    connect(m_daily_limit, &QuicksilverAmountField::valueChanged, this, &AgentAllotmentPage::updateCreateState);
    connect(m_create_button, &QPushButton::clicked, this, &AgentAllotmentPage::recordAgentAllotmentSetup);
    connect(m_policy_request_edit, &QPlainTextEdit::textChanged, this, &AgentAllotmentPage::updatePolicyReviewState);
    connect(m_policy_review_button, &QPushButton::clicked, this, &AgentAllotmentPage::reviewAgentAllotmentPolicyRequest);
    connect(m_payment_receipt_edit, &QPlainTextEdit::textChanged, this, &AgentAllotmentPage::updatePaymentReceiptReviewState);
    connect(m_payment_receipt_review_button, &QPushButton::clicked, this, &AgentAllotmentPage::reviewPaymentReceipt);
    connect(m_payment_receipt_save_button, &QPushButton::clicked, this, &AgentAllotmentPage::savePaymentReceiptToAgentInbox);
    connect(m_cosign_edit, &QPlainTextEdit::textChanged, this, [this] {
        m_reviewed_request.clear();
        ++m_cosign_generation;
        SetLabelClass(m_cosign_state, QStringLiteral("policyReviewReady"));
        m_cosign_state->setText(tr("Paste a spend request and review it before co-signing."));
        updateCosignState();
    });
    connect(m_cosign_review_button, &QPushButton::clicked, this, &AgentAllotmentPage::reviewAgentSpendRequest);
    connect(m_cosign_button, &QPushButton::clicked, this, &AgentAllotmentPage::cosignAgentSpendRequest);
    connect(m_cosign_refuse_button, &QPushButton::clicked, this, [this] {
        m_cosign_edit->clear();
        m_reviewed_request.clear();
        ++m_cosign_generation;
        m_cosign_state->setText(tr("Refused. Nothing was signed."));
        updateCosignState();
    });
    updateRecordedSetups();
    updateCreateState();
    updatePolicyReviewState();
    updatePaymentReceiptReviewState();
    updateCosignState();
}

void AgentAllotmentPage::setModel(VaultModel* model)
{
    if (m_model) {
        disconnect(m_model, &VaultModel::balanceChanged, this, &AgentAllotmentPage::updateRecordedSetups);
    }
    if (m_model) disconnect(m_model, &VaultModel::agentAllotmentConsensusChanged, this, &AgentAllotmentPage::updateCosignState);
    m_reviewed_request.clear();
    ++m_cosign_generation;
    m_model = model;
    if (m_model) {
        connect(m_model, &VaultModel::balanceChanged, this, &AgentAllotmentPage::updateRecordedSetups, Qt::UniqueConnection);
        connect(m_model, &VaultModel::agentAllotmentConsensusChanged, this, &AgentAllotmentPage::updateCosignState);
    }
    updateRecordedSetups();
    updateCreateState();
    updatePolicyReviewState();
    updatePaymentReceiptReviewState();
    updateCosignState();
}

void AgentAllotmentPage::setDisplayUnit(QuicksilverUnit unit)
{
    m_display_unit = unit;
    m_funding_limit->setDisplayUnit(unit);
    m_daily_limit->setDisplayUnit(unit);
    updateRecordedSetups();
}

void AgentAllotmentPage::refresh()
{
    updateRecordedSetups();
}

void AgentAllotmentPage::recordAgentAllotmentSetup()
{
    if (!m_model) return;

    bool funding_valid = false;
    const CAmount funding = m_funding_limit->value(&funding_valid);
    bool daily_valid = false;
    const bool daily_empty = AmountFieldEmpty(m_daily_limit);
    const CAmount daily = daily_empty ? 0 : m_daily_limit->value(&daily_valid);
    if (!funding_valid || funding <= 0 || (!daily_empty && !daily_valid) || !m_acceptance->isChecked()) {
        updateCreateState();
        return;
    }

    const QString name = m_name_edit->text().trimmed();
    QPointer<AgentAllotmentPage> page(this);
    QPointer<VaultModel> model(m_model);
    m_model->requestUnlock([page, model, name, funding, daily](std::shared_ptr<VaultModel::UnlockContext> unlock) {
        if (!page || !model || page->m_model != model || !unlock->isValid()) return;
        auto record = model->recordAgentAllotmentSetup(name, funding, daily);
        if (!record) {
            page->m_state_label->setText(QString::fromStdString(util::ErrorString(record).original));
            return;
        }
        page->m_name_edit->clear();
        page->m_funding_limit->clear();
        page->m_daily_limit->clear();
        page->m_acceptance->setChecked(false);
        page->updateRecordedSetups();
        page->m_state_label->setText(tr("Agent allotment created. Back up this vault again."));
    });
}

void AgentAllotmentPage::reviewAgentAllotmentPolicyRequest()
{
    if (!m_model || !m_policy_request_edit || !m_policy_review_state) return;

    const QString request_json = m_policy_request_edit->toPlainText().trimmed();
    if (request_json.isEmpty()) {
        updatePolicyReviewState();
        return;
    }

    const auto request = m_model->validateAgentAllotmentPolicyRequest(request_json);
    if (!request) {
        SetLabelClass(m_policy_review_state, QStringLiteral("policyReviewError"));
        m_policy_review_state->setText(QString::fromStdString(util::ErrorString(request).translated));
        return;
    }

    const QString funding_limit = QuicksilverUnits::formatWithUnit(m_display_unit, request->funding_limit, false, QuicksilverUnits::SeparatorStyle::ALWAYS);
    const QString funding_available = QuicksilverUnits::formatWithUnit(m_display_unit, request->funding_available, false, QuicksilverUnits::SeparatorStyle::ALWAYS);
    const QString daily_limit = request->daily_limit > 0 ? QuicksilverUnits::formatWithUnit(m_display_unit, request->daily_limit, false, QuicksilverUnits::SeparatorStyle::ALWAYS) : tr("none");
    SetLabelClass(m_policy_review_state, QStringLiteral("policyReviewValid"));
    m_policy_review_state->setText(tr("Valid request for %1 at %2. Funding %3 of %4; daily guardrail %5; status %6. Spending limits are checked by the agent.")
                                       .arg(QString::fromStdString(request->label),
                                            QString::fromStdString(request->funding_address),
                                            funding_available,
                                            funding_limit,
                                            daily_limit,
                                            tr("Co-signed by this vault")));
}

void AgentAllotmentPage::reviewPaymentReceipt()
{
    if (!m_payment_receipt_edit || !m_payment_receipt_state) return;

    const QString receipt_json = m_payment_receipt_edit->toPlainText().trimmed();
    if (receipt_json.isEmpty()) {
        updatePaymentReceiptReviewState();
        return;
    }

    auto receipt{agent::DecodeAllotmentPaymentReceipt(
        receipt_json.toStdString(),
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash().ToString())};
    if (!receipt) {
        SetLabelClass(m_payment_receipt_state, QStringLiteral("policyReviewError"));
        m_payment_receipt_state->setText(QString::fromStdString(util::ErrorString(receipt).translated));
        return;
    }

    const QString amount = QuicksilverUnits::formatWithUnit(m_display_unit, receipt->funding_output.amount, false, QuicksilverUnits::SeparatorStyle::ALWAYS);
    QStringList metadata;
    if (!receipt->payment_id.empty()) metadata << tr("payment id %1").arg(QString::fromStdString(receipt->payment_id));
    if (!receipt->label.empty()) metadata << tr("label %1").arg(QString::fromStdString(receipt->label));
    if (!receipt->memo.empty()) metadata << tr("memo %1").arg(QString::fromStdString(receipt->memo));
    if (!receipt->payer.empty()) metadata << tr("payer %1").arg(QString::fromStdString(receipt->payer));

    QString status = tr("Valid receipt for %1. Output %2:%3, amount %4, received %5.")
                         .arg(QString::fromStdString(receipt->funding_address),
                              QString::fromStdString(receipt->funding_output.txid),
                              QString::number(receipt->funding_output.vout),
                              amount,
                              QString::number(receipt->received_time));
    if (!metadata.empty()) {
        status += QStringLiteral(" ") + tr("Metadata: %1.").arg(metadata.join(QStringLiteral("; ")));
    }

    SetLabelClass(m_payment_receipt_state, QStringLiteral("policyReviewValid"));
    m_payment_receipt_state->setText(status);
}

void AgentAllotmentPage::savePaymentReceiptToAgentInbox()
{
    if (!m_payment_receipt_edit || !m_payment_receipt_state) return;

    const QString receipt_json = m_payment_receipt_edit->toPlainText().trimmed();
    if (receipt_json.isEmpty()) {
        updatePaymentReceiptReviewState();
        return;
    }

    auto receipt{agent::DecodeAllotmentPaymentReceipt(
        receipt_json.toStdString(),
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash().ToString())};
    if (!receipt) {
        SetLabelClass(m_payment_receipt_state, QStringLiteral("policyReviewError"));
        m_payment_receipt_state->setText(QString::fromStdString(util::ErrorString(receipt).translated));
        return;
    }

    auto receipt_path{SaveAgentPaymentReceiptToInbox(*receipt, receipt_json)};
    if (!receipt_path) {
        SetLabelClass(m_payment_receipt_state, QStringLiteral("policyReviewError"));
        m_payment_receipt_state->setText(QString::fromStdString(util::ErrorString(receipt_path).translated));
        return;
    }

    QApplication::clipboard()->setText(AgentPaymentReceiptScanCommand(AgentPaymentReceiptInboxDirectory()));

    SetLabelClass(m_payment_receipt_state, QStringLiteral("policyReviewValid"));
    m_payment_receipt_state->setText(tr("Payment receipt saved to %1. External-agent scan command copied.")
        .arg(QString::fromStdString(fs::PathToString(*receipt_path))));
}

void AgentAllotmentPage::updateRecordedSetups()
{
    if (!m_records_label) return;
    if (m_records_list) ClearLayout(m_records_list);
    if (!m_model) {
        m_records_label->setText(tr("Agent allotments: none"));
        return;
    }

    const auto records = m_model->listAgentAllotmentRecords();
    if (records.empty()) {
        m_records_label->setText(tr("Agent allotments: none"));
        return;
    }

    QStringList summaries;
    for (const auto& record : records) {
        const CAmount funding_available = m_model->agentAllotmentFundingAvailable(record);
        const QString formatted_limit = QuicksilverUnits::formatWithUnit(m_display_unit, record.funding_limit, false, QuicksilverUnits::SeparatorStyle::ALWAYS);
        const QString formatted_available = QuicksilverUnits::formatWithUnit(m_display_unit, funding_available, false, QuicksilverUnits::SeparatorStyle::ALWAYS);
        const bool funding_confirmed = record.funding_limit > 0 && funding_available >= record.funding_limit;
        const CAmount funding_remaining = funding_confirmed ? 0 : record.funding_limit - funding_available;
        const QString formatted_remaining = QuicksilverUnits::formatWithUnit(m_display_unit, funding_remaining, false, QuicksilverUnits::SeparatorStyle::ALWAYS);

        QString summary = QString::fromStdString(record.label) + QStringLiteral(" (") +
                          formatted_limit;
        if (record.daily_limit > 0) {
            summary += tr(", daily guardrail %1").arg(QuicksilverUnits::formatWithUnit(m_display_unit, record.daily_limit, false, QuicksilverUnits::SeparatorStyle::ALWAYS));
        }
        summary += record.stopped_time == 0 ? tr(", Co-signed by this vault") : tr(", Stopped");
        if (!record.funding_address.empty()) {
            summary += tr(", funding address %1").arg(QString::fromStdString(record.funding_address));
        }
        if (funding_available > 0) {
            summary += tr(", funding received %1").arg(formatted_available);
        }
        summary += QStringLiteral(")");
        summaries << summary;

        if (m_records_list && !record.funding_address.empty()) {
            auto* row = new QFrame(this);
            row->setObjectName(QStringLiteral("agentAllotmentFundingRow"));
            row->setProperty("class", QStringLiteral("agentAllotmentFundingRow"));
            auto* row_layout = new QHBoxLayout(row);
            row_layout->setContentsMargins(0, 0, 0, 0);
            row_layout->setSpacing(10);

            auto* funding_state = new QLabel(funding_confirmed ? tr("Funded") : tr("Awaiting funding"), row);
            funding_state->setObjectName(QStringLiteral("agentAllotmentFundingState"));
            funding_state->setProperty("class", QStringLiteral("launchCapabilityState"));
            row_layout->addWidget(funding_state);

            const bool stopped{record.stopped_time != 0};
            auto* policy_state = new QLabel(stopped ? tr("Stopped") : tr("Co-signed by this vault"), row);
            policy_state->setObjectName(QStringLiteral("agentAllotmentPolicyState"));
            policy_state->setProperty("class", QStringLiteral("launchCapabilityState"));
            policy_state->setToolTip(stopped ? tr("This allotment is stopped.") : tr("This vault co-signs spends from this allotment."));
            row_layout->addWidget(policy_state);

            const QString funding_text = funding_available <= 0 ? tr("%1 awaits funding up to %2.")
                                                                      .arg(QString::fromStdString(record.label), formatted_limit) :
                                         funding_confirmed ? tr("%1 has confirmed funding at its reserved address.")
                                                                 .arg(QString::fromStdString(record.label)) :
                                                             tr("%1 has %2 of %3 at its reserved address; %4 remains.")
                                                                 .arg(QString::fromStdString(record.label), formatted_available, formatted_limit, formatted_remaining);
            auto* row_label = MakeMutedLabel(funding_text, row);
            row_label->setObjectName(QStringLiteral("agentAllotmentFundingRowLabel"));
            row_layout->addWidget(row_label, 1);

            auto* fund_button = new QPushButton(funding_confirmed ? tr("Funding complete") : (funding_available > 0 ? tr("Fund remaining") : tr("Fund setup")), row);
            fund_button->setObjectName(QStringLiteral("agentAllotmentFundSetupButton"));
            fund_button->setProperty("class", QStringLiteral("benchQuiet"));
            fund_button->setEnabled(!funding_confirmed && !stopped);
            fund_button->setToolTip(stopped ? tr("This allotment is stopped. Create a new allotment to fund the agent again.") :
                                    funding_confirmed ? tr("This setup's reserved address has at least the requested funding amount.") :
                                                        tr("Prefills the transfer screen with this setup's reserved vault funding address and remaining requested funding amount."));
            row_layout->addWidget(fund_button);

            const QString address = QString::fromStdString(record.funding_address);
            const QString label = tr("Agent setup: %1").arg(QString::fromStdString(record.label));
            const CAmount amount = funding_remaining;
            connect(fund_button, &QPushButton::clicked, this, [this, address, label, amount] {
                Q_EMIT fundAgentAllotmentSetupRequested(address, label, amount);
            });
            m_records_list->addWidget(row);

            auto* handoff_row = new QFrame(this);
            handoff_row->setObjectName(QStringLiteral("agentAllotmentPolicyHandoffRow"));
            handoff_row->setProperty("class", QStringLiteral("agentAllotmentFundingRow"));
            auto* handoff_layout = new QHBoxLayout(handoff_row);
            handoff_layout->setContentsMargins(0, 0, 0, 0);
            handoff_layout->setSpacing(10);

            const QString handoff_text = funding_confirmed ? tr("Copy a co-sign bundle for this funded setup. The bundle includes the policy request, the agent's key, and current spendable outputs.") : tr("Policy request export becomes available after the reserved address has the requested funding.");
            auto* handoff_label = MakeMutedLabel(handoff_text, handoff_row);
            handoff_label->setObjectName(QStringLiteral("agentAllotmentPolicyHandoffLabel"));
            handoff_layout->addWidget(handoff_label, 1);

            auto* copy_policy_button = new QPushButton(tr("Copy agent bundle"), handoff_row);
            copy_policy_button->setObjectName(QStringLiteral("agentAllotmentCopyPolicyButton"));
            copy_policy_button->setProperty("class", QStringLiteral("benchQuiet"));
            copy_policy_button->setEnabled(funding_confirmed);
            copy_policy_button->setToolTip(funding_confirmed ? tr("Copies a JSON policy, agent key, and funding-output bundle.") : tr("Fund this setup before copying a policy request."));
            handoff_layout->addWidget(copy_policy_button);

            auto* stop_button = new QPushButton(tr("Stop"), handoff_row);
            stop_button->setObjectName(QStringLiteral("agentAllotmentStopButton"));
            stop_button->setProperty("class", QStringLiteral("benchQuiet"));
            stop_button->setEnabled(!stopped);
            handoff_layout->addWidget(stop_button);
            connect(stop_button, &QPushButton::clicked, this, [this, id = QString::fromStdString(record.id)] {
                auto* box = new QMessageBox(QMessageBox::Question, tr("Stop agent allotment"),
                    tr("Stop this allotment? This vault will refuse every later spend request from this agent. This cannot be undone; create a new allotment to fund the agent again."),
                    QMessageBox::Yes | QMessageBox::Cancel, this);
                box->setObjectName(QStringLiteral("agentAllotmentStopConfirmation"));
                box->setDefaultButton(QMessageBox::Cancel);
                QPointer<VaultModel> model(m_model);
                connect(box, &QMessageBox::finished, this, [this, model, id](int result) {
                    if (result != QMessageBox::Yes || !model || m_model != model) return;
                    if (!model->stopAgentAllotment(id)) {
                        m_state_label->setText(tr("Could not stop this agent allotment."));
                        return;
                    }
                    updateRecordedSetups();
                });
                GUIUtil::ShowModalDialogAsynchronously(box);
            });

            auto* save_receipts_button = new QPushButton(tr("Save receipts"), handoff_row);
            save_receipts_button->setObjectName(QStringLiteral("agentAllotmentSaveFundingReceiptsButton"));
            save_receipts_button->setProperty("class", QStringLiteral("benchQuiet"));
            save_receipts_button->setEnabled(funding_confirmed);
            save_receipts_button->setToolTip(funding_confirmed ? tr("Writes this setup's current spendable funding outputs into the local quicksilver-agent receipt inbox.") : tr("Fund this setup before saving funding receipts."));
            handoff_layout->addWidget(save_receipts_button);

            auto* copy_recovery_button = new QPushButton(tr("Copy recovery import"), handoff_row);
            copy_recovery_button->setObjectName(QStringLiteral("agentAllotmentCopyRecoveryScanButton"));
            copy_recovery_button->setProperty("class", QStringLiteral("benchQuiet"));
            copy_recovery_button->setToolTip(tr("Copies a scantxoutset-to-importrecovery command for recovering spendable outputs at this setup's funding address."));
            handoff_layout->addWidget(copy_recovery_button);

            connect(copy_policy_button, &QPushButton::clicked, this, [this, record, funding_available] {
                if (!m_model) return;
                QPointer<AgentAllotmentPage> page(this);
                QPointer<VaultModel> model(m_model);
                model->requestUnlock([page, model, record, funding_available](std::shared_ptr<VaultModel::UnlockContext> unlock) {
                    if (!page || !model || page->m_model != model || !unlock->isValid()) return;
                    page->copyAgentBundle(record, funding_available);
                });

            });
            connect(save_receipts_button, &QPushButton::clicked, this, [this, record, funding_available] {
                if (!m_model) return;
                const QString policy_request{m_model->agentAllotmentPolicyRequest(record, funding_available)};
                const auto bundle{m_model->agentAllotmentPolicyBundle(policy_request)};
                if (!bundle) {
                    if (m_state_label) {
                        m_state_label->setText(tr("Agent receipt export failed: %1").arg(QString::fromStdString(util::ErrorString(bundle).original)));
                    }
                    return;
                }
                if (bundle->funding_outputs.empty()) {
                    if (m_state_label) {
                        m_state_label->setText(tr("Agent receipt export found no spendable funding outputs."));
                    }
                    return;
                }

                const int64_t received_time{QDateTime::currentSecsSinceEpoch()};
                for (const vault::AgentAllotmentFundingOutput& output : bundle->funding_outputs) {
                    const agent::AllotmentPaymentReceiptArtifact receipt{AgentPaymentReceiptFromFundingOutput(*bundle, output, received_time)};
                    const auto receipt_path{SaveAgentPaymentReceiptToInbox(receipt, AgentPaymentReceiptJson(receipt))};
                    if (!receipt_path) {
                        if (m_state_label) {
                            m_state_label->setText(tr("Agent receipt export failed: %1").arg(QString::fromStdString(util::ErrorString(receipt_path).original)));
                        }
                        return;
                    }
                }

                QApplication::clipboard()->setText(AgentPaymentReceiptScanCommand(AgentPaymentReceiptInboxDirectory()));
                if (m_state_label) {
                    m_state_label->setText(tr("Agent funding receipts saved: %1. External-agent scan command copied.")
                        .arg(QString::number(bundle->funding_outputs.size())));
                }
            });
            connect(copy_recovery_button, &QPushButton::clicked, this, [this, address] {
                QApplication::clipboard()->setText(ScantxoutsetRecoveryCommand(address));
                if (m_state_label) {
                    m_state_label->setText(tr("Recovery import command copied for %1.").arg(address));
                }
            });
            m_records_list->addWidget(handoff_row);
        }
    }
    m_records_label->setText(tr("Agent allotments: %1").arg(summaries.join(QStringLiteral("; "))));
}

void AgentAllotmentPage::updateCreateState()
{
    bool funding_valid = false;
    const CAmount funding = m_funding_limit->value(&funding_valid);
    bool daily_valid = false;
    const bool daily_empty = AmountFieldEmpty(m_daily_limit);
    m_daily_limit->value(&daily_valid);
    const bool accepted = m_acceptance->isChecked();
    const bool named = !m_name_edit->text().trimmed().isEmpty();
    const bool ready_to_record = m_model && named && accepted && funding_valid && funding > 0 && (daily_empty || daily_valid);

    m_create_button->setEnabled(ready_to_record);
    if (!m_model) {
        m_state_label->setText(tr("Open a vault before recording an agent setup."));
    } else if (ready_to_record) {
        m_state_label->setText(tr("Creates an allotment and reserves its vault funding address."));
    } else {
        m_state_label->setText(tr("Enter a name, funding amount, and risk acceptance before recording setup."));
    }
}

void AgentAllotmentPage::updatePolicyReviewState()
{
    if (!m_policy_review_button || !m_policy_request_edit || !m_policy_review_state) return;

    const bool has_request = !m_policy_request_edit->toPlainText().trimmed().isEmpty();
    m_policy_review_button->setEnabled(m_model && has_request);
    if (!m_model) {
        SetLabelClass(m_policy_review_state, QStringLiteral("muted"));
        m_policy_review_state->setText(tr("Open a vault before reviewing a policy request."));
    } else if (has_request) {
        SetLabelClass(m_policy_review_state, QStringLiteral("policyReviewReady"));
        m_policy_review_state->setText(tr("Ready to review pasted policy request."));
    } else {
        SetLabelClass(m_policy_review_state, QStringLiteral("muted"));
        m_policy_review_state->setText(tr("Paste a policy request to review."));
    }
}

void AgentAllotmentPage::updatePaymentReceiptReviewState()
{
    if (!m_payment_receipt_review_button || !m_payment_receipt_edit || !m_payment_receipt_state) return;

    const bool has_receipt = !m_payment_receipt_edit->toPlainText().trimmed().isEmpty();
    m_payment_receipt_review_button->setEnabled(has_receipt);
    if (m_payment_receipt_save_button) m_payment_receipt_save_button->setEnabled(has_receipt);
    if (has_receipt) {
        SetLabelClass(m_payment_receipt_state, QStringLiteral("policyReviewReady"));
        m_payment_receipt_state->setText(tr("Ready to review pasted payment receipt."));
    } else {
        SetLabelClass(m_payment_receipt_state, QStringLiteral("muted"));
        m_payment_receipt_state->setText(tr("Paste a payment receipt to review funding output metadata."));
    }
}

void AgentAllotmentPage::copyAgentBundle(const vault::AgentAllotmentRecord& record, CAmount funding_available)
{
    const QString policy_request{m_model->agentAllotmentPolicyRequest(record, funding_available)};
    const auto bundle{m_model->agentAllotmentPolicyBundle(policy_request)};
    if (!bundle) {
        m_state_label->setText(QString::fromStdString(util::ErrorString(bundle).original));
        return;
    }
    QApplication::clipboard()->setText(QString::fromStdString(bundle->bundle_json));
    m_state_label->setText(tr("Agent bundle copied with current spendable outputs."));
}

void AgentAllotmentPage::updateCosignState()
{
    const bool running = m_model && m_model->canCosignAgentAllotmentSpend();
    const bool reviewed = !m_reviewed_request.isEmpty() && m_reviewed_request == m_cosign_edit->toPlainText();
    const bool has_request = !m_cosign_edit->toPlainText().trimmed().isEmpty();
    m_cosign_review_button->setEnabled(m_model && has_request);
    m_cosign_refuse_button->setEnabled(has_request);
    m_cosign_button->setEnabled(running && reviewed);
    m_cosign_button->setToolTip(running ? tr("Co-signs this reviewed request and broadcasts it through this desktop's node.") :
        tr("Turn on Consensus to co-sign: this desktop broadcasts the spend through its own node."));
}

void AgentAllotmentPage::reviewAgentSpendRequest()
{
    if (!m_model) return;
    m_reviewed_request.clear();
    ++m_cosign_generation;
    const QString pasted = m_cosign_edit->toPlainText();
    const QString encoded = PastedSpendRequest(pasted);
    PartiallySignedQuicksilverTransaction request;
    std::string error;
    auto refuse_review = [this](const QString& message) {
        SetLabelClass(m_cosign_state, QStringLiteral("policyReviewError"));
        m_cosign_state->setText(message);
        updateCosignState();
    };
    if (!DecodeBase64PSQT(request, encoded.toStdString(), error)) {
        refuse_review(QString::fromStdString(error));
        return;
    }
    if (!request.tx || request.inputs.empty() || request.tx->vout.empty()) {
        refuse_review(tr("Agent spend request has no inputs or outputs."));
        return;
    }
    const auto records = m_model->listAgentAllotmentRecords();
    const vault::AgentAllotmentRecord* allotment = nullptr;
    CAmount input_total{0};
    for (const auto& input : request.inputs) {
        const auto found = std::find_if(records.begin(), records.end(), [&](const auto& record) {
            return input.witness_utxo.scriptPubKey == GetScriptForDestination(DecodeDestination(record.funding_address));
        });
        if (found == records.end() || (allotment && allotment->id != found->id)) {
            refuse_review(tr("Agent spend request must spend outputs from one allotment in this vault."));
            return;
        }
        allotment = &*found;
        if (!MoneyRange(input.witness_utxo.nValue) || !MoneyRange(input_total + input.witness_utxo.nValue)) {
            refuse_review(tr("Agent spend request has an invalid input amount."));
            return;
        }
        input_total += input.witness_utxo.nValue;
    }
    const auto format = [this](CAmount value) {
        return QuicksilverUnits::formatWithUnit(m_display_unit, value, false, QuicksilverUnits::SeparatorStyle::ALWAYS);
    };
    QStringList details{tr("Allotment: %1").arg(QString::fromStdString(allotment->label)),
                        tr("Input total: %1").arg(format(input_total))};
    const CScript funding_script = GetScriptForDestination(DecodeDestination(allotment->funding_address));
    CAmount change{0};
    CAmount output_total{0};
    for (const auto& output : request.tx->vout) {
        if (!MoneyRange(output.nValue) || !MoneyRange(output_total + output.nValue)) {
            refuse_review(tr("Agent spend request has an invalid output amount."));
            return;
        }
        output_total += output.nValue;
        CTxDestination destination;
        if (!ExtractDestination(output.scriptPubKey, destination)) {
            refuse_review(tr("Agent spend request has an output without an address."));
            return;
        }
        details << tr("Output: %1 — %2").arg(QString::fromStdString(EncodeDestination(destination)), format(output.nValue));
        if (output.scriptPubKey == funding_script) change += output.nValue;
    }
    if (output_total != input_total) {
        refuse_review(tr("Agent spend request's output total does not match its input total."));
        return;
    }
    details << tr("Change back to the allotment: %1").arg(format(change));
    const auto* context = m_model->node().context();
    const QString age = context && context->chainman ?
        QString::number(int64_t{m_model->node().getNumBlocks()} - request.tx->nAnchorHeight) : tr("unavailable");
    details << tr("Anchor age: %1 blocks out of %2 maximum").arg(age, QString::number(Params().GetConsensus().nMaxAnchorAge));
    SetLabelClass(m_cosign_state, QStringLiteral("policyReviewValid"));
    m_cosign_state->setText(details.join(QLatin1Char('\n')));
    m_reviewed_request = pasted;
    updateCosignState();
}

void AgentAllotmentPage::cosignAgentSpendRequest()
{
    if (!m_model || m_reviewed_request.isEmpty() || m_reviewed_request != m_cosign_edit->toPlainText() ||
        !m_model->canCosignAgentAllotmentSpend()) return;
    const QString reviewed = m_reviewed_request;
    const quint64 generation = m_cosign_generation;
    QPointer<AgentAllotmentPage> page(this);
    QPointer<VaultModel> model(m_model);
    m_model->requestUnlock([page, model, reviewed, generation](std::shared_ptr<VaultModel::UnlockContext> unlock) {
        if (!page || !model || page->m_model != model || !unlock->isValid() ||
            generation != page->m_cosign_generation ||
            reviewed != page->m_reviewed_request || reviewed != page->m_cosign_edit->toPlainText()) return;
        const auto result = model->cosignAgentAllotmentSpend(PastedSpendRequest(reviewed));
        page->m_reviewed_request.clear();
        SetLabelClass(page->m_cosign_state, result ? QStringLiteral("policyReviewValid") : QStringLiteral("policyReviewError"));
        page->m_cosign_state->setText(result ? QString::fromStdString((*result)->GetHash().ToString()) :
            QString::fromStdString(util::ErrorString(result).original));
        page->updateCosignState();
        page->updateRecordedSetups();
    });
}
