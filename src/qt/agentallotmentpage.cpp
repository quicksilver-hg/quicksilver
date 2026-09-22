// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/agentallotmentpage.h>

#include <qt/optionsmodel.h>
#include <qt/quicksilveramountfield.h>
#include <qt/quicksilverunits.h>
#include <qt/vaultmodel.h>

#include <agent/messageio.h>
#include <agent/peertransport.h>
#include <agent/txmessages.h>
#include <agent/allotmentpolicy.h>
#include <agent/allotmentspend.h>
#include <agent/allotmentstore.h>
#include <chainparams.h>
#include <common/args.h>
#include <consensus/amount.h>
#include <consensus/params.h>
#include <core_io.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <netbase.h>
#include <node/context.h>
#include <primitives/transaction.h>
#include <protocol.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/readwritefile.h>
#include <util/result.h>
#include <util/strencodings.h>
#include <univalue.h>
#include <validation.h>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QStringList>
#include <QStyle>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace {
static constexpr size_t MAX_AGENT_RELAY_PEER_STORE_FILE_SIZE{100'000};

struct AgentSignedSpendReview {
    CTransactionRef tx;
    CAmount output_total{0};
};

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

QString ExtractAgentTransactionHex(const QString& text)
{
    const QString trimmed = text.trimmed();
    const QStringList lines = trimmed.split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        const QString candidate = line.trimmed();
        if (candidate.startsWith(QStringLiteral("hex="))) {
            return candidate.mid(4).trimmed();
        }
    }
    QString compact = trimmed.simplified();
    compact.remove(QLatin1Char(' '));
    return compact;
}

std::optional<QString> ExtractAgentTransactionPayloadHex(const QString& text)
{
    const QStringList lines = text.trimmed().split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        const QString candidate = line.trimmed();
        if (candidate.startsWith(QStringLiteral("tx_payload="))) {
            return candidate.mid(11).trimmed();
        }
    }
    return std::nullopt;
}

std::optional<QString> ExtractAgentChangePaymentReceiptJson(const QString& text)
{
    const QStringList lines = text.trimmed().split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        const QString candidate = line.trimmed();
        if (candidate.startsWith(QStringLiteral("change_paymentreceipt="))) {
            return candidate.mid(22).trimmed();
        }
    }
    return std::nullopt;
}

QString ShellQuote(QString value)
{
    value.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QStringLiteral("'%1'").arg(value);
}

bool ContainsSpace(const QString& value)
{
    for (const QChar c : value) {
        if (c.isSpace()) return true;
    }
    return false;
}

fs::path AgentPaymentReceiptInboxDirectory()
{
    const fs::path configured_path{gArgs.GetPathArg("-paymentreceiptdir", fs::path{"agent"} / "payment-receipts.d")};
    return fsbridge::AbsPathJoin(gArgs.GetDataDirNet(), configured_path);
}

fs::path AgentPaymentReceiptStorePath()
{
    const fs::path configured_path{gArgs.GetPathArg("-receiptstore", fs::path{"agent"} / "payment-receipts.dat")};
    return fsbridge::AbsPathJoin(gArgs.GetDataDirNet(), configured_path);
}

fs::path AgentPolicyBundleDirectory()
{
    return fsbridge::AbsPathJoin(gArgs.GetDataDirNet(), fs::path{"agent"} / "policy-bundles.d");
}

fs::path AgentRelayPeerStorePath()
{
    return fsbridge::AbsPathJoin(gArgs.GetDataDirNet(), fs::path{"agent"} / "relay-peers.json");
}

QString AgentPaymentReceiptScanCommand(const fs::path& receipt_dir)
{
    return QStringLiteral("quicksilver-agent -chain=%1 -paymentreceiptdir=%2 scanreceipts")
        .arg(QString::fromStdString(Params().GetChainTypeString()),
             ShellQuote(QString::fromStdString(fs::PathToString(receipt_dir))));
}

QString AgentSpendSignCommand(const fs::path& policy_bundle_path,
                              const QString& destination,
                              CAmount spend_amount,
                              std::optional<CAmount> spent_today,
                              bool allow_cpu_txpow)
{
    const QString spent_today_arg = spent_today.has_value() ? QStringLiteral(" -spenttoday=%1").arg(QString::fromStdString(util::ToString(*spent_today))) : QString();
    const QString allow_cpu_arg = allow_cpu_txpow ? QStringLiteral(" -allowcputxpow") : QString();
    return QStringLiteral("quicksilver-agent -chain=%1 -policybundle=\"$(cat %2)\" -destination=%3 -spendamount=%4%5%6 signbundle")
        .arg(QString::fromStdString(Params().GetChainTypeString()),
             ShellQuote(QString::fromStdString(fs::PathToString(policy_bundle_path))),
             ShellQuote(destination),
             QString::fromStdString(util::ToString(spend_amount)),
             spent_today_arg,
             allow_cpu_arg);
}

fs::path AgentPaymentReceiptInboxPath(const agent::AllotmentPaymentReceiptArtifact& receipt)
{
    // util::ToString, not std::to_string: this names a file on disk, so a locale that
    // formatted the index differently would write a receipt the next run cannot find.
    const std::string filename{receipt.funding_output.txid + "-" + util::ToString(receipt.funding_output.vout) + ".json"};
    return AgentPaymentReceiptInboxDirectory() /
           fs::PathFromString(filename);
}

std::string SanitizedPolicyBundleFileStem(const std::string& id)
{
    std::string stem;
    stem.reserve(id.size());
    for (const char c : id) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            stem.push_back(c);
        }
    }
    return stem.empty() ? "agent-bundle" : stem;
}

util::Result<fs::path> SaveAgentPolicyBundleForSigning(const agent::AllotmentPolicyBundleArtifact& bundle, const QString& bundle_json)
{
    const fs::path bundle_dir{AgentPolicyBundleDirectory()};
    if (!fs::is_directory(bundle_dir) && !TryCreateDirectories(bundle_dir)) {
        return util::Error{Untranslated(strprintf("Could not create agent policy bundle directory %s.", fs::PathToString(bundle_dir)))};
    }

    const fs::path bundle_path{bundle_dir / fs::PathFromString(SanitizedPolicyBundleFileStem(bundle.policy_request.id) + ".json")};
    if (!WriteBinaryFile(bundle_path, bundle_json.toStdString() + "\n")) {
        return util::Error{Untranslated(strprintf("Could not write agent policy bundle to %s.", fs::PathToString(bundle_path)))};
    }
    return bundle_path;
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

util::Result<agent::AllotmentReceiptStoreData> LoadAgentPaymentReceiptStore()
{
    return agent::LoadAllotmentReceiptStore(AgentPaymentReceiptStorePath(), Params().GetChainTypeString(), Params().GenesisBlock().GetHash());
}

util::Result<fs::path> SaveAgentPaymentReceiptStore(const agent::AllotmentReceiptStoreData& data)
{
    const fs::path receipt_store_path{AgentPaymentReceiptStorePath()};
    const fs::path parent_path{receipt_store_path.parent_path()};
    if (!parent_path.empty() && !fs::is_directory(parent_path) && !TryCreateDirectories(parent_path)) {
        return util::Error{Untranslated(strprintf("Could not create agent receipt store directory %s.", fs::PathToString(parent_path)))};
    }

    const agent::AllotmentStoreResult saved{agent::SaveAllotmentReceiptStore(data, receipt_store_path, Params().GenesisBlock().GetHash())};
    if (saved != agent::AllotmentStoreResult::OK) {
        return util::Error{Untranslated(strprintf("Could not write receipt store %s: %s", fs::PathToString(receipt_store_path), agent::AllotmentStoreResultString(saved)))};
    }
    return receipt_store_path;
}

util::Result<size_t> AddReceiptsToStore(agent::AllotmentReceiptStoreData& store,
                                        const std::vector<agent::AllotmentPaymentReceiptArtifact>& receipts,
                                        std::vector<agent::AllotmentPaymentReceiptArtifact>* added_receipts = nullptr)
{
    return agent::AddAllotmentPaymentReceipts(store, receipts, added_receipts);
}

std::vector<agent::AllotmentFundingOutputArtifact> FundingOutputsFromSpendInputs(const std::vector<agent::AllotmentSpendInput>& inputs)
{
    std::vector<agent::AllotmentFundingOutputArtifact> outputs;
    outputs.reserve(inputs.size());
    for (const agent::AllotmentSpendInput& input : inputs) {
        outputs.push_back(agent::AllotmentFundingOutputArtifact{
            .txid = input.prevout.hash.ToString(),
            .vout = input.prevout.n,
            .amount = input.amount,
        });
    }
    return outputs;
}

agent::AllotmentReceiptActivity MakeReceiptActivity(agent::AllotmentReceiptActivityType type,
                                                      const agent::AllotmentPaymentReceiptArtifact& receipt,
                                                      int64_t event_time,
                                                      std::string related_txid = {})
{
    return agent::AllotmentReceiptActivity{
        .type = type,
        .funding_address = receipt.funding_address,
        .funding_output = receipt.funding_output,
        .event_time = event_time,
        .related_txid = std::move(related_txid),
        .payment_id = receipt.payment_id,
        .label = receipt.label,
        .memo = receipt.memo,
        .payer = receipt.payer,
    };
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

bool PushUniqueRelayPeer(std::vector<CService>& peers, const CService& peer)
{
    const std::string canonical{peer.ToStringAddrPort()};
    const auto duplicate{std::find_if(peers.begin(), peers.end(), [&](const CService& existing) {
        return existing.ToStringAddrPort() == canonical;
    })};
    if (duplicate != peers.end()) return false;
    peers.push_back(peer);
    return true;
}

void SortRelayPeers(std::vector<CService>& peers)
{
    std::sort(peers.begin(), peers.end(), [](const CService& left, const CService& right) {
        return left.ToStringAddrPort() < right.ToStringAddrPort();
    });
}

util::Result<std::vector<CService>> LoadAgentRelayPeers()
{
    const fs::path peer_store_path{AgentRelayPeerStorePath()};
    if (!fs::exists(peer_store_path)) {
        return std::vector<CService>{};
    }
    if (!fs::is_regular_file(peer_store_path)) {
        return util::Error{Untranslated(strprintf("Agent relay peer store %s is not a regular file.", fs::PathToString(peer_store_path)))};
    }

    const auto [ok, contents]{ReadBinaryFile(peer_store_path, MAX_AGENT_RELAY_PEER_STORE_FILE_SIZE)};
    if (!ok) {
        return util::Error{Untranslated(strprintf("Could not read agent relay peer store %s.", fs::PathToString(peer_store_path)))};
    }
    if (util::TrimString(contents).empty()) {
        return std::vector<CService>{};
    }

    UniValue root{UniValue::VOBJ};
    if (!root.read(contents) || !root.isObject()) {
        return util::Error{Untranslated(strprintf("Agent relay peer store %s must be a JSON object.", fs::PathToString(peer_store_path)))};
    }

    const UniValue& chain{root.find_value("chain")};
    if (chain.isStr() && chain.get_str() != Params().GetChainTypeString()) {
        return util::Error{Untranslated(strprintf("Agent relay peer store %s is for chain %s, not %s.",
                                                  fs::PathToString(peer_store_path),
                                                  chain.get_str(),
                                                  Params().GetChainTypeString()))};
    }
    const UniValue& genesis_hash{root.find_value("genesis_hash")};
    if (genesis_hash.isStr() && genesis_hash.get_str() != Params().GenesisBlock().GetHash().ToString()) {
        return util::Error{Untranslated(strprintf("Agent relay peer store %s is for a different genesis hash.", fs::PathToString(peer_store_path)))};
    }

    const UniValue& stored_peers{root.find_value("peers")};
    if (!stored_peers.isArray()) {
        return util::Error{Untranslated(strprintf("Agent relay peer store %s must contain a peers array.", fs::PathToString(peer_store_path)))};
    }

    std::vector<CService> peers;
    for (const UniValue& value : stored_peers.getValues()) {
        if (!value.isStr()) {
            return util::Error{Untranslated(strprintf("Agent relay peer store %s contains a non-string peer.", fs::PathToString(peer_store_path)))};
        }
        const std::optional<CService> peer{Lookup(value.get_str(), Params().GetDefaultPort(), /*fAllowLookup=*/false)};
        if (!peer.has_value() || !peer->IsValid()) {
            return util::Error{Untranslated(strprintf("Agent relay peer store %s contains an invalid peer.", fs::PathToString(peer_store_path)))};
        }
        PushUniqueRelayPeer(peers, *peer);
    }
    SortRelayPeers(peers);
    return peers;
}

util::Result<fs::path> SaveAgentRelayPeers(const std::vector<CService>& peers)
{
    const fs::path peer_store_path{AgentRelayPeerStorePath()};
    const fs::path parent_path{peer_store_path.parent_path()};
    if (!parent_path.empty() && !fs::is_directory(parent_path) && !TryCreateDirectories(parent_path)) {
        return util::Error{Untranslated(strprintf("Could not create agent relay peer store directory %s.", fs::PathToString(parent_path)))};
    }

    UniValue peer_values{UniValue::VARR};
    for (const CService& peer : peers) {
        peer_values.push_back(peer.ToStringAddrPort());
    }

    UniValue root{UniValue::VOBJ};
    root.pushKV("type", "quicksilver.agent_relay_peers");
    root.pushKV("version", 1);
    root.pushKV("chain", Params().GetChainTypeString());
    root.pushKV("genesis_hash", Params().GenesisBlock().GetHash().ToString());
    root.pushKV("peers", std::move(peer_values));

    if (!WriteBinaryFile(peer_store_path, root.write(2) + "\n")) {
        return util::Error{Untranslated(strprintf("Could not write agent relay peer store %s.", fs::PathToString(peer_store_path)))};
    }
    return peer_store_path;
}

QString RelayPeerAddCommand(const QString& peer)
{
    return QStringLiteral("quicksilver-agent -chain=%1 -peer=%2 addpeer")
        .arg(QString::fromStdString(Params().GetChainTypeString()),
             ShellQuote(peer));
}

QString RelayPeerDiscoveryCommand(const std::optional<QString>& peer)
{
    const QString peer_arg = peer.has_value() ? QStringLiteral(" -peer=%1").arg(ShellQuote(*peer)) : QString();
    return QStringLiteral("quicksilver-agent -chain=%1%2 discoverpeers")
        .arg(QString::fromStdString(Params().GetChainTypeString()),
             peer_arg);
}

QString NodeAddressImportCommand()
{
    return QStringLiteral("quicksilver-cli -chain=%1 getnodeaddresses 64 | quicksilver-agent -chain=%1 -peeraddresses=- importnodeaddresses")
        .arg(QString::fromStdString(Params().GetChainTypeString()));
}

QString HeaderPeerSyncCommand(const std::optional<QString>& peer)
{
    const QString peer_arg = peer.has_value() ? QStringLiteral(" -peer=%1").arg(ShellQuote(*peer)) : QString();
    return QStringLiteral("quicksilver-agent -chain=%1%2 syncheaderspeer")
        .arg(QString::fromStdString(Params().GetChainTypeString()),
             peer_arg);
}

QString SignedSpendPeerRelayCommand(const QString& tx_payload, const std::optional<QString>& peer)
{
    const QString peer_arg = peer.has_value() ? QStringLiteral(" -peer=%1").arg(ShellQuote(*peer)) : QString();
    return QStringLiteral("quicksilver-agent -chain=%1%2 -message=%3 sendtxpeer")
        .arg(QString::fromStdString(Params().GetChainTypeString()),
             peer_arg,
             tx_payload);
}

std::optional<AgentSignedSpendReview> BuildAgentSignedSpendReview(CTransactionRef tx, QString& error)
{
    AgentSignedSpendReview review{.tx = std::move(tx)};
    for (const CTxOut& output : review.tx->vout) {
        if (!MoneyRange(output.nValue) || !MoneyRange(review.output_total + output.nValue)) {
            error = AgentAllotmentPage::tr("Signed spend output amount is out of range.");
            return std::nullopt;
        }
        review.output_total += output.nValue;
    }
    return review;
}

std::optional<AgentSignedSpendReview> DecodeAgentSignedSpendText(const QString& text, QString& error)
{
    if (text.trimmed().isEmpty()) {
        error = AgentAllotmentPage::tr("Signed spend output is empty.");
        return std::nullopt;
    }

    const std::optional<QString> tx_payload_hex{ExtractAgentTransactionPayloadHex(text)};
    if (tx_payload_hex.has_value()) {
        const agent::AgentMessageDecodeResult message{agent::DecodeAgentMessage(NetMsgType::TX, tx_payload_hex->toStdString())};
        if (!message.ok()) {
            error = AgentAllotmentPage::tr("Signed spend tx_payload is not valid hex.");
            return std::nullopt;
        }
        const agent::TxMessageDecodeResult tx_message{agent::DecodeTxMessage(message.message)};
        if (!tx_message.ok()) {
            error = AgentAllotmentPage::tr("Signed spend tx_payload is not a valid Quicksilver transaction payload.");
            return std::nullopt;
        }
        return BuildAgentSignedSpendReview(tx_message.transaction, error);
    }

    const QString tx_hex = ExtractAgentTransactionHex(text);
    CMutableTransaction mutable_tx;
    if (!DecodeHexTx(mutable_tx, tx_hex.toStdString())) {
        error = AgentAllotmentPage::tr("Signed spend output is not a valid serialized Quicksilver transaction.");
        return std::nullopt;
    }

    return BuildAgentSignedSpendReview(MakeTransactionRef(mutable_tx), error);
}

void ClearLayout(QLayout* layout)
{
    while (QLayoutItem* item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
}

} // namespace

static bool AgentGpuSolverConfigured()
{
    const char* solver{std::getenv("CUCKATOO_GPU_SOLVER")};
    return solver != nullptr && solver[0] != '\0';
}

struct LocalAgentSpendOutcome {
    agent::AllotmentSpendContext spend_context;
    CAmount spend_amount{0};
    uint256 anchor_hash;
    // util::Result is move-only and not assignable, so the worker constructs it in place.
    std::unique_ptr<util::Result<agent::AllotmentSignedSpend>> signed_spend;
};

AgentAllotmentPage::~AgentAllotmentPage()
{
    if (m_spend_cancel) m_spend_cancel->store(true);
}

AgentAllotmentPage::AgentAllotmentPage(QWidget* parent)
    : AgentAllotmentPage(parent, agent::SendTransactionToOnePeer)
{
}

AgentAllotmentPage::AgentAllotmentPage(QWidget* parent, PeerRelayFunction peer_relay)
    : QWidget(parent),
      m_peer_relay(std::move(peer_relay))
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
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(14);

    auto* title = new QLabel(tr("Agent allotments"), contents);
    title->setObjectName(QStringLiteral("agentAllotmentTitle"));
    title->setProperty("class", QStringLiteral("pageTitle"));
    root->addWidget(title);

    auto* intro = MakeMutedLabel(
        tr("Fund agent keys from this vault. Agent spending uses shared keys, so create one only for hosts and agents you are prepared to monitor."),
        contents);
    intro->setObjectName(QStringLiteral("agentAllotmentIntro"));
    root->addWidget(intro);

    auto* risk_panel = new QFrame(contents);
    risk_panel->setObjectName(QStringLiteral("agentAllotmentRiskPanel"));
    auto* risk_layout = new QVBoxLayout(risk_panel);
    risk_layout->setContentsMargins(16, 14, 16, 14);
    risk_layout->setSpacing(8);

    auto* risk_title = new QLabel(tr("Shared-key risk"), risk_panel);
    risk_title->setObjectName(QStringLiteral("agentAllotmentRiskTitle"));
    risk_title->setProperty("class", QStringLiteral("hudHeading"));
    risk_layout->addWidget(risk_title);

    auto* dishonest_agent = MakeMutedLabel(tr("A dishonest agent can spend any quicksilver assigned to its shared key."), risk_panel);
    dishonest_agent->setObjectName(QStringLiteral("agentAllotmentDishonestAgentRisk"));
    risk_layout->addWidget(dishonest_agent);

    auto* compromised_host = MakeMutedLabel(tr("A compromised host can spend that shared key even if the agent behaves."), risk_panel);
    compromised_host->setObjectName(QStringLiteral("agentAllotmentCompromisedHostRisk"));
    risk_layout->addWidget(compromised_host);

    auto* guarantee = MakeMutedLabel(tr("There is no guarantee that limits stop misuse; limits are guardrails for review and operations."), risk_panel);
    guarantee->setObjectName(QStringLiteral("agentAllotmentGuaranteeRisk"));
    risk_layout->addWidget(guarantee);

    root->addWidget(risk_panel);

    auto* setup_group = new QGroupBox(tr("Agent funding"), contents);
    setup_group->setObjectName(QStringLiteral("agentAllotmentFundingGroup"));
    auto* setup_layout = new QVBoxLayout(setup_group);
    setup_layout->setContentsMargins(14, 14, 14, 14);
    setup_layout->setSpacing(9);

    auto add_row = [setup_group, setup_layout](const QString& label_text, QWidget* field) {
        auto* row = new QHBoxLayout();
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(10);

        auto* label = new QLabel(label_text, setup_group);
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

    root->addWidget(setup_group);

    auto* acceptance_panel = new QFrame(contents);
    acceptance_panel->setObjectName(QStringLiteral("agentAllotmentAcceptancePanel"));
    auto* acceptance_layout = new QVBoxLayout(acceptance_panel);
    acceptance_layout->setContentsMargins(16, 14, 16, 14);
    acceptance_layout->setSpacing(10);

    m_acceptance = new QCheckBox(tr("I accept the shared-key risk for this funded agent."), acceptance_panel);
    m_acceptance->setObjectName(QStringLiteral("agentAllotmentAcceptanceCheck"));
    acceptance_layout->addWidget(m_acceptance);

    auto* action_row = new QHBoxLayout();
    action_row->setContentsMargins(0, 0, 0, 0);
    action_row->setSpacing(10);

    m_create_button = new QPushButton(tr("Record shared-key risk and setup"), acceptance_panel);
    m_create_button->setObjectName(QStringLiteral("agentAllotmentCreateButton"));
    m_create_button->setProperty("class", QStringLiteral("primaryActionButton"));
    m_create_button->setToolTip(tr("Records acceptance, requested guardrails, a reserved funding address, and pending policy status in this vault."));
    action_row->addWidget(m_create_button);

    m_state_label = new QLabel(acceptance_panel);
    m_state_label->setObjectName(QStringLiteral("agentAllotmentBackendState"));
    m_state_label->setProperty("class", QStringLiteral("muted"));
    m_state_label->setWordWrap(true);
    action_row->addWidget(m_state_label, 1);
    acceptance_layout->addLayout(action_row);

    root->addWidget(acceptance_panel);

    auto* records_panel = new QFrame(contents);
    records_panel->setObjectName(QStringLiteral("agentAllotmentRecordsPanel"));
    auto* records_layout = new QVBoxLayout(records_panel);
    records_layout->setContentsMargins(16, 14, 16, 14);
    records_layout->setSpacing(8);

    auto* records_title = new QLabel(tr("Queued setups"), records_panel);
    records_title->setObjectName(QStringLiteral("agentAllotmentRecordsTitle"));
    records_title->setProperty("class", QStringLiteral("hudHeading"));
    records_layout->addWidget(records_title);

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
    auto* review_layout = new QVBoxLayout(review_panel);
    review_layout->setContentsMargins(16, 14, 16, 14);
    review_layout->setSpacing(8);

    auto* review_title = new QLabel(tr("Policy request review"), review_panel);
    review_title->setObjectName(QStringLiteral("agentAllotmentPolicyReviewTitle"));
    review_title->setProperty("class", QStringLiteral("hudHeading"));
    review_layout->addWidget(review_title);

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
    m_policy_review_button->setProperty("class", QStringLiteral("secondaryActionButton"));
    m_policy_review_button->setToolTip(tr("Checks policy request JSON against this vault's active chain without activating enforcement."));
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
    auto* receipt_layout = new QVBoxLayout(receipt_panel);
    receipt_layout->setContentsMargins(16, 14, 16, 14);
    receipt_layout->setSpacing(8);

    auto* receipt_title = new QLabel(tr("Payment receipt review"), receipt_panel);
    receipt_title->setObjectName(QStringLiteral("agentAllotmentPaymentReceiptTitle"));
    receipt_title->setProperty("class", QStringLiteral("hudHeading"));
    receipt_layout->addWidget(receipt_title);

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
    m_payment_receipt_review_button->setProperty("class", QStringLiteral("secondaryActionButton"));
    m_payment_receipt_review_button->setToolTip(tr("Checks a pasted agent payment receipt against this desktop's active chain and shows its funding metadata."));
    receipt_action_row->addWidget(m_payment_receipt_review_button);

    m_payment_receipt_save_button = new QPushButton(tr("Save to agent inbox"), receipt_panel);
    m_payment_receipt_save_button->setObjectName(QStringLiteral("agentAllotmentSavePaymentReceiptButton"));
    m_payment_receipt_save_button->setProperty("class", QStringLiteral("secondaryActionButton"));
    m_payment_receipt_save_button->setToolTip(tr("Writes a valid payment receipt into the local quicksilver-agent scanreceipts inbox and copies the scan command."));
    receipt_action_row->addWidget(m_payment_receipt_save_button);

    m_payment_receipt_state = new QLabel(receipt_panel);
    m_payment_receipt_state->setObjectName(QStringLiteral("agentAllotmentPaymentReceiptState"));
    m_payment_receipt_state->setProperty("class", QStringLiteral("muted"));
    m_payment_receipt_state->setWordWrap(true);
    receipt_action_row->addWidget(m_payment_receipt_state, 1);
    receipt_layout->addLayout(receipt_action_row);
    root->addWidget(receipt_panel);

    auto* utxo_panel = new QFrame(contents);
    utxo_panel->setObjectName(QStringLiteral("agentAllotmentUtxoPanel"));
    auto* utxo_layout = new QVBoxLayout(utxo_panel);
    utxo_layout->setContentsMargins(16, 14, 16, 14);
    utxo_layout->setSpacing(8);

    auto* utxo_title = new QLabel(tr("Spendable agent outputs"), utxo_panel);
    utxo_title->setObjectName(QStringLiteral("agentAllotmentUtxoTitle"));
    utxo_title->setProperty("class", QStringLiteral("hudHeading"));
    utxo_layout->addWidget(utxo_title);

    auto* utxo_action_row = new QHBoxLayout();
    utxo_action_row->setContentsMargins(0, 0, 0, 0);
    utxo_action_row->setSpacing(10);

    m_agent_utxo_refresh_button = new QPushButton(tr("Refresh UTXOs"), utxo_panel);
    m_agent_utxo_refresh_button->setObjectName(QStringLiteral("agentAllotmentRefreshUtxosButton"));
    m_agent_utxo_refresh_button->setProperty("class", QStringLiteral("secondaryActionButton"));
    m_agent_utxo_refresh_button->setToolTip(tr("Scans the configured local receipt inbox and refreshes the durable agent output store."));
    utxo_action_row->addWidget(m_agent_utxo_refresh_button);

    m_agent_utxo_state = MakeMutedLabel(QString(), utxo_panel);
    m_agent_utxo_state->setObjectName(QStringLiteral("agentAllotmentUtxoState"));
    utxo_action_row->addWidget(m_agent_utxo_state, 1);
    utxo_layout->addLayout(utxo_action_row);
    root->addWidget(utxo_panel);

    auto* spend_command_panel = new QFrame(contents);
    spend_command_panel->setObjectName(QStringLiteral("agentAllotmentSpendCommandPanel"));
    auto* spend_command_layout = new QVBoxLayout(spend_command_panel);
    spend_command_layout->setContentsMargins(16, 14, 16, 14);
    spend_command_layout->setSpacing(8);

    auto* spend_command_title = new QLabel(tr("Agent spend command"), spend_command_panel);
    spend_command_title->setObjectName(QStringLiteral("agentAllotmentSpendCommandTitle"));
    spend_command_title->setProperty("class", QStringLiteral("hudHeading"));
    spend_command_layout->addWidget(spend_command_title);

    m_spend_bundle_edit = new QPlainTextEdit(spend_command_panel);
    m_spend_bundle_edit->setObjectName(QStringLiteral("agentAllotmentSpendBundleEdit"));
    m_spend_bundle_edit->setPlaceholderText(tr("Paste agent bundle JSON"));
    m_spend_bundle_edit->setMinimumHeight(84);
    m_spend_bundle_edit->setTabChangesFocus(true);
    spend_command_layout->addWidget(m_spend_bundle_edit);

    auto* spend_destination_row = new QHBoxLayout();
    spend_destination_row->setContentsMargins(0, 0, 0, 0);
    spend_destination_row->setSpacing(10);

    auto* spend_destination_label = new QLabel(tr("Destination"), spend_command_panel);
    spend_destination_label->setMinimumWidth(120);
    spend_destination_row->addWidget(spend_destination_label);

    m_spend_destination_edit = new QLineEdit(spend_command_panel);
    m_spend_destination_edit->setObjectName(QStringLiteral("agentAllotmentSpendDestinationEdit"));
    m_spend_destination_edit->setPlaceholderText(tr("quicksilver address"));
    spend_destination_label->setBuddy(m_spend_destination_edit);
    spend_destination_row->addWidget(m_spend_destination_edit, 1);
    spend_command_layout->addLayout(spend_destination_row);

    auto* spend_amount_row = new QHBoxLayout();
    spend_amount_row->setContentsMargins(0, 0, 0, 0);
    spend_amount_row->setSpacing(10);

    auto* spend_amount_label = new QLabel(tr("Spend amount"), spend_command_panel);
    spend_amount_label->setMinimumWidth(120);
    spend_amount_row->addWidget(spend_amount_label);

    m_spend_amount = new QuicksilverAmountField(spend_command_panel);
    m_spend_amount->setObjectName(QStringLiteral("agentAllotmentSpendAmount"));
    m_spend_amount->SetMinValue(0);
    m_spend_amount->SetAllowEmpty(true);
    spend_amount_label->setBuddy(m_spend_amount);
    spend_amount_row->addWidget(m_spend_amount, 1);

    auto* spent_today_label = new QLabel(tr("Spent today"), spend_command_panel);
    spent_today_label->setMinimumWidth(100);
    spend_amount_row->addWidget(spent_today_label);

    m_spent_today = new QuicksilverAmountField(spend_command_panel);
    m_spent_today->setObjectName(QStringLiteral("agentAllotmentSpentToday"));
    m_spent_today->SetMinValue(0);
    m_spent_today->SetAllowEmpty(true);
    spent_today_label->setBuddy(m_spent_today);
    spend_amount_row->addWidget(m_spent_today, 1);
    spend_command_layout->addLayout(spend_amount_row);

    auto* spend_command_action_row = new QHBoxLayout();
    spend_command_action_row->setContentsMargins(0, 0, 0, 0);
    spend_command_action_row->setSpacing(10);

    m_copy_spend_command_button = new QPushButton(tr("Copy sign command"), spend_command_panel);
    m_copy_spend_command_button->setObjectName(QStringLiteral("agentAllotmentCopySpendCommandButton"));
    m_copy_spend_command_button->setProperty("class", QStringLiteral("secondaryActionButton"));
    m_copy_spend_command_button->setToolTip(tr("Saves the pasted bundle locally and copies a quicksilver-agent signbundle command for the requested spend."));
    spend_command_action_row->addWidget(m_copy_spend_command_button);

    m_sign_spend_button = new QPushButton(tr("Sign spend"), spend_command_panel);
    m_sign_spend_button->setObjectName(QStringLiteral("agentAllotmentSignSpendButton"));
    m_sign_spend_button->setProperty("class", QStringLiteral("primaryActionButton"));
    m_sign_spend_button->setToolTip(tr("Signs the requested agent spend locally from the pasted bundle and fills the signed-spend reviewer."));
    spend_command_action_row->addWidget(m_sign_spend_button);

    m_cancel_spend_button = new QPushButton(tr("Stop"), spend_command_panel);
    m_cancel_spend_button->setObjectName(QStringLiteral("agentAllotmentCancelSpendButton"));
    m_cancel_spend_button->setProperty("class", QStringLiteral("secondaryActionButton"));
    m_cancel_spend_button->setToolTip(tr("Stops the agent spend preparation running on this computer. The spend stays unsigned."));
    m_cancel_spend_button->setEnabled(false);
    spend_command_action_row->addWidget(m_cancel_spend_button);

    m_spend_command_state = new QLabel(spend_command_panel);
    m_spend_command_state->setObjectName(QStringLiteral("agentAllotmentSpendCommandState"));
    m_spend_command_state->setProperty("class", QStringLiteral("muted"));
    m_spend_command_state->setWordWrap(true);
    spend_command_action_row->addWidget(m_spend_command_state, 1);
    spend_command_layout->addLayout(spend_command_action_row);
    root->addWidget(spend_command_panel);

    auto* signed_spend_panel = new QFrame(contents);
    signed_spend_panel->setObjectName(QStringLiteral("agentAllotmentSignedSpendPanel"));
    auto* signed_spend_layout = new QVBoxLayout(signed_spend_panel);
    signed_spend_layout->setContentsMargins(16, 14, 16, 14);
    signed_spend_layout->setSpacing(8);

    auto* signed_spend_title = new QLabel(tr("Signed spend review"), signed_spend_panel);
    signed_spend_title->setObjectName(QStringLiteral("agentAllotmentSignedSpendTitle"));
    signed_spend_title->setProperty("class", QStringLiteral("hudHeading"));
    signed_spend_layout->addWidget(signed_spend_title);

    m_signed_spend_edit = new QPlainTextEdit(signed_spend_panel);
    m_signed_spend_edit->setObjectName(QStringLiteral("agentAllotmentSignedSpendEdit"));
    m_signed_spend_edit->setPlaceholderText(tr("Paste signed spend hex or quicksilver-agent output"));
    m_signed_spend_edit->setMinimumHeight(84);
    m_signed_spend_edit->setTabChangesFocus(true);
    signed_spend_layout->addWidget(m_signed_spend_edit);

    auto* relay_peer_row = new QHBoxLayout();
    relay_peer_row->setContentsMargins(0, 0, 0, 0);
    relay_peer_row->setSpacing(10);

    auto* relay_peer_label = new QLabel(tr("Relay peer"), signed_spend_panel);
    relay_peer_label->setMinimumWidth(120);
    relay_peer_row->addWidget(relay_peer_label);

    m_relay_peer_edit = new QLineEdit(signed_spend_panel);
    m_relay_peer_edit->setObjectName(QStringLiteral("agentAllotmentRelayPeerEdit"));
    m_relay_peer_edit->setPlaceholderText(tr("host[:port]"));
    m_relay_peer_edit->setToolTip(tr("Peer address for quicksilver-agent addpeer or sendtxpeer."));
    relay_peer_label->setBuddy(m_relay_peer_edit);
    relay_peer_row->addWidget(m_relay_peer_edit, 1);

    m_copy_add_peer_command_button = new QPushButton(tr("Copy save peer"), signed_spend_panel);
    m_copy_add_peer_command_button->setObjectName(QStringLiteral("agentAllotmentCopyAddPeerCommandButton"));
    m_copy_add_peer_command_button->setProperty("class", QStringLiteral("secondaryActionButton"));
    m_copy_add_peer_command_button->setToolTip(tr("Copies a quicksilver-agent addpeer command for the relay peer."));
    relay_peer_row->addWidget(m_copy_add_peer_command_button);

    m_copy_discover_peers_command_button = new QPushButton(tr("Copy discover peers"), signed_spend_panel);
    m_copy_discover_peers_command_button->setObjectName(QStringLiteral("agentAllotmentCopyDiscoverPeersCommandButton"));
    m_copy_discover_peers_command_button->setProperty("class", QStringLiteral("secondaryActionButton"));
    m_copy_discover_peers_command_button->setToolTip(tr("Copies a quicksilver-agent discoverpeers command for stored peers or the relay peer."));
    relay_peer_row->addWidget(m_copy_discover_peers_command_button);

    m_import_node_peers_button = new QPushButton(tr("Import node peers"), signed_spend_panel);
    m_import_node_peers_button->setObjectName(QStringLiteral("agentAllotmentImportNodePeersButton"));
    m_import_node_peers_button->setProperty("class", QStringLiteral("secondaryActionButton"));
    m_import_node_peers_button->setToolTip(tr("Imports peers from the active desktop node into the local agent relay peer store."));
    relay_peer_row->addWidget(m_import_node_peers_button);

    m_copy_node_address_import_command_button = new QPushButton(tr("Copy node peers"), signed_spend_panel);
    m_copy_node_address_import_command_button->setObjectName(QStringLiteral("agentAllotmentCopyNodeAddressImportCommandButton"));
    m_copy_node_address_import_command_button->setProperty("class", QStringLiteral("secondaryActionButton"));
    m_copy_node_address_import_command_button->setToolTip(tr("Copies a local-node getnodeaddresses import pipeline for the agent relay peer store."));
    relay_peer_row->addWidget(m_copy_node_address_import_command_button);

    m_copy_sync_headers_command_button = new QPushButton(tr("Copy sync headers"), signed_spend_panel);
    m_copy_sync_headers_command_button->setObjectName(QStringLiteral("agentAllotmentCopySyncHeadersCommandButton"));
    m_copy_sync_headers_command_button->setProperty("class", QStringLiteral("secondaryActionButton"));
    m_copy_sync_headers_command_button->setToolTip(tr("Copies a quicksilver-agent syncheaderspeer command for stored peers or the relay peer."));
    relay_peer_row->addWidget(m_copy_sync_headers_command_button);
    signed_spend_layout->addLayout(relay_peer_row);

    auto* signed_spend_action_row = new QHBoxLayout();
    signed_spend_action_row->setContentsMargins(0, 0, 0, 0);
    signed_spend_action_row->setSpacing(10);

    m_signed_spend_review_button = new QPushButton(tr("Review signed spend"), signed_spend_panel);
    m_signed_spend_review_button->setObjectName(QStringLiteral("agentAllotmentReviewSignedSpendButton"));
    m_signed_spend_review_button->setProperty("class", QStringLiteral("secondaryActionButton"));
    m_signed_spend_review_button->setToolTip(tr("Decodes pasted agent spend output locally before relay transport is available."));
    signed_spend_action_row->addWidget(m_signed_spend_review_button);

    m_signed_spend_copy_relay_button = new QPushButton(tr("Copy relay payloads"), signed_spend_panel);
    m_signed_spend_copy_relay_button->setObjectName(QStringLiteral("agentAllotmentCopyRelayPayloadsButton"));
    m_signed_spend_copy_relay_button->setProperty("class", QStringLiteral("secondaryActionButton"));
    m_signed_spend_copy_relay_button->setToolTip(tr("Copies tx and inv payloads for an external node-free relay transport."));
    signed_spend_action_row->addWidget(m_signed_spend_copy_relay_button);

    m_signed_spend_copy_peer_command_button = new QPushButton(tr("Copy peer relay command"), signed_spend_panel);
    m_signed_spend_copy_peer_command_button->setObjectName(QStringLiteral("agentAllotmentCopyPeerRelayCommandButton"));
    m_signed_spend_copy_peer_command_button->setProperty("class", QStringLiteral("secondaryActionButton"));
    m_signed_spend_copy_peer_command_button->setToolTip(tr("Copies a quicksilver-agent sendtxpeer command for the reviewed signed spend and relay peer."));
    signed_spend_action_row->addWidget(m_signed_spend_copy_peer_command_button);

    m_signed_spend_copy_stored_peer_command_button = new QPushButton(tr("Copy stored-peer relay"), signed_spend_panel);
    m_signed_spend_copy_stored_peer_command_button->setObjectName(QStringLiteral("agentAllotmentCopyStoredPeerRelayCommandButton"));
    m_signed_spend_copy_stored_peer_command_button->setProperty("class", QStringLiteral("secondaryActionButton"));
    m_signed_spend_copy_stored_peer_command_button->setToolTip(tr("Copies a quicksilver-agent sendtxpeer command that uses stored relay peers."));
    signed_spend_action_row->addWidget(m_signed_spend_copy_stored_peer_command_button);

    m_signed_spend_relay_peer_button = new QPushButton(tr("Relay to peer"), signed_spend_panel);
    m_signed_spend_relay_peer_button->setObjectName(QStringLiteral("agentAllotmentRelayPeerButton"));
    m_signed_spend_relay_peer_button->setProperty("class", QStringLiteral("primaryActionButton"));
    m_signed_spend_relay_peer_button->setToolTip(tr("Relays the reviewed signed spend in the background through the typed peer, stored peers, or fixed seeds."));
    signed_spend_action_row->addWidget(m_signed_spend_relay_peer_button);

    m_signed_spend_submit_button = new QPushButton(tr("Submit signed spend"), signed_spend_panel);
    m_signed_spend_submit_button->setObjectName(QStringLiteral("agentAllotmentSubmitSignedSpendButton"));
    m_signed_spend_submit_button->setProperty("class", QStringLiteral("primaryActionButton"));
    m_signed_spend_submit_button->setToolTip(tr("Submits the reviewed signed spend through the active consensus node."));
    signed_spend_action_row->addWidget(m_signed_spend_submit_button);

    m_signed_spend_state = new QLabel(signed_spend_panel);
    m_signed_spend_state->setObjectName(QStringLiteral("agentAllotmentSignedSpendState"));
    m_signed_spend_state->setProperty("class", QStringLiteral("muted"));
    m_signed_spend_state->setWordWrap(true);
    signed_spend_action_row->addWidget(m_signed_spend_state, 1);
    signed_spend_layout->addLayout(signed_spend_action_row);
    root->addWidget(signed_spend_panel);

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
    connect(m_agent_utxo_refresh_button, &QPushButton::clicked, this, [this] { refreshStoredAgentUtxos(/*requested=*/true); });
    connect(m_spend_bundle_edit, &QPlainTextEdit::textChanged, this, &AgentAllotmentPage::updateAgentSpendCommandState);
    connect(m_spend_destination_edit, &QLineEdit::textChanged, this, &AgentAllotmentPage::updateAgentSpendCommandState);
    connect(m_spend_amount, &QuicksilverAmountField::valueChanged, this, &AgentAllotmentPage::updateAgentSpendCommandState);
    connect(m_spent_today, &QuicksilverAmountField::valueChanged, this, &AgentAllotmentPage::updateAgentSpendCommandState);
    connect(m_copy_spend_command_button, &QPushButton::clicked, this, &AgentAllotmentPage::copyAgentSpendSignCommand);
    connect(m_sign_spend_button, &QPushButton::clicked, this, &AgentAllotmentPage::signAgentSpendLocally);
    connect(m_cancel_spend_button, &QPushButton::clicked, this, &AgentAllotmentPage::cancelAgentSpendLocally);
    connect(m_signed_spend_edit, &QPlainTextEdit::textChanged, this, &AgentAllotmentPage::updateSignedSpendReviewState);
    connect(m_relay_peer_edit, &QLineEdit::textChanged, this, &AgentAllotmentPage::updatePeerRelayCommandState);
    connect(m_signed_spend_review_button, &QPushButton::clicked, this, &AgentAllotmentPage::reviewSignedAgentSpend);
    connect(m_signed_spend_copy_relay_button, &QPushButton::clicked, this, &AgentAllotmentPage::copySignedSpendRelayPayloads);
    connect(m_copy_add_peer_command_button, &QPushButton::clicked, this, &AgentAllotmentPage::copyRelayPeerAddCommand);
    connect(m_copy_discover_peers_command_button, &QPushButton::clicked, this, &AgentAllotmentPage::copyRelayPeerDiscoveryCommand);
    connect(m_import_node_peers_button, &QPushButton::clicked, this, &AgentAllotmentPage::importNodePeersToAgentStore);
    connect(m_copy_node_address_import_command_button, &QPushButton::clicked, this, &AgentAllotmentPage::copyNodeAddressImportCommand);
    connect(m_copy_sync_headers_command_button, &QPushButton::clicked, this, &AgentAllotmentPage::copyHeaderPeerSyncCommand);
    connect(m_signed_spend_copy_peer_command_button, &QPushButton::clicked, this, &AgentAllotmentPage::copySignedSpendPeerRelayCommand);
    connect(m_signed_spend_copy_stored_peer_command_button, &QPushButton::clicked, this, &AgentAllotmentPage::copySignedSpendStoredPeerRelayCommand);
    connect(m_signed_spend_relay_peer_button, &QPushButton::clicked, this, &AgentAllotmentPage::relaySignedSpendToConfiguredPeers);
    connect(m_signed_spend_submit_button, &QPushButton::clicked, this, &AgentAllotmentPage::submitSignedAgentSpend);
    auto* receipt_refresh_timer = new QTimer(this);
    receipt_refresh_timer->setInterval(5000);
    connect(receipt_refresh_timer, &QTimer::timeout, this, [this] {
        if (isVisible()) refreshStoredAgentUtxos();
    });
    receipt_refresh_timer->start();
    updateRecordedSetups();
    updateCreateState();
    updatePolicyReviewState();
    updatePaymentReceiptReviewState();
    refreshStoredAgentUtxos();
    updateAgentSpendCommandState();
    updateSignedSpendReviewState();
}

void AgentAllotmentPage::setModel(VaultModel* model)
{
    if (m_model) {
        disconnect(m_model, &VaultModel::balanceChanged, this, &AgentAllotmentPage::updateRecordedSetups);
    }
    m_model = model;
    if (m_model) {
        connect(m_model, &VaultModel::balanceChanged, this, &AgentAllotmentPage::updateRecordedSetups, Qt::UniqueConnection);
    }
    updateRecordedSetups();
    updateCreateState();
    updatePolicyReviewState();
    updatePaymentReceiptReviewState();
    refreshStoredAgentUtxos();
    updateAgentSpendCommandState();
    updateSignedSpendReviewState();
}

void AgentAllotmentPage::setDisplayUnit(QuicksilverUnit unit)
{
    m_display_unit = unit;
    m_funding_limit->setDisplayUnit(unit);
    m_daily_limit->setDisplayUnit(unit);
    m_spend_amount->setDisplayUnit(unit);
    m_spent_today->setDisplayUnit(unit);
    updateRecordedSetups();
    refreshStoredAgentUtxos();
}

void AgentAllotmentPage::refresh()
{
    updateRecordedSetups();
    refreshStoredAgentUtxos(/*requested=*/true);
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

    auto record = m_model->recordAgentAllotmentSetup(m_name_edit->text().trimmed(), funding, daily);
    if (!record) {
        m_state_label->setText(QString::fromStdString(util::ErrorString(record).translated));
        return;
    }

    m_name_edit->clear();
    m_funding_limit->clear();
    m_daily_limit->clear();
    m_acceptance->setChecked(false);
    updateRecordedSetups();
    m_state_label->setText(tr("Setup recorded with a vault funding address. Agent policy integration remains pending."));
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
    const QString policy_status = request->policy_status == vault::AgentAllotmentPolicyStatus::Enforced ? tr("enforced metadata") : tr("pending metadata");

    SetLabelClass(m_policy_review_state, QStringLiteral("policyReviewValid"));
    m_policy_review_state->setText(tr("Valid request for %1 at %2. Funding %3 of %4; daily guardrail %5; status %6. Review does not activate enforcement.")
                                       .arg(QString::fromStdString(request->label),
                                            QString::fromStdString(request->funding_address),
                                            funding_available,
                                            funding_limit,
                                            daily_limit,
                                            policy_status));
}

void AgentAllotmentPage::copyAgentSpendSignCommand()
{
    if (!m_spend_bundle_edit || !m_spend_destination_edit || !m_spend_amount || !m_spent_today || !m_spend_command_state) return;

    const QString bundle_json = m_spend_bundle_edit->toPlainText().trimmed();
    if (bundle_json.isEmpty()) {
        updateAgentSpendCommandState();
        return;
    }

    const QString destination = m_spend_destination_edit->text().trimmed();
    if (!IsValidDestination(DecodeDestination(destination.toStdString()))) {
        SetLabelClass(m_spend_command_state, QStringLiteral("policyReviewError"));
        m_spend_command_state->setText(tr("Enter a valid spend destination address."));
        return;
    }

    bool spend_valid = false;
    const CAmount spend_amount = m_spend_amount->value(&spend_valid);
    if (!spend_valid || spend_amount <= 0) {
        SetLabelClass(m_spend_command_state, QStringLiteral("policyReviewError"));
        m_spend_command_state->setText(tr("Enter a spend amount greater than zero."));
        return;
    }

    std::optional<CAmount> spent_today;
    if (!AmountFieldEmpty(m_spent_today)) {
        bool spent_today_valid = false;
        const CAmount spent_today_amount = m_spent_today->value(&spent_today_valid);
        if (!spent_today_valid || spent_today_amount < 0) {
            SetLabelClass(m_spend_command_state, QStringLiteral("policyReviewError"));
            m_spend_command_state->setText(tr("Spent today must be empty or a valid nonnegative amount."));
            return;
        }
        spent_today = spent_today_amount;
    }

    auto bundle{agent::DecodeAllotmentPolicyBundle(
        bundle_json.toStdString(),
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash().ToString())};
    if (!bundle) {
        SetLabelClass(m_spend_command_state, QStringLiteral("policyReviewError"));
        m_spend_command_state->setText(QString::fromStdString(util::ErrorString(bundle).translated));
        return;
    }

    auto bundle_path{SaveAgentPolicyBundleForSigning(*bundle, bundle_json)};
    if (!bundle_path) {
        SetLabelClass(m_spend_command_state, QStringLiteral("policyReviewError"));
        m_spend_command_state->setText(QString::fromStdString(util::ErrorString(bundle_path).translated));
        return;
    }

    bool allow_cpu_txpow{false};
    if (m_model && m_model->getOptionsModel()) {
        allow_cpu_txpow = m_model->getOptionsModel()->getOption(OptionsModel::AllowCpuAgentTxPow).toBool();
    }
    QApplication::clipboard()->setText(AgentSpendSignCommand(*bundle_path, destination, spend_amount, spent_today, allow_cpu_txpow));

    SetLabelClass(m_spend_command_state, QStringLiteral("policyReviewValid"));
    m_spend_command_state->setText(tr("Agent spend command copied for %1 using saved bundle %2.")
                                       .arg(QString::fromStdString(bundle->policy_request.id),
                                            QString::fromStdString(fs::PathToString(*bundle_path))));
}

void AgentAllotmentPage::signAgentSpendLocally()
{
    if (!m_spend_bundle_edit || !m_spend_destination_edit || !m_spend_amount || !m_spent_today || !m_spend_command_state || !m_signed_spend_edit) return;

    if (!m_model) {
        SetLabelClass(m_spend_command_state, QStringLiteral("policyReviewError"));
        m_spend_command_state->setText(tr("Open a vault before signing an agent spend locally."));
        return;
    }

    node::NodeContext* context{m_model->node().context()};
    if (!context || !context->chainman) {
        SetLabelClass(m_spend_command_state, QStringLiteral("policyReviewError"));
        m_spend_command_state->setText(tr("Start consensus before signing an agent spend locally."));
        return;
    }

    const QString bundle_json = m_spend_bundle_edit->toPlainText().trimmed();
    if (bundle_json.isEmpty()) {
        updateAgentSpendCommandState();
        return;
    }

    const QString destination_text = m_spend_destination_edit->text().trimmed();
    const CTxDestination destination{DecodeDestination(destination_text.toStdString())};
    if (!IsValidDestination(destination)) {
        SetLabelClass(m_spend_command_state, QStringLiteral("policyReviewError"));
        m_spend_command_state->setText(tr("Enter a valid spend destination address."));
        return;
    }

    bool spend_valid = false;
    const CAmount spend_amount = m_spend_amount->value(&spend_valid);
    if (!spend_valid || spend_amount <= 0) {
        SetLabelClass(m_spend_command_state, QStringLiteral("policyReviewError"));
        m_spend_command_state->setText(tr("Enter a spend amount greater than zero."));
        return;
    }

    auto spend_context{agent::ImportAllotmentBundle(
        bundle_json.toStdString(),
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash().ToString())};
    if (!spend_context) {
        SetLabelClass(m_spend_command_state, QStringLiteral("policyReviewError"));
        m_spend_command_state->setText(QString::fromStdString(util::ErrorString(spend_context).translated));
        return;
    }
    auto receipt_store{LoadAgentPaymentReceiptStore()};
    if (!receipt_store) {
        SetLabelClass(m_spend_command_state, QStringLiteral("policyReviewError"));
        m_spend_command_state->setText(QString::fromStdString(util::ErrorString(receipt_store).translated));
        return;
    }
    auto receipt_outputs{agent::ApplyPaymentReceiptsToBundle(spend_context->bundle, receipt_store->receipts, receipt_store->activities)};
    if (!receipt_outputs) {
        SetLabelClass(m_spend_command_state, QStringLiteral("policyReviewError"));
        m_spend_command_state->setText(QString::fromStdString(util::ErrorString(receipt_outputs).translated));
        return;
    }

    CAmount spent_today{0};
    if (!AmountFieldEmpty(m_spent_today)) {
        bool spent_today_valid = false;
        spent_today = m_spent_today->value(&spent_today_valid);
        if (!spent_today_valid || spent_today < 0) {
            SetLabelClass(m_spend_command_state, QStringLiteral("policyReviewError"));
            m_spend_command_state->setText(tr("Spent today must be empty or a valid nonnegative amount."));
            return;
        }
    } else {
        auto summed{agent::SpentTodayFromActivities(receipt_store->activities, spend_context->bundle.funding_address, QDateTime::currentSecsSinceEpoch())};
        if (!summed) {
            SetLabelClass(m_spend_command_state, QStringLiteral("policyReviewError"));
            m_spend_command_state->setText(QString::fromStdString(util::ErrorString(summed).translated));
            return;
        }
        spent_today = *summed;
    }

    const CBlockIndex* anchor{WITH_LOCK(context->chainman->GetMutex(), return context->chainman->ActiveChain().Tip())};
    if (!anchor) {
        SetLabelClass(m_spend_command_state, QStringLiteral("policyReviewError"));
        m_spend_command_state->setText(tr("No consensus anchor is available for the agent spend."));
        return;
    }
    if (m_spend_in_flight) return;

    bool allow_cpu_txpow{false};
    if (m_model->getOptionsModel()) {
        allow_cpu_txpow = m_model->getOptionsModel()->getOption(OptionsModel::AllowCpuAgentTxPow).toBool();
    }
    const bool processor_grind{Params().GetConsensus().nTxEdgeBits == 28 && allow_cpu_txpow && !AgentGpuSolverConfigured()};

    auto outcome{std::make_shared<LocalAgentSpendOutcome>()};
    outcome->spend_context = std::move(*spend_context);
    outcome->spend_amount = spend_amount;
    outcome->anchor_hash = anchor->GetBlockHash();

    m_spend_in_flight = true;
    const quint64 generation{++m_spend_generation};
    m_spend_cancel = std::make_shared<std::atomic<bool>>(false);
    updateAgentSpendCommandState();
    SetLabelClass(m_spend_command_state, QStringLiteral("policyReviewReady"));
    m_spend_command_state->setText(processor_grind
                                       ? tr("Preparing this agent spend on the processor. It takes many minutes and uses every core. Stop leaves it unsigned.")
                                       : tr("Preparing this agent spend. Stop leaves it unsigned."));

    const auto cancel_flag{m_spend_cancel};
    const Consensus::Params consensus{Params().GetConsensus()};
    // Same shape as the peer-relay worker below: the grind must not run on the
    // GUI thread, and a generation check drops a result the user already stopped.
    QThread* thread{QThread::create([outcome, destination, spend_amount, spent_today, anchor, consensus, allow_cpu_txpow, cancel_flag] {
        try {
            outcome->signed_spend = std::make_unique<util::Result<agent::AllotmentSignedSpend>>(agent::CreateSignedAllotmentSpendFromBundleOutputs(
                outcome->spend_context,
                agent::AllotmentBundleSpendRequest{
                    .destination = destination,
                    .spend_amount = spend_amount,
                    .spent_today = spent_today,
                    .prove = true,
                    .anchor = anchor,
                    .cancel = [cancel_flag] { return cancel_flag && cancel_flag->load(); },
                    .allow_cpu_txpow = allow_cpu_txpow,
                },
                consensus));
        } catch (const std::exception& e) {
            outcome->signed_spend = std::make_unique<util::Result<agent::AllotmentSignedSpend>>(util::Error{Untranslated(std::string{e.what()})});
        } catch (...) {
            outcome->signed_spend = std::make_unique<util::Result<agent::AllotmentSignedSpend>>(util::Error{Untranslated("Agent spend preparation failed.")});
        }
    })};
    connect(thread, &QThread::finished, this, [this, outcome, generation] {
        finishLocalAgentSpend(outcome, generation);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void AgentAllotmentPage::cancelAgentSpendLocally()
{
    if (!m_spend_in_flight) return;
    if (m_spend_cancel) m_spend_cancel->store(true);
    ++m_spend_generation;
    m_spend_in_flight = false;
    updateAgentSpendCommandState();
    SetLabelClass(m_spend_command_state, QStringLiteral("policyReviewError"));
    m_spend_command_state->setText(tr("Agent spend preparation was stopped. Nothing was signed."));
}

void AgentAllotmentPage::finishLocalAgentSpend(const std::shared_ptr<LocalAgentSpendOutcome>& outcome, quint64 generation)
{
    if (generation != m_spend_generation || !m_spend_in_flight || !outcome || !m_spend_command_state) return;

    m_spend_in_flight = false;
    m_spend_cancel.reset();
    updateAgentSpendCommandState();

    if (!outcome->signed_spend || !*outcome->signed_spend) {
        SetLabelClass(m_spend_command_state, QStringLiteral("policyReviewError"));
        const std::string message{outcome->signed_spend ? util::ErrorString(*outcome->signed_spend).translated
                                                        : std::string{"Agent spend preparation did not finish."}};
        m_spend_command_state->setText(QString::fromStdString(message));
        return;
    }
    const agent::AllotmentSignedSpend& signed_spend{outcome->signed_spend->value()};

    auto receipt_store{LoadAgentPaymentReceiptStore()};
    if (!receipt_store) {
        SetLabelClass(m_spend_command_state, QStringLiteral("policyReviewError"));
        m_spend_command_state->setText(QString::fromStdString(util::ErrorString(receipt_store).translated));
        return;
    }

    const QString txid{QString::fromStdString(signed_spend.transaction.GetHash().ToString())};
    std::optional<agent::AllotmentPaymentReceiptArtifact> change_receipt;
    size_t stored_spent_receipts_removed{0};
    size_t stored_change_receipts_added{0};
    bool receipt_store_saved{false};
    const int64_t activity_time{QDateTime::currentSecsSinceEpoch()};
    std::vector<agent::AllotmentPaymentReceiptArtifact> spent_receipts;
    const std::vector<agent::AllotmentFundingOutputArtifact> spent_outputs{FundingOutputsFromSpendInputs(signed_spend.inputs)};
    stored_spent_receipts_removed = agent::RemoveSpentReceipts(receipt_store->receipts, spent_outputs, &spent_receipts);
    agent::AppendSpentActivities(*receipt_store, outcome->spend_context.bundle.funding_address, spent_outputs, spent_receipts, activity_time, txid.toStdString());
    if (signed_spend.change_amount > 0) {
        change_receipt = agent::AllotmentPaymentReceiptArtifact{
            .chain = Params().GetChainTypeString(),
            .genesis_hash = Params().GenesisBlock().GetHash().ToString(),
            .funding_address = outcome->spend_context.bundle.funding_address,
            .funding_output = {
                .txid = txid.toStdString(),
                .vout = 1,
                .amount = signed_spend.change_amount,
            },
            .received_time = activity_time,
            .payment_id = outcome->spend_context.bundle.policy_request.id + ":change:" + txid.toStdString() + ":1",
            .label = outcome->spend_context.bundle.policy_request.label,
            .memo = "agent spend change",
            .payer = "desktop vault",
        };
        std::vector<agent::AllotmentPaymentReceiptArtifact> change_receipts_added;
        auto added{AddReceiptsToStore(*receipt_store, {*change_receipt}, &change_receipts_added)};
        if (!added) {
            SetLabelClass(m_spend_command_state, QStringLiteral("policyReviewError"));
            m_spend_command_state->setText(QString::fromStdString(util::ErrorString(added).translated));
            return;
        }
        stored_change_receipts_added = *added;
        for (const agent::AllotmentPaymentReceiptArtifact& receipt : change_receipts_added) {
            receipt_store->activities.push_back(MakeReceiptActivity(agent::AllotmentReceiptActivityType::CHANGE, receipt, activity_time, txid.toStdString()));
        }
    }
    if (!spent_outputs.empty() || stored_change_receipts_added > 0) {
        auto saved{SaveAgentPaymentReceiptStore(*receipt_store)};
        if (!saved) {
            SetLabelClass(m_spend_command_state, QStringLiteral("policyReviewError"));
            m_spend_command_state->setText(QString::fromStdString(util::ErrorString(saved).translated));
            return;
        }
        receipt_store_saved = true;
    }

    const CSerializedNetMsg tx_message{agent::MakeTxMessage(signed_spend.transaction)};
    const CInv inventory{MSG_WTX, signed_spend.transaction.GetWitnessHash()};
    const CSerializedNetMsg inv_message{agent::MakeTxInvMessage(std::span{&inventory, 1})};

    QStringList output;
    output << QStringLiteral("policy_id=%1").arg(QString::fromStdString(outcome->spend_context.bundle.policy_request.id));
    output << QStringLiteral("funding_address=%1").arg(QString::fromStdString(outcome->spend_context.bundle.funding_address));
    output << QStringLiteral("selected_input_count=%1").arg(QString::number(signed_spend.inputs.size()));
    for (size_t i{0}; i < signed_spend.inputs.size(); ++i) {
        const agent::AllotmentSpendInput& input{signed_spend.inputs[i]};
        output << QStringLiteral("selected_input_%1=%2:%3")
                      .arg(QString::number(i),
                           QString::fromStdString(input.prevout.hash.ToString()),
                           QString::number(input.prevout.n));
        output << QStringLiteral("selected_input_%1_amount_cinnabar=%2")
                      .arg(QString::number(i),
                           QString::fromStdString(util::ToString(input.amount)));
    }
    output << QStringLiteral("input_amount_cinnabar=%1").arg(QString::fromStdString(util::ToString(signed_spend.input_amount)));
    output << QStringLiteral("spend_amount_cinnabar=%1").arg(QString::fromStdString(util::ToString(outcome->spend_amount)));
    output << QStringLiteral("change_amount_cinnabar=%1").arg(QString::fromStdString(util::ToString(signed_spend.change_amount)));
    output << QStringLiteral("policy_result=%1").arg(QString::fromStdString(agent::AllotmentPolicyResultCodeString(signed_spend.policy_check.code)));
    output << QStringLiteral("proved=true");
    output << QStringLiteral("anchor_height=%1").arg(QString::number(signed_spend.transaction.nAnchorHeight));
    output << QStringLiteral("anchor_hash=%1").arg(QString::fromStdString(outcome->anchor_hash.ToString()));
    output << QStringLiteral("hex=%1").arg(QString::fromStdString(EncodeHexTx(signed_spend.transaction)));
    output << QStringLiteral("tx_payload=%1").arg(QString::fromStdString(agent::AgentMessagePayloadHex(tx_message)));
    output << QStringLiteral("inv_payload=%1").arg(QString::fromStdString(agent::AgentMessagePayloadHex(inv_message)));
    if (change_receipt.has_value()) {
        output << QStringLiteral("change_paymentreceipt=%1").arg(AgentPaymentReceiptJson(*change_receipt));
    }
    output << QStringLiteral("receipt_store_saved=%1").arg(receipt_store_saved ? QStringLiteral("true") : QStringLiteral("false"));
    output << QStringLiteral("receipt_store_spent_removed=%1").arg(QString::number(stored_spent_receipts_removed));
    output << QStringLiteral("receipt_store_change_added=%1").arg(QString::number(stored_change_receipts_added));
    output << QStringLiteral("receipt_activity_count=%1").arg(QString::number(receipt_store->activities.size()));
    output << QStringLiteral("receipt_store=%1").arg(QString::fromStdString(fs::PathToString(AgentPaymentReceiptStorePath())));

    m_signed_spend_edit->setPlainText(output.join(QLatin1Char('\n')));
    reviewSignedAgentSpend();
    refreshStoredAgentUtxos();

    SetLabelClass(m_spend_command_state, QStringLiteral("policyReviewValid"));
    m_spend_command_state->setText(tr("Agent spend signed locally for %1 and loaded into signed-spend review.")
                                       .arg(QString::fromStdString(outcome->spend_context.bundle.policy_request.id)));
}

void AgentAllotmentPage::reviewSignedAgentSpend()
{
    if (!m_signed_spend_edit || !m_signed_spend_state) return;

    QString error;
    const std::optional<AgentSignedSpendReview> review{DecodeAgentSignedSpendText(m_signed_spend_edit->toPlainText(), error)};
    if (!review) {
        SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewError"));
        m_signed_spend_state->setText(error);
        m_signed_spend_review_valid = false;
        if (m_signed_spend_copy_relay_button) m_signed_spend_copy_relay_button->setEnabled(false);
        if (m_signed_spend_copy_stored_peer_command_button) m_signed_spend_copy_stored_peer_command_button->setEnabled(false);
        updatePeerRelayCommandState();
        if (m_signed_spend_submit_button) m_signed_spend_submit_button->setEnabled(false);
        return;
    }

    m_signed_spend_review_valid = true;
    SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewValid"));
    QString review_status = tr("Valid signed spend %1 with %2 input(s), %3 output(s), and %4 total output. Submit starts consensus-backed relay.")
                                .arg(QString::fromStdString(review->tx->GetHash().ToString()),
                                     QString::number(review->tx->vin.size()),
                                     QString::number(review->tx->vout.size()),
                                     QuicksilverUnits::formatWithUnit(m_display_unit, review->output_total, false, QuicksilverUnits::SeparatorStyle::ALWAYS));
    const std::optional<QString> change_receipt_json{ExtractAgentChangePaymentReceiptJson(m_signed_spend_edit->toPlainText())};
    if (change_receipt_json.has_value() && !change_receipt_json->isEmpty()) {
        auto change_receipt{agent::DecodeAllotmentPaymentReceipt(
            change_receipt_json->toStdString(),
            Params().GetChainTypeString(),
            Params().GenesisBlock().GetHash().ToString())};
        if (!change_receipt) {
            SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewError"));
            m_signed_spend_state->setText(tr("Signed spend is valid, but the change receipt was not saved: %1")
                                              .arg(QString::fromStdString(util::ErrorString(change_receipt).translated)));
            m_signed_spend_review_valid = false;
            if (m_signed_spend_copy_relay_button) m_signed_spend_copy_relay_button->setEnabled(false);
            if (m_signed_spend_copy_stored_peer_command_button) m_signed_spend_copy_stored_peer_command_button->setEnabled(false);
            updatePeerRelayCommandState();
            if (m_signed_spend_submit_button) m_signed_spend_submit_button->setEnabled(false);
            return;
        }

        auto receipt_path{SaveAgentPaymentReceiptToInbox(*change_receipt, *change_receipt_json)};
        if (!receipt_path) {
            SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewError"));
            m_signed_spend_state->setText(tr("Signed spend is valid, but the change receipt was not saved: %1")
                                              .arg(QString::fromStdString(util::ErrorString(receipt_path).translated)));
            m_signed_spend_review_valid = false;
            if (m_signed_spend_copy_relay_button) m_signed_spend_copy_relay_button->setEnabled(false);
            if (m_signed_spend_copy_stored_peer_command_button) m_signed_spend_copy_stored_peer_command_button->setEnabled(false);
            updatePeerRelayCommandState();
            if (m_signed_spend_submit_button) m_signed_spend_submit_button->setEnabled(false);
            return;
        }

        review_status += QStringLiteral(" ") + tr("Change receipt saved to %1.")
                                               .arg(QString::fromStdString(fs::PathToString(*receipt_path)));
        refreshStoredAgentUtxos();
    }
    m_signed_spend_state->setText(review_status);
    if (m_signed_spend_copy_relay_button) m_signed_spend_copy_relay_button->setEnabled(true);
    if (m_signed_spend_copy_stored_peer_command_button) m_signed_spend_copy_stored_peer_command_button->setEnabled(true);
    updatePeerRelayCommandState();
    if (m_signed_spend_submit_button) m_signed_spend_submit_button->setEnabled(m_model != nullptr);
}

void AgentAllotmentPage::copySignedSpendRelayPayloads()
{
    if (!m_signed_spend_edit || !m_signed_spend_state) return;

    QString error;
    const std::optional<AgentSignedSpendReview> review{DecodeAgentSignedSpendText(m_signed_spend_edit->toPlainText(), error)};
    if (!review) {
        SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewError"));
        m_signed_spend_state->setText(error);
        m_signed_spend_review_valid = false;
        if (m_signed_spend_copy_relay_button) m_signed_spend_copy_relay_button->setEnabled(false);
        if (m_signed_spend_copy_stored_peer_command_button) m_signed_spend_copy_stored_peer_command_button->setEnabled(false);
        updatePeerRelayCommandState();
        if (m_signed_spend_submit_button) m_signed_spend_submit_button->setEnabled(false);
        return;
    }

    m_signed_spend_review_valid = true;
    const CSerializedNetMsg tx_message{agent::MakeTxMessage(*review->tx)};
    const CInv inventory{MSG_WTX, review->tx->GetWitnessHash()};
    const CSerializedNetMsg inv_message{agent::MakeTxInvMessage(std::span{&inventory, 1})};
    const QString payloads = QStringLiteral("tx_payload=%1\ninv_payload=%2")
                                 .arg(QString::fromStdString(agent::AgentMessagePayloadHex(tx_message)),
                                      QString::fromStdString(agent::AgentMessagePayloadHex(inv_message)));
    QApplication::clipboard()->setText(payloads);

    SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewValid"));
    m_signed_spend_state->setText(tr("Relay payloads copied for node-free transport handoff."));
    if (m_signed_spend_copy_relay_button) m_signed_spend_copy_relay_button->setEnabled(true);
    if (m_signed_spend_copy_stored_peer_command_button) m_signed_spend_copy_stored_peer_command_button->setEnabled(true);
    updatePeerRelayCommandState();
    if (m_signed_spend_submit_button) m_signed_spend_submit_button->setEnabled(m_model != nullptr);
}

void AgentAllotmentPage::copyRelayPeerAddCommand()
{
    if (!m_relay_peer_edit || !m_signed_spend_state) return;

    const QString peer = m_relay_peer_edit->text().trimmed();
    if (peer.isEmpty()) {
        SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewError"));
        m_signed_spend_state->setText(tr("Enter a relay peer before copying a save-peer command."));
        updatePeerRelayCommandState();
        return;
    }
    if (ContainsSpace(peer)) {
        SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewError"));
        m_signed_spend_state->setText(tr("Relay peer must be a host[:port] value without spaces."));
        updatePeerRelayCommandState();
        return;
    }

    QApplication::clipboard()->setText(RelayPeerAddCommand(peer));

    SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewValid"));
    m_signed_spend_state->setText(tr("Relay peer save command copied for %1.").arg(peer));
    updatePeerRelayCommandState();
}

void AgentAllotmentPage::copyRelayPeerDiscoveryCommand()
{
    if (!m_relay_peer_edit || !m_signed_spend_state) return;

    const QString peer = m_relay_peer_edit->text().trimmed();
    if (!peer.isEmpty() && ContainsSpace(peer)) {
        SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewError"));
        m_signed_spend_state->setText(tr("Relay peer must be a host[:port] value without spaces."));
        updatePeerRelayCommandState();
        return;
    }

    QApplication::clipboard()->setText(RelayPeerDiscoveryCommand(peer.isEmpty() ? std::optional<QString>{} : std::optional<QString>{peer}));

    SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewValid"));
    if (peer.isEmpty()) {
        m_signed_spend_state->setText(tr("Stored-peer discovery command copied."));
    } else {
        m_signed_spend_state->setText(tr("Relay peer discovery command copied for %1.").arg(peer));
    }
    updatePeerRelayCommandState();
}

void AgentAllotmentPage::importNodePeersToAgentStore()
{
    if (!m_signed_spend_state) return;
    if (!m_model) {
        SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewError"));
        m_signed_spend_state->setText(tr("Open a vault before importing local-node peers."));
        updatePeerRelayCommandState();
        return;
    }

    std::vector<CService> node_peers{m_model->node().getNodeAddresses(64)};
    if (node_peers.empty()) {
        SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewError"));
        m_signed_spend_state->setText(tr("No local-node peers are available to import."));
        updatePeerRelayCommandState();
        return;
    }

    auto stored_peers{LoadAgentRelayPeers()};
    if (!stored_peers) {
        SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewError"));
        m_signed_spend_state->setText(QString::fromStdString(util::ErrorString(stored_peers).translated));
        updatePeerRelayCommandState();
        return;
    }

    size_t imported{0};
    for (const CService& peer : node_peers) {
        if (PushUniqueRelayPeer(*stored_peers, peer)) {
            ++imported;
        }
    }
    SortRelayPeers(*stored_peers);

    auto saved{SaveAgentRelayPeers(*stored_peers)};
    if (!saved) {
        SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewError"));
        m_signed_spend_state->setText(QString::fromStdString(util::ErrorString(saved).translated));
        updatePeerRelayCommandState();
        return;
    }

    SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewValid"));
    m_signed_spend_state->setText(tr("Local-node peers imported: %1 new of %2 available. Agent peer store: %3.")
                                      .arg(QString::number(imported),
                                           QString::number(node_peers.size()),
                                           QString::fromStdString(fs::PathToString(*saved))));
    updatePeerRelayCommandState();
}

void AgentAllotmentPage::copyNodeAddressImportCommand()
{
    if (!m_signed_spend_state) return;

    QApplication::clipboard()->setText(NodeAddressImportCommand());

    SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewValid"));
    m_signed_spend_state->setText(tr("Local-node peer import command copied."));
    updatePeerRelayCommandState();
}

void AgentAllotmentPage::copyHeaderPeerSyncCommand()
{
    if (!m_relay_peer_edit || !m_signed_spend_state) return;

    const QString peer = m_relay_peer_edit->text().trimmed();
    if (!peer.isEmpty() && ContainsSpace(peer)) {
        SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewError"));
        m_signed_spend_state->setText(tr("Relay peer must be a host[:port] value without spaces."));
        updatePeerRelayCommandState();
        return;
    }

    QApplication::clipboard()->setText(HeaderPeerSyncCommand(peer.isEmpty() ? std::optional<QString>{} : std::optional<QString>{peer}));

    SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewValid"));
    if (peer.isEmpty()) {
        m_signed_spend_state->setText(tr("Stored-peer header sync command copied."));
    } else {
        m_signed_spend_state->setText(tr("Relay peer header sync command copied for %1.").arg(peer));
    }
    updatePeerRelayCommandState();
}

void AgentAllotmentPage::copySignedSpendPeerRelayCommand()
{
    if (!m_relay_peer_edit || !m_signed_spend_edit || !m_signed_spend_state) return;

    const QString peer = m_relay_peer_edit->text().trimmed();
    if (peer.isEmpty()) {
        SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewError"));
        m_signed_spend_state->setText(tr("Enter a relay peer before copying a peer relay command."));
        updatePeerRelayCommandState();
        return;
    }
    if (ContainsSpace(peer)) {
        SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewError"));
        m_signed_spend_state->setText(tr("Relay peer must be a host[:port] value without spaces."));
        updatePeerRelayCommandState();
        return;
    }

    QString error;
    const std::optional<AgentSignedSpendReview> review{DecodeAgentSignedSpendText(m_signed_spend_edit->toPlainText(), error)};
    if (!review) {
        SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewError"));
        m_signed_spend_state->setText(error);
        m_signed_spend_review_valid = false;
        if (m_signed_spend_copy_relay_button) m_signed_spend_copy_relay_button->setEnabled(false);
        if (m_signed_spend_copy_stored_peer_command_button) m_signed_spend_copy_stored_peer_command_button->setEnabled(false);
        updatePeerRelayCommandState();
        if (m_signed_spend_submit_button) m_signed_spend_submit_button->setEnabled(false);
        return;
    }

    m_signed_spend_review_valid = true;
    const CSerializedNetMsg tx_message{agent::MakeTxMessage(*review->tx)};
    QApplication::clipboard()->setText(SignedSpendPeerRelayCommand(QString::fromStdString(agent::AgentMessagePayloadHex(tx_message)), peer));

    SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewValid"));
    m_signed_spend_state->setText(tr("Peer relay command copied for %1.").arg(peer));
    if (m_signed_spend_copy_relay_button) m_signed_spend_copy_relay_button->setEnabled(true);
    if (m_signed_spend_copy_stored_peer_command_button) m_signed_spend_copy_stored_peer_command_button->setEnabled(true);
    updatePeerRelayCommandState();
    if (m_signed_spend_submit_button) m_signed_spend_submit_button->setEnabled(m_model != nullptr);
}

void AgentAllotmentPage::copySignedSpendStoredPeerRelayCommand()
{
    if (!m_signed_spend_edit || !m_signed_spend_state) return;

    QString error;
    const std::optional<AgentSignedSpendReview> review{DecodeAgentSignedSpendText(m_signed_spend_edit->toPlainText(), error)};
    if (!review) {
        SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewError"));
        m_signed_spend_state->setText(error);
        m_signed_spend_review_valid = false;
        if (m_signed_spend_copy_relay_button) m_signed_spend_copy_relay_button->setEnabled(false);
        if (m_signed_spend_copy_stored_peer_command_button) m_signed_spend_copy_stored_peer_command_button->setEnabled(false);
        updatePeerRelayCommandState();
        if (m_signed_spend_submit_button) m_signed_spend_submit_button->setEnabled(false);
        return;
    }

    m_signed_spend_review_valid = true;
    const CSerializedNetMsg tx_message{agent::MakeTxMessage(*review->tx)};
    QApplication::clipboard()->setText(SignedSpendPeerRelayCommand(QString::fromStdString(agent::AgentMessagePayloadHex(tx_message)), std::nullopt));

    SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewValid"));
    m_signed_spend_state->setText(tr("Stored-peer relay command copied for the reviewed signed spend."));
    if (m_signed_spend_copy_relay_button) m_signed_spend_copy_relay_button->setEnabled(true);
    if (m_signed_spend_copy_stored_peer_command_button) m_signed_spend_copy_stored_peer_command_button->setEnabled(true);
    updatePeerRelayCommandState();
    if (m_signed_spend_submit_button) m_signed_spend_submit_button->setEnabled(m_model != nullptr);
}

void AgentAllotmentPage::relaySignedSpendToConfiguredPeers()
{
    if (!m_relay_peer_edit || !m_signed_spend_edit || !m_signed_spend_state || m_peer_relay_in_flight) return;
    if (!m_peer_relay) {
        SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewError"));
        m_signed_spend_state->setText(tr("Background peer relay is not available."));
        updatePeerRelayCommandState();
        return;
    }

    QString error;
    std::optional<CSerializedNetMsg> tx_message;
    const std::optional<QString> tx_payload_hex{ExtractAgentTransactionPayloadHex(m_signed_spend_edit->toPlainText())};
    if (tx_payload_hex.has_value()) {
        const agent::AgentMessageDecodeResult message{agent::DecodeAgentMessage(NetMsgType::TX, tx_payload_hex->toStdString())};
        if (!message.ok()) {
            error = tr("Signed spend tx_payload is not valid hex.");
        } else {
            const agent::TxMessageDecodeResult decoded_tx{agent::DecodeTxMessage(message.message)};
            if (!decoded_tx.ok() || !decoded_tx.transaction) {
                error = tr("Signed spend tx_payload is not a valid Quicksilver transaction payload.");
            } else {
                tx_message.emplace(message.message.Copy());
            }
        }
    } else if (const std::optional<AgentSignedSpendReview> review{DecodeAgentSignedSpendText(m_signed_spend_edit->toPlainText(), error)}) {
        tx_message = agent::MakeTxMessage(*review->tx);
    }
    if (!tx_message.has_value()) {
        SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewError"));
        m_signed_spend_state->setText(error);
        m_signed_spend_review_valid = false;
        if (m_signed_spend_copy_relay_button) m_signed_spend_copy_relay_button->setEnabled(false);
        if (m_signed_spend_copy_stored_peer_command_button) m_signed_spend_copy_stored_peer_command_button->setEnabled(false);
        updatePeerRelayCommandState();
        if (m_signed_spend_submit_button) m_signed_spend_submit_button->setEnabled(false);
        return;
    }

    std::vector<CService> peers;
    const QString peer_text = m_relay_peer_edit->text().trimmed();
    if (!peer_text.isEmpty()) {
        if (ContainsSpace(peer_text)) {
            SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewError"));
            m_signed_spend_state->setText(tr("Relay peer must be a host[:port] value without spaces."));
            updatePeerRelayCommandState();
            return;
        }
        const std::optional<CService> peer{Lookup(peer_text.toStdString(), Params().GetDefaultPort(), /*fAllowLookup=*/true)};
        if (!peer.has_value() || !peer->IsValid()) {
            SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewError"));
            m_signed_spend_state->setText(tr("Relay peer is not a valid host[:port] value."));
            updatePeerRelayCommandState();
            return;
        }
        peers.push_back(*peer);
    } else {
        auto stored_peers{LoadAgentRelayPeers()};
        if (!stored_peers) {
            SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewError"));
            m_signed_spend_state->setText(QString::fromStdString(util::ErrorString(stored_peers).translated));
            updatePeerRelayCommandState();
            return;
        }
        if (stored_peers->empty()) {
            try {
                peers = agent::FixedSeedPeers();
            } catch (const std::exception& e) {
                SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewError"));
                m_signed_spend_state->setText(tr("Could not load fixed relay seeds: %1")
                                                  .arg(QString::fromUtf8(e.what())));
                updatePeerRelayCommandState();
                return;
            }
            if (peers.empty()) {
                SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewError"));
                m_signed_spend_state->setText(tr("No relay peers are available. Enter a peer, import local-node peers, or configure a network with fixed seeds."));
                updatePeerRelayCommandState();
                return;
            }
        } else {
            peers = std::move(*stored_peers);
        }
    }

    m_peer_relay_in_flight = true;
    const quint64 generation{++m_peer_relay_generation};
    m_signed_spend_edit->setEnabled(false);
    m_relay_peer_edit->setEnabled(false);
    if (m_signed_spend_review_button) m_signed_spend_review_button->setEnabled(false);
    if (m_signed_spend_copy_relay_button) m_signed_spend_copy_relay_button->setEnabled(false);
    if (m_signed_spend_copy_peer_command_button) m_signed_spend_copy_peer_command_button->setEnabled(false);
    if (m_signed_spend_copy_stored_peer_command_button) m_signed_spend_copy_stored_peer_command_button->setEnabled(false);
    if (m_signed_spend_submit_button) m_signed_spend_submit_button->setEnabled(false);
    updatePeerRelayCommandState();
    SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewReady"));
    m_signed_spend_state->setText(tr("Relaying the reviewed signed spend to %1 peer(s) in the background…")
                                      .arg(QString::number(peers.size())));

    const PeerRelayFunction peer_relay{m_peer_relay};
    const auto timeout{std::chrono::milliseconds{agent::DEFAULT_AGENT_PEER_TIMEOUT_MS}};
    auto results{std::make_shared<std::vector<agent::PeerTransactionRelayResult>>()};
    results->reserve(peers.size());
    QThread* thread{QThread::create([peers = std::move(peers),
                                     tx_message = std::move(*tx_message),
                                     peer_relay,
                                     timeout,
                                     results]() mutable {
        for (const CService& peer : peers) {
            try {
                results->push_back(peer_relay(peer, tx_message, timeout));
            } catch (const std::exception& e) {
                results->push_back(agent::PeerTransactionRelayResult{
                    .peer = peer,
                    .error = e.what(),
                });
            } catch (...) {
                results->push_back(agent::PeerTransactionRelayResult{
                    .peer = peer,
                    .error = "unexpected relay transport failure",
                });
            }
        }
    })};
    connect(thread, &QThread::finished, this, [this, results, generation] {
        finishPeerRelay(std::move(*results), generation);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void AgentAllotmentPage::finishPeerRelay(std::vector<agent::PeerTransactionRelayResult> results, quint64 generation)
{
    if (generation != m_peer_relay_generation || !m_peer_relay_in_flight || !m_signed_spend_state) return;

    m_peer_relay_in_flight = false;
    if (m_signed_spend_edit) m_signed_spend_edit->setEnabled(true);
    if (m_relay_peer_edit) m_relay_peer_edit->setEnabled(true);
    if (m_signed_spend_review_button) m_signed_spend_review_button->setEnabled(true);
    if (m_signed_spend_copy_relay_button) m_signed_spend_copy_relay_button->setEnabled(m_signed_spend_review_valid);
    if (m_signed_spend_copy_stored_peer_command_button) m_signed_spend_copy_stored_peer_command_button->setEnabled(m_signed_spend_review_valid);
    if (m_signed_spend_submit_button) m_signed_spend_submit_button->setEnabled(m_signed_spend_review_valid && m_model != nullptr);

    size_t sent_count{0};
    QStringList failures;
    for (const agent::PeerTransactionRelayResult& result : results) {
        if (result.sent_tx) {
            ++sent_count;
        } else {
            failures << QStringLiteral("%1: %2")
                            .arg(QString::fromStdString(result.peer.ToStringAddrPort()),
                                 QString::fromStdString(result.error.empty() ? std::string{"relay failed"} : result.error));
        }
    }

    if (sent_count == 0) {
        SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewError"));
        m_signed_spend_state->setText(tr("Background peer relay failed for %1 peer(s): %2")
                                          .arg(QString::number(results.size()),
                                               failures.join(QStringLiteral("; "))));
        updatePeerRelayCommandState();
        return;
    }

    SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewValid"));
    if (failures.empty()) {
        m_signed_spend_state->setText(tr("Background peer relay sent the reviewed signed spend to %1 peer(s).")
                                          .arg(QString::number(sent_count)));
    } else {
        m_signed_spend_state->setText(tr("Background peer relay sent to %1 of %2 peer(s). Failures: %3")
                                          .arg(QString::number(sent_count),
                                               QString::number(results.size()),
                                               failures.join(QStringLiteral("; "))));
    }
    m_signed_spend_review_valid = true;
    updatePeerRelayCommandState();
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

    const bool imported{refreshStoredAgentUtxos(/*requested=*/true)};

    if (imported) {
        SetLabelClass(m_payment_receipt_state, QStringLiteral("policyReviewValid"));
        m_payment_receipt_state->setText(tr("Payment receipt saved to %1 and imported into the durable agent output store. External-agent scan command copied.")
                                             .arg(QString::fromStdString(fs::PathToString(*receipt_path))));
    } else {
        SetLabelClass(m_payment_receipt_state, QStringLiteral("policyReviewError"));
        m_payment_receipt_state->setText(tr("Payment receipt saved to %1, but the durable agent output refresh failed. See the UTXO status. External-agent scan command copied.")
                                             .arg(QString::fromStdString(fs::PathToString(*receipt_path))));
    }
}

bool AgentAllotmentPage::refreshStoredAgentUtxos(bool requested)
{
    if (!m_agent_utxo_state) return false;

    auto scan{agent::ScanAllotmentPaymentReceiptDirectory(
        AgentPaymentReceiptInboxDirectory(),
        AgentPaymentReceiptStorePath(),
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash())};
    if (!scan) {
        SetLabelClass(m_agent_utxo_state, QStringLiteral("policyReviewError"));
        m_agent_utxo_state->setText(tr("Agent UTXO refresh failed: %1")
                                         .arg(QString::fromStdString(util::ErrorString(scan).translated)));
        return false;
    }

    CAmount total{0};
    for (const agent::AllotmentPaymentReceiptArtifact& receipt : scan->store.receipts) {
        if (!MoneyRange(total + receipt.funding_output.amount)) {
            SetLabelClass(m_agent_utxo_state, QStringLiteral("policyReviewError"));
            m_agent_utxo_state->setText(tr("Agent UTXO refresh failed: stored output total is out of range."));
            return false;
        }
        total += receipt.funding_output.amount;
    }

    SetLabelClass(m_agent_utxo_state, QStringLiteral("policyReviewValid"));
    const QString formatted_total{QuicksilverUnits::formatWithUnit(m_display_unit, total, false, QuicksilverUnits::SeparatorStyle::ALWAYS)};
    QString status{tr("Spendable agent UTXOs: %1 totaling %2. Durable activity entries: %3.")
                       .arg(QString::number(scan->store.receipts.size()),
                            formatted_total,
                            QString::number(scan->store.activities.size()))};
    if (scan->imported_receipts > 0) {
        status += QStringLiteral(" ") + tr("Imported %1 new receipt(s) from %2 inbox file(s).")
                                           .arg(QString::number(scan->imported_receipts),
                                                QString::number(scan->scanned_files));
    } else if (requested) {
        status += QStringLiteral(" ") + tr("Inbox scan is current across %1 receipt file(s).")
                                           .arg(QString::number(scan->scanned_files));
    }
    m_agent_utxo_state->setText(status);
    return true;
}

void AgentAllotmentPage::submitSignedAgentSpend()
{
    if (!m_signed_spend_edit || !m_signed_spend_state) return;

    QString error;
    const std::optional<AgentSignedSpendReview> review{DecodeAgentSignedSpendText(m_signed_spend_edit->toPlainText(), error)};
    if (!review) {
        SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewError"));
        m_signed_spend_state->setText(error);
        m_signed_spend_review_valid = false;
        if (m_signed_spend_submit_button) m_signed_spend_submit_button->setEnabled(false);
        updatePeerRelayCommandState();
        return;
    }
    if (!m_model) {
        SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewError"));
        m_signed_spend_state->setText(tr("Open a vault before submitting a signed agent spend."));
        return;
    }

    const auto submitted{m_model->broadcastAgentAllotmentSignedSpend(review->tx)};
    if (!submitted.accepted) {
        SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewError"));
        m_signed_spend_state->setText(tr("Signed spend submission failed: %1").arg(submitted.error));
        return;
    }

    SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewValid"));
    m_signed_spend_state->setText(tr("Signed spend submitted. Transaction ID: %1").arg(submitted.txid));
    if (m_signed_spend_submit_button) m_signed_spend_submit_button->setEnabled(false);
}

void AgentAllotmentPage::updateRecordedSetups()
{
    if (!m_records_label) return;
    if (m_records_list) ClearLayout(m_records_list);
    if (!m_model) {
        m_records_label->setText(tr("Queued agent setups: none"));
        return;
    }

    const auto records = m_model->listAgentAllotmentRecords();
    if (records.empty()) {
        m_records_label->setText(tr("Queued agent setups: none"));
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
        summary += record.policy_status == vault::AgentAllotmentPolicyStatus::Enforced ? tr(", policy enforced") : tr(", policy pending");
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

            const bool policy_enforced{record.policy_status == vault::AgentAllotmentPolicyStatus::Enforced};
            auto* policy_state = new QLabel(policy_enforced ? tr("Policy enforced") : tr("Policy pending"), row);
            policy_state->setObjectName(QStringLiteral("agentAllotmentPolicyState"));
            policy_state->setProperty("class", QStringLiteral("launchCapabilityState"));
            policy_state->setToolTip(policy_enforced ? tr("This setup has an active child-key policy backend.") : tr("Child-key creation and limit enforcement are not active for this setup yet."));
            row_layout->addWidget(policy_state);

            const QString funding_text = funding_available <= 0 ? tr("%1 awaits funding up to %2. Policy integration remains pending.")
                                                                      .arg(QString::fromStdString(record.label), formatted_limit) :
                                         funding_confirmed ? tr("%1 has confirmed funding at its reserved address. Policy integration remains pending.")
                                                                 .arg(QString::fromStdString(record.label)) :
                                                             tr("%1 has %2 of %3 at its reserved address; %4 remains. Policy integration remains pending.")
                                                                 .arg(QString::fromStdString(record.label), formatted_available, formatted_limit, formatted_remaining);
            auto* row_label = MakeMutedLabel(funding_text, row);
            row_label->setObjectName(QStringLiteral("agentAllotmentFundingRowLabel"));
            row_layout->addWidget(row_label, 1);

            auto* fund_button = new QPushButton(funding_confirmed ? tr("Funding complete") : (funding_available > 0 ? tr("Fund remaining") : tr("Fund setup")), row);
            fund_button->setObjectName(QStringLiteral("agentAllotmentFundSetupButton"));
            fund_button->setProperty("class", QStringLiteral("secondaryActionButton"));
            fund_button->setEnabled(!funding_confirmed);
            fund_button->setToolTip(funding_confirmed ? tr("This setup's reserved address has at least the requested funding amount.") : tr("Prefills the transfer screen with this setup's reserved vault funding address and remaining requested funding amount."));
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

            const QString handoff_text = funding_confirmed ? tr("Copy a gateway bundle for this funded setup. The bundle includes the policy request, reserved funding key, and current spendable outputs.") : tr("Policy request export becomes available after the reserved address has the requested funding.");
            auto* handoff_label = MakeMutedLabel(handoff_text, handoff_row);
            handoff_label->setObjectName(QStringLiteral("agentAllotmentPolicyHandoffLabel"));
            handoff_layout->addWidget(handoff_label, 1);

            auto* copy_policy_button = new QPushButton(tr("Copy agent bundle"), handoff_row);
            copy_policy_button->setObjectName(QStringLiteral("agentAllotmentCopyPolicyButton"));
            copy_policy_button->setProperty("class", QStringLiteral("secondaryActionButton"));
            copy_policy_button->setEnabled(funding_confirmed);
            copy_policy_button->setToolTip(funding_confirmed ? tr("Copies a JSON policy, key, and funding-output bundle for the Agent Allotment Gateway handoff.") : tr("Fund this setup before copying a policy request."));
            handoff_layout->addWidget(copy_policy_button);

            auto* save_receipts_button = new QPushButton(tr("Save receipts"), handoff_row);
            save_receipts_button->setObjectName(QStringLiteral("agentAllotmentSaveFundingReceiptsButton"));
            save_receipts_button->setProperty("class", QStringLiteral("secondaryActionButton"));
            save_receipts_button->setEnabled(funding_confirmed);
            save_receipts_button->setToolTip(funding_confirmed ? tr("Writes this setup's current spendable funding outputs into the local quicksilver-agent receipt inbox.") : tr("Fund this setup before saving funding receipts."));
            handoff_layout->addWidget(save_receipts_button);

            auto* copy_recovery_button = new QPushButton(tr("Copy recovery import"), handoff_row);
            copy_recovery_button->setObjectName(QStringLiteral("agentAllotmentCopyRecoveryScanButton"));
            copy_recovery_button->setProperty("class", QStringLiteral("secondaryActionButton"));
            copy_recovery_button->setToolTip(tr("Copies a scantxoutset-to-importrecovery command for recovering spendable outputs at this setup's funding address."));
            handoff_layout->addWidget(copy_recovery_button);

            connect(copy_policy_button, &QPushButton::clicked, this, [this, record, funding_available] {
                if (!m_model) return;
                const QString policy_request{m_model->agentAllotmentPolicyRequest(record, funding_available)};
                const auto bundle{m_model->agentAllotmentPolicyBundle(policy_request)};
                if (!bundle) {
                    if (m_state_label) {
                        m_state_label->setText(tr("Agent bundle export failed: %1").arg(QString::fromStdString(util::ErrorString(bundle).original)));
                    }
                    return;
                }
                QApplication::clipboard()->setText(QString::fromStdString(bundle->bundle_json));
                if (m_state_label) {
                    m_state_label->setText(tr("Agent bundle copied with current spendable outputs."));
                }
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
                const bool imported{refreshStoredAgentUtxos(/*requested=*/true)};
                if (m_state_label) {
                    m_state_label->setText(imported ?
                                               tr("Agent funding receipts saved and imported: %1. External-agent scan command copied.").arg(QString::number(bundle->funding_outputs.size())) :
                                               tr("Agent funding receipts saved, but the durable output refresh failed. See the UTXO status. External-agent scan command copied."));
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
    m_records_label->setText(tr("Queued agent setups: %1").arg(summaries.join(QStringLiteral("; "))));
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
        m_state_label->setText(tr("Reserves a vault funding address and marks agent policy integration pending."));
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
        SetLabelClass(m_policy_review_state, QStringLiteral("policyReviewIdle"));
        m_policy_review_state->setText(tr("Open a vault before reviewing a policy request."));
    } else if (has_request) {
        SetLabelClass(m_policy_review_state, QStringLiteral("policyReviewReady"));
        m_policy_review_state->setText(tr("Ready to review pasted policy request."));
    } else {
        SetLabelClass(m_policy_review_state, QStringLiteral("policyReviewIdle"));
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
        SetLabelClass(m_payment_receipt_state, QStringLiteral("policyReviewIdle"));
        m_payment_receipt_state->setText(tr("Paste a payment receipt to review funding output metadata."));
    }
}

void AgentAllotmentPage::updateAgentSpendCommandState()
{
    if (!m_copy_spend_command_button || !m_sign_spend_button || !m_spend_bundle_edit || !m_spend_destination_edit || !m_spend_amount || !m_spent_today || !m_spend_command_state) return;

    bool spend_valid = false;
    const CAmount spend_amount = m_spend_amount->value(&spend_valid);
    bool spent_today_valid = true;
    if (!AmountFieldEmpty(m_spent_today)) {
        m_spent_today->value(&spent_today_valid);
    }
    const bool has_bundle = !m_spend_bundle_edit->toPlainText().trimmed().isEmpty();
    const bool has_destination = !m_spend_destination_edit->text().trimmed().isEmpty();
    const bool ready = has_bundle && has_destination && spend_valid && spend_amount > 0 && spent_today_valid;

    m_copy_spend_command_button->setEnabled(ready && !m_spend_in_flight);
    m_sign_spend_button->setEnabled(ready && !m_spend_in_flight);
    if (m_cancel_spend_button) m_cancel_spend_button->setEnabled(m_spend_in_flight);
    if (m_spend_bundle_edit) m_spend_bundle_edit->setReadOnly(m_spend_in_flight);
    if (m_spend_destination_edit) m_spend_destination_edit->setReadOnly(m_spend_in_flight);
    if (m_spend_amount) m_spend_amount->setEnabled(!m_spend_in_flight);
    if (m_spent_today) m_spent_today->setEnabled(!m_spend_in_flight);
    if (m_spend_in_flight) return;
    if (ready) {
        SetLabelClass(m_spend_command_state, QStringLiteral("policyReviewReady"));
        m_spend_command_state->setText(tr("Ready to sign locally or copy a signbundle command."));
    } else {
        SetLabelClass(m_spend_command_state, QStringLiteral("policyReviewIdle"));
        m_spend_command_state->setText(tr("Paste an agent bundle, destination, and spend amount to sign locally or copy a sign command."));
    }
}

void AgentAllotmentPage::updateSignedSpendReviewState()
{
    if (!m_signed_spend_review_button || !m_signed_spend_edit || !m_signed_spend_state) return;

    m_signed_spend_review_valid = false;
    const bool has_spend = !m_signed_spend_edit->toPlainText().trimmed().isEmpty();
    m_signed_spend_review_button->setEnabled(has_spend);
    if (m_signed_spend_copy_relay_button) m_signed_spend_copy_relay_button->setEnabled(false);
    if (m_signed_spend_copy_stored_peer_command_button) m_signed_spend_copy_stored_peer_command_button->setEnabled(false);
    updatePeerRelayCommandState();
    if (m_signed_spend_submit_button) m_signed_spend_submit_button->setEnabled(false);
    if (has_spend) {
        SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewReady"));
        m_signed_spend_state->setText(tr("Ready to review pasted signed spend output."));
    } else {
        SetLabelClass(m_signed_spend_state, QStringLiteral("policyReviewIdle"));
        m_signed_spend_state->setText(tr("Paste signed spend output from the agent gateway to review."));
    }
}

void AgentAllotmentPage::updatePeerRelayCommandState()
{
    if (!m_relay_peer_edit) return;

    const QString peer = m_relay_peer_edit->text().trimmed();
    const bool has_peer = !peer.isEmpty() && !ContainsSpace(peer);
    if (m_copy_add_peer_command_button) m_copy_add_peer_command_button->setEnabled(has_peer);
    if (m_copy_discover_peers_command_button) m_copy_discover_peers_command_button->setEnabled(peer.isEmpty() || has_peer);
    if (m_import_node_peers_button) m_import_node_peers_button->setEnabled(m_model != nullptr);
    if (m_copy_sync_headers_command_button) m_copy_sync_headers_command_button->setEnabled(peer.isEmpty() || has_peer);
    if (m_signed_spend_copy_peer_command_button) m_signed_spend_copy_peer_command_button->setEnabled(m_signed_spend_review_valid && has_peer);
    if (m_signed_spend_relay_peer_button) m_signed_spend_relay_peer_button->setEnabled(!m_peer_relay_in_flight && m_signed_spend_review_valid && (peer.isEmpty() || has_peer));
}
