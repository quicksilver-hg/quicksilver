// Copyright (c) 2015-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/util.h>
#include <qt/test/vaulttests.h>

#include <addresstype.h>
#include <agent/headerstore.h>
#include <base58.h>
#include <chainparams.h>
#include <common/args.h>
#include <core_io.h>
#include <interfaces/chain.h>
#include <interfaces/handler.h>
#include <interfaces/node.h>
#include <key.h>
#include <key_io.h>
#include <outputtype.h>
#include <psqt.h>
#include <streams.h>
#include <util/strencodings.h>
#include <qt/agentallotmentpage.h>
#include <qt/askpassphrasedialog.h>
#include <qt/createvaultdialog.h>
#include <qt/benchpanel.h>
#include <qt/clientmodel.h>
#include <qt/coincontroldialog.h>
#include <qt/addressbookpage.h>
#include <qt/desktoplaunchpage.h>
#include <qt/minemintpage.h>
#include <qt/miningmodel.h>
#include <qt/modaloverlay.h>
#include <qt/networkpage.h>
#include <qt/networkstyle.h>
#include <qt/quicksilvergui.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/rpcconsole.h>
#include <qt/vaultcontroller.h>
#include <qt/quicksilveramountfield.h>
#include <qt/quicksilverstyle.h>
#include <qt/quicksilverunits.h>
#include <qt/qvalidatedlineedit.h>
#include <qt/receivecoinsdialog.h>
#include <qt/peertablemodel.h>
#include <qt/signverifymessagedialog.h>
#include <qt/receiverequestdialog.h>
#include <qt/recentrequeststablemodel.h>
#include <qt/sendcoinsdialog.h>
#include <qt/sendcoinsentry.h>
#include <qt/transactionrecord.h>
#include <qt/transactiontablemodel.h>
#include <qt/transactionview.h>
#include <qt/thinvaultheadersource.h>
#include <qt/vaultframe.h>
#include <qt/vaultmodel.h>
#include <qt/vaultview.h>
#include <qt/utilitydialog.h>
#include <QTextBrowser>
#include <QTextBlock>
#include <QTextLayout>
#include <rpc/server.h>
#include <script/solver.h>
#include <test/util/setup_common.h>
#include <util/fs.h>
#include <util/readwritefile.h>
#include <univalue.h>
#include <validation.h>
#include <vault/coincontrol.h>
#include <vault/context.h>
#include <vault/spend.h>
#include <vault/test/util.h>
#include <vault/vault.h>
#include <vault/vaultdb.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QFontDatabase>
#include <QFrame>
#include <QGroupBox>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenuBar>
#include <QObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScopedPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QFontInfo>
#include <QStyleOption>
#include <QSpinBox>
#include <QSet>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QDir>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

using vault::AddVault;
using vault::CreateMockableVaultDatabase;
using vault::CVault;
using vault::RemoveVault;
using vault::VAULT_FLAG_DESCRIPTORS;
using vault::VAULT_FLAG_DISABLE_PRIVATE_KEYS;
using vault::VaultContext;
using vault::VaultDescriptor;
using vault::VaultRescanReserver;

namespace {
class ScopedNodeContext
{
public:
    ScopedNodeContext(interfaces::Node& node, node::NodeContext& context) : m_node(node), m_previous_context(node.context())
    {
        m_node.setContext(&context);
    }

    ~ScopedNodeContext()
    {
        m_node.setContext(m_previous_context);
    }

private:
    interfaces::Node& m_node;
    node::NodeContext* const m_previous_context;
};

class PollMarkerVault final : public interfaces::Vault
{
public:
    uint256 block_hash;
    CAmount balance{0};
    CAmount unconfirmed{0};
    CAmount immature{0};
    CAmount delegated{0};
    std::vector<vault::AgentAllotmentRecord> agent_records;
    int cosign_calls{0};
    CTransactionRef cosign_result;
    bool crypted{false};
    bool locked{false};
    bool unlock_ok{true};
    bool unlock_throws{false};
    bool transaction_creation_available{false};
    bool coin_listing_available{false};
    CoinsList coin_list;
    std::vector<interfaces::VaultTxOut> selected_coins;
    int marker_calls{0};
    int balance_calls{0};
    int auto_selection_bound_calls{0};
    bool auto_selection_bound_busy{false};
    bool auto_selection_bound_uninitialized{false};
    int selected_balance_calls{0};
    int try_list_coin_calls{0};
    int try_get_coin_calls{0};
    int create_calls{0};
    int commit_calls{0};
    int set_address_calls{0};
    std::optional<bilingual_str> commit_error;
    //! Set by the test so createTransaction can observe the model that is driving it.
    VaultModel* vault_model_under_test{nullptr};
    bool in_flight_during_create{false};
    bool cancel_requested_during_create{false};
    std::optional<bool> remove_load_on_start{true}; // an unset value the calls cannot produce

    bool encryptVault(const SecureString&) override { return false; }
    bool isCrypted() override { return crypted; }
    bool lock() override { return true; }
    bool unlock(const SecureString&) override
    {
        if (unlock_throws) throw std::runtime_error("unlock boom");
        return unlock_ok;
    }
    bool isLocked() override { return locked; }
    bool changeVaultPassphrase(const SecureString&, const SecureString&) override { return false; }
    void abortRescan() override {}
    bool backupVault(const std::string&) override { return false; }
    bool isBackupRecorded() override { return false; }
    bool setBackupRecorded(bool) override { return false; }
    util::Result<vault::AgentAllotmentRecord> recordAgentAllotmentSetup(const std::string&, CAmount, CAmount) override { return util::Error{Untranslated("unsupported")}; }
    std::vector<vault::AgentAllotmentRecord> listAgentAllotmentRecords() override { return agent_records; }
    std::string agentAllotmentPolicyRequest(const vault::AgentAllotmentRecord&, CAmount) override { return {}; }
    util::Result<vault::AgentAllotmentPolicyRequestMetadata> validateAgentAllotmentPolicyRequest(const std::string&) override { return util::Error{Untranslated("unsupported")}; }
    util::Result<vault::AgentAllotmentPolicyBundle> agentAllotmentPolicyBundle(const std::string&) override { return util::Error{Untranslated("unsupported")}; }
    util::Result<CTransactionRef> cosignAgentAllotmentSpend(const std::string&) override
    {
        ++cosign_calls;
        if (cosign_result) return cosign_result;
        return util::Error{Untranslated(agent_records.at(0).stopped_time ?
            "This agent allotment is stopped. The vault no longer co-signs for it." : "backend refusal verbatim")};
    }
    bool stopAgentAllotment(const std::string& id) override
    {
        for (auto& record : agent_records) if (record.id == id) { record.stopped_time = 1; return true; }
        return false;
    }
    std::string getVaultName() override { return "poll-marker-vault"; }
    util::Result<CTxDestination> getNewDestination(const OutputType, const std::string&) override { return CTxDestination{}; }
    bool getPubKey(const CScript&, const CKeyID&, CPubKey&) override { return false; }
    SigningResult signMessage(const std::string&, const PKHash&, std::string&) override { return SigningResult::PRIVATE_KEY_NOT_AVAILABLE; }
    bool isSpendable(const CTxDestination&) override { return false; }
    bool setAddressBook(const CTxDestination&, const std::string&, const std::optional<vault::AddressPurpose>&) override
    {
        ++set_address_calls;
        return true;
    }
    bool delAddressBook(const CTxDestination&) override { return false; }
    bool getAddress(const CTxDestination&, std::string*, vault::isminetype*, vault::AddressPurpose*) override { return false; }
    std::vector<interfaces::VaultAddress> getAddresses() override { return {}; }
    std::vector<std::string> getAddressReceiveRequests() override { return {}; }
    bool setAddressReceiveRequest(const CTxDestination&, const std::string&, const std::string&) override { return false; }
    util::Result<void> displayAddress(const CTxDestination&) override { return {}; }
    bool lockCoin(const COutPoint&, const bool) override { return false; }
    bool unlockCoin(const COutPoint&) override { return false; }
    bool isLockedCoin(const COutPoint&) override { return false; }
    void listLockedCoins(std::vector<COutPoint>&) override {}
    bool canCreateTransactionsNow() override { return transaction_creation_available; }
    bool canCreateTransactions() override { return transaction_creation_available; }
    util::Result<CTransactionRef> createTransaction(const std::vector<vault::CRecipient>&, const vault::CCoinControl&, bool, int&, const std::function<void(uint32_t nonce)>& = {}, const std::function<bool()>& tx_proof_cancel = {}) override
    {
        ++create_calls;
        // Stands in for the grind: records what the vault model reported while a
        // transfer was in flight, which is what the close paths consult.
        in_flight_during_create = vault_model_under_test && vault_model_under_test->proofOfWorkInFlight();
        cancel_requested_during_create = tx_proof_cancel && tx_proof_cancel();
        return util::Error{Untranslated("unsupported")};
    }
    util::Result<void> commitTransaction(CTransactionRef, interfaces::VaultValueMap, interfaces::VaultOrderForm) override
    {
        ++commit_calls;
        if (commit_error) return util::Error{*commit_error};
        return {};
    }
    bool transactionCanBeAbandoned(const uint256&) override { return false; }
    bool abandonTransaction(const uint256&) override { return false; }
    CTransactionRef getTx(const uint256&) override { return {}; }
    interfaces::VaultTx getVaultTx(const uint256&) override { return {}; }
    std::set<interfaces::VaultTx> getVaultTxs() override { return {}; }
    bool tryGetTxStatus(const uint256&, interfaces::VaultTxStatus&, int&, int64_t&) override { return false; }
    bool spendsUnconfirmedChange(const CTransaction&) override { return false; }
    bool tryGetVaultTxDetails(const uint256&, interfaces::VaultTx&, interfaces::VaultTxStatus&, interfaces::VaultOrderForm&, bool&, int&) override { return false; }
    std::optional<common::PSQTError> fillPSQT(int, bool, bool, size_t*, PartiallySignedQuicksilverTransaction&, bool&) override { return {}; }
    interfaces::VaultBalances getBalances() override
    {
        interfaces::VaultBalances balances;
        balances.balance = balance;
        balances.unconfirmed_balance = unconfirmed;
        balances.immature_balance = immature;
        balances.delegated_balance = delegated;
        return balances;
    }
    bool tryGetBalances(interfaces::VaultBalances& balances, uint256& hash) override
    {
        ++balance_calls;
        hash = block_hash;
        balances = getBalances();
        return true;
    }
    std::optional<CAmount> tryGetAutoSelectionBound(bool avoid_address_reuse) override
    {
        ++auto_selection_bound_calls;
        // This fake cannot certify reuse policy when used addresses are allowed.
        if (!avoid_address_reuse || auto_selection_bound_busy || auto_selection_bound_uninitialized || !MoneyRange(balance)) {
            return std::nullopt;
        }
        return balance;
    }
    bool tryGetBalanceUpdateBlockHash(uint256& hash) override
    {
        ++marker_calls;
        hash = block_hash;
        return true;
    }
    CAmount getAvailableBalance(const vault::CCoinControl&) override
    {
        ++selected_balance_calls;
        return balance;
    }
    vault::isminetype txinIsMine(const CTxIn&) override { return vault::ISMINE_NO; }
    vault::isminetype txoutIsMine(const CTxOut&) override { return vault::ISMINE_NO; }
    CAmount getDebit(const CTxIn&, vault::isminefilter) override { return 0; }
    CAmount getCredit(const CTxOut&, vault::isminefilter) override { return 0; }
    bool tryListCoins(CoinsList& coins) override
    {
        ++try_list_coin_calls;
        if (!coin_listing_available) return false;
        coins = coin_list;
        return true;
    }
    bool tryGetCoins(const std::vector<COutPoint>&, std::vector<interfaces::VaultTxOut>& coins) override
    {
        ++try_get_coin_calls;
        if (!coin_listing_available) return false;
        coins = selected_coins;
        return true;
    }
    bool hdEnabled() override { return true; }
    bool canGetAddresses() override { return true; }
    bool privateKeysDisabled() override { return false; }
    bool taprootEnabled() override { return true; }
    bool hasExternalSigner() override { return false; }
    OutputType getDefaultAddressType() override { return OutputType::BECH32M; }
    void remove(std::optional<bool> load_on_start = false) override { remove_load_on_start = load_on_start; }

    std::unique_ptr<interfaces::Handler> handleUnload(UnloadFn) override
    {
        return interfaces::MakeCleanupHandler([] {});
    }
    std::unique_ptr<interfaces::Handler> handleShowProgress(ShowProgressFn) override
    {
        return interfaces::MakeCleanupHandler([] {});
    }
    std::unique_ptr<interfaces::Handler> handleStatusChanged(StatusChangedFn) override
    {
        return interfaces::MakeCleanupHandler([] {});
    }
    std::unique_ptr<interfaces::Handler> handleAddressBookChanged(AddressBookChangedFn) override
    {
        return interfaces::MakeCleanupHandler([] {});
    }
    std::unique_ptr<interfaces::Handler> handleTransactionChanged(TransactionChangedFn) override
    {
        return interfaces::MakeCleanupHandler([] {});
    }
    std::unique_ptr<interfaces::Handler> handleCanGetAddressesChanged(CanGetAddressesChangedFn) override
    {
        return interfaces::MakeCleanupHandler([] {});
    }
};

//! Press "Yes" or "Cancel" buttons in modal send confirmation dialog.
bool ConfirmOpenSendDialog(QString* text = nullptr, QMessageBox::StandardButton confirm_type = QMessageBox::Yes)
{
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->inherits("SendConfirmationDialog")) {
            SendConfirmationDialog* dialog = qobject_cast<SendConfirmationDialog*>(widget);
            if (text) *text = dialog->text();
            QAbstractButton* button = dialog->button(confirm_type);
            button->setEnabled(true);
            button->click();
            return true;
        }
    }
    return false;
}

//! Transfer funds to address and return txid.
uint256 SendCoins(CVault& vault, SendCoinsDialog& sendCoinsDialog, const CTxDestination& address, CAmount amount,
                  QMessageBox::StandardButton confirm_type = QMessageBox::Yes,
                  QString* confirmation_text = nullptr)
{
    QVBoxLayout* entries = sendCoinsDialog.findChild<QVBoxLayout*>("entries");
    SendCoinsEntry* entry = qobject_cast<SendCoinsEntry*>(entries->itemAt(0)->widget());
    entry->findChild<QValidatedLineEdit*>("payTo")->setText(QString::fromStdString(EncodeDestination(address)));
    entry->findChild<QuicksilverAmountField*>("payAmount")->setValue(amount);
    uint256 txid;
    boost::signals2::scoped_connection c(vault.NotifyTransactionChanged.connect([&txid](const uint256& hash, ChangeType status) {
        if (status == CT_NEW) txid = hash;
    }));

    QSignalSpy prepare_finished(&sendCoinsDialog, &SendCoinsDialog::sendPreparationFinishedForTesting);
    QSignalSpy confirmation_ready(&sendCoinsDialog, &SendCoinsDialog::prepareSendConfirmationReadyForTesting);
    // `sendCoinsDialog` outlives this function, so a connection made directly to it
    // survives the call. Every SendCoins() added another one, and the next call's
    // signal then re-entered every stale handler with references into a frame that
    // had already returned -- ASan: stack-use-after-return on `confirmation_text`,
    // reached through the deferred single-shot below. Scoping both to a local
    // context object severs them on every exit path, including the early returns,
    // the same way the scoped_connection above bounds the vault signal.
    QObject confirmation_scope;
    QObject::connect(&sendCoinsDialog, &SendCoinsDialog::prepareSendConfirmationReadyForTesting, &confirmation_scope, [&]() {
        QTimer::singleShot(0, &confirmation_scope, [&]() {
            QVERIFY(ConfirmOpenSendDialog(confirmation_text, confirm_type));
        });
    });

    bool invoked = QMetaObject::invokeMethod(&sendCoinsDialog, "sendButtonClicked", Q_ARG(bool, false));
    assert(invoked);
    QFrame* progress_panel = sendCoinsDialog.findChild<QFrame*>(QStringLiteral("sendWorkProgressPanel"));
    QLabel* progress_status = sendCoinsDialog.findChild<QLabel*>(QStringLiteral("sendWorkStatusLabel"));
    QLabel* progress_elapsed = sendCoinsDialog.findChild<QLabel*>(QStringLiteral("sendWorkElapsedLabel"));
    QLabel* progress_graphs = sendCoinsDialog.findChild<QLabel*>(QStringLiteral("sendWorkGraphsLabel"));
    QProgressBar* progress_bar = sendCoinsDialog.findChild<QProgressBar*>(QStringLiteral("sendWorkProgressBar"));
    QPushButton* progress_cancel = sendCoinsDialog.findChild<QPushButton*>(QStringLiteral("sendWorkCancelButton"));
    assert(progress_panel);
    assert(progress_status);
    assert(progress_elapsed);
    assert(progress_graphs);
    assert(progress_bar);
    assert(progress_cancel);
    assert(!progress_panel->isHidden());
    assert(progress_status->text() == QStringLiteral("Solving transfer proof-of-work"));
    assert(progress_elapsed->text().startsWith(QStringLiteral("Elapsed: ")));
    assert(progress_graphs->text() == QStringLiteral("Graphs tried: pending"));
    assert(progress_bar->minimum() == 0);
    assert(progress_bar->maximum() == 0);
    assert(progress_cancel->isEnabled());
    if (prepare_finished.count() == 0 && !prepare_finished.wait(30000)) {
        const QString diagnostic = QStringLiteral("send preparation did not finish in 30 seconds (no_cycle=%1, status='%2', graphs='%3')")
                                       .arg(Params().GetConsensus().fTxPowNoCycle)
                                       .arg(progress_status->text(), progress_graphs->text());
        QTest::qFail(qPrintable(diagnostic), __FILE__, __LINE__);
        return {};
    }
    const QList<QVariant> prepare_result = prepare_finished.takeFirst();
    const auto prepare_status = static_cast<VaultModel::StatusCode>(prepare_result.at(0).toInt());
    if (prepare_status != VaultModel::OK) {
        const QString diagnostic = QStringLiteral("send preparation failed with status %1: %2")
                                       .arg(static_cast<int>(prepare_status))
                                       .arg(prepare_result.at(1).toString());
        QTest::qFail(qPrintable(diagnostic), __FILE__, __LINE__);
        return {};
    }
    if (confirmation_ready.count() != 1) {
        QTest::qFail("successful send preparation did not open confirmation", __FILE__, __LINE__);
        return {};
    }
    // The click that records confirmation_text is deferred to the next turn.
    // A Cancel send never waits on a txid, so without this the caller would
    // observe the text before that turn runs.
    if (confirmation_text) {
        for (int i = 0; i < 50 && confirmation_text->isEmpty(); ++i) {
            QTest::qWait(10);
        }
    }
    assert(progress_graphs->text().startsWith(QStringLiteral("Graphs tried:")));
    for (int i = 0; i < 300 && txid.IsNull() && confirm_type == QMessageBox::Yes; ++i) {
        QTest::qWait(100);
    }
    assert(!txid.IsNull() || confirm_type != QMessageBox::Yes);
    return txid;
}

//! Find index of txid in transaction list.
QModelIndex FindTx(const QAbstractItemModel& model, const uint256& txid)
{
    QString hash = QString::fromStdString(txid.ToString());
    int rows = model.rowCount({});
    for (int row = 0; row < rows; ++row) {
        QModelIndex index = model.index(row, 0, {});
        if (model.data(index, TransactionTableModel::TxHashRole) == hash) {
            return index;
        }
    }
    return {};
}

void CompareBalance(VaultModel& vaultModel, CAmount expected_balance, QLabel* balance_label_to_check)
{
    QuicksilverUnit unit = vaultModel.getOptionsModel()->getDisplayUnit();
    QString balanceComparison = QuicksilverUnits::formatWithUnit(unit, expected_balance, false, QuicksilverUnits::SeparatorStyle::ALWAYS);
    QCOMPARE(balance_label_to_check->text().trimmed(), balanceComparison);
}

// Verify the 'useAvailableBalance' functionality. With and without manually selected coins.
// Case 1: No coin control selected coins.
// 'useAvailableBalance' should fill the amount edit box with the total available balance
// Case 2: With coin control selected coins.
// 'useAvailableBalance' should fill the amount edit box with the sum of the selected coins values.
void VerifyUseAvailableBalance(SendCoinsDialog& sendCoinsDialog, const VaultModel& vaultModel)
{
    // Verify first entry amount and "useAvailableBalance" button
    QVBoxLayout* entries = sendCoinsDialog.findChild<QVBoxLayout*>("entries");
    QVERIFY(entries->count() == 1); // only one entry
    SendCoinsEntry* send_entry = qobject_cast<SendCoinsEntry*>(entries->itemAt(0)->widget());
    QVERIFY(send_entry->findChild<QCheckBox*>(QStringLiteral("checkbox"
                                                             "SubtractFeeFromAmount")) == nullptr);
    QVERIFY(send_entry->getValue().amount == 0);
    // Now click "useAvailableBalance", check updated balance (the entire vault balance should be set)
    Q_EMIT send_entry->useAvailableBalance(send_entry);
    QVERIFY(send_entry->getValue().amount == vaultModel.getCachedBalance().balance);

    // Now manually select two coins and click on "useAvailableBalance". Then check updated balance
    // (only the sum of the selected coins should be set).
    int COINS_TO_SELECT = 2;
    interfaces::Vault::CoinsList coins;
    QVERIFY(vaultModel.tryListCoins(coins));
    CAmount sum_selected_coins = 0;
    int selected = 0;
    QVERIFY(coins.size() == 1); // context check, coins received only on one destination
    for (const auto& [outpoint, tx_out] : coins.begin()->second) {
        sendCoinsDialog.getCoinControl()->Select(outpoint);
        sum_selected_coins += tx_out.txout.nValue;
        if (++selected == COINS_TO_SELECT) break;
    }
    QVERIFY(selected == COINS_TO_SELECT);

    // Now that we have 2 coins selected, "useAvailableBalance" should update the balance label only with
    // the sum of them.
    Q_EMIT send_entry->useAvailableBalance(send_entry);
    QVERIFY(send_entry->getValue().amount == sum_selected_coins);
}

void SyncUpVault(const std::shared_ptr<CVault>& vault, interfaces::Node& node)
{
    VaultRescanReserver reserver(*vault);
    reserver.reserve();
    CVault::ScanResult result = vault->ScanForVaultTransactions(Params().GetConsensus().hashGenesisBlock, /*start_height=*/0, /*max_height=*/{}, reserver, /*fUpdate=*/true, /*save_progress=*/false);
    QCOMPARE(result.status, CVault::ScanResult::SUCCESS);
    QCOMPARE(result.last_scanned_block, WITH_LOCK(node.context()->chainman->GetMutex(), return node.context()->chainman->ActiveChain().Tip()->GetBlockHash()));
    QVERIFY(result.last_failed_block.IsNull());
}

std::shared_ptr<CVault> SetupDescriptorsVault(interfaces::Node& node, TestChain100Setup& test)
{
    std::shared_ptr<CVault> vault = std::make_shared<CVault>(node.context()->chain.get(), "", CreateMockableVaultDatabase());
    vault->LoadVault();
    LOCK(vault->cs_vault);
    vault->SetVaultFlag(VAULT_FLAG_DESCRIPTORS);
    vault->SetupDescriptorScriptPubKeyMans();

    // Add the coinbase key
    FlatSigningProvider provider;
    std::string error;
    auto descs = Parse("combo(" + EncodeSecret(test.coinbaseKey) + ")", provider, error, /* require_checksum=*/false);
    assert(!descs.empty());
    assert(descs.size() == 1);
    auto& desc = descs.at(0);
    VaultDescriptor w_desc(std::move(desc), 0, 0, 1, 1);
    if (!vault->AddVaultDescriptor(w_desc, provider, "", false)) assert(false);
    CTxDestination dest{WitnessV0KeyHash(test.coinbaseKey.GetPubKey())};
    vault->SetAddressBook(dest, "", vault::AddressPurpose::RECEIVE);
    vault->SetLastBlockProcessed(105, WITH_LOCK(node.context()->chainman->GetMutex(), return node.context()->chainman->ActiveChain().Tip()->GetBlockHash()));
    SyncUpVault(vault, node);
    vault->SetBroadcastTransactions(true);
    return vault;
}

struct MiniGUI {
public:
    SendCoinsDialog sendCoinsDialog;
    TransactionView transactionView;
    OptionsModel optionsModel;
    std::unique_ptr<ClientModel> clientModel;
    std::unique_ptr<VaultModel> vaultModel;

    MiniGUI(interfaces::Node& node, const PlatformStyle* platformStyle) : sendCoinsDialog(platformStyle), transactionView(platformStyle), optionsModel(node)
    {
        bilingual_str error;
        QVERIFY(optionsModel.Init(error));
        clientModel = std::make_unique<ClientModel>(node, &optionsModel);
    }

    void initModelForVault(interfaces::Node& node, const std::shared_ptr<CVault>& vault, const PlatformStyle* platformStyle)
    {
        VaultContext& context = *node.vaultLoader().context();
        AddVault(context, vault);
        vaultModel = std::make_unique<VaultModel>(interfaces::MakeVault(context, vault), *clientModel, platformStyle);
        RemoveVault(context, vault, /* load_on_start= */ std::nullopt);
        sendCoinsDialog.setModel(vaultModel.get());
        transactionView.setModel(vaultModel.get());
    }
};

//! Simple qt vault tests.
//
// Test widgets can be debugged interactively calling show() on them and
// manually running the event loop, e.g.:
//
//     sendCoinsDialog.show();
//     QEventLoop().exec();
//
// This also requires overriding the default minimal Qt platform:
//
//     QT_QPA_PLATFORM=xcb     build/bin/test_quicksilver-qt  # Linux
//     QT_QPA_PLATFORM=windows build/bin/test_quicksilver-qt  # Windows
//     QT_QPA_PLATFORM=cocoa   build/bin/test_quicksilver-qt  # macOS
void TestGUI(interfaces::Node& node, const std::shared_ptr<CVault>& vault)
{
    // Create widgets for transfers and listing transactions.
    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    MiniGUI mini_gui(node, platformStyle.get());
    mini_gui.initModelForVault(node, vault, platformStyle.get());
    VaultModel& vaultModel = *mini_gui.vaultModel;
    SendCoinsDialog& sendCoinsDialog = mini_gui.sendCoinsDialog;
    TransactionView& transactionView = mini_gui.transactionView;
    QVERIFY(transactionView.findChild<QAction*>(QStringLiteral("bump"
                                                               "FeeAction")) == nullptr);
    QVERIFY(sendCoinsDialog.findChild<QFrame*>(QStringLiteral("frame"
                                                              "Fee")) == nullptr);
    QVERIFY(sendCoinsDialog.findChild<QCheckBox*>(QStringLiteral("opt"
                                                                 "InRBF")) == nullptr);
    QVERIFY(sendCoinsDialog.findChild<QWidget*>(QStringLiteral("custom"
                                                               "Fee")) == nullptr);
    QLabel* send_work_disclosure = sendCoinsDialog.findChild<QLabel*>(QStringLiteral("sendWorkDisclosureLabel"));
    QVERIFY(send_work_disclosure);
    QVERIFY(send_work_disclosure->text().contains(QStringLiteral("full amount")));
    QVERIFY(send_work_disclosure->text().contains(QStringLiteral("prepares a transfer proof")));
    QVERIFY(send_work_disclosure->text().contains(QStringLiteral("quick built-in preparation")));
    QVERIFY(send_work_disclosure->text().contains(QStringLiteral("do not need graphics acceleration")));
    QFrame* send_work_progress = sendCoinsDialog.findChild<QFrame*>(QStringLiteral("sendWorkProgressPanel"));
    QVERIFY(send_work_progress);
    QVERIFY(send_work_progress->isHidden());

    // Update vaultModel cached balance which will trigger an update for the 'labelBalance' QLabel.
    vaultModel.pollBalanceChanged();
    // Check balance in send dialog
    CompareBalance(vaultModel, vaultModel.getCachedBalance().balance, sendCoinsDialog.findChild<QLabel*>(QStringLiteral("transferSummarySpendable")));

    // Check 'UseAvailableBalance' functionality
    VerifyUseAvailableBalance(sendCoinsDialog, vaultModel);

    // Send two transactions, and verify they are added to transaction list.
    TransactionTableModel* transactionTableModel = vaultModel.getTransactionTableModel();
    QCOMPARE(transactionTableModel->rowCount({}), 105);
    QString send_confirmation_text;
    uint256 txid1 = SendCoins(*vault.get(), sendCoinsDialog, PKHash(), 5 * COIN, QMessageBox::Yes, &send_confirmation_text);
    QVERIFY(send_confirmation_text.contains(QStringLiteral("Sending uses proof-of-work")));
    QVERIFY(send_confirmation_text.contains(QStringLiteral("Nothing is deducted from the amount")));
    QVERIFY(send_confirmation_text.contains(QStringLiteral("Proof-of-work completed after")));
    QVERIFY(send_confirmation_text.contains(QStringLiteral("graphs tried")));
    QVERIFY(send_work_progress->isHidden());
    uint256 txid2 = SendCoins(*vault.get(), sendCoinsDialog, PKHash(), 10 * COIN);
    QVERIFY(send_work_progress->isHidden());
    // Transaction table model updates on a QueuedConnection, so process events to ensure it's updated.
    qApp->processEvents();
    QCOMPARE(transactionTableModel->rowCount({}), 107);
    QVERIFY(FindTx(*transactionTableModel, txid1).isValid());
    QVERIFY(FindTx(*transactionTableModel, txid2).isValid());

    VaultView vaultView(&vaultModel, platformStyle.get(), nullptr);
    vaultView.setClientModel(mini_gui.clientModel.get());

    vaultView.gotoMineMintPage();
    MineMintPage* mineMintPage = qobject_cast<MineMintPage*>(vaultView.currentWidget());
    QVERIFY(mineMintPage);
    QCOMPARE(mineMintPage->objectName(), QStringLiteral("mineMintPage"));
    // SP3: the page is now live. With the model wired and the role idle, Start is
    // enabled (you can start), Stop is disabled (nothing to stop), and the status
    // label reflects the polled state rather than the scaffold placeholder.
    QVERIFY(mineMintPage->findChild<QPushButton*>(QStringLiteral("startMiningButton"))->isEnabled());
    QVERIFY(!mineMintPage->findChild<QPushButton*>(QStringLiteral("stopMiningButton"))->isEnabled());
    QCOMPARE(mineMintPage->findChild<QLabel*>(QStringLiteral("miningStatusValue"))->text(), QStringLiteral("Idle"));

    vaultView.gotoNetworkPage();
    NetworkPage* networkPage = qobject_cast<NetworkPage*>(vaultView.currentWidget());
    QVERIFY(networkPage);
    QCOMPARE(networkPage->objectName(), QStringLiteral("networkPage"));
    // SP3: page is live off ClientModel. The test node has no peers, so status reads
    // "Offline" rather than the scaffold placeholder.
    QCOMPARE(networkPage->findChild<QLabel*>(QStringLiteral("networkStatusValue"))->text(), QStringLiteral("Offline"));

    QSettings().setValue(QStringLiteral("Desktop/ConsensusEnabled"), false);
    VaultFrame noVaultFrame(platformStyle.get(), nullptr);
    noVaultFrame.setClientModel(mini_gui.clientModel.get());
    noVaultFrame.gotoVaultPage();
    QStackedWidget* no_vault_stack = noVaultFrame.findChild<QStackedWidget*>(QStringLiteral("vaultFrameStack"));
    QVERIFY(no_vault_stack);
    QCOMPARE(no_vault_stack->currentWidget()->objectName(), QStringLiteral("noVaultPage"));
    QPushButton* no_vault_create = no_vault_stack->currentWidget()->findChild<QPushButton*>(QStringLiteral("emptyVaultCreateButton"));
    QPushButton* no_vault_open = no_vault_stack->currentWidget()->findChild<QPushButton*>(QStringLiteral("emptyVaultOpenButton"));
    QVERIFY(no_vault_create);
    QVERIFY(no_vault_open);
    QCOMPARE(no_vault_create->property("class").toString(), QStringLiteral("primaryActionButton"));
    QCOMPARE(no_vault_open->property("class").toString(), QStringLiteral("secondaryActionButton"));
    QVERIFY(!no_vault_create->isEnabled());
    QVERIFY(!no_vault_open->isEnabled());
    QLabel* no_vault_body = no_vault_stack->currentWidget()->findChild<QLabel*>(QStringLiteral("emptyVaultBody"));
    QVERIFY(no_vault_body);
    QVERIFY(no_vault_body->text().contains(QStringLiteral("startup attaches the vault runtime")));
    noVaultFrame.setVaultRuntimeAvailable(true);
    QVERIFY(no_vault_create->isEnabled());
    QVERIFY(no_vault_open->isEnabled());
    QVERIFY(no_vault_body->text().contains(QStringLiteral("Create a new vault")));
    noVaultFrame.gotoNetworkPage();
    QCOMPARE(no_vault_stack->currentWidget()->objectName(), QStringLiteral("consensusReviewPage"));
    QPushButton* no_vault_continue = no_vault_stack->currentWidget()->findChild<QPushButton*>(QStringLiteral("consensusContinueButton"));
    QVERIFY(no_vault_continue);
    no_vault_continue->click();
    noVaultFrame.gotoMineMintPage();
    QCOMPARE(no_vault_stack->currentWidget()->objectName(), QStringLiteral("mineMintPage"));
    QVERIFY(!noVaultFrame.currentVaultView());
    QPushButton* no_vault_new_address = no_vault_stack->currentWidget()->findChild<QPushButton*>(QStringLiteral("newAddressButton"));
    QVERIFY(no_vault_new_address);
    QVERIFY(!no_vault_new_address->isEnabled());

    // A launch refused for want of Tor lands on the network page before the
    // startup vaults finish loading. The first vault to arrive must not replace
    // that explanation; choosing a vault afterwards still works. The frame lives
    // as long as vaultModel, like the frames below: its views stay connected.
    VaultFrame torMissingFrame(platformStyle.get(), nullptr);
    {
        auto* lateVaultView = new VaultView(&vaultModel, platformStyle.get(), &torMissingFrame);
        torMissingFrame.markConsensusTorMissing();
        QStackedWidget* tor_missing_stack = torMissingFrame.findChild<QStackedWidget*>(QStringLiteral("vaultFrameStack"));
        QVERIFY(tor_missing_stack);
        QCOMPARE(tor_missing_stack->currentWidget()->objectName(), QStringLiteral("networkPage"));
        QVERIFY(torMissingFrame.addView(lateVaultView));
        torMissingFrame.setCurrentVault(&vaultModel);
        QCOMPARE(tor_missing_stack->currentWidget()->objectName(), QStringLiteral("networkPage"));
        QCOMPARE(torMissingFrame.currentVaultView(), lateVaultView);
        // Choosing the vault again keeps the page the rail names (Network); the
        // vault's own pages are a rail choice away.
        torMissingFrame.setCurrentVault(&vaultModel);
        QCOMPARE(tor_missing_stack->currentWidget()->objectName(), QStringLiteral("networkPage"));
        torMissingFrame.gotoHistoryPage();
        QCOMPARE(tor_missing_stack->currentWidget(), lateVaultView);
    }

    // A vault opened from the no-vault page while the app runs lands on Home.
    VaultFrame openedFrame(platformStyle.get(), nullptr);
    {
        openedFrame.setVaultRuntimeAvailable(true);
        openedFrame.gotoVaultPage();
        QStackedWidget* opened_stack = openedFrame.findChild<QStackedWidget*>(QStringLiteral("vaultFrameStack"));
        QVERIFY(opened_stack);
        QCOMPARE(opened_stack->currentWidget()->objectName(), QStringLiteral("noVaultPage"));
        auto* openedVaultView = new VaultView(&vaultModel, platformStyle.get(), &openedFrame);
        QVERIFY(openedFrame.addView(openedVaultView));
        openedFrame.setCurrentVault(&vaultModel);
        QCOMPARE(opened_stack->currentWidget()->objectName(), QStringLiteral("desktopLaunchPage"));
    }

    QSettings().setValue(QStringLiteral("Desktop/ConsensusEnabled"), false);
    VaultFrame vaultFrame(platformStyle.get(), nullptr);
    vaultFrame.setClientModel(mini_gui.clientModel.get());
    // The node's warnings (this build is not a release) read in Home's Node panel.
    {
        QLabel* node_alerts = vaultFrame.findChild<QLabel*>(QStringLiteral("homeNodeAlerts"));
        QVERIFY(node_alerts);
        const QString warnings = mini_gui.clientModel->getStatusBarWarnings();
        QVERIFY(!warnings.isEmpty());
        QCOMPARE(node_alerts->text(), warnings);
        QVERIFY(!node_alerts->isHidden());
    }
    auto* routedVaultView = new VaultView(&vaultModel, platformStyle.get(), &vaultFrame);
    QVERIFY(vaultFrame.addView(routedVaultView));
    vaultFrame.setCurrentVault(&vaultModel);
    QCOMPARE(vaultModel.backupRecorded(), false);

    // F-442 owner walk: with a vault open, Home is the new Home. A vault loaded at
    // startup, the Vault panel's command and choosing the vault again each land
    // where the rail points; there is no other "home" page left to land on.
    {
        QStackedWidget* routed_stack = vaultFrame.findChild<QStackedWidget*>(QStringLiteral("vaultFrameStack"));
        QVERIFY(routed_stack);
        QCOMPARE(routed_stack->currentWidget()->objectName(), QStringLiteral("desktopLaunchPage"));
        QVERIFY(!vaultFrame.findChild<QLabel*>(QStringLiteral("homeBootstrapTitle")));
        vaultFrame.gotoVaultPage();
        QCOMPARE(routed_stack->currentWidget()->objectName(), QStringLiteral("desktopLaunchPage"));
        vaultFrame.setCurrentVault(&vaultModel);
        QCOMPARE(routed_stack->currentWidget()->objectName(), QStringLiteral("desktopLaunchPage"));
        // From a vault page, choosing a vault keeps that page.
        vaultFrame.gotoHistoryPage();
        QWidget* history_page = routedVaultView->currentWidget();
        vaultFrame.setCurrentVault(&vaultModel);
        QCOMPARE(routed_stack->currentWidget(), routedVaultView);
        QCOMPARE(routedVaultView->currentWidget(), history_page);
        vaultFrame.gotoLaunchPage();
    }

    vaultFrame.gotoNetworkPage();
    QStackedWidget* vault_stack = vaultFrame.findChild<QStackedWidget*>(QStringLiteral("vaultFrameStack"));
    QVERIFY(vault_stack);
    QCOMPARE(vault_stack->currentWidget()->objectName(), QStringLiteral("consensusReviewPage"));
    QPushButton* consensus_continue = vault_stack->currentWidget()->findChild<QPushButton*>(QStringLiteral("consensusContinueButton"));
    QVERIFY(consensus_continue);
    QCOMPARE(consensus_continue->text(), QStringLiteral("Enable consensus"));
    consensus_continue->click();
    networkPage = qobject_cast<NetworkPage*>(vault_stack->currentWidget());
    QVERIFY(networkPage);
    QCOMPARE(networkPage->objectName(), QStringLiteral("networkPage"));
    QVERIFY(vaultFrame.consensusEnabled());
    vaultFrame.gotoLaunchPage();
    // The Vault panel is label/value rows. Balances are the ticker's, so the
    // panel names the vault, its encryption and its backup.
    QLabel* launch_summary = vaultFrame.findChild<QLabel*>(QStringLiteral("launchVaultCardBody"));
    QVERIFY(launch_summary);
    QVERIFY(launch_summary->isHidden());
    QCOMPARE(vaultFrame.findChild<QLabel*>(QStringLiteral("launchVaultCardState"))->text(), vaultModel.getDisplayName());
    QCOMPARE(vaultFrame.findChild<QLabel*>(QStringLiteral("homeVaultEncryption"))->text(), QStringLiteral("Not encrypted"));
    QVERIFY(vaultFrame.findChild<QLabel*>(QStringLiteral("homeVaultAgents"))->isHidden());
    // Home is the open vault: there is nothing for an "Open vault" command to open.
    QVERIFY(vaultFrame.findChild<QPushButton*>(QStringLiteral("launchVaultCardButton"))->isHidden());
    QPushButton* launch_privacy = vaultFrame.findChild<QPushButton*>(QStringLiteral("launchVaultBalancePrivacyButton"));
    QVERIFY(launch_privacy);
    QVERIFY(!launch_privacy->isHidden());
    QCOMPARE(launch_privacy->text(), QStringLiteral("Hide balance"));
    QCOMPARE(launch_privacy->property("class").toString(), QStringLiteral("benchQuiet"));
    QSignalSpy launch_privacy_spy(&vaultFrame, &VaultFrame::privacyRequested);
    QVERIFY(launch_privacy_spy.isValid());
    launch_privacy->click();
    QCOMPARE(launch_privacy_spy.count(), 1);
    QCOMPARE(launch_privacy_spy.takeFirst().at(0).toBool(), true);
    // Privacy masks the Home ledger's amounts.
    QTableView* home_rows = vaultFrame.findChild<QTableView*>(QStringLiteral("homeLedgerTable"));
    QVERIFY(home_rows);
    QVERIFY(home_rows->model()->rowCount() > 0);
    const auto home_amount = [&] {
        return home_rows->model()->index(0, TransactionTableModel::Amount).data().toString();
    };
    QVERIFY(!home_amount().contains(QLatin1Char('#')));
    vaultFrame.setPrivacy(true);
    QVERIFY2(home_amount().contains(QLatin1Char('#')), qPrintable(home_amount()));
    QCOMPARE(launch_privacy->text(), QStringLiteral("Show balance"));
    launch_privacy->click();
    QCOMPARE(launch_privacy_spy.count(), 1);
    QCOMPARE(launch_privacy_spy.takeFirst().at(0).toBool(), false);
    vaultFrame.setPrivacy(false);
    QVERIFY(!home_amount().contains(QLatin1Char('#')));
    QCOMPARE(launch_privacy->text(), QStringLiteral("Hide balance"));
    QWidget* backup_panel = vaultFrame.findChild<QWidget*>(QStringLiteral("desktopLaunchBackupPanel"));
    QVERIFY(backup_panel);
    QVERIFY(!backup_panel->isHidden());
    QCOMPARE(vaultFrame.findChild<QLabel*>(QStringLiteral("desktopLaunchBackupState"))->text(), QStringLiteral("Needed"));
    QLabel* backup_summary = vaultFrame.findChild<QLabel*>(QStringLiteral("desktopLaunchBackupSummary"));
    QVERIFY(backup_summary);
    QVERIFY(!backup_summary->isHidden());
    QVERIFY(backup_summary->text().contains(QStringLiteral("only a current vault-file backup restores both spending keys and transaction history")));
    QPushButton* backup_button = vaultFrame.findChild<QPushButton*>(QStringLiteral("desktopLaunchBackupButton"));
    QVERIFY(backup_button);
    QCOMPARE(backup_button->text(), QStringLiteral("Back up vault"));
    QCOMPARE(backup_button->property("class").toString(), QStringLiteral("benchQuiet"));
    QPushButton* launch_mining = vaultFrame.findChild<QPushButton*>(QStringLiteral("launchMiningCardButton"));
    QVERIFY(launch_mining);
    QVERIFY(launch_mining->isEnabled());

    QTemporaryDir backup_dir;
    QVERIFY(backup_dir.isValid());
    const QString backup_file = backup_dir.filePath(QStringLiteral("vault-backup.dat"));
    QVERIFY(vaultModel.vault().backupVault(backup_file.toLocal8Bit().data()));
    QCOMPARE(vaultModel.backupRecorded(), true);
    vaultFrame.setCurrentVault(&vaultModel);
    vaultFrame.gotoLaunchPage();
    QCOMPARE(vaultFrame.findChild<QLabel*>(QStringLiteral("desktopLaunchBackupState"))->text(), QStringLiteral("Done"));
    // A recorded backup is said by the row; the warning sentence goes away.
    QVERIFY(vaultFrame.findChild<QLabel*>(QStringLiteral("desktopLaunchBackupSummary"))->isHidden());
    QCOMPARE(vaultFrame.findChild<QPushButton*>(QStringLiteral("desktopLaunchBackupButton"))->text(), QStringLiteral("Back up again"));

    vaultFrame.gotoAgentAllotmentPage();
    QVERIFY(vaultFrame.currentVaultView());
    QWidget* agent_allotment_page = vaultFrame.currentVaultView()->currentWidget();
    QVERIFY(agent_allotment_page);
    QCOMPARE(agent_allotment_page->objectName(), QStringLiteral("agentAllotmentPage"));
    // No page heading: the breadcrumb names the page.
    QVERIFY(!agent_allotment_page->findChild<QLabel*>(QStringLiteral("agentAllotmentTitle")));
    QVERIFY(agent_allotment_page->findChild<QLabel*>(QStringLiteral("agentAllotmentDishonestAgentRisk"))->text().contains(QStringLiteral("cannot spend without this vault")));
    QVERIFY(agent_allotment_page->findChild<QLabel*>(QStringLiteral("agentAllotmentCompromisedHostRisk"))->text().contains(QStringLiteral("refuses every later request")));
    QLabel* guarantee_risk = agent_allotment_page->findChild<QLabel*>(QStringLiteral("agentAllotmentGuaranteeRisk"));
    QVERIFY(guarantee_risk);
    QVERIFY(guarantee_risk->text().contains(QStringLiteral("limits are not guaranteed")));
    QLineEdit* agent_name = agent_allotment_page->findChild<QLineEdit*>(QStringLiteral("agentAllotmentNameEdit"));
    QVERIFY(agent_name);
    QuicksilverAmountField* agent_funding = agent_allotment_page->findChild<QuicksilverAmountField*>(QStringLiteral("agentAllotmentFundingLimit"));
    QVERIFY(agent_funding);
    QuicksilverAmountField* agent_daily = agent_allotment_page->findChild<QuicksilverAmountField*>(QStringLiteral("agentAllotmentDailyLimit"));
    QVERIFY(agent_daily);
    QCheckBox* agent_acceptance = agent_allotment_page->findChild<QCheckBox*>(QStringLiteral("agentAllotmentAcceptanceCheck"));
    QVERIFY(agent_acceptance);
    QPushButton* agent_create = agent_allotment_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentCreateButton"));
    QVERIFY(agent_create);
    QVERIFY(!agent_create->isEnabled());
    QCOMPARE(agent_create->text(), QStringLiteral("Create agent allotment"));
    QLabel* backend_state = agent_allotment_page->findChild<QLabel*>(QStringLiteral("agentAllotmentBackendState"));
    QVERIFY(backend_state);
    QVERIFY(backend_state->text().contains(QStringLiteral("Enter a name")));
    QLabel* recorded_setups = agent_allotment_page->findChild<QLabel*>(QStringLiteral("agentAllotmentRecordedSetups"));
    QVERIFY(recorded_setups);
    QFrame* recorded_setups_panel = agent_allotment_page->findChild<QFrame*>(QStringLiteral("agentAllotmentRecordsPanel"));
    QVERIFY(recorded_setups_panel);
    QVERIFY(recorded_setups->text().contains(QStringLiteral("none")));
    agent_name->setText(QStringLiteral("test-agent"));
    agent_funding->setValue(COIN);
    agent_daily->setValue(COIN / 2);
    agent_acceptance->setChecked(true);
    QVERIFY(agent_create->isEnabled());
    QVERIFY(backend_state->text().contains(QStringLiteral("reserves its vault funding address")));
    agent_create->click();
    const auto agent_records = vaultModel.listAgentAllotmentRecords();
    QCOMPARE(agent_records.size(), size_t{1});
    QCOMPARE(QString::fromStdString(agent_records[0].label), QStringLiteral("test-agent"));
    QCOMPARE(agent_records[0].funding_limit, COIN);
    QCOMPARE(agent_records[0].daily_limit, COIN / 2);
    QVERIFY(agent_records[0].risk_accepted_time > 0);
    QCOMPARE(agent_records[0].stopped_time, static_cast<int64_t>(0));
    QVERIFY(!agent_records[0].funding_descriptor.empty());
    QVERIFY(IsValidDestinationString(agent_records[0].funding_address));
    QVERIFY(backend_state->text().contains(QStringLiteral("Agent allotment created. Back up this vault again.")));
    QVERIFY(recorded_setups->text().contains(QStringLiteral("test-agent")));
    QVERIFY(recorded_setups->text().contains(QStringLiteral("daily guardrail")));
    QVERIFY(recorded_setups->text().contains(QStringLiteral("Co-signed by this vault")));
    QVERIFY(recorded_setups->text().contains(QString::fromStdString(agent_records[0].funding_address)));
    QCOMPARE(vaultModel.agentAllotmentFundingAvailable(agent_records[0]), CAmount{0});
    QLabel* agent_funding_state = agent_allotment_page->findChild<QLabel*>(QStringLiteral("agentAllotmentFundingState"));
    QVERIFY(agent_funding_state);
    QCOMPARE(agent_funding_state->text(), QStringLiteral("Awaiting funding"));
    QLabel* agent_policy_state = agent_allotment_page->findChild<QLabel*>(QStringLiteral("agentAllotmentPolicyState"));
    QVERIFY(agent_policy_state);
    QCOMPARE(agent_policy_state->text(), QStringLiteral("Co-signed by this vault"));
    QLabel* agent_funding_row = agent_allotment_page->findChild<QLabel*>(QStringLiteral("agentAllotmentFundingRowLabel"));
    QVERIFY(agent_funding_row);
    QVERIFY(agent_funding_row->text().contains(QStringLiteral("awaits funding")));
    QVERIFY(agent_funding_row->text().contains(QStringLiteral("test-agent")));
    QPushButton* agent_fund_button = agent_allotment_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentFundSetupButton"));
    QVERIFY(agent_fund_button);
    QCOMPARE(agent_fund_button->text(), QStringLiteral("Fund setup"));
    agent_fund_button->click();
    SendCoinsDialog* agent_send_page = qobject_cast<SendCoinsDialog*>(vaultFrame.currentVaultView()->currentWidget());
    QVERIFY(agent_send_page);
    QVBoxLayout* agent_send_entries = agent_send_page->findChild<QVBoxLayout*>(QStringLiteral("entries"));
    QVERIFY(agent_send_entries);
    SendCoinsEntry* agent_send_entry = qobject_cast<SendCoinsEntry*>(agent_send_entries->itemAt(0)->widget());
    QVERIFY(agent_send_entry);
    QCOMPARE(agent_send_entry->findChild<QValidatedLineEdit*>(QStringLiteral("payTo"))->text(), QString::fromStdString(agent_records[0].funding_address));
    QCOMPARE(agent_send_entry->findChild<QLineEdit*>(QStringLiteral("addAsLabel"))->text(), QStringLiteral("Agent setup: test-agent"));
    QCOMPARE(agent_send_entry->findChild<QuicksilverAmountField*>(QStringLiteral("payAmount"))->value(), COIN);
    const CTxDestination agent_funding_dest = DecodeDestination(agent_records[0].funding_address);
    QVERIFY(IsValidDestination(agent_funding_dest));
    QVERIFY(!SendCoins(*vault.get(), *agent_send_page, agent_funding_dest, COIN / 2).IsNull());
    qApp->processEvents();
    QCOMPARE(vaultModel.agentAllotmentFundingAvailable(agent_records[0]), COIN / 2);
    vaultFrame.gotoAgentAllotmentPage();
    agent_allotment_page = vaultFrame.currentVaultView()->currentWidget();
    QVERIFY(agent_allotment_page);
    agent_funding_state = agent_allotment_page->findChild<QLabel*>(QStringLiteral("agentAllotmentFundingState"));
    QVERIFY(agent_funding_state);
    QCOMPARE(agent_funding_state->text(), QStringLiteral("Awaiting funding"));
    agent_policy_state = agent_allotment_page->findChild<QLabel*>(QStringLiteral("agentAllotmentPolicyState"));
    QVERIFY(agent_policy_state);
    QCOMPARE(agent_policy_state->text(), QStringLiteral("Co-signed by this vault"));
    agent_funding_row = agent_allotment_page->findChild<QLabel*>(QStringLiteral("agentAllotmentFundingRowLabel"));
    QVERIFY(agent_funding_row);
    QVERIFY(agent_funding_row->text().contains(QStringLiteral("remains")));
    agent_fund_button = agent_allotment_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentFundSetupButton"));
    QVERIFY(agent_fund_button);
    QVERIFY(agent_fund_button->isEnabled());
    QCOMPARE(agent_fund_button->text(), QStringLiteral("Fund remaining"));
    QPushButton* agent_policy_button = agent_allotment_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentCopyPolicyButton"));
    QVERIFY(agent_policy_button);
    QVERIFY(!agent_policy_button->isEnabled());
    QPushButton* agent_save_receipts_button = agent_allotment_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentSaveFundingReceiptsButton"));
    QVERIFY(agent_save_receipts_button);
    QVERIFY(!agent_save_receipts_button->isEnabled());
    QPushButton* agent_recovery_scan_button = agent_allotment_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentCopyRecoveryScanButton"));
    QVERIFY(agent_recovery_scan_button);
    QVERIFY(agent_recovery_scan_button->isEnabled());
    agent_fund_button->click();
    agent_send_page = qobject_cast<SendCoinsDialog*>(vaultFrame.currentVaultView()->currentWidget());
    QVERIFY(agent_send_page);
    agent_send_entries = agent_send_page->findChild<QVBoxLayout*>(QStringLiteral("entries"));
    QVERIFY(agent_send_entries);
    agent_send_entry = qobject_cast<SendCoinsEntry*>(agent_send_entries->itemAt(0)->widget());
    QVERIFY(agent_send_entry);
    QCOMPARE(agent_send_entry->findChild<QValidatedLineEdit*>(QStringLiteral("payTo"))->text(), QString::fromStdString(agent_records[0].funding_address));
    QCOMPARE(agent_send_entry->findChild<QLineEdit*>(QStringLiteral("addAsLabel"))->text(), QStringLiteral("Agent setup: test-agent"));
    QCOMPARE(agent_send_entry->findChild<QuicksilverAmountField*>(QStringLiteral("payAmount"))->value(), COIN / 2);
    QVERIFY(!SendCoins(*vault.get(), *agent_send_page, agent_funding_dest, COIN / 2).IsNull());
    qApp->processEvents();
    QCOMPARE(vaultModel.agentAllotmentFundingAvailable(agent_records[0]), COIN);
    vaultFrame.gotoAgentAllotmentPage();
    agent_allotment_page = vaultFrame.currentVaultView()->currentWidget();
    QVERIFY(agent_allotment_page);
    agent_funding_state = agent_allotment_page->findChild<QLabel*>(QStringLiteral("agentAllotmentFundingState"));
    QVERIFY(agent_funding_state);
    QCOMPARE(agent_funding_state->text(), QStringLiteral("Funded"));
    agent_policy_state = agent_allotment_page->findChild<QLabel*>(QStringLiteral("agentAllotmentPolicyState"));
    QVERIFY(agent_policy_state);
    QCOMPARE(agent_policy_state->text(), QStringLiteral("Co-signed by this vault"));
    agent_funding_row = agent_allotment_page->findChild<QLabel*>(QStringLiteral("agentAllotmentFundingRowLabel"));
    QVERIFY(agent_funding_row);
    QVERIFY(agent_funding_row->text().contains(QStringLiteral("confirmed funding")));
    agent_fund_button = agent_allotment_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentFundSetupButton"));
    QVERIFY(agent_fund_button);
    QVERIFY(!agent_fund_button->isEnabled());
    QCOMPARE(agent_fund_button->text(), QStringLiteral("Funding complete"));
    agent_policy_button = agent_allotment_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentCopyPolicyButton"));
    QVERIFY(agent_policy_button);
    QVERIFY(agent_policy_button->isEnabled());
    agent_save_receipts_button = agent_allotment_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentSaveFundingReceiptsButton"));
    QVERIFY(agent_save_receipts_button);
    QVERIFY(agent_save_receipts_button->isEnabled());
    agent_recovery_scan_button = agent_allotment_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentCopyRecoveryScanButton"));
    QVERIFY(agent_recovery_scan_button);
    QVERIFY(agent_recovery_scan_button->isEnabled());
    agent_policy_button->click();
    const QString agent_bundle = QApplication::clipboard()->text();
    QVERIFY(agent_bundle.contains(QStringLiteral("\"type\":\"quicksilver.agent_allotment_cosign_bundle\"")));
    QVERIFY(agent_bundle.contains(QStringLiteral("\"version\":1")));
    QVERIFY(agent_bundle.contains(QStringLiteral("\"policy_request\":{")));
    QVERIFY(agent_bundle.contains(QStringLiteral("\"agent_secret_wif\":\"")));
    QVERIFY(!agent_bundle.contains(QStringLiteral("funding_secret_wif")));
    QVERIFY(agent_bundle.contains(QStringLiteral("\"funding_outputs\":[")));
    QVERIFY(agent_bundle.contains(QStringLiteral("\"amount_cinnabar\":\"")));
    QVERIFY(agent_bundle.contains(QStringLiteral("\"type\":\"quicksilver.agent_allotment_cosign_bundle\"")));
    QVERIFY(agent_bundle.contains(QStringLiteral("\"funding_address\":\"%1\"").arg(QString::fromStdString(agent_records[0].funding_address))));
    QVERIFY(backend_state->text().contains(QStringLiteral("Agent bundle copied")));
    agent_recovery_scan_button->click();
    const QString recovery_scan_command = QApplication::clipboard()->text();
    QVERIFY(recovery_scan_command.startsWith(QStringLiteral("quicksilver-cli -chain=%1 scantxoutset start ").arg(QString::fromStdString(Params().GetChainTypeString()))));
    QVERIFY(recovery_scan_command.contains(QStringLiteral("[\"addr(%1)\"]").arg(QString::fromStdString(agent_records[0].funding_address))));
    QVERIFY(recovery_scan_command.contains(QStringLiteral("| quicksilver-agent -chain=%1 ").arg(QString::fromStdString(Params().GetChainTypeString()))));
    QVERIFY(recovery_scan_command.contains(QStringLiteral("-fundingaddress='%1'").arg(QString::fromStdString(agent_records[0].funding_address))));
    QVERIFY(recovery_scan_command.endsWith(QStringLiteral("-scantxoutset=- importrecovery")));
    QVERIFY(backend_state->text().contains(QStringLiteral("Recovery import command copied")));
    const QString policy_request = vaultModel.agentAllotmentPolicyRequest(agent_records[0], COIN);
    const auto exported_bundle = vaultModel.agentAllotmentPolicyBundle(policy_request);
    QVERIFY(exported_bundle);
    QVERIFY(!exported_bundle->funding_outputs.empty());
    agent_save_receipts_button->click();
    const fs::path funding_receipt_inbox = gArgs.GetDataDirNet() / "agent" / "payment-receipts.d";
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("quicksilver-agent -chain=%1 -paymentreceiptdir='%2' scanreceipts")
                                                 .arg(QString::fromStdString(Params().GetChainTypeString()),
                                                      QString::fromStdString(fs::PathToString(funding_receipt_inbox))));
    QVERIFY(backend_state->text().contains(QStringLiteral("Agent funding receipts saved")));
    QVERIFY(backend_state->text().contains(QString::number(exported_bundle->funding_outputs.size())));
    for (const vault::AgentAllotmentFundingOutput& output : exported_bundle->funding_outputs) {
        const fs::path receipt_path = funding_receipt_inbox / fs::PathFromString(output.txid + "-" + util::ToString(output.vout) + ".json");
        const auto saved_receipt = ReadBinaryFile(receipt_path);
        QVERIFY(saved_receipt.first);
        const QString receipt_json = QString::fromStdString(saved_receipt.second);
        QVERIFY(receipt_json.contains(QStringLiteral("\"type\":\"quicksilver.agent_payment_receipt\"")));
        QVERIFY(receipt_json.contains(QStringLiteral("\"funding_address\":\"%1\"").arg(QString::fromStdString(agent_records[0].funding_address))));
        QVERIFY(receipt_json.contains(QStringLiteral("\"txid\":\"%1\"").arg(QString::fromStdString(output.txid))));
        QVERIFY(receipt_json.contains(QStringLiteral("\"vout\":%1").arg(output.vout)));
        QVERIFY(receipt_json.contains(QStringLiteral("\"amount_cinnabar\":\"%1\"").arg(output.amount)));
        QVERIFY(receipt_json.contains(QStringLiteral("\"label\":\"test-agent\"")));
        QVERIFY(receipt_json.contains(QStringLiteral("\"memo\":\"agent setup funding\"")));
        QVERIFY(receipt_json.contains(QStringLiteral("\"payer\":\"desktop vault\"")));
    }
    QVERIFY(policy_request.contains(QStringLiteral("\"type\":\"quicksilver.agent_allotment_policy_request\"")));
    QVERIFY(policy_request.contains(QStringLiteral("\"chain\":\"%1\"").arg(QString::fromStdString(Params().GetChainTypeString()))));
    QVERIFY(policy_request.contains(QStringLiteral("\"genesis_hash\":\"%1\"").arg(QString::fromStdString(Params().GenesisBlock().GetHash().ToString()))));
    QVERIFY(policy_request.contains(QStringLiteral("\"id\":\"agent-1\"")));
    QVERIFY(policy_request.contains(QStringLiteral("\"label\":\"test-agent\"")));
    QVERIFY(policy_request.contains(QStringLiteral("\"funding_address\":\"%1\"").arg(QString::fromStdString(agent_records[0].funding_address))));
    QVERIFY(policy_request.contains(QStringLiteral("\"funding_limit_cinnabar\":\"100000000\"")));
    QVERIFY(policy_request.contains(QStringLiteral("\"funding_available_cinnabar\":\"100000000\"")));
    QVERIFY(policy_request.contains(QStringLiteral("\"daily_limit_cinnabar\":\"50000000\"")));
    QVERIFY(policy_request.contains(QStringLiteral("\"request_created_time\":\"")));
    QVERIFY(!policy_request.contains(QStringLiteral("policy_status")));
    QVERIFY(!policy_request.contains(QStringLiteral("backend_created")));
    QPlainTextEdit* agent_policy_review_edit = agent_allotment_page->findChild<QPlainTextEdit*>(QStringLiteral("agentAllotmentPolicyRequestEdit"));
    QVERIFY(agent_policy_review_edit);
    QPushButton* agent_policy_review_button = agent_allotment_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentReviewPolicyButton"));
    QVERIFY(agent_policy_review_button);
    QVERIFY(!agent_policy_review_button->isEnabled());
    QLabel* agent_policy_review_state = agent_allotment_page->findChild<QLabel*>(QStringLiteral("agentAllotmentPolicyReviewState"));
    QVERIFY(agent_policy_review_state);
    QVERIFY(agent_policy_review_state->text().contains(QStringLiteral("Paste a policy request")));
    QCOMPARE(agent_policy_review_state->property("class").toString(), QStringLiteral("muted"));
    agent_policy_review_edit->setPlainText(policy_request);
    QVERIFY(agent_policy_review_button->isEnabled());
    QVERIFY(agent_policy_review_state->text().contains(QStringLiteral("Ready to review")));
    QCOMPARE(agent_policy_review_state->property("class").toString(), QStringLiteral("policyReviewReady"));
    agent_policy_review_button->click();
    QVERIFY(agent_policy_review_state->text().contains(QStringLiteral("Valid request for test-agent")));
    QVERIFY(agent_policy_review_state->text().contains(QString::fromStdString(agent_records[0].funding_address)));
    QVERIFY(agent_policy_review_state->text().contains(QStringLiteral("Spending limits are checked by the agent")));
    QCOMPARE(agent_policy_review_state->property("class").toString(), QStringLiteral("policyReviewValid"));
    agent_policy_review_edit->setPlainText(QStringLiteral("[]"));
    QVERIFY(agent_policy_review_button->isEnabled());
    QCOMPARE(agent_policy_review_state->property("class").toString(), QStringLiteral("policyReviewReady"));
    agent_policy_review_button->click();
    QVERIFY(agent_policy_review_state->text().contains(QStringLiteral("must be a JSON object")));
    QCOMPARE(agent_policy_review_state->property("class").toString(), QStringLiteral("policyReviewError"));
    QString unknown_policy_request = policy_request;
    QVERIFY(unknown_policy_request.replace(QStringLiteral("\"id\":\"agent-1\""), QStringLiteral("\"id\":\"agent-unknown\"")) != policy_request);
    agent_policy_review_edit->setPlainText(unknown_policy_request);
    QVERIFY(agent_policy_review_button->isEnabled());
    QCOMPARE(agent_policy_review_state->property("class").toString(), QStringLiteral("policyReviewReady"));
    agent_policy_review_button->click();
    QVERIFY(agent_policy_review_state->text().contains(QStringLiteral("not recorded in this vault")));
    QCOMPARE(agent_policy_review_state->property("class").toString(), QStringLiteral("policyReviewError"));
    QPlainTextEdit* agent_payment_receipt_edit = agent_allotment_page->findChild<QPlainTextEdit*>(QStringLiteral("agentAllotmentPaymentReceiptEdit"));
    QVERIFY(agent_payment_receipt_edit);
    QPushButton* agent_payment_receipt_review_button = agent_allotment_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentReviewPaymentReceiptButton"));
    QVERIFY(agent_payment_receipt_review_button);
    QVERIFY(!agent_payment_receipt_review_button->isEnabled());
    QPushButton* agent_payment_receipt_save_button = agent_allotment_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentSavePaymentReceiptButton"));
    QVERIFY(agent_payment_receipt_save_button);
    QVERIFY(!agent_payment_receipt_save_button->isEnabled());
    QLabel* agent_payment_receipt_state = agent_allotment_page->findChild<QLabel*>(QStringLiteral("agentAllotmentPaymentReceiptState"));
    QVERIFY(agent_payment_receipt_state);
    QVERIFY(agent_payment_receipt_state->text().contains(QStringLiteral("Paste a payment receipt")));
    QCOMPARE(agent_payment_receipt_state->property("class").toString(), QStringLiteral("muted"));
    CKey discovered_funding_key;
    discovered_funding_key.MakeNewKey(true);
    const std::string discovered_funding_address{EncodeDestination(WitnessV0KeyHash(discovered_funding_key.GetPubKey()))};
    const QString payment_receipt = QStringLiteral(R"({"type":"quicksilver.agent_payment_receipt","version":1,"chain":"%1","genesis_hash":"%2","funding_address":"%3","txid":"%4","vout":5,"amount_cinnabar":"50000000","received_time":"789","payment_id":"desk-42","label":"Desk receipt","memo":"funding discovery","payer":"local vault"})")
                                        .arg(QString::fromStdString(Params().GetChainTypeString()),
                                             QString::fromStdString(Params().GenesisBlock().GetHash().ToString()),
                                             QString::fromStdString(discovered_funding_address),
                                             QString::fromStdString(Txid::FromUint256(ArithToUint256(701)).ToString()));
    agent_payment_receipt_edit->setPlainText(payment_receipt);
    QVERIFY(agent_payment_receipt_review_button->isEnabled());
    QVERIFY(agent_payment_receipt_save_button->isEnabled());
    QCOMPARE(agent_payment_receipt_state->property("class").toString(), QStringLiteral("policyReviewReady"));
    agent_payment_receipt_review_button->click();
    QVERIFY(agent_payment_receipt_state->text().contains(QStringLiteral("Valid receipt")));
    QVERIFY(agent_payment_receipt_state->text().contains(QString::fromStdString(discovered_funding_address)));
    QVERIFY(agent_payment_receipt_state->text().contains(QStringLiteral("desk-42")));
    QVERIFY(agent_payment_receipt_state->text().contains(QStringLiteral("Desk receipt")));
    QVERIFY(agent_payment_receipt_state->text().contains(QStringLiteral("funding discovery")));
    QVERIFY(agent_payment_receipt_state->text().contains(QStringLiteral("local vault")));
    QCOMPARE(agent_payment_receipt_state->property("class").toString(), QStringLiteral("policyReviewValid"));
    agent_payment_receipt_save_button->click();
    const fs::path receipt_inbox = gArgs.GetDataDirNet() / "agent" / "payment-receipts.d";
    const fs::path receipt_path = receipt_inbox / fs::PathFromString(Txid::FromUint256(ArithToUint256(701)).ToString() + "-5.json");
    const auto saved_receipt = ReadBinaryFile(receipt_path);
    QVERIFY(saved_receipt.first);
    QCOMPARE(QString::fromStdString(saved_receipt.second), payment_receipt + QStringLiteral("\n"));
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("quicksilver-agent -chain=%1 -paymentreceiptdir='%2' scanreceipts")
                                                 .arg(QString::fromStdString(Params().GetChainTypeString()),
                                                      QString::fromStdString(fs::PathToString(receipt_inbox))));
    QVERIFY(agent_payment_receipt_state->text().contains(QStringLiteral("Payment receipt saved")));
    QVERIFY(agent_payment_receipt_state->text().contains(QStringLiteral("External-agent scan command copied")));
    QVERIFY(agent_payment_receipt_state->text().contains(QStringLiteral("External-agent scan command copied")));
    QCOMPARE(agent_payment_receipt_state->property("class").toString(), QStringLiteral("policyReviewValid"));
    agent_payment_receipt_edit->setPlainText(QStringLiteral("[]"));
    QVERIFY(agent_payment_receipt_review_button->isEnabled());
    QVERIFY(agent_payment_receipt_save_button->isEnabled());
    QCOMPARE(agent_payment_receipt_state->property("class").toString(), QStringLiteral("policyReviewReady"));
    agent_payment_receipt_review_button->click();
    QVERIFY(agent_payment_receipt_state->text().contains(QStringLiteral("must be a JSON object")));
    QCOMPARE(agent_payment_receipt_state->property("class").toString(), QStringLiteral("policyReviewError"));
    vaultFrame.setCurrentVault(&vaultModel);
    vaultFrame.gotoLaunchPage();
    QLabel* launch_agents = vaultFrame.findChild<QLabel*>(QStringLiteral("homeVaultAgents"));
    QVERIFY(launch_agents);
    QVERIFY(!launch_agents->isHidden());
    QCOMPARE(launch_agents->text(), QStringLiteral("Funding 1 of 1 confirmed"));

    vaultFrame.gotoMineMintPage();
    QVERIFY(vaultFrame.currentVaultView());
    mineMintPage = qobject_cast<MineMintPage*>(vaultFrame.currentVaultView()->currentWidget());
    QVERIFY(mineMintPage);
    QCOMPARE(mineMintPage->objectName(), QStringLiteral("mineMintPage"));
    QVERIFY(mineMintPage->findChild<QFrame*>(QStringLiteral("mineMintPayoutPanel")));
    QCOMPARE(mineMintPage->findChild<QPushButton*>(QStringLiteral("startMiningButton"))->property("class").toString(), QStringLiteral("primaryActionButton"));

    // Check request button
    ReceiveCoinsDialog receiveCoinsDialog(platformStyle.get());
    receiveCoinsDialog.setModel(&vaultModel);
    QFrame* requests_panel = receiveCoinsDialog.findChild<QFrame*>(QStringLiteral("receiveHistoryBenchPanel"));
    QVERIFY(requests_panel);
    QCOMPARE(requests_panel->findChild<QLabel*>(QStringLiteral("benchPanelTitle"))->text(), QStringLiteral("RECENT REQUESTS"));
    RecentRequestsTableModel* requestTableModel = vaultModel.getRecentRequestsTableModel();

    // Label input
    QLineEdit* labelInput = receiveCoinsDialog.findChild<QLineEdit*>("reqLabel");
    labelInput->setText("TEST_LABEL_1");

    // Amount input
    QuicksilverAmountField* amountInput = receiveCoinsDialog.findChild<QuicksilverAmountField*>("reqAmount");
    amountInput->setValue(1);

    // Message input
    QLineEdit* messageInput = receiveCoinsDialog.findChild<QLineEdit*>("reqMessage");
    messageInput->setText("TEST_MESSAGE_1");
    int initialRowCount = requestTableModel->rowCount({});
    QPushButton* requestPaymentButton = receiveCoinsDialog.findChild<QPushButton*>("receiveButton");
    requestPaymentButton->click();
    QString address;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->inherits("ReceiveRequestDialog")) {
            ReceiveRequestDialog* receiveRequestDialog = qobject_cast<ReceiveRequestDialog*>(widget);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("payment_header")->text(), QString("Request details"));
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("uri_tag")->text(), QString("URI:"));
            QString uri = receiveRequestDialog->QObject::findChild<QLabel*>("uri_content")->text();
            QCOMPARE(uri.count("quicksilver:"), 2);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("address_tag")->text(), QString("Address:"));
            QVERIFY(address.isEmpty());
            address = receiveRequestDialog->QObject::findChild<QLabel*>("address_content")->text();
            QVERIFY(!address.isEmpty());

            QCOMPARE(uri.count("amount=0.00000001"), 2);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("amount_tag")->text(), QString("Amount:"));
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("amount_content")->text(), QString::fromStdString("0.00000001 " + CURRENCY_UNIT));

            QCOMPARE(uri.count("label=TEST_LABEL_1"), 2);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("label_tag")->text(), QString("Label:"));
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("label_content")->text(), QString("TEST_LABEL_1"));

            QCOMPARE(uri.count("message=TEST_MESSAGE_1"), 2);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("message_tag")->text(), QString("Message:"));
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("message_content")->text(), QString("TEST_MESSAGE_1"));
        }
    }

    // Clear button
    QPushButton* clearButton = receiveCoinsDialog.findChild<QPushButton*>("clearButton");
    clearButton->click();
    QCOMPARE(labelInput->text(), QString(""));
    QCOMPARE(amountInput->value(), CAmount(0));
    QCOMPARE(messageInput->text(), QString(""));

    // Check addition to history
    int currentRowCount = requestTableModel->rowCount({});
    QCOMPARE(currentRowCount, initialRowCount + 1);

    // Check addition to vault
    std::vector<std::string> requests = vaultModel.vault().getAddressReceiveRequests();
    QCOMPARE(requests.size(), size_t{1});
    RecentRequestEntry entry;
    DataStream{MakeUCharSpan(requests[0])} >> entry;
    QCOMPARE(entry.nVersion, int{1});
    QCOMPARE(entry.id, int64_t{1});
    QVERIFY(entry.date.isValid());
    QCOMPARE(entry.recipient.address, address);
    QCOMPARE(entry.recipient.label, QString{"TEST_LABEL_1"});
    QCOMPARE(entry.recipient.amount, CAmount{1});
    QCOMPARE(entry.recipient.message, QString{"TEST_MESSAGE_1"});

    // Check Remove button
    QTableView* table = receiveCoinsDialog.findChild<QTableView*>("recentRequestsView");
    table->selectRow(currentRowCount - 1);
    QPushButton* removeRequestButton = receiveCoinsDialog.findChild<QPushButton*>("removeRequestButton");
    removeRequestButton->click();
    QCOMPARE(requestTableModel->rowCount({}), currentRowCount - 1);

    // Check removal from vault
    QCOMPARE(vaultModel.vault().getAddressReceiveRequests().size(), size_t{0});
}

void TestGUI(interfaces::Node& node)
{
    // Set up vault and chain with 105 blocks (5 mature blocks for spending).
    // This is a GUI send-flow test, not a Cuckatoo solver test. Keep the live
    // transaction target check, but use the sandbox-only no-cycle path so proof
    // luck cannot put the asynchronous confirmation on the 30-second boundary.
    TestChain100Setup test{ChainType::SANDBOX, {.extra_args = {"-txpownocycle=1"}}};
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(node, test.m_node);

    // "Full" GUI tests, use vault
    const std::shared_ptr<CVault>& desc_vault = SetupDescriptorsVault(node, test);
    TestGUI(node, desc_vault);
}

} // namespace

namespace {
struct AgentPageFixture {
    TestChain100Setup chain;
    ScopedNodeContext context;
    std::unique_ptr<const PlatformStyle> style{PlatformStyle::instantiate("other")};
    OptionsModel options;
    PollMarkerVault* vault;
    std::unique_ptr<VaultModel> model;
    ClientModel client;
    AgentAllotmentPage page;
    QString request;

    explicit AgentPageFixture(interfaces::Node& node) : context(node, chain.m_node), options(node), client(node, &options)
    {
        bilingual_str error;
        if (!options.Init(error)) throw std::runtime_error(error.original);
        auto owned = std::make_unique<PollMarkerVault>();
        vault = owned.get();
        vault::AgentAllotmentRecord record;
        record.id = "agent-ui";
        record.label = "UI agent";
        record.funding_limit = COIN;
        record.funding_address = EncodeDestination(WitnessV1Taproot{XOnlyPubKey{chain.coinbaseKey.GetPubKey()}});
        vault->agent_records.push_back(record);
        model = std::make_unique<VaultModel>(std::move(owned), node, &options, style.get());
        page.setModel(model.get());
        CMutableTransaction tx;
        tx.vin.emplace_back(Txid::FromUint256(ArithToUint256(2)), 0);
        tx.vout.emplace_back(COIN / 4, GetScriptForDestination(PKHash{chain.coinbaseKey.GetPubKey()}));
        tx.vout.emplace_back(COIN * 3 / 4, GetScriptForDestination(DecodeDestination(record.funding_address)));
        tx.nAnchorHeight = 100;
        tx.nCycle[0] = 1;
        PartiallySignedQuicksilverTransaction psqt{tx};
        psqt.inputs[0].witness_utxo = CTxOut(COIN, GetScriptForDestination(DecodeDestination(record.funding_address)));
        DataStream stream;
        stream << psqt;
        request = QStringLiteral("psqt=") + QString::fromStdString(EncodeBase64(stream.str()));
    }
    template <typename T> T* get(const char* name) { return page.findChild<T*>(QString::fromLatin1(name)); }
    void pasteAndReview()
    {
        get<QPlainTextEdit>("agentAllotmentCosignEdit")->setPlainText(request);
        get<QPushButton>("agentAllotmentReviewCosignButton")->click();
    }
};
}

void VaultTests::agentAllotmentSetupAsksToUnlock()
{
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext context(m_node, test.m_node);
    const auto vault = SetupDescriptorsVault(m_node, test);
    const SecureString passphrase{"test unlock"};
    QVERIFY(vault->EncryptVault(passphrase));
    QVERIFY(vault->IsLocked());
    std::unique_ptr<const PlatformStyle> style(PlatformStyle::instantiate("other"));
    MiniGUI gui(m_node, style.get());
    gui.initModelForVault(m_node, vault, style.get());
    VaultModel& model = *gui.vaultModel;
    AgentAllotmentPage page;
    page.setModel(&model);
    page.findChild<QLineEdit*>("agentAllotmentNameEdit")->setText("locked agent");
    page.findChild<QuicksilverAmountField*>("agentAllotmentFundingLimit")->setValue(COIN);
    page.findChild<QCheckBox*>("agentAllotmentAcceptanceCheck")->setChecked(true);
    QSignalSpy unlock(&model, &VaultModel::requireUnlock);
    connect(&model, &VaultModel::requireUnlock, &page, [&] { model.notifyUnlockDialogShown(); });
    page.findChild<QPushButton*>("agentAllotmentCreateButton")->click();
    QCOMPARE(unlock.count(), 1);
    QVERIFY(model.listAgentAllotmentRecords().empty());
    QVERIFY(vault->Unlock(passphrase));
    model.completePendingUnlock();
    QCOMPARE(model.listAgentAllotmentRecords().size(), size_t{1});
    QVERIFY(vault->IsLocked());
}

void VaultTests::agentAllotmentStatusNeverPromisesEnforcementLater()
{
    AgentPageFixture f(m_node);
    for (QWidget* widget : f.page.findChildren<QWidget*>()) {
        QString text = widget->toolTip();
        if (auto* label = qobject_cast<QLabel*>(widget)) text += label->text();
        const QRegularExpression forbidden(QStringLiteral("\\b(?:yet|pending|enforced)\\b"), QRegularExpression::CaseInsensitiveOption);
        QVERIFY2(!forbidden.match(text).hasMatch(), qPrintable(QStringLiteral("%1: %2").arg(widget->objectName(), text)));
    }
}

void VaultTests::agentAllotmentRiskTextNamesTheCosigner()
{
    AgentAllotmentPage page;
    QCOMPARE(page.findChild<QLabel*>("agentAllotmentDishonestAgentRisk")->text(), QStringLiteral("The agent holds one key and this vault holds the other. The agent cannot spend without this vault's signature."));
    QCOMPARE(page.findChild<QLabel*>("agentAllotmentCompromisedHostRisk")->text(), QStringLiteral("Stop an allotment and this vault refuses every later request. A request the vault has already signed and broadcast still confirms."));
    QCOMPARE(page.findChild<QLabel*>("agentAllotmentGuaranteeRisk")->text(), QStringLiteral("Spending limits are the agent's own check. This vault does not enforce them, and limits are not guaranteed."));
    for (QLabel* label : page.findChildren<QLabel*>()) {
        QVERIFY(!label->text().contains("dishonest agent"));
        QVERIFY(!label->text().contains("compromised host"));
    }
}

void VaultTests::agentAllotmentStopAsksThenRefusesCosign()
{
    TestChain100Setup test{ChainType::SANDBOX, {.extra_args = {"-txpownocycle=1"}}};
    for (int i = 0; i < 5; ++i) test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    auto loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = loader.get();
    ScopedNodeContext context(m_node, test.m_node);
    auto vault = SetupDescriptorsVault(m_node, test);
    std::unique_ptr<const PlatformStyle> style(PlatformStyle::instantiate("other"));
    MiniGUI gui(m_node, style.get());
    gui.initModelForVault(m_node, vault, style.get());
    VaultModel& model = *gui.vaultModel;
    auto record = model.recordAgentAllotmentSetup("stopped agent", COIN, 0);
    QVERIFY(record);
    const CScript funding_script = GetScriptForDestination(DecodeDestination(record->funding_address));
    auto funding = vault::CreateTransaction(*vault, {vault::CRecipient{DecodeDestination(record->funding_address), COIN}},
        std::nullopt, vault::CCoinControl{});
    QVERIFY2(funding, util::ErrorString(funding).original.c_str());
    QVERIFY(vault->CommitTransaction(funding->tx, {}, {}));
    auto bundle = model.agentAllotmentPolicyBundle(model.agentAllotmentPolicyRequest(*record, COIN));
    QVERIFY2(bundle, util::ErrorString(bundle).original.c_str());
    QVERIFY(!bundle->funding_outputs.empty());
    FlatSigningProvider provider;
    std::string error;
    auto descriptors = Parse(bundle->funding_descriptor, provider, error, true);
    QCOMPARE(descriptors.size(), size_t{1});
    std::vector<CScript> scripts;
    QVERIFY(descriptors[0]->Expand(0, provider, scripts, provider));
    const CKey agent = DecodeSecret(bundle->agent_secret);
    provider.keys.emplace(agent.GetPubKey().GetID(), agent);
    CMutableTransaction tx;
    for (const auto& coin : bundle->funding_outputs) tx.vin.emplace_back(Txid::FromHex(coin.txid).value(), coin.vout);
    tx.vout.emplace_back(COIN / 4, GetScriptForDestination(PKHash{test.coinbaseKey.GetPubKey()}));
    tx.vout.emplace_back(COIN * 3 / 4, funding_script);
    PartiallySignedQuicksilverTransaction request{tx};
    for (size_t i = 0; i < request.inputs.size(); ++i) request.inputs[i].witness_utxo = CTxOut(bundle->funding_outputs[i].amount, funding_script);
    auto data = PrecomputePSQTData(request);
    for (size_t i = 0; i < request.inputs.size(); ++i) {
        SignPSQTInput(provider, request, i, &data, SIGHASH_DEFAULT, nullptr, false);
        QCOMPARE(request.inputs[i].m_tap_script_sigs.size(), size_t{1});
    }
    CMutableTransaction maximum{*request.tx};
    for (size_t i = 0; i < maximum.vin.size(); ++i) {
        const auto& leaf = *request.inputs[i].m_tap_scripts.begin();
        maximum.vin[i].scriptWitness.stack = {std::vector<unsigned char>(65), std::vector<unsigned char>(65), leaf.first.first, *leaf.second.begin()};
    }
    {
        LOCK(::cs_main);
        ProveTxPowForTest(maximum, *Assert(test.m_node.chainman->ActiveChain().Tip()), Params().GetConsensus());
    }
    request.tx->nAnchorHeight = maximum.nAnchorHeight;
    request.tx->nPowNonce = maximum.nPowNonce;
    request.tx->nCycle = maximum.nCycle;
    DataStream stream;
    stream << request;
    const QString pasted = QStringLiteral("psqt=") + QString::fromStdString(EncodeBase64(stream.str()));
    const SecureString passphrase{"stop locked vault"};
    QVERIFY(vault->EncryptVault(passphrase));
    QVERIFY(vault->IsLocked());
    AgentAllotmentPage page;
    page.setModel(&model);
    auto* stop = page.findChild<QPushButton*>("agentAllotmentStopButton");
    QVERIFY(stop);
    QSignalSpy unlock(&model, &VaultModel::requireUnlock);
    stop->click();
    auto* confirm = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
    QVERIFY(confirm);
    QCOMPARE(confirm->text(), QStringLiteral("Stop this allotment? This vault will refuse every later spend request from this agent. This cannot be undone; create a new allotment to fund the agent again."));
    QCOMPARE(model.listAgentAllotmentRecords()[0].stopped_time, int64_t{0});
    confirm->button(QMessageBox::Cancel)->click();
    QCOMPARE(model.listAgentAllotmentRecords()[0].stopped_time, int64_t{0});
    stop->click();
    confirm = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
    QVERIFY(confirm);
    confirm->button(QMessageBox::Yes)->click();
    QTRY_VERIFY(model.listAgentAllotmentRecords()[0].stopped_time != 0);
    QVERIFY(vault->IsLocked());
    QCOMPARE(unlock.count(), 0);
    QCOMPARE(page.findChild<QLabel*>("agentAllotmentPolicyState")->text(), QStringLiteral("Stopped"));
    page.refresh();
    QCOMPARE(page.findChild<QLabel*>("agentAllotmentPolicyState")->text(), QStringLiteral("Stopped"));
    vault::VaultBatch batch(vault->GetDatabase());
    std::vector<vault::AgentAllotmentRecord> stored;
    QVERIFY(batch.ReadAgentAllotmentRecords(stored));
    QCOMPARE(stored.size(), size_t{1});
    QVERIFY(stored[0].stopped_time != 0);
    auto* paste = page.findChild<QPlainTextEdit*>("agentAllotmentCosignEdit");
    QVERIFY(paste);
    paste->setPlainText(pasted);
    const auto transactions_before = WITH_LOCK(vault->cs_vault, return vault->mapVault.size());
    page.findChild<QPushButton*>("agentAllotmentReviewCosignButton")->click();
    QCOMPARE(unlock.count(), 0);
    QCOMPARE(WITH_LOCK(vault->cs_vault, return vault->mapVault.size()), transactions_before);
    auto* cosign = page.findChild<QPushButton*>("agentAllotmentSubmitSignedSpendButton");
    QVERIFY(cosign->isEnabled());
    connect(&model, &VaultModel::requireUnlock, &page, [&] { model.notifyUnlockDialogShown(); });
    cosign->click();
    QCOMPARE(unlock.count(), 1);
    QVERIFY(vault->Unlock(passphrase));
    model.completePendingUnlock();
    QCOMPARE(page.findChild<QLabel*>("agentAllotmentCosignState")->text(), QStringLiteral("This agent allotment is stopped. The vault no longer co-signs for it."));
    QCOMPARE(WITH_LOCK(vault->cs_vault, return vault->mapVault.size()), transactions_before);
    QVERIFY(vault->IsLocked());
}

void VaultTests::agentAllotmentCosignDisabledWithoutNode()
{
    AgentPageFixture f(m_node);
    QVERIFY(f.get<QPlainTextEdit>("agentAllotmentCosignEdit"));
    f.pasteAndReview();
    QCOMPARE(f.vault->cosign_calls, 0);
    auto* cosign = f.get<QPushButton>("agentAllotmentSubmitSignedSpendButton");
    QVERIFY(!cosign->isEnabled());
    QCOMPARE(cosign->toolTip(), QStringLiteral("Turn on Consensus to co-sign: this desktop broadcasts the spend through its own node."));
    const QString review = f.get<QLabel>("agentAllotmentCosignState")->text();
    QVERIFY2(review.contains("UI agent"), qPrintable(review));
    QVERIFY(review.contains("Input total"));
    QVERIFY(review.contains("Change back to the allotment"));
    QVERIFY(review.contains("Anchor age: 0 blocks"));
    const auto format = [](CAmount amount) { return QuicksilverUnits::formatWithUnit(QuicksilverUnit::HG, amount, false, QuicksilverUnits::SeparatorStyle::ALWAYS); };
    QVERIFY(review.contains(format(COIN)));
    QVERIFY(review.contains(format(COIN / 4)));
    QVERIFY(review.contains(format(COIN * 3 / 4)));
    QVERIFY(review.contains(QString::fromStdString(f.vault->agent_records[0].funding_address)));
    QVERIFY(review.contains(QString::fromStdString(EncodeDestination(PKHash{f.chain.coinbaseKey.GetPubKey()}))));
    const auto refused = f.model->cosignAgentAllotmentSpend(f.request.mid(5));
    QVERIFY(!refused);
    QCOMPARE(f.vault->cosign_calls, 0);
    f.model->setClientModel(&f.client);
    QVERIFY(cosign->isEnabled());
    f.model->setClientModel(nullptr);
    QVERIFY(!cosign->isEnabled());
    QVERIFY(!f.model->cosignAgentAllotmentSpend(f.request.mid(5)));
    QCOMPARE(f.vault->cosign_calls, 0);
}

void VaultTests::agentAllotmentCosignReviewTracksPastedText()
{
    AgentPageFixture f(m_node);
    f.model->setClientModel(&f.client);
    QVERIFY(f.get<QPlainTextEdit>("agentAllotmentCosignEdit"));
    f.pasteAndReview();
    auto* cosign = f.get<QPushButton>("agentAllotmentSubmitSignedSpendButton");
    QVERIFY(cosign->isEnabled());
    QCOMPARE(f.vault->cosign_calls, 0);
    f.get<QPlainTextEdit>("agentAllotmentCosignEdit")->setPlainText("invalid");
    QVERIFY(!cosign->isEnabled());
    f.pasteAndReview();
    QVERIFY(cosign->isEnabled());
    f.get<QPushButton>("agentAllotmentRefuseButton")->click();
    QVERIFY(!cosign->isEnabled());
    QVERIFY(f.get<QPlainTextEdit>("agentAllotmentCosignEdit")->toPlainText().isEmpty());
    QCOMPARE(f.get<QLabel>("agentAllotmentCosignState")->text(), QStringLiteral("Refused. Nothing was signed."));
    QCOMPARE(f.vault->cosign_calls, 0);
    f.pasteAndReview();
    cosign->click();
    QCOMPARE(f.get<QLabel>("agentAllotmentCosignState")->text(), QStringLiteral("backend refusal verbatim"));
    f.pasteAndReview();
    f.vault->cosign_result = MakeTransactionRef(CMutableTransaction{});
    cosign->click();
    QCOMPARE(f.get<QLabel>("agentAllotmentCosignState")->text(), QString::fromStdString(f.vault->cosign_result->GetHash().ToString()));
    QVERIFY(!cosign->isEnabled());
    f.pasteAndReview();
    f.vault->crypted = true;
    f.vault->locked = true;
    connect(f.model.get(), &VaultModel::requireUnlock, &f.page, [&] { f.model->notifyUnlockDialogShown(); });
    const int calls_before_unlock = f.vault->cosign_calls;
    cosign->click();
    QCOMPARE(f.vault->cosign_calls, calls_before_unlock);
    f.get<QPushButton>("agentAllotmentRefuseButton")->click();
    f.vault->locked = false;
    f.model->completePendingUnlock();
    QCOMPARE(f.vault->cosign_calls, calls_before_unlock);
    QVERIFY(!cosign->isEnabled());
}

void VaultTests::agentAllotmentCosignUnlockDiesWithItsReview()
{
    AgentPageFixture f(m_node);
    f.model->setClientModel(&f.client);
    f.vault->cosign_result = MakeTransactionRef(CMutableTransaction{});
    f.vault->crypted = true;
    f.vault->locked = true;
    connect(f.model.get(), &VaultModel::requireUnlock, &f.page, [&] { f.model->notifyUnlockDialogShown(); });
    auto* cosign = f.get<QPushButton>("agentAllotmentSubmitSignedSpendButton");

    // Refuse, then review the same request again: the first approval stays cancelled.
    f.pasteAndReview();
    cosign->click();
    f.get<QPushButton>("agentAllotmentRefuseButton")->click();
    f.pasteAndReview();
    QVERIFY(cosign->isEnabled());
    f.vault->locked = false;
    f.model->completePendingUnlock();
    QCOMPARE(f.vault->cosign_calls, 0);

    // Replacing the model and restoring it also cancels a pending approval.
    f.vault->locked = true;
    cosign->click();
    f.page.setModel(nullptr);
    f.page.setModel(f.model.get());
    f.pasteAndReview();
    f.vault->locked = false;
    f.model->completePendingUnlock();
    QCOMPARE(f.vault->cosign_calls, 0);

    // A fresh click after the new review is still honoured.
    f.vault->locked = true;
    cosign->click();
    f.vault->locked = false;
    f.model->completePendingUnlock();
    QCOMPARE(f.vault->cosign_calls, 1);
}

void VaultTests::agentAllotmentStoppedAllotmentOffersNoFunding()
{
    AgentPageFixture f(m_node);
    const CTxDestination dest = DecodeDestination(f.vault->agent_records[0].funding_address);
    const QString stopped_tip = QStringLiteral("This allotment is stopped. Create a new allotment to fund the agent again.");
    const auto check_row = [&](const QString& fund_text) {
        f.page.refresh();
        auto* fund = f.get<QPushButton>("agentAllotmentFundSetupButton");
        QVERIFY(fund);
        QCOMPARE(fund->text(), fund_text);
        QVERIFY(!fund->isEnabled());
        QCOMPARE(fund->toolTip(), stopped_tip);
        QVERIFY(f.get<QPushButton>("agentAllotmentCopyRecoveryScanButton")->isEnabled());
        QCOMPARE(f.get<QLabel>("agentAllotmentPolicyState")->text(), QStringLiteral("Stopped"));
    };

    f.vault->agent_records[0].stopped_time = 1;
    check_row(QStringLiteral("Fund setup"));

    interfaces::VaultTxOut partial;
    partial.txout = CTxOut(COIN / 2, GetScriptForDestination(dest));
    f.vault->coin_list[dest].emplace_back(COutPoint{Txid::FromUint256(ArithToUint256(7)), 0}, partial);
    f.vault->coin_listing_available = true;
    check_row(QStringLiteral("Fund remaining"));

    // The same partly funded row stays fundable while the allotment is active.
    f.vault->agent_records[0].stopped_time = 0;
    f.page.refresh();
    QVERIFY(f.get<QPushButton>("agentAllotmentFundSetupButton")->isEnabled());
}

void VaultTests::agentAllotmentPageScrollsWithinLaptopViewport()
{
    QStackedWidget stack;
    stack.resize(900, 650);

    auto* home_page = new QWidget(&stack);
    stack.addWidget(home_page);
    auto* agent_page = new AgentAllotmentPage(&stack);
    stack.addWidget(agent_page);
    stack.setCurrentWidget(agent_page);

    QScrollArea* scroll_area = agent_page->findChild<QScrollArea*>(QStringLiteral("agentAllotmentScrollArea"));
    QVERIFY(scroll_area);
    QCOMPARE(scroll_area->verticalScrollBarPolicy(), Qt::ScrollBarAsNeeded);
    QVERIFY(scroll_area->widget());
    QVERIFY(scroll_area->widget()->minimumSizeHint().height() > 650);

    QPushButton* final_action = agent_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentSubmitSignedSpendButton"));
    QVERIFY(final_action);
    QVERIFY(scroll_area->widget()->isAncestorOf(final_action));
}

//! F-442 send-back 2, item 5: in the desktop window at 1200x800 the Agents
//! page fits its viewport's width; nothing scrolls sideways.
void VaultTests::agentsPageHasNoSideScrollAt1200()
{
    const RestoreApplicationStyle restore;
    QuicksilverStyle::Apply(*qApp);
    TestChain100Setup test{ChainType::SANDBOX, {.extra_args = {"-txpownocycle=1"}}};
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);
    std::shared_ptr<CVault> vault = SetupDescriptorsVault(m_node, test);

    QSettings().setValue(QStringLiteral("Desktop/ConsensusEnabled"), false);
    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    QScopedPointer<const NetworkStyle> network_style(NetworkStyle::instantiate(Params().GetChainType()));
    MiniGUI mini_gui(m_node, platform_style.get());
    mini_gui.initModelForVault(m_node, vault, platform_style.get());

    QuicksilverGUI window(m_node, platform_style.get(), network_style.data());
    VaultFrame* frame = window.findChild<VaultFrame*>(QStringLiteral("vaultFrame"));
    QVERIFY(frame);
    auto* view = new VaultView(mini_gui.vaultModel.get(), platform_style.get(), frame);
    QVERIFY(frame->addView(view));
    frame->setCurrentVault(mini_gui.vaultModel.get());
    window.resize(1200, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QAction* agents = window.findChild<QAction*>(QStringLiteral("agentAllotmentAction"));
    QVERIFY(agents);
    agents->trigger();
    QScrollArea* scroll_area = window.findChild<QScrollArea*>(QStringLiteral("agentAllotmentScrollArea"));
    QVERIFY(scroll_area);
    QTRY_VERIFY(scroll_area->isVisible());
    QCoreApplication::processEvents();
    QWidget* contents = scroll_area->widget();
    QVERIFY2(!scroll_area->horizontalScrollBar()->isVisible(),
             qPrintable(QStringLiteral("contents need %1 px in a %2 px viewport").arg(contents->minimumSizeHint().width()).arg(scroll_area->viewport()->width())));
    // Name the widest offender, so a regression says where to look.
    for (QWidget* child : contents->findChildren<QWidget*>()) {
        if (!child->isVisibleTo(contents)) continue;
        QVERIFY2(child->mapTo(contents, QPoint(child->width(), 0)).x() <= scroll_area->viewport()->width(),
                 qPrintable(QStringLiteral("%1 (%2) ends at %3 px").arg(child->objectName(), QString::fromLatin1(child->metaObject()->className())).arg(child->mapTo(contents, QPoint(child->width(), 0)).x())));
    }
}

//! F-442 send-back 2, item 8: every line of text on a rail page sits in the
//! panel it describes; none floats above the panels.
void VaultTests::pagesKeepTheirTextInsidePanels()
{
    const RestoreApplicationStyle restore;
    QuicksilverStyle::Apply(*qApp);
    TestChain100Setup test{ChainType::SANDBOX, {.extra_args = {"-txpownocycle=1"}}};
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);
    std::shared_ptr<CVault> vault = SetupDescriptorsVault(m_node, test);

    QSettings().setValue(QStringLiteral("Desktop/ConsensusEnabled"), false);
    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    QScopedPointer<const NetworkStyle> network_style(NetworkStyle::instantiate(Params().GetChainType()));
    MiniGUI mini_gui(m_node, platform_style.get());
    mini_gui.initModelForVault(m_node, vault, platform_style.get());

    QuicksilverGUI window(m_node, platform_style.get(), network_style.data());
    VaultFrame* frame = window.findChild<VaultFrame*>(QStringLiteral("vaultFrame"));
    QVERIFY(frame);
    auto* view = new VaultView(mini_gui.vaultModel.get(), platform_style.get(), frame);
    QVERIFY(frame->addView(view));
    frame->setCurrentVault(mini_gui.vaultModel.get());
    frame->setClientModel(mini_gui.clientModel.get());
    window.resize(1200, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    // The network page reads its running state; give its intro the line the
    // owner saw.
    NetworkPage* network = window.findChild<NetworkPage*>();
    QVERIFY(network);

    const auto in_panel = [](QWidget* widget, QWidget* page) {
        for (QWidget* at = widget; at && at != page; at = at->parentWidget()) {
            if (at->property("benchPanel").toBool()) return true;
        }
        return false;
    };
    for (const char* action_name : {"homeBootstrapAction", "sendCoinsAction", "receiveCoinsAction", "historyAction",
                                    "agentAllotmentAction", "mineMintAction", "networkAction"}) {
        QAction* action = window.findChild<QAction*>(QString::fromLatin1(action_name));
        QVERIFY2(action, action_name);
        action->trigger();
        QCoreApplication::processEvents();
        QWidget* page = nullptr;
        for (QStackedWidget* stack : window.findChildren<QStackedWidget*>()) {
            QWidget* current = stack->currentWidget();
            if (current && current->isVisible() && current->property("class").toString() == QLatin1String("quicksilverPage")) page = current;
        }
        if (!page) {
            // Home and the pages that are not quicksilverPage-classed: take the
            // vault view's current page.
            page = view->currentWidget();
        }
        QVERIFY2(page, action_name);
        for (QLabel* label : page->findChildren<QLabel*>()) {
            if (!label->isVisibleTo(page) || label->text().trimmed().isEmpty()) continue;
            if (qobject_cast<QAbstractButton*>(label->parentWidget())) continue;
            QVERIFY2(in_panel(label, page),
                     qPrintable(QStringLiteral("%1: \"%2\" (%3) floats outside a panel").arg(QLatin1String(action_name), label->text().left(60), label->objectName())));
        }
    }
}

void VaultTests::vaultTests()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        // Disable for mac on "minimal" platform to avoid crashes inside the Qt
        // framework when it tries to look up unimplemented cocoa functions,
        // and fails to handle returned nulls
        // (https://bugreports.qt.io/browse/QTBUG-49686).
        qWarning() << "Skipping VaultTests on mac build with 'minimal' platform set due to Qt bugs. To run AppTests, invoke "
                      "with 'QT_QPA_PLATFORM=cocoa test_quicksilver-qt' on mac, or else use a linux or windows build.";
        return;
    }
#endif
    TestGUI(m_node);
}

void VaultTests::benchTickerMatchesVaultBalances()
{
    QSettings().setValue(QStringLiteral("Desktop/ConsensusEnabled"), false);
    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    QScopedPointer<const NetworkStyle> network_style(NetworkStyle::instantiate(Params().GetChainType()));
    OptionsModel options_model(m_node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));

    auto vault = std::make_unique<PollMarkerVault>();
    PollMarkerVault* vault_ptr = vault.get();
    vault_ptr->balance = 50 * COIN;
    vault_ptr->unconfirmed = 2 * COIN;
    vault_ptr->immature = 3 * COIN;
    VaultModel model(std::move(vault), m_node, &options_model, platform_style.get());

    QuicksilverGUI window(m_node, platform_style.get(), network_style.data());
    VaultFrame* frame = window.findChild<VaultFrame*>(QStringLiteral("vaultFrame"));
    QVERIFY(frame);
    auto* view = new VaultView(&model, platform_style.get(), frame);
    QVERIFY(frame->addView(view));
    frame->setCurrentVault(&model);
    vault_ptr->block_hash = ArithToUint256(1);
    model.pollBalanceChanged();

    // Each figure is the bare amount; its unit is a separate small label, as in
    // the Assay Bench ticker. TOTAL is everything the vault holds.
    const auto format_amount = [&](CAmount amount, bool privacy) {
        return QuicksilverUnits::formatInlineValueWithPrivacy(
            model.getOptionsModel()->getDisplayUnit(), amount, QuicksilverUnits::SeparatorStyle::ALWAYS, privacy);
    };
    const auto expect_ticker = [&](bool privacy) {
        const interfaces::VaultBalances balances = model.getCachedBalance();
        QCOMPARE(window.findChild<QLabel*>(QStringLiteral("benchTickerSpendable"))->text(), format_amount(balances.balance, privacy));
        QCOMPARE(window.findChild<QLabel*>(QStringLiteral("benchTickerPending"))->text(), format_amount(balances.unconfirmed_balance, privacy));
        QCOMPARE(window.findChild<QLabel*>(QStringLiteral("benchTickerMaturing"))->text(), format_amount(balances.immature_balance, privacy));
        QCOMPARE(window.findChild<QLabel*>(QStringLiteral("benchTickerDelegated"))->text(), format_amount(balances.delegated_balance, privacy));
        QCOMPARE(window.findChild<QLabel*>(QStringLiteral("benchTickerTotal"))->text(),
                 format_amount(balances.balance + balances.unconfirmed_balance + balances.immature_balance + balances.delegated_balance, privacy));
        QCOMPARE(window.findChild<QWidget*>(QStringLiteral("benchTickerDelegatedBox"))->isHidden(), balances.delegated_balance == 0);
        const QString unit = QuicksilverUnits::shortName(model.getOptionsModel()->getDisplayUnit());
        const QList<QLabel*> units = window.findChild<QWidget*>(QStringLiteral("benchTickerFigures"))->findChildren<QLabel*>(QStringLiteral("benchTickerUnit"));
        QCOMPARE(units.size(), 5);
        // The symbol is spelled Hg everywhere, the ticker included (F-442 owner walk).
        QCOMPARE(unit, QStringLiteral("Hg"));
        for (const QLabel* label : units) QCOMPARE(label->text(), unit);
    };

    const interfaces::VaultBalances initial = model.getCachedBalance();
    QCOMPARE(initial.balance, 50 * COIN);
    QCOMPARE(initial.unconfirmed_balance, 2 * COIN);
    QCOMPARE(initial.immature_balance, 3 * COIN);
    QCOMPARE(initial.delegated_balance, 0 * COIN);
    expect_ticker(false);
    if (QTest::currentTestFailed()) return;
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("benchTickerEmpty"))->isHidden());
    QLabel* crumb = window.findChild<QLabel*>(QStringLiteral("benchBreadcrumb"));
    QVERIFY(crumb);
    QCOMPARE(crumb->text(), model.getDisplayName().toUpper() + QStringLiteral(" / HOME"));
    QLabel* vault_status = window.findChild<QLabel*>(QStringLiteral("benchStatusVault"));
    QVERIFY(vault_status);
    QCOMPARE(vault_status->text(), QStringLiteral("Vault not encrypted · backup needed"));
    for (QLabel* label : {window.findChild<QLabel*>(QStringLiteral("benchTickerSpendable")),
                          window.findChild<QLabel*>(QStringLiteral("benchTickerPending")),
                          window.findChild<QLabel*>(QStringLiteral("benchTickerMaturing")),
                          window.findChild<QLabel*>(QStringLiteral("benchTickerDelegated")),
                          window.findChild<QLabel*>(QStringLiteral("benchTickerTotal"))}) {
        QVERIFY(label);
        const QString charge_word = QStringLiteral("fee");
        QVERIFY(!label->text().contains(charge_word, Qt::CaseInsensitive));
    }

    const QString spendable_before = window.findChild<QLabel*>(QStringLiteral("benchTickerSpendable"))->text();
    vault_ptr->balance = 40 * COIN;
    vault_ptr->block_hash = ArithToUint256(2);
    model.pollBalanceChanged();
    expect_ticker(false);
    if (QTest::currentTestFailed()) return;
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("benchTickerSpendable"))->text() != spendable_before);

    vault_ptr->delegated = 7 * COIN;
    vault_ptr->block_hash = uint256{3};
    model.pollBalanceChanged();
    expect_ticker(false);
    if (QTest::currentTestFailed()) return;
    QVERIFY(!window.findChild<QWidget*>(QStringLiteral("benchTickerDelegatedBox"))->isHidden());

    QAction* mask = nullptr;
    for (QAction* action : window.findChildren<QAction*>()) {
        QString text = action->text();
        text.remove(QLatin1Char('&'));
        if (text == QStringLiteral("Mask values")) {
            mask = action;
            break;
        }
    }
    QVERIFY(mask);
    mask->setChecked(true);
    expect_ticker(true);
    if (QTest::currentTestFailed()) return;
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("benchTickerSpendable"))->text().contains(QLatin1Char('#')));
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("benchTickerSpendable"))->text() != spendable_before);
    mask->setChecked(false);
    expect_ticker(false);
    if (QTest::currentTestFailed()) return;

    vault_ptr->delegated = 0;
    vault_ptr->block_hash = uint256{4};
    model.pollBalanceChanged();
    expect_ticker(false);

    QAction* transfer = window.findChild<QAction*>(QStringLiteral("sendCoinsAction"));
    QVERIFY(transfer);
    transfer->setChecked(true);
    QCOMPARE(crumb->text(), model.getDisplayName().toUpper() + QStringLiteral(" / TRANSFER"));
}

void VaultTests::homeLedgerListsVaultTransactions()
{
    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    DesktopLaunchPage bare(platform_style.get(), /*blockchain_size_gb=*/128, /*chain_state_size_gb=*/26, nullptr);
    QVERIFY(bare.findChild<QWidget*>(QStringLiteral("homeLedger")));
    QVERIFY(bare.findChild<QLabel*>(QStringLiteral("homeLedgerEmpty")));
    QVERIFY(bare.findChild<QLabel*>(QStringLiteral("homeNodeHeight")));
    QVERIFY(bare.findChild<QLabel*>(QStringLiteral("homeNodePeers")));
    QVERIFY(bare.findChild<QLabel*>(QStringLiteral("homeNodeLastBlock")));
    QCOMPARE(bare.findChild<QLabel*>(QStringLiteral("homeLedgerEmpty"))->text(), QStringLiteral("No vault is open"));

    // The application test window has no funded vault, so the ledger rows are
    // checked here, on the same chain the transfer tests already use.
    TestChain100Setup test{ChainType::SANDBOX, {.extra_args = {"-txpownocycle=1"}}};
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);
    std::shared_ptr<CVault> vault = SetupDescriptorsVault(m_node, test);

    QSettings().setValue(QStringLiteral("Desktop/ConsensusEnabled"), false);
    QSettings().remove(QStringLiteral("TransactionViewHeaderState"));
    QScopedPointer<const NetworkStyle> network_style(NetworkStyle::instantiate(Params().GetChainType()));
    MiniGUI mini_gui(m_node, platform_style.get());
    mini_gui.initModelForVault(m_node, vault, platform_style.get());
    // One transfer still waiting for a block, beside mature and maturing rewards.
    const uint256 pending_txid = SendCoins(*vault, mini_gui.sendCoinsDialog, PKHash(), COIN);
    QVERIFY(!pending_txid.IsNull());
    qApp->processEvents();
    TransactionTableModel* source = mini_gui.vaultModel->getTransactionTableModel();
    QVERIFY(source);
    const int all_rows = source->rowCount({});
    QVERIFY(all_rows > 0);

    int generated = 0;
    int transferred = 0;
    int pending = 0;
    int maturing = 0;
    for (int row = 0; row < all_rows; ++row) {
        const QModelIndex index = source->index(row, 0);
        switch (index.data(TransactionTableModel::TypeRole).toInt()) {
        case TransactionRecord::Generated: ++generated; break;
        case TransactionRecord::SendToAddress:
        case TransactionRecord::SendToOther: ++transferred; break;
        }
        const int status = index.data(TransactionTableModel::StatusRole).toInt();
        if (status == TransactionStatus::Unconfirmed) ++pending;
        if (status == TransactionStatus::Immature) ++maturing;
    }
    QVERIFY(generated > 0);
    QCOMPARE(transferred, 1);
    // More rows than Home's snapshot holds, so the snapshot has to choose.
    QVERIFY(all_rows > 10);
    QDateTime tenth_newest;
    {
        QList<QDateTime> dates;
        for (int row = 0; row < all_rows; ++row) dates << source->index(row, 0).data(TransactionTableModel::DateRole).toDateTime();
        std::sort(dates.begin(), dates.end(), std::greater<>());
        tenth_newest = dates.at(9);
    }
    QVERIFY(pending >= 1);
    QVERIFY(maturing > 0);

    QByteArray history_header;
    {
        QuicksilverGUI window(m_node, platform_style.get(), network_style.data());
        VaultFrame* frame = window.findChild<VaultFrame*>(QStringLiteral("vaultFrame"));
        QVERIFY(frame);
        auto* view = new VaultView(mini_gui.vaultModel.get(), platform_style.get(), frame);
        QVERIFY(frame->addView(view));
        frame->setCurrentVault(mini_gui.vaultModel.get());
        QAction* home = window.findChild<QAction*>(QStringLiteral("homeBootstrapAction"));
        QVERIFY(home);
        home->trigger();

        QWidget* launch = window.findChild<QWidget*>(QStringLiteral("desktopLaunchPage"));
        QVERIFY(launch);
        QVERIFY(launch->findChild<QLabel*>(QStringLiteral("homeLedgerEmpty"))->isHidden());
        // The launch heading and tagline are gone; the breadcrumb names the page.
        QVERIFY(!launch->findChild<QLabel*>(QStringLiteral("desktopLaunchTitle")));
        QVERIFY(!launch->findChild<QLabel*>(QStringLiteral("desktopLaunchSubtitle")));
        // The maturity countdown (F-432) reads on Home, above the ledger.
        QLabel* maturing_note = launch->findChild<QLabel*>(QStringLiteral("homeVaultMaturing"));
        QVERIFY(maturing_note);
        QVERIFY(!maturing_note->isHidden());
        QVERIFY2(maturing_note->text().contains(QStringLiteral("in about")), qPrintable(maturing_note->text()));

        // F-451: Home's ledger is a snapshot, the newest 10 rows, with how many
        // there are in all in its head. Filtering lives on the Ledger page, which
        // a link in the head opens.
        QFrame* ledger = launch->findChild<QFrame*>(QStringLiteral("homeLedger"));
        QVERIFY(ledger);
        QVERIFY(!ledger->isHidden());
        QCOMPARE(ledger->property("benchPanel").toBool(), true);
        QCOMPARE(ledger->findChild<QLabel*>(QStringLiteral("benchPanelTitle"))->text(), QStringLiteral("LEDGER"));
        QLabel* count = ledger->findChild<QLabel*>(QStringLiteral("homeLedgerCount"));
        QVERIFY(count);
        QCOMPARE(count->text(), QStringLiteral("10 of %1 entries").arg(all_rows));
        QVERIFY(ledger->findChildren<QComboBox*>().isEmpty());
        QVERIFY(ledger->findChildren<QLineEdit*>().isEmpty());
        for (const char* segment : {"homeLedgerFilterAll", "homeLedgerFilterReceived", "homeLedgerFilterTransferred", "homeLedgerFilterMining"}) {
            QVERIFY2(!ledger->findChild<QPushButton*>(QString::fromLatin1(segment)), segment);
        }
        QTableView* table = ledger->findChild<QTableView*>(QStringLiteral("homeLedgerTable"));
        QVERIFY(table);
        QAbstractItemModel* rows = table->model();
        QVERIFY(rows);
        QCOMPARE(rows->rowCount(), 10);
        for (int row = 0; row < rows->rowCount(); ++row) {
            const QDateTime when = rows->index(row, 0).data(TransactionTableModel::DateRole).toDateTime();
            QVERIFY2(when >= tenth_newest, qPrintable(when.toString(Qt::ISODate)));
            if (row > 0) QVERIFY(when <= rows->index(row - 1, 0).data(TransactionTableModel::DateRole).toDateTime());
        }

        // Columns: date (with the state dot), type, label, signed amount, and
        // STATE last. No status-icon column.
        QHeaderView* header = table->horizontalHeader();
        QStringList columns;
        for (int visual = 0; visual < header->count(); ++visual) {
            const int logical = header->logicalIndex(visual);
            if (header->isSectionHidden(logical)) continue;
            columns << rows->headerData(logical, Qt::Horizontal).toString();
        }
        const QString unit = QuicksilverUnits::shortName(mini_gui.vaultModel->getOptionsModel()->getDisplayUnit());
        QCOMPARE(columns, (QStringList{QStringLiteral("DATE"), QStringLiteral("TYPE"), QStringLiteral("LABEL"),
                                       QStringLiteral("AMOUNT  ") + unit, QStringLiteral("STATE")}));
        int state_column = -1;
        int amount_column = -1;
        int date_column = -1;
        for (int logical = 0; logical < rows->columnCount(); ++logical) {
            const QString name = rows->headerData(logical, Qt::Horizontal).toString();
            if (name == QStringLiteral("STATE")) state_column = logical;
            if (name.startsWith(QStringLiteral("AMOUNT"))) amount_column = logical;
            if (name == QStringLiteral("DATE")) date_column = logical;
        }
        QVERIFY(state_column >= 0 && amount_column >= 0 && date_column >= 0);
        int seen_pending = 0;
        int seen_maturing = 0;
        for (int row = 0; row < rows->rowCount(); ++row) {
            const int status = rows->index(row, 0).data(TransactionTableModel::StatusRole).toInt();
            const QString state = rows->index(row, state_column).data().toString();
            const QString amount = rows->index(row, amount_column).data().toString();
            QVERIFY2(!amount.contains(QLatin1Char('[')), qPrintable(amount));
            QVERIFY(!rows->index(row, date_column).data(Qt::DecorationRole).isNull());
            switch (status) {
            case TransactionStatus::Unconfirmed:
                QCOMPARE(state, QStringLiteral("Pending"));
                ++seen_pending;
                break;
            case TransactionStatus::Immature:
                QCOMPARE(state, QStringLiteral("Maturing"));
                ++seen_maturing;
                break;
            case TransactionStatus::Confirmed:
                QCOMPARE(state, QStringLiteral("Confirmed"));
                break;
            default:
                QVERIFY2(!state.isEmpty(), "every row names its state");
            }
            const qint64 net = rows->index(row, 0).data(TransactionTableModel::AmountRole).toLongLong();
            QVERIFY2(amount.startsWith(net < 0 ? QStringLiteral("− ") : QStringLiteral("+ ")), qPrintable(amount));
        }
        // The transfer still waiting for a block is the newest row; the newest
        // rewards are still maturing.
        QCOMPARE(seen_pending, pending);
        QVERIFY(seen_maturing > 0);

        // A new transaction joins the snapshot and the oldest shown row leaves
        // it; the count in the head follows. Both transfers are pending at the
        // same mock time, so either may sort first.
        const uint256 second_txid = SendCoins(*vault, mini_gui.sendCoinsDialog, PKHash(), COIN);
        QVERIFY(!second_txid.IsNull());
        QTRY_COMPARE(source->rowCount({}), all_rows + 1);
        QTRY_COMPARE(count->text(), QStringLiteral("10 of %1 entries").arg(all_rows + 1));
        QCOMPARE(rows->rowCount(), 10);
        QCOMPARE(rows->match(rows->index(0, 0), TransactionTableModel::TxHashRole, QString::fromStdString(second_txid.GetHex()), 1, Qt::MatchExactly).size(), 1);

        // "Full ledger" opens the Ledger page, and the rail follows.
        QPushButton* full = ledger->findChild<QPushButton*>(QStringLiteral("homeLedgerOpenFull"));
        QVERIFY(full);
        QCOMPARE(full->text(), QStringLiteral("Full ledger"));
        QAction* history = window.findChild<QAction*>(QStringLiteral("historyAction"));
        QVERIFY(history);
        QVERIFY(!history->isChecked());
        full->click();
        QVERIFY(history->isChecked());
        QWidget* history_page = window.findChild<QStackedWidget*>(QStringLiteral("vaultFrameStack"))->currentWidget();
        QVERIFY(history_page);
        QVERIFY(history_page != launch);
        QTableView* history_table = history_page->findChild<QTableView*>(QStringLiteral("transactionView"));
        QVERIFY(history_table);
        QVERIFY(history_table->model());
        QCOMPARE(history_table->model()->rowCount(), all_rows + 1);

        table->setColumnWidth(TransactionTableModel::Date, 55);
        history_table->setColumnWidth(TransactionTableModel::Date, 177);
        history_header = history_table->horizontalHeader()->saveState();
    }

    TransactionView restored(platform_style.get());
    QTableView* restored_table = restored.findChild<QTableView*>(QStringLiteral("transactionView"));
    QVERIFY(restored_table);
    QCOMPARE(restored_table->horizontalHeader()->saveState(), history_header);
    QCOMPARE(restored_table->columnWidth(TransactionTableModel::Date), 177);
}

namespace {
double RelativeLuminance(const QColor& color)
{
    const auto channel = [](double c) { return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
    return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) + 0.0722 * channel(color.blueF());
}

double ContrastRatio(const QColor& a, const QColor& b)
{
    const double la = RelativeLuminance(a);
    const double lb = RelativeLuminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

bool Reddish(const QColor& color)
{
    return color.red() > 150 && color.red() - std::max(color.green(), color.blue()) > 60;
}

int CountPixels(const QImage& image, const std::function<bool(const QColor&)>& match)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (match(image.pixelColor(x, y))) ++count;
        }
    }
    return count;
}
} // namespace

//! F-442 owner walk, items 3, 5, 11 and 12: one type scale with labels as
//! legible as their values, focus and checked states that do not read as
//! errors, spin box arrows that draw, and primary commands that differ from
//! the rest in colour only.
void VaultTests::benchStyleKeepsOneScaleAndQuietStates()
{
    const RestoreApplicationStyle restore;
    QuicksilverStyle::Apply(*qApp);
    using QuicksilverStyle::Type;

    QSet<int> scale;
    for (Type type : {Type::Caption, Type::Label, Type::Body, Type::Value, Type::Figure, Type::Title}) {
        scale.insert(QuicksilverStyle::FontPx(type));
    }
    const QRegularExpression size_rule(QStringLiteral("font-size:\\s*(\\d+)px"));
    for (auto it = size_rule.globalMatch(qApp->styleSheet()); it.hasNext();) {
        const int px = it.next().captured(1).toInt();
        QVERIFY2(scale.contains(px), qPrintable(QStringLiteral("font-size %1px is not on the type scale").arg(px)));
    }
    QVERIFY(QuicksilverStyle::FontPx(Type::Caption) >= 10);

    // Labels read as strongly as the values they name: AAA contrast on a panel.
    // Captions are short upper-case words and need AA.
    const QColor panel = QuicksilverStyle::Color(QuicksilverStyle::Token::BenchSurface);
    QWidget sample;
    auto* key = new QLabel(QStringLiteral("Payout target"), &sample);
    key->setProperty("class", QStringLiteral("benchKey"));
    auto* note = new QLabel(QStringLiteral("A note under the rows"), &sample);
    note->setProperty("class", QStringLiteral("benchNote"));
    const BenchPanel::Parts parts = BenchPanel::Make(QStringLiteral("samplePanel"), QStringLiteral("Mining state"), &sample);
    QLabel* caption = parts.frame->findChild<QLabel*>(QStringLiteral("benchPanelTitle"));
    QVERIFY(caption);
    for (QLabel* label : {key, note, caption}) label->ensurePolished();
    for (QLabel* label : {key, note}) {
        const QColor color = label->palette().color(label->foregroundRole());
        QVERIFY2(ContrastRatio(color, panel) >= 7.0,
                 qPrintable(QStringLiteral("%1 is %2 on the panel, %3:1").arg(label->text(), color.name()).arg(ContrastRatio(color, panel), 0, 'f', 2)));
    }
    const QColor caption_color = caption->palette().color(caption->foregroundRole());
    QVERIFY2(ContrastRatio(caption_color, panel) >= 4.5, qPrintable(caption_color.name()));

    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto* edit = new QLineEdit(&host);
    auto* check = new QCheckBox(QStringLiteral("Enable RPC server"), &host);
    check->setChecked(true);
    auto* spin = new QSpinBox(&host);
    spin->setRange(0, 1000);
    spin->setValue(450);
    spin->setMinimumWidth(140);
    auto* primary = new QPushButton(QStringLiteral("Request"), &host);
    primary->setProperty("class", QStringLiteral("primaryActionButton"));
    auto* secondary = new QPushButton(QStringLiteral("Request"), &host);
    auto* quiet = new QPushButton(QStringLiteral("Request"), &host);
    quiet->setProperty("class", QStringLiteral("benchQuiet"));
    for (QWidget* widget : std::initializer_list<QWidget*>{edit, check, spin, primary, secondary, quiet}) layout->addWidget(widget, 0, Qt::AlignLeft);
    host.resize(360, 320);
    host.show();
    QVERIFY(QTest::qWaitForWindowExposed(&host));
    host.activateWindow();
    edit->setFocus();
    QTRY_VERIFY(edit->hasFocus());

    // Focus is not a validation error.
    const QImage focused = edit->grab().toImage();
    const QColor focus_border = focused.pixelColor(0, focused.height() / 2);
    QVERIFY2(!Reddish(focus_border), qPrintable(QStringLiteral("focused input border %1").arg(focus_border.name())));

    // A checked box shows a check mark, and is not a red block.
    QStyleOptionButton check_option;
    check_option.initFrom(check);
    const QRect indicator = check->style()->subElementRect(QStyle::SE_CheckBoxIndicator, &check_option, check);
    const QImage box = check->grab(indicator).toImage();
    QCOMPARE(CountPixels(box, Reddish), 0);
    QVERIFY2(CountPixels(box, [](const QColor& c) { return c.lightness() > 150; }) > 0, "checked box has no check mark");

    // The spin box's buttons sit inside its border and draw their arrows.
    QStyleOptionSpinBox spin_option;
    spin_option.initFrom(spin);
    spin_option.subControls = QStyle::SC_SpinBoxUp | QStyle::SC_SpinBoxDown | QStyle::SC_SpinBoxFrame | QStyle::SC_SpinBoxEditField;
    spin_option.buttonSymbols = spin->buttonSymbols();
    spin_option.frame = true;
    spin_option.stepEnabled = QAbstractSpinBox::StepUpEnabled | QAbstractSpinBox::StepDownEnabled;
    for (QStyle::SubControl part : {QStyle::SC_SpinBoxUp, QStyle::SC_SpinBoxDown}) {
        const QRect button = spin->style()->subControlRect(QStyle::CC_SpinBox, &spin_option, part, spin);
        QVERIFY2(button.width() >= 14 && button.right() < spin->width() - 1,
                 qPrintable(QStringLiteral("spin button %1,%2 %3x%4 in a %5 px box").arg(button.x()).arg(button.y()).arg(button.width()).arg(button.height()).arg(spin->width())));
        const QImage arrow = spin->grab(button).toImage();
        QVERIFY2(CountPixels(arrow, [](const QColor& c) { return c.lightness() > 130; }) >= 4, "spin button draws no arrow");
    }

    // Primary, secondary and quiet commands share height, padding and type.
    QCOMPARE(primary->height(), secondary->height());
    QCOMPARE(quiet->height(), secondary->height());
    QCOMPARE(primary->sizeHint(), secondary->sizeHint());
    QCOMPARE(quiet->sizeHint(), secondary->sizeHint());
    QCOMPARE(QFontInfo(primary->font()).pixelSize(), QFontInfo(secondary->font()).pixelSize());
    QCOMPARE(QFontInfo(quiet->font()).pixelSize(), QFontInfo(secondary->font()).pixelSize());
}

//! F-442 send-back 2, items 2 and 3: a spin box always draws both arrows,
//! dimming the one that cannot step, and a check box reads against its panel
//! whether or not it is checked.
void VaultTests::benchSpinAndCheckControlsStayWhole()
{
    const RestoreApplicationStyle restore;
    QuicksilverStyle::Apply(*qApp);
    const QColor panel = QuicksilverStyle::Color(QuicksilverStyle::Token::BenchSurface);

    QWidget host;
    host.setAutoFillBackground(true);
    QPalette host_palette = host.palette();
    host_palette.setColor(QPalette::Window, panel);
    host.setPalette(host_palette);
    auto* layout = new QVBoxLayout(&host);
    auto* at_min = new QSpinBox(&host);
    at_min->setRange(0, 10);
    at_min->setValue(0);
    auto* at_max = new QSpinBox(&host);
    at_max->setRange(0, 10);
    at_max->setValue(10);
    auto* disabled = new QSpinBox(&host);
    disabled->setRange(0, 10);
    disabled->setValue(5);
    disabled->setEnabled(false);
    auto* unchecked = new QCheckBox(QStringLiteral("Route change to custom address"), &host);
    auto* checked = new QCheckBox(QStringLiteral("Visible checkbox"), &host);
    checked->setChecked(true);
    for (QAbstractSpinBox* spin : {static_cast<QAbstractSpinBox*>(at_min), static_cast<QAbstractSpinBox*>(at_max), static_cast<QAbstractSpinBox*>(disabled)}) {
        spin->setMinimumWidth(140);
        layout->addWidget(spin, 0, Qt::AlignLeft);
    }
    layout->addWidget(unchecked, 0, Qt::AlignLeft);
    layout->addWidget(checked, 0, Qt::AlignLeft);
    host.resize(360, 260);
    host.show();
    QVERIFY(QTest::qWaitForWindowExposed(&host));

    // Brightest pixel in a spin button: the arrow, dimmed or not, is drawn.
    const auto arrow_lightness = [](QSpinBox* spin, QStyle::SubControl part) {
        QStyleOptionSpinBox option;
        option.initFrom(spin);
        option.subControls = QStyle::SC_SpinBoxUp | QStyle::SC_SpinBoxDown | QStyle::SC_SpinBoxFrame | QStyle::SC_SpinBoxEditField;
        option.buttonSymbols = spin->buttonSymbols();
        option.frame = true;
        const QRect button = spin->style()->subControlRect(QStyle::CC_SpinBox, &option, part, spin);
        const QImage image = spin->grab(button).toImage();
        int arrow_pixels = 0;
        int brightest = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor c = image.pixelColor(x, y);
                if (c.lightness() > 70) ++arrow_pixels;
                brightest = std::max(brightest, c.lightness());
            }
        }
        return std::make_pair(arrow_pixels, brightest);
    };
    const auto min_up = arrow_lightness(at_min, QStyle::SC_SpinBoxUp);
    const auto min_down = arrow_lightness(at_min, QStyle::SC_SpinBoxDown);
    const auto max_up = arrow_lightness(at_max, QStyle::SC_SpinBoxUp);
    const auto max_down = arrow_lightness(at_max, QStyle::SC_SpinBoxDown);
    const auto off_up = arrow_lightness(disabled, QStyle::SC_SpinBoxUp);
    const auto off_down = arrow_lightness(disabled, QStyle::SC_SpinBoxDown);
    for (const auto& [name, arrow] : std::initializer_list<std::pair<const char*, std::pair<int, int>>>{
             {"minimum up", min_up}, {"minimum down", min_down}, {"maximum up", max_up}, {"maximum down", max_down},
             {"disabled up", off_up}, {"disabled down", off_down}}) {
        QVERIFY2(arrow.first >= 4, qPrintable(QStringLiteral("%1 arrow draws %2 pixels").arg(QLatin1String(name)).arg(arrow.first)));
    }
    // The arrow that cannot step is dimmer than the one that can.
    QVERIFY2(min_down.second < min_up.second, qPrintable(QStringLiteral("at minimum down %1 vs up %2").arg(min_down.second).arg(min_up.second)));
    QVERIFY2(max_up.second < max_down.second, qPrintable(QStringLiteral("at maximum up %1 vs down %2").arg(max_up.second).arg(max_down.second)));

    // Both check boxes show their box against the panel (non-text contrast,
    // 3:1), and the checked one shows a tick that is not an error red.
    const auto indicator_image = [](QCheckBox* box) {
        QStyleOptionButton option;
        option.initFrom(box);
        const QRect indicator = box->style()->subElementRect(QStyle::SE_CheckBoxIndicator, &option, box);
        return box->grab(indicator).toImage();
    };
    for (QCheckBox* box : {unchecked, checked}) {
        const QImage image = indicator_image(box);
        double best = 1.0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) best = std::max(best, ContrastRatio(image.pixelColor(x, y), panel));
        }
        QVERIFY2(best >= 3.0, qPrintable(QStringLiteral("%1: box border reaches %2:1 on the panel").arg(box->text()).arg(best, 0, 'f', 2)));
    }
    const QImage tick = indicator_image(checked);
    QCOMPARE(CountPixels(tick, Reddish), 0);
    QVERIFY2(CountPixels(tick, [](const QColor& c) { return c.lightness() > 200; }) >= 6, "checked box has no visible tick");
    QVERIFY2(CountPixels(indicator_image(unchecked), [](const QColor& c) { return c.lightness() > 200; }) == 0, "unchecked box draws a tick");
}

//! F-442 owner walk, item 4: the Ledger destination lines up with every other
//! page, names both filters, gives Date and Label their content before Amount,
//! keeps Export inside its panel and says what a bracketed amount means.
void VaultTests::ledgerPageNamesItsFiltersAndFitsItsColumns()
{
    const RestoreApplicationStyle restore;
    QuicksilverStyle::Apply(*qApp);
    TestChain100Setup test{ChainType::SANDBOX, {.extra_args = {"-txpownocycle=1"}}};
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);
    std::shared_ptr<CVault> vault = SetupDescriptorsVault(m_node, test);

    QSettings().setValue(QStringLiteral("Desktop/ConsensusEnabled"), false);
    QSettings().remove(QStringLiteral("TransactionViewHeaderState"));
    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    QScopedPointer<const NetworkStyle> network_style(NetworkStyle::instantiate(Params().GetChainType()));
    MiniGUI mini_gui(m_node, platform_style.get());
    mini_gui.initModelForVault(m_node, vault, platform_style.get());

    QuicksilverGUI window(m_node, platform_style.get(), network_style.data());
    VaultFrame* frame = window.findChild<VaultFrame*>(QStringLiteral("vaultFrame"));
    QVERIFY(frame);
    auto* view = new VaultView(mini_gui.vaultModel.get(), platform_style.get(), frame);
    QVERIFY(frame->addView(view));
    frame->setCurrentVault(mini_gui.vaultModel.get());
    window.resize(1200, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    QAction* home = window.findChild<QAction*>(QStringLiteral("homeBootstrapAction"));
    QVERIFY(home);
    home->trigger();
    QCoreApplication::processEvents();
    QWidget* home_ledger = window.findChild<QWidget*>(QStringLiteral("homeLedger"));
    QVERIFY(home_ledger);
    const int page_left = home_ledger->mapTo(&window, QPoint(0, 0)).x();

    QAction* history = window.findChild<QAction*>(QStringLiteral("historyAction"));
    QVERIFY(history);
    history->trigger();
    QCoreApplication::processEvents();
    QFrame* panel = view->findChild<QFrame*>(QStringLiteral("transactionLedgerPanel"));
    QVERIFY(panel);
    QTRY_VERIFY(panel->isVisible());
    QCOMPARE(panel->mapTo(&window, QPoint(0, 0)).x(), page_left);

    // Both filters say what they filter.
    QList<QComboBox*> combos = panel->findChildren<QComboBox*>();
    QCOMPARE(combos.size(), 2);
    QStringList first_items;
    for (QComboBox* combo : combos) first_items << combo->itemText(0);
    first_items.sort();
    QCOMPARE(first_items, (QStringList{QStringLiteral("All dates"), QStringLiteral("All types")}));

    // Export is one of the panel's commands.
    QPushButton* export_button = window.findChild<QPushButton*>(QStringLiteral("transactionExportButton"));
    QVERIFY(export_button);
    QVERIFY(panel->isAncestorOf(export_button));

    // The page says which states are not spendable yet. Send-back 2 item 4
    // replaced the bracketed amount with Home's signed amount and state word,
    // so the legend names the states, not brackets.
    QLabel* legend = panel->findChild<QLabel*>(QStringLiteral("transactionLedgerLegend"));
    QVERIFY(legend);
    QVERIFY(legend->isVisible());
    QVERIFY2(!legend->text().contains(QStringLiteral("brackets")), qPrintable(legend->text()));
    QVERIFY2(legend->text().contains(QStringLiteral("Pending")) && legend->text().contains(QStringLiteral("Maturing")), qPrintable(legend->text()));
    // F-451: the page reads rows as Home does, a state word and a dot on the
    // date, so there is no status clock for the legend to explain.
    QVERIFY2(!legend->text().contains(QStringLiteral("clock")), qPrintable(legend->text()));

    // Date and Label get their content width; Amount takes no slack.
    QTableView* table = panel->findChild<QTableView*>(QStringLiteral("transactionView"));
    QVERIFY(table);
    // QTableView narrows the public QAbstractItemView call to protected.
    QAbstractItemView* cells = table;
    QTRY_VERIFY(table->model() && table->model()->rowCount() > 0);
    QCoreApplication::processEvents();
    for (int row = 0; row < table->model()->rowCount(); ++row) {
        QVERIFY(table->model()->index(row, TransactionTableModel::Status).data(Qt::DecorationRole).isNull());
    }
    const auto fits = [&](int column) { return table->columnWidth(column) >= cells->sizeHintForColumn(column); };
    QVERIFY2(fits(TransactionTableModel::Date),
             qPrintable(QStringLiteral("Date %1 px of %2").arg(table->columnWidth(TransactionTableModel::Date)).arg(cells->sizeHintForColumn(TransactionTableModel::Date))));
    QVERIFY2(fits(TransactionTableModel::ToAddress),
             qPrintable(QStringLiteral("Label %1 px of %2").arg(table->columnWidth(TransactionTableModel::ToAddress)).arg(cells->sizeHintForColumn(TransactionTableModel::ToAddress))));
    // A header saved before the state had a word kept a 30 px icon column;
    // the next fit widens it to the word.
    table->setColumnWidth(TransactionTableModel::Status, 30);
    TransactionView* ledger_view = window.findChild<TransactionView*>();
    QVERIFY(ledger_view && ledger_view->isAncestorOf(table));
    QVERIFY(QMetaObject::invokeMethod(ledger_view, "fitColumns"));
    QVERIFY2(fits(TransactionTableModel::Status),
             qPrintable(QStringLiteral("State %1 px of %2").arg(table->columnWidth(TransactionTableModel::Status)).arg(cells->sizeHintForColumn(TransactionTableModel::Status))));
    const int amount_slack = table->columnWidth(TransactionTableModel::Amount) - std::max(cells->sizeHintForColumn(TransactionTableModel::Amount),
                                                                                         table->horizontalHeader()->sectionSizeHint(TransactionTableModel::Amount));
    QVERIFY2(amount_slack <= 24, qPrintable(QStringLiteral("Amount carries %1 px of slack").arg(amount_slack)));
}

//! F-442 send-back 2, item 4, and F-451: Home's ledger and the Ledger page read
//! the same transaction the same way in every column (state word, friendly date
//! with its state dot, type, label, signed amount); Home gives the
//! label its content width before any slack; and a reward that is still
//! maturing reads as a caution, not an error.
void VaultTests::homeAndLedgerReadTransactionsAlike()
{
    const RestoreApplicationStyle restore;
    QuicksilverStyle::Apply(*qApp);
    TestChain100Setup test{ChainType::SANDBOX, {.extra_args = {"-txpownocycle=1"}}};
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);
    // TestChain100Setup's rewards pay a bare pubkey, which has no address and
    // so no label. One reward to the key's P2WPKH address gives a labelled row.
    test.CreateAndProcessBlock({}, GetScriptForDestination(WitnessV0KeyHash(test.coinbaseKey.GetPubKey())));
    std::shared_ptr<CVault> vault = SetupDescriptorsVault(m_node, test);
    const QString payout_label = QStringLiteral("founding-payout-b58");
    {
        LOCK(vault->cs_vault);
        vault->SetAddressBook(CTxDestination{WitnessV0KeyHash(test.coinbaseKey.GetPubKey())}, payout_label.toStdString(), vault::AddressPurpose::RECEIVE);
    }

    QSettings().setValue(QStringLiteral("Desktop/ConsensusEnabled"), false);
    QSettings().remove(QStringLiteral("TransactionViewHeaderState"));
    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    QScopedPointer<const NetworkStyle> network_style(NetworkStyle::instantiate(Params().GetChainType()));
    MiniGUI mini_gui(m_node, platform_style.get());
    mini_gui.initModelForVault(m_node, vault, platform_style.get());

    QuicksilverGUI window(m_node, platform_style.get(), network_style.data());
    VaultFrame* frame = window.findChild<VaultFrame*>(QStringLiteral("vaultFrame"));
    QVERIFY(frame);
    auto* view = new VaultView(mini_gui.vaultModel.get(), platform_style.get(), frame);
    QVERIFY(frame->addView(view));
    frame->setCurrentVault(mini_gui.vaultModel.get());
    window.resize(1200, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    QAction* home = window.findChild<QAction*>(QStringLiteral("homeBootstrapAction"));
    QVERIFY(home);
    home->trigger();
    QTableView* home_table = window.findChild<QTableView*>(QStringLiteral("homeLedgerTable"));
    QVERIFY(home_table);
    QTRY_VERIFY(home_table->isVisible() && home_table->model() && home_table->model()->rowCount() > 0);
    QCoreApplication::processEvents();
    QAbstractItemModel* home_rows = home_table->model();

    QAction* history = window.findChild<QAction*>(QStringLiteral("historyAction"));
    QVERIFY(history);
    history->trigger();
    QTableView* ledger_table = window.findChild<QTableView*>(QStringLiteral("transactionView"));
    QVERIFY(ledger_table);
    QTRY_VERIFY(ledger_table->model() && ledger_table->model()->rowCount() > 0);
    QAbstractItemModel* ledger_rows = ledger_table->model();
    // Home holds the newest 10; the Ledger page holds them all.
    QVERIFY(ledger_rows->rowCount() > 10);
    QCOMPARE(home_rows->rowCount(), 10);
    for (int column = 0; column < ledger_rows->columnCount(); ++column) {
        QCOMPARE(ledger_rows->headerData(column, Qt::Horizontal).toString(), home_rows->headerData(column, Qt::Horizontal).toString());
    }

    // Row by row, matched on the transaction, both views say the same words.
    int maturing = -1;
    bool labelled = false;
    for (int row = 0; row < home_rows->rowCount(); ++row) {
        const QModelIndex home_index = home_rows->index(row, 0);
        const QString hash = home_index.data(TransactionTableModel::TxHashRole).toString();
        const QModelIndexList found = ledger_rows->match(ledger_rows->index(0, 0), TransactionTableModel::TxHashRole, hash, 1, Qt::MatchExactly);
        QVERIFY2(found.size() == 1, qPrintable(hash));
        for (int column : {TransactionTableModel::Date, TransactionTableModel::Type, TransactionTableModel::ToAddress, TransactionTableModel::Amount, TransactionTableModel::Status}) {
            const QString on_home = home_rows->index(row, column).data(Qt::DisplayRole).toString();
            const QString on_ledger = ledger_rows->index(found.first().row(), column).data(Qt::DisplayRole).toString();
            QVERIFY2(on_home == on_ledger, qPrintable(QStringLiteral("column %1: Home \"%2\", Ledger \"%3\"").arg(column).arg(on_home, on_ledger)));
            QVERIFY2(!on_home.isEmpty(), qPrintable(QStringLiteral("column %1 is empty").arg(column)));
        }
        QVERIFY(!ledger_rows->index(found.first().row(), TransactionTableModel::Date).data(Qt::DecorationRole).isNull());
        if (home_index.data(TransactionTableModel::StatusRole).toInt() == TransactionStatus::Immature) maturing = row;
        if (home_rows->index(row, TransactionTableModel::ToAddress).data().toString() == payout_label) labelled = true;
    }
    QVERIFY(labelled);

    // A maturing reward is a caution: the warning tone, never the error red.
    QVERIFY(maturing >= 0);
    const QColor state_color = home_rows->index(maturing, TransactionTableModel::Status).data(Qt::ForegroundRole).value<QColor>();
    QVERIFY2(state_color == QuicksilverStyle::Color(QuicksilverStyle::Token::Warning), qPrintable(state_color.name()));

    // Home: the label gets its content width before Amount takes any slack.
    home->trigger();
    QTRY_VERIFY(home_table->isVisible());
    QCoreApplication::processEvents();
    QAbstractItemView* home_cells = home_table;
    // Whether the label fits whole at 1200 px is a question about real glyph
    // metrics; "minimal" measures text with a placeholder font database, about
    // twice as wide, so the fit runs where fonts are real (as on Home's layout).
    const bool real_fonts = !QFontDatabase().families().isEmpty();
    const int label_need = home_cells->sizeHintForColumn(TransactionTableModel::ToAddress);
    if (real_fonts) {
        QVERIFY2(home_table->columnWidth(TransactionTableModel::ToAddress) >= label_need,
                 qPrintable(QStringLiteral("Label %1 px of %2").arg(home_table->columnWidth(TransactionTableModel::ToAddress)).arg(label_need)));
        QVERIFY(home_table->fontMetrics().horizontalAdvance(payout_label) < home_table->columnWidth(TransactionTableModel::ToAddress));
    }
    const int amount_slack = home_table->columnWidth(TransactionTableModel::Amount) - std::max(home_cells->sizeHintForColumn(TransactionTableModel::Amount),
                                                                                               home_table->horizontalHeader()->sectionSizeHint(TransactionTableModel::Amount));
    QVERIFY2(amount_slack <= 24, qPrintable(QStringLiteral("Amount carries %1 px of slack").arg(amount_slack)));
}

//! Home at the first-launch size: the ledger takes the width, the instruments
//! keep a narrow fixed column, and nothing scrolls.
//!
//! The first port gave the ledger about a third of the width, cut its labels and
//! amounts short, and left Home taller than a 1200x800 window.
void VaultTests::homeLaysOutLedgerBesideInstruments()
{
    const RestoreApplicationStyle restore;
    QuicksilverStyle::Apply(*qApp);
    TestChain100Setup test{ChainType::SANDBOX, {.extra_args = {"-txpownocycle=1"}}};
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);
    std::shared_ptr<CVault> vault = SetupDescriptorsVault(m_node, test);

    QSettings().setValue(QStringLiteral("Desktop/ConsensusEnabled"), false);
    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    QScopedPointer<const NetworkStyle> network_style(NetworkStyle::instantiate(Params().GetChainType()));
    MiniGUI mini_gui(m_node, platform_style.get());
    mini_gui.initModelForVault(m_node, vault, platform_style.get());

    QuicksilverGUI window(m_node, platform_style.get(), network_style.data());
    VaultFrame* frame = window.findChild<VaultFrame*>(QStringLiteral("vaultFrame"));
    QVERIFY(frame);
    auto* view = new VaultView(mini_gui.vaultModel.get(), platform_style.get(), frame);
    QVERIFY(frame->addView(view));
    frame->setCurrentVault(mini_gui.vaultModel.get());
    QAction* home = window.findChild<QAction*>(QStringLiteral("homeBootstrapAction"));
    QVERIFY(home);
    home->trigger();
    window.resize(1200, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    DesktopLaunchPage* launch = window.findChild<DesktopLaunchPage*>(QStringLiteral("desktopLaunchPage"));
    QVERIFY(launch);
    QWidget* ledger = launch->findChild<QWidget*>(QStringLiteral("homeLedger"));
    QWidget* instruments = launch->findChild<QWidget*>(QStringLiteral("homeInstruments"));
    QScrollArea* scroll = window.findChild<QScrollArea*>(QStringLiteral("mainContentScrollArea"));
    QVERIFY(ledger);
    QVERIFY(instruments);
    QVERIFY(scroll);

    // Whether Home fits is a question about real glyph metrics. The default
    // test platform ("minimal") measures text with a placeholder font database,
    // so the scroll checks run where fonts are real (QT_QPA_PLATFORM=offscreen
    // or a display); the proportions hold either way.
    const bool real_fonts = !QFontDatabase().families().isEmpty();

    // Before consensus, with consensus on, and with consensus on and no peer
    // (the tallest instrument column the page has).
    const auto check = [&](const char* state) {
        QCoreApplication::processEvents();
        const QString where = QString::fromLatin1(state);
        QVERIFY2(instruments->width() <= 340, qPrintable(where + QStringLiteral(": instruments %1 px").arg(instruments->width())));
        QVERIFY2(ledger->width() > instruments->width(),
                 qPrintable(where + QStringLiteral(": ledger %1 px, instruments %2 px").arg(ledger->width()).arg(instruments->width())));
        if (!real_fonts) return;
        QTRY_VERIFY2(scroll->verticalScrollBar()->maximum() == 0 && scroll->horizontalScrollBar()->maximum() == 0,
                     qPrintable(where + QStringLiteral(": Home scrolls %1 px down, %2 px across; the instruments need %3 px")
                                            .arg(scroll->verticalScrollBar()->maximum()).arg(scroll->horizontalScrollBar()->maximum())
                                            .arg(instruments->minimumSizeHint().height())));
    };
    check("before consensus");
    if (QTest::currentTestFailed()) return;
    launch->setConsensusEnabled(true);
    launch->setPeerCount(8);
    launch->setChainTip(105, QDateTime::currentDateTime());
    launch->setSyncState(true, 1.0);
    check("consensus on");
    if (QTest::currentTestFailed()) return;
    launch->setPeerCount(0);
    check("consensus on, no peers");
    if (QTest::currentTestFailed()) return;
    if (!real_fonts) QSKIP("Home's fit at 1200x800 needs real font metrics; run with QT_QPA_PLATFORM=offscreen");
}

void VaultTests::sendConfirmationNamesUnconfirmedChange()
{
    // Same chain the send-flow fixture uses: five mature coinbases, and the
    // sandbox no-cycle proof so preparation finishes inside the helper's wait.
    TestChain100Setup test{ChainType::SANDBOX, {.extra_args = {"-txpownocycle=1"}}};
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);

    std::shared_ptr<CVault> vault = SetupDescriptorsVault(m_node, test);
    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    MiniGUI mini_gui(m_node, platformStyle.get());
    mini_gui.initModelForVault(m_node, vault, platformStyle.get());
    SendCoinsDialog& sendCoinsDialog = mini_gui.sendCoinsDialog;

    const QString warning = QStringLiteral(
        "This transfer spends change from an earlier transfer that has not confirmed yet. "
        "If that transfer is dropped, this one fails too and its work is lost. "
        "To avoid this, wait for the earlier transfer to confirm.");

    QString confirmed_text;
    const uint256 confirmed_txid = SendCoins(*vault, sendCoinsDialog, PKHash(), COIN, QMessageBox::Cancel, &confirmed_text);
    QVERIFY(confirmed_txid.IsNull());
    QVERIFY(confirmed_text.contains(QStringLiteral("Sending uses proof-of-work")));
    QVERIFY(!confirmed_text.contains(warning));

    {
        LOCK(vault->cs_vault);
        const std::vector<vault::COutput> coins{vault::AvailableCoins(*vault).All()};
        QVERIFY(coins.size() >= 2);
        const vault::COutput* keep = &coins.front();
        for (const vault::COutput& coin : coins) {
            if (coin.txout.nValue > keep->txout.nValue) keep = &coin;
        }
        QVERIFY(keep->txout.nValue > 2 * COIN);
        QVERIFY(keep->depth >= 1);
        for (const vault::COutput& coin : coins) {
            if (coin.outpoint == keep->outpoint) continue;
            QVERIFY(vault->LockCoin(coin.outpoint));
        }
    }

    QString parent_text;
    const uint256 parent_txid = SendCoins(*vault, sendCoinsDialog, PKHash(), COIN, QMessageBox::Yes, &parent_text);
    QVERIFY(!parent_txid.IsNull());
    QVERIFY(!parent_text.contains(warning));

    {
        LOCK(vault->cs_vault);
        const std::vector<vault::COutput> coins{vault::AvailableCoins(*vault).All()};
        QCOMPARE(coins.size(), size_t{1});
        QCOMPARE(coins.front().depth, 0);
        QVERIFY(coins.front().from_me);
        QVERIFY(coins.front().txout.nValue > COIN);
    }

    QString unconfirmed_text;
    const uint256 unconfirmed_txid = SendCoins(*vault, sendCoinsDialog, PKHash(), COIN, QMessageBox::Cancel, &unconfirmed_text);
    QVERIFY(unconfirmed_txid.IsNull());
    QVERIFY(unconfirmed_text.contains(QStringLiteral("Sending uses proof-of-work")));
    QVERIFY(unconfirmed_text.contains(warning));
}

void VaultTests::coinControlMarksUnconfirmedChange()
{
    // The send-flow chain again: five mature coinbases and the sandbox no-cycle
    // proof, so one real transfer leaves this vault holding depth-0 change.
    TestChain100Setup test{ChainType::SANDBOX, {.extra_args = {"-txpownocycle=1"}}};
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);

    std::shared_ptr<CVault> vault = SetupDescriptorsVault(m_node, test);
    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    MiniGUI mini_gui(m_node, platformStyle.get());
    mini_gui.initModelForVault(m_node, vault, platformStyle.get());

    const uint256 parent_txid = SendCoins(*vault, mini_gui.sendCoinsDialog, PKHash(), COIN);
    QVERIFY(!parent_txid.IsNull());

    COutPoint change_outpoint;
    COutPoint confirmed_outpoint;
    {
        LOCK(vault->cs_vault);
        for (const vault::COutput& coin : vault::AvailableCoins(*vault).All()) {
            if (coin.outpoint.hash == parent_txid) {
                QCOMPARE(coin.depth, 0);
                change_outpoint = coin.outpoint;
            } else if (coin.depth >= 1) {
                confirmed_outpoint = coin.outpoint;
            }
        }
    }
    QVERIFY(!change_outpoint.IsNull());
    QVERIFY(!confirmed_outpoint.IsNull());

    vault::CCoinControl coin_control;
    CoinControlDialog dialog(coin_control, mini_gui.vaultModel.get(), platformStyle.get());
    dialog.findChild<QRadioButton*>("radioListMode")->click();
    QTreeWidget* tree = dialog.findChild<QTreeWidget*>("treeWidget");
    QVERIFY(tree);

    // CoinControlDialog's column and role enums are private; these are their values.
    constexpr int column_address{3};
    constexpr int column_confirmations{5};
    constexpr int tx_hash_role{Qt::UserRole};
    constexpr int vout_role{Qt::UserRole + 1};
    const auto find_row = [&](const COutPoint& outpoint) -> QTreeWidgetItem* {
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* item = tree->topLevelItem(i);
            if (item->data(column_address, tx_hash_role).toString() == QString::fromStdString(outpoint.hash.GetHex()) &&
                item->data(column_address, vout_role).toUInt() == outpoint.n) {
                return item;
            }
        }
        return nullptr;
    };

    const QString tooltip = QStringLiteral(
        "Change from an earlier transfer that has not confirmed yet. "
        "A transfer that spends it will fail if the earlier one is dropped. "
        "To avoid this, wait for the earlier transfer to confirm.");

    QTreeWidgetItem* change_row = find_row(change_outpoint);
    QVERIFY(change_row);
    QCOMPARE(change_row->text(column_confirmations), QStringLiteral("0 (unconfirmed change)"));
    QCOMPARE(change_row->data(column_confirmations, Qt::UserRole).toLongLong(), qlonglong{0});
    for (int column = 0; column < tree->columnCount(); ++column) {
        QCOMPARE(change_row->toolTip(column), tooltip);
    }
    QVERIFY(change_row->flags() & Qt::ItemIsEnabled);
    QVERIFY(change_row->flags() & Qt::ItemIsUserCheckable);
    QVERIFY(!change_row->isDisabled());

    QTreeWidgetItem* confirmed_row = find_row(confirmed_outpoint);
    QVERIFY(confirmed_row);
    QVERIFY(confirmed_row->text(column_confirmations).toInt() >= 1);
    QVERIFY(!confirmed_row->text(column_confirmations).contains(QStringLiteral("unconfirmed change")));
    for (int column = 0; column < tree->columnCount(); ++column) {
        QVERIFY(confirmed_row->toolTip(column) != tooltip);
    }
}

void VaultTests::minePageSeparatesDraftPayoutAndHealthNote()
{
    const RestoreApplicationStyle restore;
    QuicksilverStyle::Apply(*qApp);
    MineMintPage page;
    page.setPayoutAddress(QStringLiteral("hg1qdraft"));
    interfaces::MiningStatus status;
    page.setStatus(status);
    auto* payout = page.findChild<QLabel*>(QStringLiteral("payoutTargetValue"));
    QVERIFY(payout);
    // Owner ruling, send-back 2 item 6: while idle, an entered address is
    // what the row names, marked as applied on Start; "Not set" only when the
    // field is empty too; while running, the active payout.
    QCOMPARE(payout->text(), QStringLiteral("hg1qdraft (applied on Start)"));
    QCOMPARE(page.payoutAddress(), QStringLiteral("hg1qdraft"));
    status.address = "hg1qprevious";
    page.setStatus(status);
    QCOMPARE(payout->text(), QStringLiteral("hg1qdraft (applied on Start)"));
    page.setPayoutAddress(QStringLiteral("hg1qnext"));
    QCOMPARE(payout->text(), QStringLiteral("hg1qnext (applied on Start)"));
    status.active = true;
    page.setStatus(status);
    QCOMPARE(payout->text(), QStringLiteral("hg1qprevious"));
    QCOMPARE(page.payoutAddress(), QStringLiteral("hg1qnext"));
    status.active = false;
    status.address.clear();
    page.setPayoutAddress(QString());
    page.setStatus(status);
    QCOMPARE(payout->text(), QStringLiteral("Not set"));
    auto* panel = page.findChild<QFrame*>(QStringLiteral("mineMintPayoutPanel"));
    auto* intro = page.findChild<QLabel*>(QStringLiteral("mineMintEmptyState"));
    QVERIFY(panel && intro);
    QVERIFY(panel->isAncestorOf(intro));
    auto* health = page.findChild<QLabel*>(QStringLiteral("solverHealthValue"));
    QVERIFY(health);
    QCOMPARE(health->property("class").toString(), QStringLiteral("benchNote"));
    QVERIFY(!QFontInfo(health->font()).fixedPitch());
    page.resize(980, 600);
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));
    QVERIFY(health->width() > page.width() / 2);
    QCOMPARE(page.findChild<QPushButton*>(QStringLiteral("startMiningButton"))->height(),
             page.findChild<QPushButton*>(QStringLiteral("stopMiningButton"))->height());
}

void VaultTests::aboutDialogWrapsAtWords()
{
    HelpMessageDialog dialog(nullptr, true);
    auto* text = dialog.findChild<QTextBrowser*>(QStringLiteral("aboutMessage"));
    QVERIFY(text);
    QCOMPARE(text->wordWrapMode(), QTextOption::WordWrap);
    QVERIFY(text->toPlainText().contains(QRegularExpression(QStringLiteral("Copyright \\(C\\) [0-9-]+ The Bitcoin Core developers"))));
    QVERIFY(text->openExternalLinks());
    dialog.resize(620, 400);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    for (QTextBlock block = text->document()->begin(); block.isValid(); block = block.next()) {
        const QString content = block.text();
        const auto* layout = block.layout();
        for (int i = 1; i < layout->lineCount(); ++i) {
            const int start = layout->lineAt(i).textStart();
            QVERIFY2(start == 0 || !content.at(start - 1).isLetter() || !content.at(start).isLetter(), qPrintable(content));
        }
    }
}

void VaultTests::mineMintPageRendersStatus()
{
    MineMintPage page;
    interfaces::MiningStatus st;
    st.active = true;
    st.address = "hg1qexample";
    st.blocks_found = 3;
    st.coins_minted_session = 150 * COIN;
    st.attempts_per_second = 0.29;
    st.graphs_attempted = 7;
    st.congestion_multiplier = 1.0;
    st.gpu_solver = true;
    page.setStatus(st);

    QCOMPARE(page.findChild<QLabel*>("miningStatusValue")->text(), QString("Active"));
    QCOMPARE(page.findChild<QLabel*>("payoutTargetValue")->text(), QString("hg1qexample"));
    QCOMPARE(page.findChild<QLabel*>("solverStatusValue")->text(), QString("GPU bridge"));
    QCOMPARE(page.findChild<QLabel*>("blocksFoundValue")->text(), QString("3"));
    // S3: the expected text is spelled out here rather than composed by re-invoking the
    // very numerus call the page made. Building the oracle out of the production
    // expression made this assertion a tautology with respect to the only property it
    // looks like it checks -- it passed identically whether the label read "7 graphs
    // attempted" or the unresolved numerus form, which is exactly the defect that
    // shipped. No translator is installed in this binary, nor in the application (see
    // initTranslations() in qt/quicksilver.cpp), so the rendered English is knowable
    // and must be asserted.
    QCOMPARE(page.findChild<QLabel*>("attemptsRateValue")->text(),
             QStringLiteral("0.290 (7 graphs attempted)"));

    // One is the count the numerus idiom was there to get right, so it is the count
    // worth pinning: "1 graphs attempted" would be just as wrong as "%n graph(s)".
    st.graphs_attempted = 1;
    page.setStatus(st);
    QCOMPARE(page.findChild<QLabel*>("attemptsRateValue")->text(),
             QStringLiteral("0.290 (1 graph attempted)"));

    // An armed miner that has not finished its first graph has no rate to show.
    // Printing 0.000 is what made it indistinguishable from a dead card, so the
    // page must say which state it is in.
    st.attempts_per_second.reset();
    st.graphs_attempted = 0;
    page.setStatus(st);
    QCOMPARE(page.findChild<QLabel*>("attemptsRateValue")->text(),
             QStringLiteral("Warming up"));

    st.active = false;
    page.setStatus(st);
    QCOMPARE(page.findChild<QLabel*>("attemptsRateValue")->text(),
             QStringLiteral("Idle"));

    // F-109: the core fault string is written for quicksilver-daemon and names a
    // command-line flag. This window owns the setting, so the missing-solver case
    // must name the control instead. The flag must not survive anywhere in the row:
    // it is the whole defect, and asserting only the new prose would let it back in
    // as an appended "(-cuckatoosolver=<path>)".
    QLabel* health = page.findChild<QLabel*>("solverHealthValue");
    QVERIFY(health);
    st.active = true;
    st.solver_ok = false;
    st.solver_missing = true;
    st.last_solver_error = "No GPU solver is configured; set -cuckatoosolver=<path>";
    page.setStatus(st);
    QCOMPARE(health->text(),
             QStringLiteral("No GPU solver is configured. Choose one in Settings > Options > Main."));
    QVERIFY(!health->text().contains(QStringLiteral("-cuckatoosolver")));
    // And the control itself, not just its address: this is the only health state
    // the user can act on from this page.
    QPushButton* configure = page.findChild<QPushButton*>("configureSolverButton");
    QVERIFY(configure);
    QVERIFY(configure->isVisibleTo(&page));
    QSignalSpy solver_settings_spy(&page, &MineMintPage::solverSettingsRequested);
    QVERIFY(solver_settings_spy.isValid());
    configure->click();
    QCOMPARE(solver_settings_spy.count(), 1);

    // A solver that ran and failed is a different thing, and its message is the
    // useful one: it names the card, the driver or the log. Keep passing it through.
    st.solver_missing = false;
    st.last_solver_error = "The GPU solver exited with an error; see the debug log";
    page.setStatus(st);
    QCOMPARE(health->text(),
             QStringLiteral("The GPU solver exited with an error; see the debug log"));
    // Nothing on this page fixes a dead card, so the button must go away again
    // rather than offer a setting that is already correct.
    QVERIFY(!configure->isVisibleTo(&page));
}

//! Four inputs, four row sets. The halted input is the one the page used to
//! render as Active / CPU / Warming up: armed, no solver, fallback not permitted.
//! Checked on the pure helper, then applied to a page so a missed wiring fails too.
void VaultTests::mineMintPageNamesAnArmedMinerWithNoPermittedSolver()
{
    const auto rows = [](bool active, bool gpu, bool possible, bool solver_ok, bool missing,
                         uint64_t graphs, std::optional<double> rate, const char* error) {
        interfaces::MiningStatus st;
        st.active = active;
        st.gpu_solver = gpu;
        st.block_solving_possible = possible;
        st.solver_ok = solver_ok;
        st.solver_missing = missing;
        st.graphs_attempted = graphs;
        st.attempts_per_second = rate;
        if (error) st.last_solver_error = error;
        return MineMintPage::statusTextForTesting(st);
    };

    const auto idle = rows(false, false, true, true, false, 0, std::nullopt, nullptr);
    QCOMPARE(idle.block_mining, QStringLiteral("Idle"));
    QCOMPARE(idle.solver, QStringLiteral("CPU"));
    QCOMPARE(idle.attempts, QStringLiteral("Idle"));
    QCOMPARE(idle.health, QStringLiteral("Not running"));
    QVERIFY(!idle.show_configure_solver);

    // Stopped after mining: idle, with the graphs it attempted, and no rate.
    QCOMPARE(rows(false, false, true, true, false, 7, std::nullopt, nullptr).attempts, QStringLiteral("Idle (7 graphs attempted)"));
    QCOMPARE(rows(false, false, true, true, false, 1, 0.29, nullptr).attempts, QStringLiteral("Idle (1 graph attempted)"));

    const auto warming = rows(true, true, true, true, false, 0, std::nullopt, nullptr);
    QCOMPARE(warming.block_mining, QStringLiteral("Active"));
    QCOMPARE(warming.solver, QStringLiteral("GPU bridge"));
    QCOMPARE(warming.attempts, QStringLiteral("Warming up"));
    QCOMPARE(warming.health, QStringLiteral("Working — no graph finished yet"));
    QVERIFY(!warming.show_configure_solver);

    const auto dead = rows(true, true, true, false, false, 4, 0.29,
                           "The GPU solver exited with an error; see the debug log");
    QCOMPARE(dead.block_mining, QStringLiteral("Active"));
    QCOMPARE(dead.solver, QStringLiteral("GPU bridge"));
    QCOMPARE(dead.attempts, QStringLiteral("0.290 (4 graphs attempted)"));
    QCOMPARE(dead.health, QStringLiteral("The GPU solver exited with an error; see the debug log"));
    QVERIFY(!dead.show_configure_solver);

    // Before the first solve attempt the node has already refused the
    // configuration, and solver_ok is still the default. This is the reading
    // that used to say the miner was warming up on the CPU.
    const auto halted = rows(true, false, false, true, false, 0, std::nullopt, nullptr);
    QCOMPARE(halted.block_mining, QStringLiteral("Halted"));
    QCOMPARE(halted.solver, QStringLiteral("None"));
    QCOMPARE(halted.attempts, QStringLiteral("Not solving"));
    QCOMPARE(halted.health, QStringLiteral("No GPU solver is configured, and processor block mining is off. Choose a solver, or allow processor block mining, in Settings > Options > Main."));
    QVERIFY(!halted.health.contains(QStringLiteral("-cuckatoosolver")));
    QVERIFY(!halted.health.contains(QStringLiteral("-allowcpumining")));
    QVERIFY(halted.show_configure_solver);

    // After the attempt that never ran, solver_missing becomes true. Same rows:
    // the page must not fall through to the older "choose a solver" sentence
    // or back to Warming up.
    const auto halted_after = rows(true, false, false, false, true, 0, std::nullopt,
                                   "No GPU solver is configured; set -cuckatoosolver=<path>");
    QCOMPARE(halted_after.block_mining, halted.block_mining);
    QCOMPARE(halted_after.solver, halted.solver);
    QCOMPARE(halted_after.attempts, halted.attempts);
    QCOMPARE(halted_after.health, halted.health);
    QVERIFY(!halted_after.health.contains(QStringLiteral("-cuckatoosolver")));
    QVERIFY(halted_after.show_configure_solver);

    MineMintPage page;
    interfaces::MiningStatus st;
    st.active = true;
    st.gpu_solver = false;
    st.block_solving_possible = false;
    st.solver_ok = true;
    st.graphs_attempted = 0;
    page.setStatus(st);
    QCOMPARE(page.findChild<QLabel*>(QStringLiteral("miningStatusValue"))->text(), halted.block_mining);
    QCOMPARE(page.findChild<QLabel*>(QStringLiteral("solverStatusValue"))->text(), halted.solver);
    QCOMPARE(page.findChild<QLabel*>(QStringLiteral("attemptsRateValue"))->text(), halted.attempts);
    QCOMPARE(page.findChild<QLabel*>(QStringLiteral("solverHealthValue"))->text(), halted.health);
    QPushButton* configure = page.findChild<QPushButton*>(QStringLiteral("configureSolverButton"));
    QVERIFY(configure);
    QVERIFY(configure->isVisibleTo(&page));
    QSignalSpy solver_settings_spy(&page, &MineMintPage::solverSettingsRequested);
    QVERIFY(solver_settings_spy.isValid());
    configure->click();
    QCOMPARE(solver_settings_spy.count(), 1);
    // Still armed: the user can stop the halted role. Start stays disabled.
    QVERIFY(!page.findChild<QPushButton*>(QStringLiteral("startMiningButton"))->isEnabled());
    QVERIFY(page.findChild<QPushButton*>(QStringLiteral("stopMiningButton"))->isEnabled());
}

//! F-108: a node with no peers mines onto a chain of its own, and every reading on
//! this page looks the same either way -- solver working, graphs climbing, blocks
//! found. On the Windows walk the page reported a mined coin while the node had
//! never connected to anything, and nothing on it said so.
void VaultTests::mineMintPageRefusesToPresentIsolatedMiningAsSuccess()
{
    MineMintPage page;
    // Shown, because the confirmation below is parented to this page: a dialog whose
    // parent was never on screen does not go modal the way the user's does.
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));
    QWidget* panel = page.findChild<QWidget*>(QStringLiteral("mineMintIsolationPanel"));
    QVERIFY(panel);

    interfaces::MiningStatus st;
    st.active = true;
    st.blocks_found = 1;
    st.gpu_solver = true;
    page.setStatus(st);

    // Before any client model there is no peer count, and "unknown" is not "zero":
    // a page with no node behind it must not accuse the user of being offline.
    QVERIFY(!panel->isVisibleTo(&page));
    QLabel* blocks = page.findChild<QLabel*>(QStringLiteral("blocksFoundValue"));
    QVERIFY(blocks);
    QCOMPARE(blocks->text(), QStringLiteral("1"));

    page.setPeerCount(0);
    QVERIFY(panel->isVisibleTo(&page));
    QLabel* banner = page.findChild<QLabel*>(QStringLiteral("mineMintIsolationBanner"));
    QVERIFY(banner);
    QVERIFY(banner->text().contains(QStringLiteral("Not connected to any peer")));
    // The count is the sentence the walk read as success. While the node is isolated
    // it is not a count of blocks on the network, and the row has to say which it is.
    QVERIFY(blocks->text().contains(QStringLiteral("1")));
    QVERIFY(blocks->text().contains(QStringLiteral("not the network's")));

    // The banner is a dead end without a route to the fix, and the fix is a page.
    QSignalSpy network_spy(&page, &MineMintPage::networkHelpRequested);
    QVERIFY(network_spy.isValid());
    QPushButton* open_network = page.findChild<QPushButton*>(QStringLiteral("mineMintIsolationNetworkButton"));
    QVERIFY(open_network);
    open_network->click();
    QCOMPARE(network_spy.count(), 1);

    // A peer arriving retires all of it, including the qualifier on the count.
    page.setPeerCount(3);
    QVERIFY(!panel->isVisibleTo(&page));
    QCOMPARE(blocks->text(), QStringLiteral("1"));

    // Arming with peers is unchanged: no dialog, straight through. Asserted first so
    // the confirmation below is known to be caused by the peer count and not by the
    // button having grown a dialog unconditionally.
    QSignalSpy start_spy(&page, &MineMintPage::startRequested);
    QVERIFY(start_spy.isValid());
    page.setPayoutAddress(QStringLiteral("hg1qexample"));
    QPushButton* start = page.findChild<QPushButton*>(QStringLiteral("startMiningButton"));
    QVERIFY(start);
    // Start is disabled while the miner is already running, so put the page in the
    // state a user actually presses it from.
    st.active = false;
    page.setStatus(st);
    QVERIFY(start->isEnabled());
    start->click();
    QCOMPARE(start_spy.count(), 1);
    QCOMPARE(start_spy.takeFirst().at(0).toString(), QStringLiteral("hg1qexample"));
    QVERIFY(!QApplication::activeModalWidget());

    // Arming with none asks first, and the default answer is no. ExpectModal dismisses
    // through the escape button, which is Cancel.
    page.setPeerCount(0);
    ExpectModalWithoutNestedEventLoop("QMessageBox", [&] { start->click(); });
    QCOMPARE(start_spy.count(), 0);

    // Not a refusal, though: doc/bootstrapping.md makes zero-peer mining the supported
    // way the first node on a new chain starts, so the user must be able to say yes.
    bool clicked_proceed = false;
    QTimer::singleShot(0, [&clicked_proceed] {
        // Scanning top-level widgets rather than asking for the active modal: an
        // offscreen test run never activates a window, so activeModalWidget() is null
        // there while the dialog is plainly on screen.
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* proceed = widget->findChild<QPushButton*>(QStringLiteral("isolatedMiningProceedButton"));
            if (proceed && widget->isVisible()) {
                clicked_proceed = true;
                proceed->click();
                return;
            }
        }
    });
    start->click();
    QTRY_VERIFY_WITH_TIMEOUT(clicked_proceed, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(start_spy.count(), 1, 2000);
    QCOMPARE(start_spy.takeFirst().at(0).toString(), QStringLiteral("hg1qexample"));
}

void VaultTests::mineMintPageShowsARawScriptPayout()
{
    MineMintPage page;
    QLabel* payout = page.findChild<QLabel*>(QStringLiteral("payoutTargetValue"));
    QVERIFY(payout);

    interfaces::MiningStatus st;
    auto rows = MineMintPage::statusTextForTesting(st);
    QCOMPARE(rows.payout, QStringLiteral("Not set"));
    page.setStatus(st);
    QCOMPARE(payout->text(), rows.payout);

    st.payout_script = "6a";
    rows = MineMintPage::statusTextForTesting(st);
    QCOMPARE(rows.payout, QStringLiteral("Raw script (provably unspendable)"));
    page.setStatus(st);
    QCOMPARE(payout->text(), rows.payout);

    st.payout_script = "51";
    rows = MineMintPage::statusTextForTesting(st);
    QCOMPARE(rows.payout, QStringLiteral("Raw script 51"));
    page.setStatus(st);
    QCOMPARE(payout->text(), rows.payout);
}

//! Idle with nothing permitted used to read Solver: CPU, because the permit
//! defaults true until arming. The halted sentence is about the configuration,
//! so it is the right health row before anyone presses Start. The other three
//! idle combinations keep the text they already had.
void VaultTests::mineMintPageNamesAnIdleNodeWithNothingPermitted()
{
    const auto rows = [](bool gpu, bool possible) {
        interfaces::MiningStatus st;
        st.active = false;
        st.gpu_solver = gpu;
        st.block_solving_possible = possible;
        return MineMintPage::statusTextForTesting(st);
    };
    const QString sentence = QStringLiteral("No GPU solver is configured, and processor block mining is off. Choose a solver, or allow processor block mining, in Settings > Options > Main.");

    const auto cpu = rows(false, true);
    QCOMPARE(cpu.block_mining, QStringLiteral("Idle"));
    QCOMPARE(cpu.solver, QStringLiteral("CPU"));
    QCOMPARE(cpu.attempts, QStringLiteral("Idle"));
    QCOMPARE(cpu.health, QStringLiteral("Not running"));
    QVERIFY(!cpu.show_configure_solver);

    const auto gpu = rows(true, true);
    QCOMPARE(gpu.block_mining, QStringLiteral("Idle"));
    QCOMPARE(gpu.solver, QStringLiteral("GPU bridge"));
    QCOMPARE(gpu.attempts, QStringLiteral("Idle"));
    QCOMPARE(gpu.health, QStringLiteral("Not running"));
    QVERIFY(!gpu.show_configure_solver);

    // A GPU solver is configured, so the permit bit is not what the row
    // is allowed to contradict. This input is not one BuildMiningStatus emits
    // while idle; the page must still not call the solver CPU.
    const auto gpu_denied = rows(true, false);
    QCOMPARE(gpu_denied.block_mining, QStringLiteral("Idle"));
    QCOMPARE(gpu_denied.solver, QStringLiteral("GPU bridge"));
    QCOMPARE(gpu_denied.attempts, QStringLiteral("Idle"));
    QCOMPARE(gpu_denied.health, QStringLiteral("Not running"));
    QVERIFY(!gpu_denied.show_configure_solver);

    const auto denied = rows(false, false);
    QCOMPARE(denied.block_mining, QStringLiteral("Idle"));
    QCOMPARE(denied.solver, QStringLiteral("None"));
    QCOMPARE(denied.attempts, QStringLiteral("Idle"));
    QCOMPARE(denied.health, sentence);
    QVERIFY(!denied.health.contains(QStringLiteral("-cuckatoosolver")));
    QVERIFY(!denied.health.contains(QStringLiteral("-allowcpumining")));
    QVERIFY(denied.show_configure_solver);

    MineMintPage page;
    interfaces::MiningStatus st;
    st.active = false;
    st.gpu_solver = false;
    st.block_solving_possible = false;
    page.setStatus(st);
    QCOMPARE(page.findChild<QLabel*>(QStringLiteral("miningStatusValue"))->text(), denied.block_mining);
    QCOMPARE(page.findChild<QLabel*>(QStringLiteral("solverStatusValue"))->text(), denied.solver);
    QCOMPARE(page.findChild<QLabel*>(QStringLiteral("attemptsRateValue"))->text(), denied.attempts);
    QCOMPARE(page.findChild<QLabel*>(QStringLiteral("solverHealthValue"))->text(), denied.health);
    QPushButton* configure = page.findChild<QPushButton*>(QStringLiteral("configureSolverButton"));
    QVERIFY(configure);
    QVERIFY(configure->isVisibleTo(&page));
    QSignalSpy solver_settings_spy(&page, &MineMintPage::solverSettingsRequested);
    QVERIFY(solver_settings_spy.isValid());
    configure->click();
    QCOMPARE(solver_settings_spy.count(), 1);
    // Still idle: Start is the control, Stop stays disabled.
    QVERIFY(page.findChild<QPushButton*>(QStringLiteral("startMiningButton"))->isEnabled());
    QVERIFY(!page.findChild<QPushButton*>(QStringLiteral("stopMiningButton"))->isEnabled());
}

//! F-108: "Peers: 0" is a reading, not an explanation, and the two states behind it
//! are not alike. A Windows first run has no route to the network at all -- no DNS
//! seeds, one onion fixed seed, no Tor -- and nothing said so or offered a way out.
void VaultTests::networkPageDiagnosesBootstrapAndAddsPeers()
{
    // The pure half first: the running binary is on sandbox, so the case that matters
    // for launch -- an onion-only seed set with no Tor -- is the one it cannot reach.
    using Obstacle = NetworkPage::BootstrapObstacle;
    QCOMPARE(NetworkPage::diagnoseBootstrap(/*has_fixed_seeds=*/false, /*onion_only_seeds=*/false, /*onion_proxy_configured=*/false), Obstacle::NoSeeds);
    QCOMPARE(NetworkPage::diagnoseBootstrap(false, true, true), Obstacle::NoSeeds);
    QCOMPARE(NetworkPage::diagnoseBootstrap(true, true, false), Obstacle::OnionSeedsNeedTor);
    // A configured proxy retires it, and so does one clearnet seed: the onion-only
    // answer is decoded from the seed bytes, so adding a dialable seed must silently
    // stop the page telling people to install Tor.
    QCOMPARE(NetworkPage::diagnoseBootstrap(true, true, true), Obstacle::None);
    QCOMPARE(NetworkPage::diagnoseBootstrap(true, false, false), Obstacle::None);

    NetworkPage page;
    QLabel* diagnosis = page.findChild<QLabel*>(QStringLiteral("bootstrapDiagnosis"));
    QVERIFY(diagnosis);
    QLineEdit* peer_edit = page.findChild<QLineEdit*>(QStringLiteral("addPeerEdit"));
    QPushButton* peer_button = page.findChild<QPushButton*>(QStringLiteral("addPeerButton"));
    QLabel* peer_result = page.findChild<QLabel*>(QStringLiteral("addPeerResult"));
    QVERIFY(peer_edit);
    QVERIFY(peer_button);
    QVERIFY(peer_result);

    // Nothing to add a peer to before the node is running, and no diagnosis to give.
    page.showInitializing();
    QVERIFY(!diagnosis->isVisibleTo(&page));
    QVERIFY(!peer_button->isEnabled());
    page.showStartFailed();
    QVERIFY(!diagnosis->isVisibleTo(&page));
    QVERIFY(!peer_button->isEnabled());

    page.updateStatus(/*peers=*/0, /*verification_progress=*/1.0, /*synced=*/true);
    QVERIFY(diagnosis->isVisibleTo(&page));
    // Sandbox ships no fixed seeds at all, which is a different obstacle from the one
    // main and publictest have, and it must not be described as a Tor problem.
    QVERIFY(diagnosis->text().contains(QStringLiteral("no seed addresses")));
    QVERIFY(!diagnosis->text().contains(QStringLiteral("Tor")));
    QVERIFY(peer_button->isEnabled());

    // A peer retires the diagnosis: zero is the only count that needs explaining.
    page.updateStatus(2, 1.0, true);
    QVERIFY(!diagnosis->isVisibleTo(&page));

    QSignalSpy add_spy(&page, &NetworkPage::addPeerRequested);
    QVERIFY(add_spy.isValid());
    // An address the connection manager would accept and then never resolve is worse
    // than a rejection: it looks exactly like a peer that is merely down.
    peer_edit->setText(QStringLiteral("   "));
    peer_button->click();
    QCOMPARE(add_spy.count(), 0);
    QVERIFY(peer_result->text().contains(QStringLiteral("no spaces")));

    peer_edit->setText(QStringLiteral("  198.51.100.7:9555  "));
    peer_button->click();
    QCOMPARE(add_spy.count(), 1);
    QCOMPARE(add_spy.takeFirst().at(0).toString(), QStringLiteral("198.51.100.7:9555"));
}

//! H10/W-25: the manual Tor recipe was added unconditionally -- only an #ifdef picked
//! platform text -- so it sat on screen telling the user to edit /etc/tor/torrc while
//! the desktop's own supervised Tor was bootstrapped, connected to the seed and synced.
//! Following it moves a working desktop off its working Tor.
//!
//! The fact is passed in rather than read from netbase: SetProxy() refuses an invalid
//! Proxy, so a proxy installed here could never be removed and would change the answer
//! for every case that ran after this one in the same binary.
void VaultTests::networkPageHidesTheManualTorRecipeWhenAProxyIsCarryingOnions()
{
    NetworkPage page;
    QFrame* panel = page.findChild<QFrame*>(QStringLiteral("torSetupPanel"));
    QLabel* warning = page.findChild<QLabel*>(QStringLiteral("torSetupWarning"));
    QLabel* steps = page.findChild<QLabel*>(QStringLiteral("torSetupSteps"));
    QLabel* working = page.findChild<QLabel*>(QStringLiteral("torSetupWorking"));
    QVERIFY(panel);
    QVERIFY(warning);
    QVERIFY(steps);
    QVERIFY(working);

    // No onion proxy: the recipe is the only route to the seed, so it must be present.
    page.applyTorSetupAdvice(/*onion_proxy_configured=*/false);
    QVERIFY(warning->isVisibleTo(&page));
    QVERIFY(steps->isVisibleTo(&page));
    QVERIFY(!working->isVisibleTo(&page));

    // A proxy is carrying onion traffic: the recipe is now actively harmful.
    page.applyTorSetupAdvice(/*onion_proxy_configured=*/true);
    QVERIFY(!warning->isVisibleTo(&page));
    QVERIFY(!steps->isVisibleTo(&page));
    QVERIFY(working->isVisibleTo(&page));
    // The panel itself stays, so Tor does not become another silently empty surface.
    QVERIFY(panel->isVisibleTo(&page));
    QVERIFY(working->text().contains(QStringLiteral("No manual setup is needed")));
    QCOMPARE(working->property("class").toString(), QStringLiteral("benchNote"));

    // And it is reversible: a Tor that dies must bring the recipe back.
    page.applyTorSetupAdvice(/*onion_proxy_configured=*/false);
    QVERIFY(steps->isVisibleTo(&page));
    QVERIFY(!working->isVisibleTo(&page));
}

//! F-442 send-back 2, item 9: "This build is not a released version" is a
//! caution, so Home and the Node window draw it in the warning tone, not the
//! error red.
void VaultTests::devBuildBannerUsesTheWarningTone()
{
    const RestoreApplicationStyle restore;
    QuicksilverStyle::Apply(*qApp);
    using QuicksilverStyle::Token;
    const QString warnings = QStringLiteral("This build is not a released version. It contains changes made since the last release.");
    QScopedPointer<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));

    DesktopLaunchPage home(platform_style.data(), 0, 0, nullptr);
    home.setAlerts(warnings);
    QLabel* home_alerts = home.findChild<QLabel*>(QStringLiteral("homeNodeAlerts"));
    QVERIFY(home_alerts);

    RPCConsole console(m_node, platform_style.data(), nullptr);
    QVERIFY(QMetaObject::invokeMethod(&console, "updateAlerts", Q_ARG(QString, warnings)));
    QLabel* console_alerts = console.findChild<QLabel*>(QStringLiteral("label_alerts"));
    QVERIFY(console_alerts);

    for (QLabel* label : {home_alerts, console_alerts}) {
        label->ensurePolished();
        const QColor color = label->palette().color(label->foregroundRole());
        QVERIFY2(color == QuicksilverStyle::Color(Token::Warning),
                 qPrintable(QStringLiteral("%1 draws %2, not the warning %3").arg(label->objectName(), color.name(), QuicksilverStyle::Color(Token::Warning).name())));
        QVERIFY(color != QuicksilverStyle::Color(Token::CinnabarSoft));
        QVERIFY(color != QuicksilverStyle::Color(Token::Cinnabar));
    }
}

//! F-442 send-back 2, item 7: every window the Window menu opens takes the
//! bench grammar: no eyebrow or page title, content in a titled panel,
//! label/value rows with key and value roles, section heads as captions,
//! and quiet commands. The Node window, all four tabs, and both used-address
//! lists.
void VaultTests::agentSpendRequestRefuseWaitsForARequest()
{
    AgentAllotmentPage page;
    // A hint that is waiting for input is plain text: drawn as a bordered box it
    // reads as a field to type into.
    QCOMPARE(page.findChild<QLabel*>(QStringLiteral("agentAllotmentPolicyReviewState"))->property("class").toString(), QStringLiteral("muted"));
    QCOMPARE(page.findChild<QLabel*>(QStringLiteral("agentAllotmentPaymentReceiptState"))->property("class").toString(), QStringLiteral("muted"));

    QPlainTextEdit* request = page.findChild<QPlainTextEdit*>(QStringLiteral("agentAllotmentCosignEdit"));
    QPushButton* refuse = page.findChild<QPushButton*>(QStringLiteral("agentAllotmentRefuseButton"));
    QVERIFY(request && refuse);
    QVERIFY(!refuse->isEnabled());
    request->setPlainText(QStringLiteral("psqt=cHNidP8B"));
    QVERIFY(refuse->isEnabled());
    refuse->click();
    QVERIFY(request->toPlainText().isEmpty());
    QVERIFY(!refuse->isEnabled());
}

void VaultTests::noVaultPageIsOneBenchPanel()
{
    const RestoreApplicationStyle restore;
    QuicksilverStyle::Apply(*qApp);
    QScopedPointer<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));

    VaultFrame frame(platform_style.data(), nullptr);
    frame.setVaultRuntimeAvailable(true);
    frame.gotoVaultPage();
    frame.resize(1000, 700);
    frame.show();
    QVERIFY(QTest::qWaitForWindowExposed(&frame));

    QWidget* page = frame.findChild<QWidget*>(QStringLiteral("noVaultPage"));
    QVERIFY(page);
    QFrame* panel = page->findChild<QFrame*>(QStringLiteral("noVaultState"));
    QVERIFY(panel);
    QCOMPARE(panel->property("benchPanel").toBool(), true);
    bool titled{false};
    for (QLabel* label : panel->findChildren<QLabel*>()) titled |= label->text() == QStringLiteral("VAULT ACCESS");
    QVERIFY(titled);
    // A page's panels span the page, as on every other page.
    QCOMPARE(panel->width(), page->width() - 28);

    QLabel* title = panel->findChild<QLabel*>(QStringLiteral("emptyVaultTitle"));
    QLabel* body = panel->findChild<QLabel*>(QStringLiteral("emptyVaultBody"));
    QVERIFY(title);
    QVERIFY(body);
    QCOMPARE(title->property("class").toString(), QStringLiteral("benchValue"));
    QCOMPARE(body->property("class").toString(), QStringLiteral("benchNote"));
    for (const char* gone : {"emptyVaultMark", "emptyVaultEyebrow"}) {
        QVERIFY2(!page->findChild<QWidget*>(QString::fromLatin1(gone)), gone);
    }

    // Nothing in the panel is drawn over anything else.
    QList<QWidget*> shown;
    for (QWidget* widget : panel->findChildren<QWidget*>()) {
        if (widget->isVisible() && (qobject_cast<QLabel*>(widget) || qobject_cast<QPushButton*>(widget))) shown << widget;
    }
    QVERIFY(shown.size() >= 4);
    for (int i = 0; i < shown.size(); ++i) {
        for (int j = i + 1; j < shown.size(); ++j) {
            const QRect a{shown[i]->mapTo(panel, QPoint()), shown[i]->size()};
            const QRect b{shown[j]->mapTo(panel, QPoint()), shown[j]->size()};
            QVERIFY2(!a.intersects(b), qPrintable(QStringLiteral("%1 overlaps %2").arg(shown[i]->objectName(), shown[j]->objectName())));
        }
    }
}

void VaultTests::peerTableNamesTheConnectionInOneColumn()
{
    PeerTableModel model(m_node, nullptr);
    QStringList headers;
    for (int column = 0; column < model.columnCount(); ++column) {
        headers << model.headerData(column, Qt::Horizontal, Qt::DisplayRole).toString();
    }
    // "Inbound" carries no further type, so a separate Type column was blank for
    // every inbound peer. One column names direction and type together.
    QCOMPARE(headers, (QStringList{QStringLiteral("Peer"), QStringLiteral("Age"), QStringLiteral("Address"), QStringLiteral("Connection"),
                                   QStringLiteral("Network"), QStringLiteral("Ping"), QStringLiteral("Sent"), QStringLiteral("Received"),
                                   QStringLiteral("User Agent")}));
}

//! The windows behind the menus carry the grammar of the pages: their content in
//! one titled panel, the dialog's own buttons outside it, quiet commands.
void VaultTests::menuDialogsUseTheBenchGrammar()
{
    const RestoreApplicationStyle restore;
    QuicksilverStyle::Apply(*qApp);
    QScopedPointer<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));

    auto check = [](QDialog& dialog, const char* panel_name, const QString& title, QWidget* inside, QWidget* outside) {
        QFrame* panel = dialog.findChild<QFrame*>(QString::fromLatin1(panel_name));
        QVERIFY2(panel, panel_name);
        QCOMPARE(panel->property("benchPanel").toBool(), true);
        bool titled{false};
        for (QLabel* label : panel->findChildren<QLabel*>()) titled |= label->text() == title.toUpper();
        QVERIFY2(titled, qPrintable(title));
        QVERIFY2(inside && panel->isAncestorOf(inside), panel_name);
        if (outside) QVERIFY2(!panel->isAncestorOf(outside), panel_name);
        for (QPushButton* button : dialog.findChildren<QPushButton*>()) {
            QVERIFY2(button->property("class").toString() == QLatin1String("benchQuiet"),
                     qPrintable(QStringLiteral("%1 %2 is class \"%3\"").arg(QString::fromLatin1(panel_name), button->text(), button->property("class").toString())));
            // A command is its words; only a button with no words carries an icon.
            if (!button->text().isEmpty()) {
                QVERIFY2(button->icon().isNull(), qPrintable(QStringLiteral("%1 %2 carries an icon").arg(QString::fromLatin1(panel_name), button->text())));
            }
        }
    };

    for (const auto mode : {AskPassphraseDialog::Encrypt, AskPassphraseDialog::Unlock, AskPassphraseDialog::ChangePass}) {
        AskPassphraseDialog dialog(mode, nullptr);
        check(dialog, "passphrasePanel", dialog.windowTitle(), dialog.findChild<QLineEdit*>(QStringLiteral("passEdit1")),
              dialog.findChild<QDialogButtonBox*>(QStringLiteral("buttonBox")));
        QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("warningLabel"))->property("class").toString(), QStringLiteral("benchNote"));
    }

    {
        CreateVaultDialog dialog(nullptr);
        check(dialog, "createVaultPanel", QStringLiteral("Create vault"), dialog.findChild<QLineEdit*>(QStringLiteral("vault_name_line_edit")),
              dialog.findChild<QDialogButtonBox*>(QStringLiteral("buttonBox")));
        QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("label_description"))->property("class").toString(), QStringLiteral("benchNote"));
    }

    {
        HelpMessageDialog dialog(nullptr, /*about=*/true);
        check(dialog, "helpPanel", QStringLiteral("About Quicksilver"), dialog.findChild<QTextBrowser*>(QStringLiteral("aboutMessage")),
              dialog.findChild<QDialogButtonBox*>(QStringLiteral("okButton")));
    }
    {
        HelpMessageDialog dialog(nullptr, /*about=*/false);
        check(dialog, "helpPanel", QStringLiteral("Command-line options"), dialog.findChild<QTextEdit*>(QStringLiteral("helpMessage")),
              dialog.findChild<QDialogButtonBox*>(QStringLiteral("okButton")));
        // The URI form is Quicksilver's own scheme, not a numbered upstream proposal.
        const QString help = dialog.findChild<QTextEdit*>(QStringLiteral("helpMessage"))->toPlainText();
        QVERIFY2(!help.contains(QRegularExpression(QStringLiteral("\\bBIP ?[0-9]"))), qPrintable(help.left(400)));
        QVERIFY(help.contains(QStringLiteral("quicksilver:")));
    }

    {
        SignVerifyMessageDialog dialog(platform_style.data(), nullptr);
        check(dialog, "signaturesPanel", QStringLiteral("Signatures"), dialog.findChild<QTabWidget*>(QStringLiteral("tabWidget")), nullptr);
        for (const char* note : {"infoLabel_SM", "infoLabel_VM"}) {
            QCOMPARE(dialog.findChild<QLabel*>(QString::fromLatin1(note))->property("class").toString(), QStringLiteral("benchNote"));
        }
    }
}

void VaultTests::panelsMenuWindowsUseTheBenchGrammar()
{
    const RestoreApplicationStyle restore;
    QuicksilverStyle::Apply(*qApp);
    QScopedPointer<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));

    RPCConsole console(m_node, platform_style.data(), nullptr);
    for (const char* old : {"nodeWindowEyebrow", "nodeWindowTitle", "nodeWindowSubtitle"}) {
        QVERIFY2(!console.findChild<QLabel*>(QString::fromLatin1(old)), old);
    }
    QFrame* panel = console.findChild<QFrame*>(QStringLiteral("nodeWindowPanel"));
    QVERIFY(panel);
    QCOMPARE(panel->property("benchPanel").toBool(), true);
    QTabWidget* tabs = console.findChild<QTabWidget*>(QStringLiteral("tabWidget"));
    QVERIFY(tabs && panel->isAncestorOf(tabs));
    QCOMPARE(tabs->count(), 4);
    // Information: section heads are captions, not bold body text; keys and
    // values carry their roles.
    for (const char* head : {"label_9", "labelNetwork"}) {
        QLabel* label = console.findChild<QLabel*>(QString::fromLatin1(head));
        QVERIFY2(label, head);
        QCOMPARE(label->property("class").toString(), QStringLiteral("benchSection"));
    }
    QCOMPARE(console.findChild<QLabel*>(QStringLiteral("label_6"))->property("class").toString(), QStringLiteral("benchKey"));
    QCOMPARE(console.findChild<QLabel*>(QStringLiteral("clientVersion"))->property("class").toString(), QStringLiteral("benchValue"));
    // Peers: the detail rows too.
    QCOMPARE(console.findChild<QLabel*>(QStringLiteral("peerConnectionTypeLabel"))->property("class").toString(), QStringLiteral("benchKey"));
    QCOMPARE(console.findChild<QLabel*>(QStringLiteral("peerConnectionType"))->property("class").toString(), QStringLiteral("benchValue"));
    // Commands on every tab are quiet bench buttons.
    for (QPushButton* button : console.findChildren<QPushButton*>()) {
        QVERIFY2(button->property("class").toString() == QLatin1String("benchQuiet"),
                 qPrintable(QStringLiteral("%1 is class \"%2\"").arg(button->objectName(), button->property("class").toString())));
    }

    for (AddressBookPage::Tabs tab : {AddressBookPage::SendingTab, AddressBookPage::ReceivingTab}) {
        AddressBookPage book(platform_style.data(), AddressBookPage::ForEditing, tab);
        QFrame* book_panel = book.findChild<QFrame*>(QStringLiteral("addressBookPanel"));
        QVERIFY(book_panel);
        QCOMPARE(book_panel->property("benchPanel").toBool(), true);
        QVERIFY(book_panel->isAncestorOf(book.findChild<QTableView*>(QStringLiteral("tableView"))));
        QLabel* explanation = book.findChild<QLabel*>(QStringLiteral("labelExplanation"));
        QVERIFY(explanation && book_panel->isAncestorOf(explanation));
        QCOMPARE(explanation->property("class").toString(), QStringLiteral("benchNote"));
        for (QPushButton* button : book.findChildren<QPushButton*>()) {
            QVERIFY2(button->property("class").toString() == QLatin1String("benchQuiet"), qPrintable(button->objectName()));
        }
    }
}

//! H1: with consensus off, RPCConsole::setClientModel is never called, so every value on
//! the Information tab keeps the "N/A" placeholder baked into debugwindow.ui -- including
//! Client version and Datadir, which do not depend on a node at all. The window reads
//! broken rather than idle, and nothing on it points at the opt-in that would fix it.
void VaultTests::nodeWindowSaysWhyEveryFieldReadsNotApplicable()
{
    QScopedPointer<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    QVERIFY(platform_style);
    RPCConsole console(m_node, platform_style.data(), nullptr);

    QLabel* banner = console.findChild<QLabel*>(QStringLiteral("consensusOffBanner"));
    QVERIFY(banner);
    // Visible by default, in the form: a banner that had to be switched on could be missed
    // by a path nobody considered, which is precisely the defect being fixed.
    QVERIFY(banner->isVisibleTo(&console));
    QVERIFY(banner->text().contains(QStringLiteral("Consensus is not running")));
    QVERIFY(banner->text().contains(QStringLiteral("Home")));

    // And the claim it makes about the fields is true at this moment.
    QLabel* client_version = console.findChild<QLabel*>(QStringLiteral("clientVersion"));
    QVERIFY(client_version);
    QCOMPARE(client_version->text(), QStringLiteral("N/A"));
}

//! F-108: the launch screen is the first thing a first run sees, and it said
//! "Enabled" over a node that had never reached a peer.
void VaultTests::launchPageSaysWhenConsensusHasNoPeers()
{
    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    DesktopLaunchPage page(platform_style.get(), /*blockchain_size_gb=*/128, /*chain_state_size_gb=*/26, nullptr);

    QLabel* state = page.findChild<QLabel*>(QStringLiteral("launchConsensusCardState"));
    QLabel* consensus_body = page.findChild<QLabel*>(QStringLiteral("launchConsensusCardBody"));
    QLabel* mining_body = page.findChild<QLabel*>(QStringLiteral("launchMiningCardBody"));
    QVERIFY(state);
    QVERIFY(consensus_body);
    QVERIFY(mining_body);

    page.setConsensusEnabled(true);
    QCOMPARE(state->text(), QStringLiteral("Enabled"));
    QVERIFY(consensus_body->text().contains(QStringLiteral("Independent verification is enabled")));

    page.setPeerCount(0);
    // The chip still answers only "is consensus on" -- a different question from
    // whether the node found anyone, and the one the opt-in tests pin.
    QCOMPARE(state->text(), QStringLiteral("Enabled"));
    QVERIFY(consensus_body->text().contains(QStringLiteral("connected to no peer")));
    QVERIFY(mining_body->text().contains(QStringLiteral("chain of this computer's own")));
    // Not locked: the first node on a new chain has no peers either.
    QVERIFY(page.findChild<QPushButton*>(QStringLiteral("launchMiningCardButton"))->isEnabled());

    page.setPeerCount(4);
    QVERIFY(consensus_body->text().contains(QStringLiteral("Independent verification is enabled")));
    QVERIFY(mining_body->text().contains(QStringLiteral("consensus has been accepted")));

    // -1 is "not known yet", and it must read like a connected node rather than an
    // isolated one -- the launch page is built before any client model exists.
    page.setPeerCount(-1);
    QVERIFY(consensus_body->text().contains(QStringLiteral("Independent verification is enabled")));

    // A failed start still wins over both: it is the more specific truth.
    page.setPeerCount(0);
    page.setConsensusStartupFailed(true);
    QCOMPARE(state->text(), QStringLiteral("Restart needed"));
    page.setPeerCount(0);
    QCOMPARE(state->text(), QStringLiteral("Restart needed"));
}

//! The mining model wraps the node handle a client model handed over and polls it
//! once a second. Shutdown clears the client model first and tears the node down
//! afterwards, so a poll that outlives the client model is a poll into a node that is
//! going away. It survived only because the status call happens to null-check one
//! member before touching anything else.
//!
//! Found by a test that merely spun the event loop after the application test had
//! shut down: the application's frame had been cleared but its poll was still running,
//! and it dereferenced a node context that had since been swapped for a destroyed one.
void VaultTests::miningPollStopsWithTheClientModel()
{
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    OptionsModel options_model(m_node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    ClientModel client_model(m_node, &options_model);

    VaultFrame frame(platformStyle.get(), nullptr);
    frame.setClientModel(&client_model);
    QVERIFY(frame.findChild<MiningModel*>());
    frame.setClientModel(nullptr);
    QVERIFY(!frame.findChild<MiningModel*>());
    // And it comes back: the desktop can enable consensus after declining it once.
    frame.setClientModel(&client_model);
    QVERIFY(frame.findChild<MiningModel*>());
    frame.setClientModel(nullptr);
}

void VaultTests::networkPageFormatsStatus()
{
    NetworkPage page;
    QSignalSpy restart_spy(&page, &NetworkPage::restartRequested);
    QVERIFY(restart_spy.isValid());
    QVERIFY(!page.findChild<QLabel*>(QStringLiteral("developerNetworkCurrentValue")));
    QLabel* developer_banner = page.findChild<QLabel*>("developerNetworkBanner");
    QVERIFY(developer_banner);
    QVERIFY(!developer_banner->isHidden());
    QVERIFY(developer_banner->text().contains(QString("not using the live ledger")));
    QCOMPARE(page.findChild<QLabel*>("developerNetworkSandboxCardState")->text(), QString("Current"));
    // Owner ruling, send-back 2 item 10: a network that is not current shows
    // its Restart command only; "Current" sits in the same state/action
    // column, at the same right edge as the Restart buttons.
    QVERIFY(!page.findChild<QLabel*>("developerNetworkMainCardState"));
    QPushButton* main_restart = page.findChild<QPushButton*>("developerNetworkMainCardButton");
    QVERIFY(main_restart);
    QCOMPARE(main_restart->toolTip(), QString("Restart Quicksilver with this network context."));
    QVERIFY(!page.findChild<QPushButton*>("developerNetworkSandboxCardButton"));
    {
        page.resize(980, 900);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        QLabel* current = page.findChild<QLabel*>("developerNetworkSandboxCardState");
        const int current_right = current->mapTo(&page, QPoint(current->width(), 0)).x();
        const int restart_right = main_restart->mapTo(&page, QPoint(main_restart->width(), 0)).x();
        QCOMPARE(current_right, restart_right);
        page.hide();
    }
    QVERIFY(page.findChild<QLabel*>("developerNetworkMainCardToken")->text().contains(QString("main")));
    QVERIFY(page.findChild<QLabel*>("developerNetworkPublicTestCardDatadir")->text().contains(QString("publictest")));
    QVERIFY(page.findChild<QLabel*>("developerNetworkSandboxCardDatadir")->text().contains(QString("sandbox")));
    QLabel* tor_steps = page.findChild<QLabel*>(QStringLiteral("torSetupSteps"));
    QVERIFY(tor_steps);
    QVERIFY(tor_steps->text().contains(QStringLiteral("ControlPort 9051")));
    QVERIFY(tor_steps->text().contains(QStringLiteral("-onion")));
    // F-110: this panel is the only in-app guidance for the transport that reaches
    // the fixed seed, so it has to describe the host it is running on. The negative
    // half is the assertion that matters -- the defect was Linux paths shown on
    // Windows, and that reads as perfectly good advice unless you check for absence.
    QLabel* tor_warning = page.findChild<QLabel*>(QStringLiteral("torSetupWarning"));
    QVERIFY(tor_warning);
#ifdef Q_OS_WIN
    QVERIFY(tor_steps->text().contains(QStringLiteral("tor.exe")));
    QVERIFY(!tor_steps->text().contains(QStringLiteral("/etc/tor/torrc")));
    QVERIFY(!tor_steps->text().contains(QStringLiteral("Tor service group")));
    QVERIFY(!tor_steps->text().contains(QStringLiteral("CookieAuthFileGroupReadable")));
    QVERIFY(!tor_warning->text().contains(QStringLiteral("Debian")));
#else
    QVERIFY(tor_steps->text().contains(QStringLiteral("/etc/tor/torrc")));
    QVERIFY(!tor_steps->text().contains(QStringLiteral("tor.exe")));
#endif
    // The menu is Controls, not Settings; the old text sent the reader to a menu
    // this window does not have.
    QVERIFY(tor_steps->text().contains(QStringLiteral("Settings > Options > Network")));
    main_restart = page.findChild<QPushButton*>("developerNetworkMainCardButton");
    QPushButton* publictest_restart = page.findChild<QPushButton*>("developerNetworkPublicTestCardButton");
    QPushButton* sandbox_restart = page.findChild<QPushButton*>("developerNetworkSandboxCardButton");
    QVERIFY(main_restart);
    QVERIFY(publictest_restart);
    QVERIFY(!sandbox_restart);
    QCOMPARE(main_restart->text(), QString("Restart"));
    QVERIFY(main_restart->isEnabled());
    QVERIFY(publictest_restart->isEnabled());
    publictest_restart->click();
    QCOMPARE(restart_spy.count(), 1);
    QCOMPARE(restart_spy.takeFirst().at(0).toString(), QString("publictest"));

    page.showInitializing();
    QCOMPARE(page.findChild<QLabel*>("networkStatusValue")->text(), QString("Starting consensus"));
    QCOMPARE(page.findChild<QLabel*>("peerCountValue")->text(), QString("Waiting"));
    QCOMPARE(page.findChild<QLabel*>("syncStatusValue")->text(), QString("Waiting for consensus"));

    page.showStartFailed();
    QCOMPARE(page.findChild<QLabel*>("networkIntro")->text(), QString("Consensus did not start. The saved opt-in was cleared; restart after fixing node settings to try again."));
    QCOMPARE(page.findChild<QLabel*>("networkStatusValue")->text(), QString("Startup failed"));
    QCOMPARE(page.findChild<QLabel*>("peerCountValue")->text(), QString("Unavailable"));
    QCOMPARE(page.findChild<QLabel*>("syncStatusValue")->text(), QString("Not running"));

    page.updateStatus(0, 0.5, false);
    QCOMPARE(page.findChild<QLabel*>("networkStatusValue")->text(), QString("Offline"));
    QCOMPARE(page.findChild<QLabel*>("peerCountValue")->text(), QString("0"));

    page.updateStatus(8, 1.0, true);
    QCOMPARE(page.findChild<QLabel*>("networkStatusValue")->text(), QString("Online"));
    QCOMPARE(page.findChild<QLabel*>("peerCountValue")->text(), QString("8"));
    QCOMPARE(page.findChild<QLabel*>("syncStatusValue")->text(), QString("Synced"));
}

void VaultTests::consensusCancelDoesNotShowStarting()
{
    QSettings().setValue(QStringLiteral("Desktop/ConsensusEnabled"), false);
    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    VaultFrame frame(platform_style.get(), nullptr);
    QObject::connect(&frame, &VaultFrame::consensusStateChanged, &frame, [&frame](bool enabled) {
        // Models the synchronous "Not now" path in QuicksilverGUI's handover
        // dialog: the opt-in is reverted before the click handler resumes.
        if (enabled) frame.revertConsensusOptIn();
    });

    frame.gotoNetworkPage();
    QStackedWidget* stack = frame.findChild<QStackedWidget*>(QStringLiteral("vaultFrameStack"));
    QVERIFY(stack);
    QPushButton* enable = stack->currentWidget()->findChild<QPushButton*>(QStringLiteral("consensusContinueButton"));
    QVERIFY(enable);
    enable->click();

    QVERIFY(!frame.consensusEnabled());
    QCOMPARE(stack->currentWidget()->objectName(), QStringLiteral("desktopLaunchPage"));
    QLabel* state = frame.findChild<QLabel*>(QStringLiteral("launchConsensusCardState"));
    QVERIFY(state);
    QCOMPARE(state->text(), QStringLiteral("Optional"));
    QSettings().setValue(QStringLiteral("Desktop/ConsensusEnabled"), false);
}

void VaultTests::vaultInterfaceSkipsUninitializedTipWithoutConsensus()
{
    TestChain100Setup test;
    VaultContext context;
    context.args = &test.m_args;

    std::shared_ptr<CVault> raw_vault = vault::TestLoadVault(context);
    std::unique_ptr<interfaces::Vault> vault = interfaces::MakeVault(context, raw_vault);
    QVERIFY(vault);

    CMutableTransaction tx;
    tx.vout.emplace_back(COIN, CScript() << OP_TRUE);
    raw_vault->AddToVault(MakeTransactionRef(tx), vault::TxStateInactive{});
    const uint256 txid = tx.GetHash();

    uint256 block_hash;
    interfaces::VaultBalances balances;
    QVERIFY(!vault->canCreateTransactionsNow());
    QVERIFY(!vault->tryGetBalanceUpdateBlockHash(block_hash));
    QVERIFY(!vault->tryGetBalances(balances, block_hash));
    interfaces::Vault::CoinsList coins_by_address;
    QVERIFY(!vault->tryListCoins(coins_by_address));
    std::vector<interfaces::VaultTxOut> selected_coins;
    QVERIFY(!vault->tryGetCoins({COutPoint{Txid::FromUint256(txid), 0}}, selected_coins));

    interfaces::VaultTxStatus tx_status;
    int num_blocks = 0;
    int64_t block_time = 0;
    QVERIFY(!vault->tryGetTxStatus(uint256::ONE, tx_status, num_blocks, block_time));

    interfaces::VaultTx tx_details;
    interfaces::VaultOrderForm order_form;
    bool in_relaypool = true;
    QVERIFY(!vault->tryGetVaultTxDetails(txid, tx_details, tx_status, order_form, in_relaypool, num_blocks));
    QVERIFY(!tx_details.tx);

    vault.reset();
    WaitForDeleteVault(std::move(raw_vault));
}

void VaultTests::vaultModelLoadsStoredConfirmationBeforeConsensus()
{
    BasicTestingSetup test{ChainType::SANDBOX};
    std::unique_ptr<interfaces::Chain> chain = interfaces::MakeChain(test.m_node);
    QVERIFY(!chain->hasChainstate());

    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vout.emplace_back(COIN, CScript() << OP_TRUE);
    vault::CVaultTx stored{
        MakeTransactionRef(coinbase),
        vault::TxStateConfirmed{Params().GenesisBlock().GetHash(), /*height=*/0, /*index=*/0}};
    stored.nOrderPos = 0;

    std::unique_ptr<vault::VaultDatabase> database = CreateMockableVaultDatabase();
    QVERIFY(vault::VaultBatch(*database).WriteTx(stored));
    std::shared_ptr<CVault> raw_vault = std::make_shared<CVault>(chain.get(), "", std::move(database));
    QCOMPARE(raw_vault->LoadVault(), vault::DBErrors::LOAD_OK);

    VaultContext context;
    context.args = &test.m_args;
    context.chain = chain.get();
    std::unique_ptr<interfaces::Vault> vault = interfaces::MakeVault(context, raw_vault);
    ScopedNodeContext scoped_context(m_node, test.m_node);
    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    OptionsModel options_model(m_node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));

    // Constructing the transaction table exercises the startup path that used
    // to ask a height-less deserialized confirmation for its depth and abort.
    VaultModel vault_model(std::move(vault), m_node, &options_model, platform_style.get());
    QVERIFY(vault_model.getTransactionTableModel());
}

void VaultTests::thinHeaderSourceRefreshesDetachedVault()
{
    BasicTestingSetup test{ChainType::SANDBOX};
    std::unique_ptr<interfaces::Chain> chain = interfaces::MakeChain(test.m_node);
    QVERIFY(!chain->hasChainstate());
    std::unique_ptr<interfaces::VaultLoader> loader = interfaces::MakeVaultLoader(*chain, test.m_args);
    VaultContext& context = *loader->context();

    bilingual_str create_error;
    std::vector<bilingual_str> warnings;
    std::shared_ptr<CVault> raw_vault = CVault::Create(
        context, "", CreateMockableVaultDatabase(), VAULT_FLAG_DESCRIPTORS, create_error, warnings);
    QVERIFY2(raw_vault, create_error.original.c_str());
    QVERIFY(AddVault(context, raw_vault));
    std::unique_ptr<interfaces::Vault> vault = interfaces::MakeVault(context, raw_vault);

    const fs::path missing_store{test.m_args.GetDataDirNet() / "agent" / "desktop-header-test.dat"};
    ThinVaultHeaderSource source{missing_store};
    std::string refresh_error;
    QVERIFY2(source.refresh(*loader, refresh_error), refresh_error.c_str());

    uint256 update_hash;
    interfaces::VaultBalances balances;
    QVERIFY(vault->tryGetBalanceUpdateBlockHash(update_hash));
    QCOMPARE(update_hash, Params().GenesisBlock().GetHash());
    QVERIFY(vault->tryGetBalances(balances, update_hash));
    QVERIFY(!vault->canCreateTransactionsNow());

    fs::create_directories(missing_store.parent_path());
    agent::HeaderChain stored_headers{
        Params().GetConsensus(), Params().GenesisBlock()};
    QCOMPARE(agent::SaveHeaderChain(stored_headers, missing_store), agent::HeaderStoreResult::OK);
    QVERIFY2(source.refresh(*loader, refresh_error), refresh_error.c_str());

    QVERIFY(RemoveVault(context, raw_vault, /*load_on_start=*/std::nullopt));
    vault.reset();
    WaitForDeleteVault(std::move(raw_vault));
}

void VaultTests::vaultModelSkipsCoinControlOutputsBeforeConsensusClientModel()
{
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    auto vault = std::make_unique<PollMarkerVault>();
    PollMarkerVault* vault_ptr = vault.get();

    OptionsModel options_model(m_node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    VaultModel vault_model(std::move(vault), m_node, &options_model, platformStyle.get());

    interfaces::Vault::CoinsList coins_by_address;
    QVERIFY(!vault_model.tryListCoins(coins_by_address));
    QCOMPARE(vault_ptr->try_list_coin_calls, 1);

    std::vector<interfaces::VaultTxOut> selected_coins;
    QVERIFY(!vault_model.tryGetCoins({COutPoint{Txid::FromUint256(uint256::ONE), 0}}, selected_coins));
    QCOMPARE(vault_ptr->try_get_coin_calls, 1);

    CKey key;
    key.MakeNewKey(true);
    vault::AgentAllotmentRecord record;
    record.funding_address = EncodeDestination(WitnessV0KeyHash(key.GetPubKey()));
    QCOMPARE(vault_model.agentAllotmentFundingAvailable(record), 0);
    QCOMPARE(vault_ptr->try_list_coin_calls, 2);
}

void VaultTests::vaultViewMasksValuesWithoutClientModel()
{
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    OptionsModel options_model(m_node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    VaultModel vault_model(std::make_unique<PollMarkerVault>(), m_node, &options_model, platformStyle.get());

    // Opening a vault with consensus off leaves the client model unset, and the vault
    // view is handed the privacy setting the moment the vault is added.
    VaultView vault_view(&vault_model, platformStyle.get(), nullptr);

    options_model.setOption(OptionsModel::OptionID::MaskValues, false);
    Q_EMIT vault_view.setPrivacy(true);
    QVERIFY(options_model.getOption(OptionsModel::OptionID::MaskValues).toBool());

    Q_EMIT vault_view.setPrivacy(false);
    QVERIFY(!options_model.getOption(OptionsModel::OptionID::MaskValues).toBool());
}

//! A transfer's proof-of-work runs on a worker thread inside the vault, so any path
//! that would unload the vault has to be able to see that it is busy. Nothing joins
//! that thread; closing during a grind destroys the CVault the worker is standing in.
void VaultTests::vaultModelReportsProofOfWorkInFlightWhilePreparing()
{
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    auto vault = std::make_unique<PollMarkerVault>();
    PollMarkerVault* vault_ptr = vault.get();
    vault_ptr->balance = 2 * COIN;
    vault_ptr->transaction_creation_available = true;

    OptionsModel options_model(m_node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    VaultModel vault_model(std::move(vault), m_node, &options_model, platformStyle.get());
    vault_ptr->vault_model_under_test = &vault_model;

    CKey key;
    key.MakeNewKey(true);
    QList<SendCoinsRecipient> recipients;
    recipients.append(SendCoinsRecipient(
        QString::fromStdString(EncodeDestination(WitnessV0KeyHash(key.GetPubKey()))),
        QString{},
        COIN,
        QString{}));
    VaultModelTransaction transaction(recipients);
    vault::CCoinControl coin_control;

    QVERIFY(!vault_model.proofOfWorkInFlight());
    vault_model.prepareTransaction(transaction, coin_control);

    // Async preparation must use the vault's locked, authoritative balance rather
    // than racing the GUI-owned cache. This model deliberately never polled it.
    QCOMPARE(vault_ptr->selected_balance_calls, 1);
    QCOMPARE(vault_ptr->create_calls, 1);
    // In flight for the duration of the call...
    QVERIFY(vault_ptr->in_flight_during_create);
    // ...and released when it returns, however it returns. This one returned an error.
    QVERIFY(!vault_model.proofOfWorkInFlight());
}

//! One failed transfer raised two dialogs: the model put the specific reason on
//! screen and then returned a status the send dialog announced again in the
//! abstract. Whichever landed on top might be the one carrying no information.
//! The reason now travels back with the status so the caller shows it once.
void VaultTests::vaultModelReturnsSendFailureReasonWithoutShowingIt()
{
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    auto vault = std::make_unique<PollMarkerVault>();
    PollMarkerVault* vault_ptr = vault.get();
    vault_ptr->balance = 2 * COIN;
    vault_ptr->transaction_creation_available = true;

    OptionsModel options_model(m_node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    VaultModel vault_model(std::move(vault), m_node, &options_model, platformStyle.get());
    vault_ptr->vault_model_under_test = &vault_model;

    int messages_shown{0};
    QObject::connect(&vault_model, &VaultModel::message, [&](const QString&, const QString&, unsigned int) {
        ++messages_shown;
    });

    CKey key;
    key.MakeNewKey(true);
    QList<SendCoinsRecipient> recipients;
    recipients.append(SendCoinsRecipient(
        QString::fromStdString(EncodeDestination(WitnessV0KeyHash(key.GetPubKey()))),
        QString{},
        COIN,
        QString{}));
    VaultModelTransaction transaction(recipients);
    vault::CCoinControl coin_control;
    const VaultModel::SendCoinsReturn status = vault_model.prepareTransaction(transaction, coin_control);

    QCOMPARE(vault_ptr->selected_balance_calls, 1);
    QCOMPARE(vault_ptr->create_calls, 1);
    QCOMPARE(status.status, VaultModel::TransactionCreationFailed);
    QCOMPARE(status.reason, QStringLiteral("unsupported"));
    QCOMPARE(messages_shown, 0);

    // The asynchronous dialog reports every accepted preparation result, not only
    // success. A failure used to emit no signal visible to the helper above, which
    // turned this immediate "unsupported" result into a misleading 30-second wait.
    SendCoinsDialog send_dialog(platformStyle.get());
    send_dialog.setModel(&vault_model);
    QVBoxLayout* entries = send_dialog.findChild<QVBoxLayout*>(QStringLiteral("entries"));
    QVERIFY(entries);
    SendCoinsEntry* entry = qobject_cast<SendCoinsEntry*>(entries->itemAt(0)->widget());
    QVERIFY(entry);
    entry->findChild<QValidatedLineEdit*>(QStringLiteral("payTo"))->setText(recipients.front().address);
    entry->findChild<QuicksilverAmountField*>(QStringLiteral("payAmount"))->setValue(COIN);

    QSignalSpy prepare_finished(&send_dialog, &SendCoinsDialog::sendPreparationFinishedForTesting);
    QSignalSpy dialog_messages(&send_dialog, &SendCoinsDialog::message);
    QVERIFY(QMetaObject::invokeMethod(&send_dialog, "sendButtonClicked", Q_ARG(bool, false)));
    QVERIFY(prepare_finished.wait(3000));
    QCOMPARE(prepare_finished.count(), 1);
    const QList<QVariant> prepare_result = prepare_finished.takeFirst();
    QCOMPARE(prepare_result.at(0).toInt(), static_cast<int>(VaultModel::TransactionCreationFailed));
    QCOMPARE(prepare_result.at(1).toString(), QStringLiteral("unsupported"));
    QCOMPARE(dialog_messages.count(), 1);
    QCOMPARE(vault_ptr->selected_balance_calls, 2);
    QCOMPARE(vault_ptr->create_calls, 2);
}

void VaultTests::vaultModelReturnsCommitFailureWithoutEmittingSendSuccess()
{
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    auto vault = std::make_unique<PollMarkerVault>();
    PollMarkerVault* vault_ptr = vault.get();
    vault_ptr->commit_error = Untranslated("Transaction is invalid by consensus rules: tx-pow-invalid");

    OptionsModel options_model(m_node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    VaultModel vault_model(std::move(vault), m_node, &options_model, platformStyle.get());

    CKey key;
    key.MakeNewKey(true);
    QList<SendCoinsRecipient> recipients;
    recipients.append(SendCoinsRecipient(
        QString::fromStdString(EncodeDestination(WitnessV0KeyHash(key.GetPubKey()))),
        QStringLiteral("rejected recipient"),
        COIN,
        QString{}));
    VaultModelTransaction transaction(recipients);
    CMutableTransaction mutable_tx;
    mutable_tx.vout.emplace_back(COIN, GetScriptForDestination(WitnessV0KeyHash(key.GetPubKey())));
    transaction.setWtx(MakeTransactionRef(std::move(mutable_tx)));

    QSignalSpy sent(&vault_model, &VaultModel::coinsSent);
    const VaultModel::SendCoinsReturn status{vault_model.sendCoins(transaction)};

    QCOMPARE(status.status, VaultModel::TransactionCommitFailed);
    QCOMPARE(status.reason, QStringLiteral("Transaction is invalid by consensus rules: tx-pow-invalid"));
    QCOMPARE(vault_ptr->commit_calls, 1);
    QCOMPARE(vault_ptr->set_address_calls, 0);
    QCOMPARE(sent.count(), 0);
}

//! Cancelling has to reach the grind, not merely disown its answer. The generation
//! token alone left the worker running for minutes, holding the vault and the GPU,
//! while the interface reported the work stopped.
void VaultTests::vaultModelPassesCancelRequestIntoTheGrind()
{
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    auto vault = std::make_unique<PollMarkerVault>();
    PollMarkerVault* vault_ptr = vault.get();
    vault_ptr->balance = 2 * COIN;
    vault_ptr->transaction_creation_available = true;

    OptionsModel options_model(m_node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    VaultModel vault_model(std::move(vault), m_node, &options_model, platformStyle.get());
    vault_ptr->vault_model_under_test = &vault_model;

    CKey key;
    key.MakeNewKey(true);
    QList<SendCoinsRecipient> recipients;
    recipients.append(SendCoinsRecipient(
        QString::fromStdString(EncodeDestination(WitnessV0KeyHash(key.GetPubKey()))),
        QString{},
        COIN,
        QString{}));
    vault::CCoinControl coin_control;
    vault_model.pollBalanceChanged();

    // A cancel asked for while nothing is running must not carry into the next
    // transfer: the user who pressed Cancel was talking about the transfer they could
    // see, and the one they start afterwards has to be allowed to finish.
    vault_model.requestProofOfWorkCancel();
    QVERIFY(vault_model.proofOfWorkCancelRequested());
    {
        VaultModelTransaction stale_request(recipients);
        vault_model.prepareTransaction(stale_request, coin_control);
    }
    QVERIFY(!vault_ptr->cancel_requested_during_create);

    // A cancel asked for while a grind is running does reach it.
    vault_ptr->cancel_requested_during_create = false;
    {
        VaultModel::ProofOfWorkScope scope(vault_model);
        QVERIFY(vault_model.proofOfWorkInFlight());
        vault_model.requestProofOfWorkCancel();
        VaultModelTransaction live_request(recipients);
        vault_model.prepareTransaction(live_request, coin_control);
    }
    QVERIFY(vault_ptr->cancel_requested_during_create);
    QVERIFY(!vault_model.proofOfWorkInFlight());
}

void VaultTests::vaultModelRejectsSendBeforeConsensusClientModel()
{
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    auto vault = std::make_unique<PollMarkerVault>();
    PollMarkerVault* vault_ptr = vault.get();
    vault_ptr->balance = 2 * COIN;

    OptionsModel options_model(m_node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    VaultModel vault_model(std::move(vault), m_node, &options_model, platformStyle.get());

    CKey key;
    key.MakeNewKey(true);
    QList<SendCoinsRecipient> recipients;
    recipients.append(SendCoinsRecipient(
        QString::fromStdString(EncodeDestination(WitnessV0KeyHash(key.GetPubKey()))),
        QString{},
        COIN,
        QString{}));
    VaultModelTransaction transaction(recipients);
    vault::CCoinControl coin_control;

    const VaultModel::SendCoinsReturn status = vault_model.prepareTransaction(transaction, coin_control);
    QCOMPARE(status.status, VaultModel::VaultSyncUnavailable);
    QVERIFY(!transaction.getWtx());
    QCOMPARE(vault_ptr->selected_balance_calls, 0);
    QCOMPARE(vault_ptr->create_calls, 0);
}

void VaultTests::vaultModelPollsBalanceFromVaultTipWithoutClientModel()
{
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    auto vault = std::make_unique<PollMarkerVault>();
    PollMarkerVault* vault_ptr = vault.get();
    OptionsModel options_model(m_node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    VaultModel vault_model(std::move(vault), m_node, &options_model, platformStyle.get());
    int balance_signals{0};
    QObject::connect(&vault_model, &VaultModel::balanceChanged, [&](const interfaces::VaultBalances&) {
        ++balance_signals;
    });

    vault_ptr->block_hash.SetNull();
    vault_ptr->balance = COIN;
    vault_model.pollBalanceChanged();
    QCOMPARE(vault_ptr->marker_calls, 1);
    QCOMPARE(vault_ptr->balance_calls, 1);
    QCOMPARE(vault_model.getCachedBalance().balance, COIN);
    QCOMPARE(balance_signals, 1);

    vault_model.pollBalanceChanged();
    QCOMPARE(vault_ptr->marker_calls, 2);
    QCOMPARE(vault_ptr->balance_calls, 1);

    vault_ptr->block_hash = Txid::FromUint256(uint256::ONE).ToUint256();
    vault_ptr->balance = 2 * COIN;
    vault_model.pollBalanceChanged();
    QCOMPARE(vault_ptr->marker_calls, 3);
    QCOMPARE(vault_ptr->balance_calls, 2);
    QCOMPARE(vault_model.getCachedBalance().balance, 2 * COIN);
    QCOMPARE(balance_signals, 2);
}

void VaultTests::vaultModelCachesEncryptionStatus()
{
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    auto vault = std::make_unique<PollMarkerVault>();
    PollMarkerVault* vault_ptr = vault.get();
    OptionsModel options_model(m_node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    VaultModel vault_model(std::move(vault), m_node, &options_model, platformStyle.get());
    QSignalSpy encryption_changed(&vault_model, &VaultModel::encryptionStatusChanged);

    vault_ptr->crypted = true;
    vault_ptr->locked = true;
    QVERIFY(QMetaObject::invokeMethod(&vault_model, "updateStatus"));
    QCOMPARE(vault_model.getEncryptionStatus(), VaultModel::Locked);
    QCOMPARE(encryption_changed.count(), 1);

    // Polling an unchanged state must not emit the transition again.
    QVERIFY(QMetaObject::invokeMethod(&vault_model, "updateStatus"));
    QCOMPARE(encryption_changed.count(), 1);

    vault_ptr->locked = false;
    QVERIFY(QMetaObject::invokeMethod(&vault_model, "updateStatus"));
    QCOMPARE(vault_model.getEncryptionStatus(), VaultModel::Unlocked);
    QCOMPARE(encryption_changed.count(), 2);
}

void VaultTests::sendGenerationGuardDropsStaleResult()
{
    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    SendCoinsDialog dlg(platformStyle.get());
    const quint64 g0 = dlg.currentSolveGeneration();
    QVERIFY(dlg.acceptSolveResult(g0));  // in-flight generation is accepted
    dlg.bumpSolveGeneration();           // simulates Cancel / a new send
    QVERIFY(!dlg.acceptSolveResult(g0)); // stale worker result is dropped
}

void VaultTests::sendWorkResourceTextClassifiesGpuSolver()
{
    const QString sandbox_text = SendCoinsDialog::sendWorkResourceTextForTesting(false, QString());
    QVERIFY(sandbox_text.contains(QStringLiteral("quick built-in preparation")));
    QVERIFY(sandbox_text.contains(QStringLiteral("do not need graphics acceleration")));

    const QString missing_arg_text = SendCoinsDialog::sendWorkResourceTextForTesting(true, QString());
    QVERIFY(missing_arg_text.contains(QStringLiteral("processor will take over")));
    QVERIFY(missing_arg_text.contains(QStringLiteral("Settings > Options > Main")));

    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    const QString absent_solver_path = temp_dir.filePath(QStringLiteral("missing-solver"));
    const QString absent_solver_text = SendCoinsDialog::sendWorkResourceTextForTesting(true, absent_solver_path);
    QVERIFY(absent_solver_text.contains(QStringLiteral("could not be found")));
    QVERIFY(absent_solver_text.contains(QStringLiteral("Settings > Options > Main")));

    const QString directory_solver_text = SendCoinsDialog::sendWorkResourceTextForTesting(true, temp_dir.path());
    QVERIFY(directory_solver_text.contains(QStringLiteral("cannot be started")));

    // "Executable" is spelled differently per platform, and QFileInfo::isExecutable()
    // -- which sendWorkResourceText() classifies on -- reports each platform's own
    // spelling: POSIX reads the owner-execute bit, Windows reads the file extension
    // (.exe/.com/.bat/.cmd). So a single fixture toggled by setPermissions() can only
    // ever be non-executable on Windows. Give each case a name that means what the
    // case needs on both platforms instead.
    const QString non_executable_path = temp_dir.filePath(QStringLiteral("qsgpusolve-not-executable"));
    QFile non_executable_file(non_executable_path);
    QVERIFY(non_executable_file.open(QIODevice::WriteOnly));
    non_executable_file.write("#!/bin/sh\nexit 0\n");
    non_executable_file.close();
    QVERIFY(non_executable_file.setPermissions(QFile::ReadOwner | QFile::WriteOwner));
    const QString non_executable_solver_text = SendCoinsDialog::sendWorkResourceTextForTesting(true, non_executable_path);
    QVERIFY(non_executable_solver_text.contains(QStringLiteral("cannot be started")));

#ifdef Q_OS_WIN
    const QString solver_path = temp_dir.filePath(QStringLiteral("qsgpusolve.exe"));
#else
    const QString solver_path = temp_dir.filePath(QStringLiteral("qsgpusolve"));
#endif
    QFile solver_file(solver_path);
    QVERIFY(solver_file.open(QIODevice::WriteOnly));
    solver_file.write("#!/bin/sh\nexit 0\n");
    solver_file.close();
    QVERIFY(solver_file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    QVERIFY(QFileInfo(solver_path).isExecutable());
    const QString executable_solver_text = SendCoinsDialog::sendWorkResourceTextForTesting(true, solver_path);
    QVERIFY(executable_solver_text.contains(QStringLiteral("GPU solver is configured")));
    QVERIFY(executable_solver_text.contains(QStringLiteral("will check it before preparation starts")));

    const QString checking_solver_text = SendCoinsDialog::sendWorkResourceTextForTesting(true, solver_path, SendCoinsDialog::GpuSolverProbeStatus::Checking);
    QVERIFY(checking_solver_text.contains(QStringLiteral("Checking whether graphics acceleration is ready")));

    const QString available_solver_text = SendCoinsDialog::sendWorkResourceTextForTesting(true, solver_path, SendCoinsDialog::GpuSolverProbeStatus::Available);
    QVERIFY(available_solver_text.contains(QStringLiteral("Graphics acceleration is ready")));
    // F-107: the success branch must state the wait too. It previously said only
    // that acceleration was ready, immediately before a search whose median is
    // tens of seconds and whose tail is unbounded -- so the user whose hardware
    // worked was the one told least about what was coming. Both halves are
    // pinned: a typical case alone would be wrong about half the time.
    QVERIFY(available_solver_text.contains(QStringLiteral("one to two minutes")));
    QVERIFY(available_solver_text.contains(QStringLiteral("past five minutes")));
    QVERIFY(available_solver_text.contains(QStringLiteral("stop the work at any time")));

    const QString failed_solver_text = SendCoinsDialog::sendWorkResourceTextForTesting(true, solver_path, SendCoinsDialog::GpuSolverProbeStatus::Failed);
    QVERIFY(failed_solver_text.contains(QStringLiteral("Graphics acceleration did not start")));

    const QString timed_out_solver_text = SendCoinsDialog::sendWorkResourceTextForTesting(true, solver_path, SendCoinsDialog::GpuSolverProbeStatus::TimedOut);
    QVERIFY(timed_out_solver_text.contains(QStringLiteral("acceleration check timed out")));

    // Structural half of F-107. Asserting the one string only pins the branch
    // that was wrong; this pins the property that was violated -- every terminal
    // disclosure a sender can act on names a duration. A new branch added without
    // one fails here rather than silently reinstating the inverted warning.
    const QList<QString> terminal_texts{
        missing_arg_text,
        absent_solver_text,
        directory_solver_text,
        non_executable_solver_text,
        available_solver_text,
        failed_solver_text,
        timed_out_solver_text,
    };
    for (const QString& text : terminal_texts) {
        QVERIFY2(text.contains(QStringLiteral("minute")),
                 qPrintable(QStringLiteral("disclosure states no duration: ") + text));
    }

    // F-453: every case where the processor takes over is a warning, set
    // apart from the notes; the others are not.
    using Status = SendCoinsDialog::GpuSolverProbeStatus;
    // No default argument on the lambda: MSVC cannot name the local alias
    // in one (C2653).
    const auto warns = [](bool requires_gpu, const QString& path, Status status) {
        return SendCoinsDialog::sendWorkNotesForTesting(requires_gpu, path, status).processor_fallback;
    };
    QVERIFY(!warns(false, QString(), Status::Unchecked));
    QVERIFY(warns(true, QString(), Status::Unchecked));
    QVERIFY(warns(true, absent_solver_path, Status::Unchecked));
    QVERIFY(warns(true, temp_dir.path(), Status::Unchecked));
    QVERIFY(warns(true, non_executable_path, Status::Unchecked));
    QVERIFY(!warns(true, solver_path, Status::Unchecked));
    QVERIFY(!warns(true, solver_path, Status::Checking));
    QVERIFY(!warns(true, solver_path, Status::Available));
    QVERIFY(warns(true, solver_path, Status::Failed));
    QVERIFY(warns(true, solver_path, Status::TimedOut));
    // The notes and the warning together say what the one text says.
    const SendCoinsDialog::SendWorkNotes missing = SendCoinsDialog::sendWorkNotesForTesting(true, QString(), Status::Unchecked);
    QCOMPARE(missing.intro + QLatin1Char('\n') + missing.acceleration, missing_arg_text);
    QVERIFY(missing.acceleration.startsWith(QStringLiteral("Graphics acceleration is not configured")));

    // On the page the warning is its own line in the warning tone, under the
    // notes; a note that is not a warning stays with them.
    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    SendCoinsDialog page(platform_style.get());
    QLabel* notes = page.findChild<QLabel*>(QStringLiteral("sendWorkDisclosureLabel"));
    QLabel* warning = page.findChild<QLabel*>(QStringLiteral("sendWorkAccelerationWarning"));
    QVERIFY(notes);
    QVERIFY(warning);
    QCOMPARE(warning->property("class").toString(), QStringLiteral("benchNote"));
    QCOMPARE(warning->property("benchTone").toString(), QStringLiteral("warn"));
    page.showSendWorkNotesForTesting(missing);
    QVERIFY(!warning->isHidden());
    QCOMPARE(warning->text(), missing.acceleration);
    QCOMPARE(notes->text(), missing.intro);
    QVERIFY(notes->parentWidget() == warning->parentWidget());
    const SendCoinsDialog::SendWorkNotes ready = SendCoinsDialog::sendWorkNotesForTesting(true, solver_path, Status::Available);
    page.showSendWorkNotesForTesting(ready);
    QVERIFY(warning->isHidden());
    QCOMPARE(notes->text(), ready.intro + QLatin1Char('\n') + ready.acceleration);
}

void VaultTests::cpuFallbackWarningPolicyMatchesNetworkAndPreference()
{
    using Status = SendCoinsDialog::GpuSolverProbeStatus;
    QVERIFY(!SendCoinsDialog::cpuFallbackWarningRequiredForTesting(false, Status::Failed, true));
    QVERIFY(!SendCoinsDialog::cpuFallbackWarningRequiredForTesting(true, Status::Available, true));
    QVERIFY(!SendCoinsDialog::cpuFallbackWarningRequiredForTesting(true, Status::Failed, false));
    QVERIFY(SendCoinsDialog::cpuFallbackWarningRequiredForTesting(true, Status::Unchecked, true));
    QVERIFY(SendCoinsDialog::cpuFallbackWarningRequiredForTesting(true, Status::TimedOut, true));
}

void VaultTests::transferSummaryTracksAmountsAgainstSpendable()
{
    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    OptionsModel options_model(m_node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));

    auto vault = std::make_unique<PollMarkerVault>();
    PollMarkerVault* vault_ptr = vault.get();
    vault_ptr->balance = 10 * COIN;
    VaultModel model(std::move(vault), m_node, &options_model, platform_style.get());

    SendCoinsDialog dialog(platform_style.get());
    dialog.setModel(&model);
    vault_ptr->block_hash = ArithToUint256(1);
    model.pollBalanceChanged();

    QFrame* panel = dialog.findChild<QFrame*>(QStringLiteral("transferSummaryPanel"));
    QVERIFY(panel);
    QLabel* recipients = panel->findChild<QLabel*>(QStringLiteral("transferSummaryRecipients"));
    QLabel* total = panel->findChild<QLabel*>(QStringLiteral("transferSummaryTotal"));
    QLabel* spendable = panel->findChild<QLabel*>(QStringLiteral("transferSummarySpendable"));
    QLabel* remaining = panel->findChild<QLabel*>(QStringLiteral("transferSummaryRemaining"));
    QLabel* over = panel->findChild<QLabel*>(QStringLiteral("transferSummaryFlag"));
    QVERIFY(recipients);
    QVERIFY(total);
    QVERIFY(spendable);
    QVERIFY(remaining);
    QVERIFY(over);

    const auto format_amount = [&](CAmount amount) {
        return QuicksilverUnits::formatInlineWithPrivacy(
            model.getOptionsModel()->getDisplayUnit(), amount, QuicksilverUnits::SeparatorStyle::ALWAYS, false);
    };
    const auto expect_summary = [&](int count, CAmount sent, bool above) {
        QCOMPARE(recipients->text(), QString::number(count));
        QCOMPARE(total->text(), format_amount(sent));
        QCOMPARE(spendable->text(), format_amount(model.getCachedBalance().balance));
        const CAmount left = sent > model.getCachedBalance().balance ? 0 : model.getCachedBalance().balance - sent;
        QCOMPARE(remaining->text(), format_amount(left));
        QCOMPARE(over->isHidden(), !above);
        if (above) {
            QCOMPARE(over->text(), QStringLiteral("Above spendable balance"));
        }
    };

    expect_summary(1, 0, false);
    if (QTest::currentTestFailed()) return;

    QList<QuicksilverAmountField*> amounts = dialog.findChildren<QuicksilverAmountField*>(QStringLiteral("payAmount"));
    QCOMPARE(amounts.size(), 1);
    amounts.at(0)->setValue(3 * COIN);
    expect_summary(1, 3 * COIN, false);
    if (QTest::currentTestFailed()) return;

    SendCoinsEntry* second = dialog.addEntry();
    QVERIFY(second);
    QuicksilverAmountField* second_amount = second->findChild<QuicksilverAmountField*>(QStringLiteral("payAmount"));
    QVERIFY(second_amount);
    second_amount->setValue(2 * COIN);
    expect_summary(2, 5 * COIN, false);
    if (QTest::currentTestFailed()) return;

    amounts.at(0)->setValue(9 * COIN);
    expect_summary(2, 11 * COIN, true);
    if (QTest::currentTestFailed()) return;

    QToolButton* remove = second->findChild<QToolButton*>(QStringLiteral("deleteButton"));
    QVERIFY(remove);
    QVERIFY(QMetaObject::invokeMethod(second, "deleteClicked"));
    expect_summary(1, 9 * COIN, false);
    if (QTest::currentTestFailed()) return;

    vault_ptr->balance = 4 * COIN;
    vault_ptr->block_hash = ArithToUint256(2);
    model.pollBalanceChanged();
    expect_summary(1, 9 * COIN, true);
    if (QTest::currentTestFailed()) return;

    const QString charge_word = QStringLiteral("fee");
    for (QLabel* label : panel->findChildren<QLabel*>()) {
        QVERIFY2(!label->text().contains(charge_word, Qt::CaseInsensitive),
                 qPrintable(label->objectName() + QStringLiteral(": ") + label->text()));
    }

    QPushButton* review = dialog.findChild<QPushButton*>(QStringLiteral("sendButton"));
    QVERIFY(review);
    QVERIFY(panel->isAncestorOf(review));
    QString review_text = review->text();
    review_text.remove(QLatin1Char('&'));
    QCOMPARE(review_text, QStringLiteral("Review"));
}

void VaultTests::captureBenchPageScreenshots()
{
    const QString dir = qEnvironmentVariable("QS_BENCH_SCREENSHOTS");
    if (dir.isEmpty()) return;
    QVERIFY(QDir().mkpath(dir));

    // Render as the application does: its style, palette and sheet, and its
    // bundled monospace face. A capture without them shows a different program.
    const RestoreApplicationStyle restore;
    QuicksilverStyle::Apply(*qApp);
    QVERIFY2(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/monospace")) != -1,
             "the capture needs a platform that loads application fonts: QT_QPA_PLATFORM=offscreen");

    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    QScopedPointer<const NetworkStyle> network_style(NetworkStyle::instantiate(Params().GetChainType()));
    QSettings().setValue(QStringLiteral("Desktop/ConsensusEnabled"), false);

    QString unstyled;
    auto save_page = [&](QuicksilverGUI& window, const char* action_name, const QString& file) {
        if (action_name) {
            QAction* action = window.findChild<QAction*>(QString::fromLatin1(action_name));
            if (!action) return false;
            action->trigger();
        }
        window.resize(1200, 800);
        window.show();
        if (!QTest::qWaitForWindowExposed(&window)) return false;
        QCoreApplication::processEvents();
        const QImage image = window.grab().toImage();
        // A capture is evidence only if it was painted by the app sheet. Probe
        // two surfaces only the sheet colours: the top bar and the rail.
        const QWidget* top = window.findChild<QWidget*>(QStringLiteral("benchTopBar"));
        const QWidget* rail = window.findChild<QWidget*>(QStringLiteral("primaryCommandRail"));
        if (!top || !rail) return false;
        const auto probe = [&](const QWidget* widget, QPoint at, QuicksilverStyle::Token token) {
            const QColor seen = image.pixelColor(widget->mapTo(&window, at) * image.devicePixelRatio());
            if (seen == QuicksilverStyle::Color(token)) return true;
            unstyled = QStringLiteral("%1: %2 at %3 is %4, not %5").arg(file, widget->objectName())
                           .arg(QStringLiteral("%1,%2").arg(at.x()).arg(at.y()), seen.name(), QuicksilverStyle::Color(token).name());
            return false;
        };
        if (!probe(top, QPoint(4, 4), QuicksilverStyle::Token::BenchTop)) return false;
        if (!probe(rail, QPoint(4, rail->height() - 60), QuicksilverStyle::Token::Rail)) return false;
        return image.save(QDir(dir).filePath(file));
    };

    QuicksilverGUI bare(m_node, platform_style.get(), network_style.data());
    if (ModalOverlay* overlay = bare.findChild<ModalOverlay*>()) {
        const QDateTime stamp = QDateTime::currentDateTime();
        overlay->setKnownBestHeight(1, stamp, false);
        bare.setNumBlocks(1, stamp, 1.0, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    }
    QVERIFY2(save_page(bare, "homeBootstrapAction", QStringLiteral("home-none.png")), qPrintable(unstyled));
    QVERIFY2(save_page(bare, "sendCoinsAction", QStringLiteral("transfer-none.png")), qPrintable(unstyled));
    QVERIFY2(save_page(bare, "receiveCoinsAction", QStringLiteral("request.png")), qPrintable(unstyled));
    QVERIFY2(save_page(bare, "historyAction", QStringLiteral("ledger.png")), qPrintable(unstyled));
    QVERIFY2(save_page(bare, "agentAllotmentAction", QStringLiteral("agents.png")), qPrintable(unstyled));
    QVERIFY2(save_page(bare, "mineMintAction", QStringLiteral("mine.png")), qPrintable(unstyled));
    QVERIFY2(save_page(bare, "networkAction", QStringLiteral("network.png")), qPrintable(unstyled));
    QVERIFY2(save_page(bare, "homeBootstrapAction", QStringLiteral("home-none.png")), qPrintable(unstyled));
    QPushButton* cost = bare.findChild<QPushButton*>(QStringLiteral("launchConsensusCardButton"));
    QVERIFY(cost);
    cost->click();
    bare.resize(1200, 800);
    QVERIFY(QTest::qWaitForWindowExposed(&bare));
    QVERIFY(bare.grab().save(QDir(dir).filePath(QStringLiteral("consensus.png"))));

    // A vault with a ledger worth looking at: mature and maturing mining
    // rewards, and two transfers still waiting for a block.
    TestChain100Setup test{ChainType::SANDBOX, {.extra_args = {"-txpownocycle=1"}}};
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);
    std::shared_ptr<CVault> vault = SetupDescriptorsVault(m_node, test);
    MiniGUI mini_gui(m_node, platform_style.get());
    mini_gui.initModelForVault(m_node, vault, platform_style.get());
    QVERIFY(!SendCoins(*vault, mini_gui.sendCoinsDialog, PKHash(), 3 * COIN).IsNull());
    QVERIFY(!SendCoins(*vault, mini_gui.sendCoinsDialog, PKHash(), COIN / 4).IsNull());
    qApp->processEvents();
    // Consensus on, so Mine / Mint and Network show their own pages rather than
    // the consensus review.
    QSettings().setValue(QStringLiteral("Desktop/ConsensusEnabled"), true);
    QuicksilverGUI funded(m_node, platform_style.get(), network_style.data());
    VaultFrame* frame = funded.findChild<VaultFrame*>(QStringLiteral("vaultFrame"));
    QVERIFY(frame);
    auto* view = new VaultView(mini_gui.vaultModel.get(), platform_style.get(), frame);
    QVERIFY(frame->addView(view));
    frame->setCurrentVault(mini_gui.vaultModel.get());
    QVERIFY2(save_page(funded, "homeBootstrapAction", QStringLiteral("home-vault.png")), qPrintable(unstyled));
    QVERIFY2(save_page(funded, "sendCoinsAction", QStringLiteral("transfer-vault.png")), qPrintable(unstyled));
    for (QAction* action : funded.findChildren<QAction*>()) {
        if (action->objectName().isEmpty()) continue;
        action->setEnabled(true);
    }
    QVERIFY2(save_page(funded, "receiveCoinsAction", QStringLiteral("request-vault.png")), qPrintable(unstyled));
    QVERIFY2(save_page(funded, "historyAction", QStringLiteral("ledger-vault.png")), qPrintable(unstyled));
    QVERIFY2(save_page(funded, "agentAllotmentAction", QStringLiteral("agents-vault.png")), qPrintable(unstyled));
    QVERIFY2(save_page(funded, "mineMintAction", QStringLiteral("mine-vault.png")), qPrintable(unstyled));
    QVERIFY2(save_page(funded, "networkAction", QStringLiteral("network-vault.png")), qPrintable(unstyled));
    QSettings().setValue(QStringLiteral("Desktop/ConsensusEnabled"), false);
}

void VaultTests::transferPageShowsItsFormAndTransmitButtonTogether()
{
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    OptionsModel options_model(m_node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    VaultModel vault_model(std::make_unique<PollMarkerVault>(), m_node, &options_model, platformStyle.get());

    // Reproduce how the main window presents a vault: a vault frame, holding the
    // vault's pages, inside one scroll area that resizes its widget --
    // QuicksilverGUI's mainContentScrollArea.
    QScrollArea content;
    content.setFrameShape(QFrame::NoFrame);
    content.setWidgetResizable(true);
    auto* frame = new VaultFrame(platformStyle.get(), nullptr);
    auto* view = new VaultView(&vault_model, platformStyle.get(), frame);
    QVERIFY(frame->addView(view));
    frame->setCurrentVault(&vault_model);
    content.setWidget(frame);
    // A 1080p desktop's work area, less the menu bar and the status bar. This is
    // the size the Phase 6 first-run walk had, on a machine larger than most.
    content.resize(1920, 965);
    frame->gotoSendCoinsPage();
    content.show();
    QVERIFY(QTest::qWaitForWindowExposed(&content));
    // The minimal backend can expose zero-range scrollbars for one event-loop
    // turn. Wait for the resizable child and frameless viewport to settle.
    QTRY_COMPARE(content.viewport()->size(), content.size());

    auto* transfer = qobject_cast<SendCoinsDialog*>(view->currentWidget());
    QVERIFY(transfer);
    QWidget* destination = transfer->findChild<QValidatedLineEdit*>(QStringLiteral("payTo"));
    QWidget* transmit = transfer->findChild<QPushButton*>(QStringLiteral("sendButton"));
    QVERIFY(destination);
    QVERIFY(transmit);

    // Name the tallest page in the failure, because that is what a stacked widget
    // hands to every one of its siblings.
    QString pages{QStringLiteral("\n  the frame is %1 px tall and asks for at least %2; the page stack is %3")
                      .arg(frame->height())
                      .arg(frame->minimumSizeHint().height())
                      .arg(view->height())};
    for (int i = 0; i < view->count(); ++i) {
        pages += QStringLiteral("\n  %1 min height %2")
                     .arg(view->widget(i)->metaObject()->className())
                     .arg(view->widget(i)->minimumSizeHint().height());
    }

    const QRect viewport{content.viewport()->rect()};
    const auto in_viewport = [&content](QWidget* widget) {
        return QRect(widget->mapTo(content.viewport(), QPoint(0, 0)), widget->size());
    };
    const auto describe = [](const QRect& rect) {
        return QStringLiteral("y %1..%2, x %3..%4")
            .arg(rect.top()).arg(rect.bottom()).arg(rect.left()).arg(rect.right());
    };

    // Both halves of the decision must be on screen at once. Scrolling away from
    // the address to reach the button means committing an irreversible transfer
    // to something no longer in view.
    QVERIFY2(viewport.contains(in_viewport(destination)),
             qPrintable(QStringLiteral("Destination field (%1) is outside the viewport (%2).%3")
                            .arg(describe(in_viewport(destination)), describe(viewport), pages)));
    QVERIFY2(viewport.contains(in_viewport(transmit)),
             qPrintable(QStringLiteral("Transmit button (%1) is outside the viewport (%2).%3")
                            .arg(describe(in_viewport(transmit)), describe(viewport), pages)));
}

namespace {
//! Records Qt messages while a form is built, and puts the previous handler back
//! on every exit, including an assertion failure.
class FormOccupancyCapture
{
public:
    QStringList messages;
    QtMessageHandler previous{nullptr};

    FormOccupancyCapture()
    {
        current = this;
        previous = qInstallMessageHandler(&FormOccupancyCapture::handle);
    }

    ~FormOccupancyCapture()
    {
        current = nullptr;
        qInstallMessageHandler(previous);
    }

    FormOccupancyCapture(const FormOccupancyCapture&) = delete;
    FormOccupancyCapture& operator=(const FormOccupancyCapture&) = delete;

    static void handle(QtMsgType type, const QMessageLogContext& context, const QString& message)
    {
        if (current) {
            QString line = message;
            if (context.function && context.function[0] != '\0') {
                line = QLatin1String(context.function) + QLatin1String(": ") + message;
            }
            current->messages.append(line);
            if (current->previous) {
                current->previous(type, context, message);
            }
        }
    }

    static FormOccupancyCapture* current;
};

FormOccupancyCapture* FormOccupancyCapture::current = nullptr;

struct CoinControlFormSetup {
    TestChain100Setup chain;
    std::unique_ptr<interfaces::VaultLoader> loader;
    ScopedNodeContext context;
    std::unique_ptr<const PlatformStyle> style;
    OptionsModel options;
    std::unique_ptr<VaultModel> model;
    bool ok{false};

    explicit CoinControlFormSetup(interfaces::Node& node)
        : loader(interfaces::MakeVaultLoader(*chain.m_node.chain, *Assert(chain.m_node.args))),
          context(node, chain.m_node),
          style(PlatformStyle::instantiate("other")),
          options(node)
    {
        chain.m_node.vault_loader = loader.get();
        bilingual_str error;
        if (!options.Init(error)) {
            QTest::qFail(error.original.c_str(), __FILE__, __LINE__);
            return;
        }
        if (!options.setOption(OptionsModel::CoinControlFeatures, true)) {
            QTest::qFail("could not enable coin control features", __FILE__, __LINE__);
            return;
        }
        model = std::make_unique<VaultModel>(std::make_unique<PollMarkerVault>(), node, &options, style.get());
        ok = true;
    }
};

QString describeFormCell(const char* name, QFormLayout* layout, QWidget* widget)
{
    if (!widget) return QStringLiteral("%1: missing widget").arg(QLatin1String(name));
    if (!layout) return QStringLiteral("%1: missing layout").arg(QLatin1String(name));
    int row = -1;
    QFormLayout::ItemRole role = QFormLayout::SpanningRole;
    layout->getWidgetPosition(widget, &row, &role);
    if (row < 0) return QStringLiteral("%1: row -1").arg(QLatin1String(name));
    const char* role_name = "other";
    if (role == QFormLayout::LabelRole)
        role_name = "LabelRole";
    else if (role == QFormLayout::FieldRole)
        role_name = "FieldRole";
    return QStringLiteral("%1: row %2 %3").arg(QLatin1String(name)).arg(row).arg(QLatin1String(role_name));
}

bool cellIs(QFormLayout* layout, QWidget* widget, int expected_row, QFormLayout::ItemRole expected_role)
{
    if (!layout || !widget) return false;
    int row = -1;
    QFormLayout::ItemRole role = QFormLayout::SpanningRole;
    layout->getWidgetPosition(widget, &row, &role);
    return row == expected_row && role == expected_role;
}

//! Quantity on row 0 and Bytes on row 1, labels in LabelRole and fields in
//! FieldRole. A widget the layout refused is row -1.
bool quantityAndBytesUseSeparateRows(QWidget* root, const QStringList& messages)
{
    auto* layout = root->findChild<QFormLayout*>(QStringLiteral("formLayoutCoinControl1"));
    auto* quantity_text = root->findChild<QLabel*>(QStringLiteral("labelCoinControlQuantityText"));
    auto* quantity = root->findChild<QLabel*>(QStringLiteral("labelCoinControlQuantity"));
    auto* bytes_text = root->findChild<QLabel*>(QStringLiteral("labelCoinControlBytesText"));
    auto* bytes = root->findChild<QLabel*>(QStringLiteral("labelCoinControlBytes"));

    const QString detail = QStringLiteral("formLayoutCoinControl1 rowCount=%1\n%2\n%3\n%4\n%5\ncaptured:\n%6")
                               .arg(layout ? layout->rowCount() : -1)
                               .arg(describeFormCell("Quantity label", layout, quantity_text))
                               .arg(describeFormCell("Quantity field", layout, quantity))
                               .arg(describeFormCell("Bytes label", layout, bytes_text))
                               .arg(describeFormCell("Bytes field", layout, bytes))
                               .arg(messages.join(QLatin1Char('\n')));
    const QByteArray detail_bytes = detail.toUtf8();

    const bool positions_ok = cellIs(layout, quantity_text, 0, QFormLayout::LabelRole) &&
                              cellIs(layout, quantity, 0, QFormLayout::FieldRole) &&
                              cellIs(layout, bytes_text, 1, QFormLayout::LabelRole) &&
                              cellIs(layout, bytes, 1, QFormLayout::FieldRole);
    const bool positions_pass = QTest::qVerify(positions_ok,
                                               "Quantity row 0 and Bytes row 1 in formLayoutCoinControl1",
                                               detail_bytes.constData(), __FILE__, __LINE__);

    QStringList occupied;
    for (const QString& line : messages) {
        if (line.contains(QStringLiteral("already occupied"))) occupied.append(line);
    }
    const QByteArray occupied_bytes = occupied.join(QLatin1Char('\n')).toUtf8();
    const bool messages_pass = QTest::qVerify(occupied.isEmpty(),
                                              "no QFormLayout cell already occupied",
                                              occupied_bytes.constData(), __FILE__, __LINE__);
    return positions_pass && messages_pass;
}

} // namespace

//! Quantity and Bytes were both declared at form row 0. QFormLayout keeps the
//! first pair and refuses the second, so Bytes is not in the layout at all.
void VaultTests::transferCoinControlQuantityAndBytesUseSeparateRows()
{
    CoinControlFormSetup setup(m_node);
    if (!setup.ok) return;

    // setupUi runs in the constructor, so the handler has to be installed first.
    std::unique_ptr<SendCoinsDialog> dialog;
    QStringList messages;
    {
        FormOccupancyCapture capture;
        dialog = std::make_unique<SendCoinsDialog>(setup.style.get());
        dialog->setModel(setup.model.get());
        messages = capture.messages;
    }

    QWidget* frame = dialog->findChild<QWidget*>(QStringLiteral("frameCoinControl"));
    QVERIFY(frame);
    QVERIFY(!frame->isHidden());
    if (!quantityAndBytesUseSeparateRows(dialog.get(), messages)) return;
}

void VaultTests::coinControlDialogQuantityAndBytesUseSeparateRows()
{
    CoinControlFormSetup setup(m_node);
    if (!setup.ok) return;

    vault::CCoinControl coin_control;
    std::unique_ptr<CoinControlDialog> dialog;
    QStringList messages;
    {
        FormOccupancyCapture capture;
        dialog = std::make_unique<CoinControlDialog>(coin_control, setup.model.get(), setup.style.get());
        messages = capture.messages;
    }

    if (!quantityAndBytesUseSeparateRows(dialog.get(), messages)) return;
}

//! Transfer: a recipients panel of two-line rows with the inputs line under
//! them, beside a summary of label/value rows.
//!
//! The first port kept the old stacked form, three labelled rows and three icon
//! buttons per recipient, under an input-control box that took half the page.
//! Send-back 1 made each recipient one line, as in the concept; send-back 2
//! (owner ruling, 2026-10-09) gives the address its own line, because a whole
//! Bech32 address and the amount and label do not fit one line at 1200 px.
void VaultTests::transferRecipientsAreCompactRows()
{
    CoinControlFormSetup setup(m_node);
    if (!setup.ok) return;
    PollMarkerVault* vault = nullptr;
    {
        auto owned = std::make_unique<PollMarkerVault>();
        vault = owned.get();
        vault->balance = 10 * COIN;
        setup.model = std::make_unique<VaultModel>(std::move(owned), m_node, &setup.options, setup.style.get());
    }
    SendCoinsDialog dialog(setup.style.get());
    dialog.setModel(setup.model.get());
    vault->block_hash = ArithToUint256(1);
    setup.model->pollBalanceChanged();
    dialog.resize(1000, 700);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));

    QFrame* recipients = dialog.findChild<QFrame*>(QStringLiteral("transferRecipientsPanel"));
    QVERIFY(recipients);
    QCOMPARE(recipients->property("benchPanel").toBool(), true);
    QCOMPARE(recipients->findChild<QLabel*>(QStringLiteral("benchPanelTitle"))->text(), QStringLiteral("TRANSFER · RECIPIENTS"));
    QWidget* header = recipients->findChild<QWidget*>(QStringLiteral("transferRecipientsHeader"));
    QVERIFY(header);
    QStringList heads;
    for (QLabel* label : header->findChildren<QLabel*>()) {
        if (!label->text().isEmpty()) heads << label->text();
    }
    const QString unit = QuicksilverUnits::shortName(setup.model->getOptionsModel()->getDisplayUnit());
    QCOMPARE(heads, (QStringList{QStringLiteral("#"), QStringLiteral("ADDRESS")}));

    // Two lines per recipient: the address and its commands, then the amount
    // and label, each named by a key in the bench label grammar.
    const auto entries = [&] {
        QList<SendCoinsEntry*> list;
        for (SendCoinsEntry* entry : dialog.findChildren<SendCoinsEntry*>()) {
            if (!entry->isHidden()) list << entry;
        }
        std::sort(list.begin(), list.end(), [&](SendCoinsEntry* a, SendCoinsEntry* b) {
            return a->mapTo(&dialog, QPoint(0, 0)).y() < b->mapTo(&dialog, QPoint(0, 0)).y();
        });
        return list;
    };
    QCOMPARE(entries().size(), 1);
    SendCoinsEntry* first = entries().at(0);
    QVERIFY(recipients->isAncestorOf(first));
    for (const char* caption : {"payToLabel", "labellLabel", "amountLabel"}) {
        QVERIFY2(!first->findChild<QLabel*>(QString::fromLatin1(caption)), caption);
    }
    const auto line_ok = [&](std::initializer_list<const char*> names) -> int {
        int previous_right = -1;
        int middle = -1;
        for (const char* name : names) {
            QWidget* field = first->findChild<QWidget*>(QString::fromLatin1(name));
            if (!field) return -1;
            const QPoint at = field->mapTo(first, QPoint(0, field->height() / 2));
            if (middle < 0) middle = at.y();
            if (qAbs(at.y() - middle) > 4 || at.x() <= previous_right) {
                qWarning("%s at %d,%d; line middle %d, previous right %d", name, at.x(), at.y(), middle, previous_right);
                return -1;
            }
            previous_right = at.x() + field->width() - 1;
        }
        return middle;
    };
    const int address_line = line_ok({"sendCoinsEntryIndex", "payTo", "addressBookButton", "pasteButton", "deleteButton"});
    const int amount_line = line_ok({"recipientAmountKey", "payAmount", "useAvailableBalanceButton", "recipientLabelKey", "addAsLabel"});
    QVERIFY(address_line >= 0);
    QVERIFY(amount_line > address_line);
    QLabel* amount_key = first->findChild<QLabel*>(QStringLiteral("recipientAmountKey"));
    QCOMPARE(amount_key->text(), QStringLiteral("Amount ") + unit);
    QCOMPARE(amount_key->property("class").toString(), QStringLiteral("benchKey"));
    QCOMPARE(first->findChild<QLabel*>(QStringLiteral("recipientLabelKey"))->property("class").toString(), QStringLiteral("benchKey"));
    // The second line starts under the address.
    QCOMPARE(amount_key->mapTo(first, QPoint(0, 0)).x(), first->findChild<QWidget*>(QStringLiteral("payTo"))->mapTo(first, QPoint(0, 0)).x());
    QVERIFY2(first->height() <= 80, qPrintable(QStringLiteral("a recipient is %1 px tall").arg(first->height())));

    // Rows are numbered, and renumbered when one goes.
    const auto index_of = [](SendCoinsEntry* entry) {
        QLabel* index = entry->findChild<QLabel*>(QStringLiteral("sendCoinsEntryIndex"));
        return index ? index->text() : QString();
    };
    QCOMPARE(index_of(first), QStringLiteral("1"));
    SendCoinsEntry* second = dialog.addEntry();
    QCoreApplication::processEvents();
    QCOMPARE(index_of(second), QStringLiteral("2"));
    QVERIFY(QMetaObject::invokeMethod(first, "deleteClicked"));
    QCoreApplication::processEvents();
    QCOMPARE(entries().size(), 1);
    QCOMPARE(index_of(entries().at(0)), QStringLiteral("1"));

    // Add and Clear sit under the rows; inputs are one line in the same panel,
    // the coin control command at its right.
    QVERIFY(recipients->isAncestorOf(dialog.findChild<QPushButton*>(QStringLiteral("addButton"))));
    QVERIFY(recipients->isAncestorOf(dialog.findChild<QPushButton*>(QStringLiteral("clearButton"))));
    QWidget* inputs = dialog.findChild<QWidget*>(QStringLiteral("frameCoinControl"));
    QVERIFY(inputs);
    QVERIFY(recipients->isAncestorOf(inputs));
    QVERIFY(!inputs->isHidden());
    QLabel* automatic = inputs->findChild<QLabel*>(QStringLiteral("labelCoinControlAutomaticallySelected"));
    QPushButton* coin_control = inputs->findChild<QPushButton*>(QStringLiteral("pushButtonCoinControl"));
    QVERIFY(automatic);
    QVERIFY(coin_control);
    QCOMPARE(automatic->text(), QStringLiteral("Automatic selection"));
    QVERIFY(coin_control->mapTo(inputs, QPoint(0, 0)).x() > automatic->mapTo(inputs, QPoint(0, 0)).x());
    QVERIFY2(inputs->height() <= 80, qPrintable(QStringLiteral("inputs take %1 px").arg(inputs->height())));
    // A custom change address is a choice on the inputs line; its field opens
    // under it once chosen.
    QCheckBox* custom_change = inputs->findChild<QCheckBox*>(QStringLiteral("checkBoxCoinControlChange"));
    QWidget* change_address = inputs->findChild<QWidget*>(QStringLiteral("lineEditCoinControlChange"));
    QVERIFY(custom_change);
    QVERIFY(change_address);
    const int change_middle = custom_change->mapTo(inputs, QPoint(0, custom_change->height() / 2)).y();
    const int command_top = coin_control->mapTo(inputs, QPoint(0, 0)).y();
    QVERIFY(change_middle >= command_top && change_middle < command_top + coin_control->height());
    QVERIFY(change_address->isHidden());
    custom_change->setChecked(true);
    QVERIFY(!change_address->isHidden());
    custom_change->setChecked(false);
    QVERIFY(change_address->isHidden());

    // The summary is label/value rows; the full-amount and preparation notes
    // and the review command live in it.
    QFrame* summary = dialog.findChild<QFrame*>(QStringLiteral("transferSummaryPanel"));
    QVERIFY(summary);
    QCOMPARE(summary->findChild<QLabel*>(QStringLiteral("benchPanelTitle"))->text(), QStringLiteral("SUMMARY"));
    QCOMPARE(summary->findChild<QLabel*>(QStringLiteral("transferSummaryFrom"))->text(), setup.model->getDisplayName());
    QCOMPARE(summary->findChild<QLabel*>(QStringLiteral("transferSummaryRecipients"))->text(), QStringLiteral("1"));
    QVERIFY(summary->isAncestorOf(dialog.findChild<QLabel*>(QStringLiteral("sendWorkDisclosureLabel"))));
    QVERIFY(summary->isAncestorOf(dialog.findChild<QPushButton*>(QStringLiteral("sendButton"))));
    QVERIFY(summary->width() <= 340);
}

//! F-442 owner walk, items 5 and 12: at the desktop's content width the
//! address field shows its whole placeholder and outweighs the label, Sending
//! is set like the other summary values, and Review comes straight after the
//! figures at the height of the page's other commands.
void VaultTests::transferKeepsTheAddressReadableAndReviewInReach()
{
    const RestoreApplicationStyle restore;
    QuicksilverStyle::Apply(*qApp);
    TestChain100Setup test{ChainType::SANDBOX, {.extra_args = {"-txpownocycle=1"}}};
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);
    std::shared_ptr<CVault> vault = SetupDescriptorsVault(m_node, test);

    QSettings().setValue(QStringLiteral("Desktop/ConsensusEnabled"), false);
    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    QScopedPointer<const NetworkStyle> network_style(NetworkStyle::instantiate(Params().GetChainType()));
    MiniGUI mini_gui(m_node, platform_style.get());
    mini_gui.initModelForVault(m_node, vault, platform_style.get());

    QuicksilverGUI window(m_node, platform_style.get(), network_style.data());
    VaultFrame* frame = window.findChild<VaultFrame*>(QStringLiteral("vaultFrame"));
    QVERIFY(frame);
    auto* view = new VaultView(mini_gui.vaultModel.get(), platform_style.get(), frame);
    QVERIFY(frame->addView(view));
    frame->setCurrentVault(mini_gui.vaultModel.get());
    window.resize(1200, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QAction* transfer = window.findChild<QAction*>(QStringLiteral("sendCoinsAction"));
    QVERIFY(transfer);
    transfer->trigger();
    QCoreApplication::processEvents();
    SendCoinsDialog* page = view->findChild<SendCoinsDialog*>();
    QVERIFY(page);
    QTRY_VERIFY(page->isVisible());
    QWidget& dialog = *page;

    QValidatedLineEdit* address = dialog.findChild<QValidatedLineEdit*>(QStringLiteral("payTo"));
    QLineEdit* label = dialog.findChild<QLineEdit*>(QStringLiteral("addAsLabel"));
    QVERIFY(address);
    QVERIFY(label);
    const int placeholder = address->fontMetrics().horizontalAdvance(address->placeholderText());
    QVERIFY2(address->width() >= placeholder + 20,
             qPrintable(QStringLiteral("address %1 px for a %2 px placeholder").arg(address->width()).arg(placeholder)));
    QVERIFY2(address->width() > label->width(), qPrintable(QStringLiteral("address %1 px, label %2 px").arg(address->width()).arg(label->width())));

    // Send-back 2 item 1: a whole Bech32 address reads without scrolling, at
    // 1200 px with the summary panel beside it: the text fits the room
    // QLineEdit paints it in (the contents rect less text margins and the
    // edit's 2 px side margins and cursor).
    const QString bech32 = QString::fromStdString(EncodeDestination(WitnessV0KeyHash(test.coinbaseKey.GetPubKey())));
    QVERIFY2(bech32.startsWith(QString::fromStdString(Params().Bech32HRP()) + QStringLiteral("1q")), qPrintable(bech32));
    address->setText(bech32);
    QCoreApplication::processEvents();
    const int text_width = address->fontMetrics().horizontalAdvance(bech32);
    QStyleOptionFrame frame_option;
    frame_option.initFrom(address);
    frame_option.rect = address->rect();
    frame_option.lineWidth = address->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, &frame_option, address);
    const QRect contents = address->style()->subElementRect(QStyle::SE_LineEditContents, &frame_option, address);
    const QMargins text_margins = address->textMargins();
    const int room = contents.width() - text_margins.left() - text_margins.right() - 2 * 2 - 1;
    QVERIFY2(text_width <= room,
             qPrintable(QStringLiteral("address field %1 px has %2 px of room for a %3 px address").arg(address->width()).arg(room).arg(text_width)));
    QFrame* summary_panel = dialog.findChild<QFrame*>(QStringLiteral("transferSummaryPanel"));
    QVERIFY(summary_panel && summary_panel->isVisible());
    QVERIFY(summary_panel->mapTo(&window, QPoint(0, 0)).x() > address->mapTo(&window, QPoint(address->width(), 0)).x());
    // F-452: Amount is as wide as an amount with an eight-digit whole part,
    // not the 1 000 000 000 cap; a longer one scrolls inside the box. Label
    // takes the rest of its line, out to the row's remove button.
    QWidget* amount_field = dialog.findChild<QWidget*>(QStringLiteral("payAmount"));
    QVERIFY(amount_field);
    QVERIFY2(amount_field->width() <= amount_field->sizeHint().width(), qPrintable(QStringLiteral("amount %1 px, content %2 px").arg(amount_field->width()).arg(amount_field->sizeHint().width())));
    {
        QLineEdit* amount_edit = amount_field->findChild<QLineEdit*>();
        QVERIFY(amount_edit);
        const QFontMetrics amount_metrics = amount_edit->fontMetrics();
        const int eight_digits = amount_metrics.horizontalAdvance(QuicksilverUnits::format(QuicksilverUnit::HG, 99'999'999 * COIN + 99'999'999, false, QuicksilverUnits::SeparatorStyle::ALWAYS));
        const int cap = amount_metrics.horizontalAdvance(QuicksilverUnits::format(QuicksilverUnit::HG, QuicksilverUnits::maxMoney(), false, QuicksilverUnits::SeparatorStyle::ALWAYS));
        // The room QLineEdit paints text in: its width less 2 px a side and
        // the cursor, as for the address and label above.
        const int amount_room = amount_edit->width() - 2 * 2 - 1;
        // Whether eight digits fit is a question about real glyphs: in
        // "minimal"'s placeholder font they are wider than the box's cap.
        if (!QFontDatabase().families().isEmpty()) {
            QVERIFY2(amount_room >= eight_digits, qPrintable(QStringLiteral("amount has %1 px of room for %2 px").arg(amount_room).arg(eight_digits)));
        }
        QVERIFY2(amount_room < cap, qPrintable(QStringLiteral("amount has %1 px of room, enough for the %2 px cap").arg(amount_room).arg(cap)));
    }
    QWidget* remove = dialog.findChild<QWidget*>(QStringLiteral("deleteButton"));
    QVERIFY(remove);
    QCOMPARE(label->mapTo(&window, QPoint(label->width(), 0)).x(), remove->mapTo(&window, QPoint(remove->width(), 0)).x());
    // Label's content is its text: a label filled in from the address book
    // shows whole too.
    const QString known_label = QStringLiteral("founding-payout-b58");
    label->setText(known_label);
    QCoreApplication::processEvents();
    QStyleOptionFrame label_option;
    label_option.initFrom(label);
    label_option.rect = label->rect();
    label_option.lineWidth = label->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, &label_option, label);
    const int label_room = label->style()->subElementRect(QStyle::SE_LineEditContents, &label_option, label).width() - 2 * 2 - 1;
    QVERIFY2(label->fontMetrics().horizontalAdvance(known_label) <= label_room,
             qPrintable(QStringLiteral("label field has %1 px of room for a %2 px label").arg(label_room).arg(label->fontMetrics().horizontalAdvance(known_label))));
    label->clear();
    address->clear();

    auto* maximum = dialog.findChild<QPushButton*>(QStringLiteral("useAvailableBalanceButton"));
    QVERIFY(maximum);
    // A fit at 1200 px: in "minimal"'s placeholder glyphs the whole address
    // alone over-fills the row and the layout takes the shortfall from Max, so
    // this runs where fonts are real.
    if (!QFontDatabase().families().isEmpty()) {
        QVERIFY2(maximum->width() >= maximum->sizeHint().width(),
                 qPrintable(QStringLiteral("Max is %1 px wide but needs %2 px with shared padding")
                     .arg(maximum->width()).arg(maximum->sizeHint().width())));
    }

    QLabel* sending = dialog.findChild<QLabel*>(QStringLiteral("transferSummaryTotal"));
    QLabel* recipients = dialog.findChild<QLabel*>(QStringLiteral("transferSummaryRecipients"));
    QVERIFY(sending);
    QVERIFY(recipients);
    QCOMPARE(QFontInfo(sending->font()).pixelSize(), QFontInfo(recipients->font()).pixelSize());

    QPushButton* review = dialog.findChild<QPushButton*>(QStringLiteral("sendButton"));
    QLabel* notes = dialog.findChild<QLabel*>(QStringLiteral("sendWorkDisclosureLabel"));
    QFrame* summary = dialog.findChild<QFrame*>(QStringLiteral("transferSummaryPanel"));
    QVERIFY(review);
    QVERIFY(notes);
    QVERIFY(summary);
    QVERIFY2(review->mapTo(summary, QPoint(0, 0)).y() < notes->mapTo(summary, QPoint(0, 0)).y(), "Review sits under the solver notes");
    QCOMPARE(review->width(), review->sizeHint().width());
    QPushButton* add = dialog.findChild<QPushButton*>(QStringLiteral("addButton"));
    QPushButton* clear = dialog.findChild<QPushButton*>(QStringLiteral("clearButton"));
    QVERIFY(add);
    QVERIFY(clear);
    QCOMPARE(review->height(), add->height());
    QCOMPARE(clear->height(), add->height());
}

//! Request, Ledger, Agents, Mine / Mint and Network follow the Bench page
//! grammar: no page heading (the breadcrumb names the page), content in
//! titled panels rather than group boxes and red captions, state as
//! label/value rows, and at most one primary command.
void VaultTests::remainingPagesUseTheBenchGrammar()
{
    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    ReceiveCoinsDialog request(platform_style.get());
    TransactionView ledger(platform_style.get());
    AgentAllotmentPage agents;
    MineMintPage mine;
    NetworkPage network;
    const QList<QWidget*> pages{&request, &ledger, &agents, &mine, &network};
    for (QWidget* page : pages) {
        const QString name = QString::fromLatin1(page->metaObject()->className());
        for (QLabel* label : page->findChildren<QLabel*>()) {
            const QString role = label->property("class").toString();
            QVERIFY2(role != QStringLiteral("pageTitle"), qPrintable(name + QStringLiteral(" heading ") + label->text()));
            QVERIFY2(role != QStringLiteral("hudHeading"), qPrintable(name + QStringLiteral(" caption ") + label->text()));
        }
        const QList<QGroupBox*> groups = page->findChildren<QGroupBox*>();
        QVERIFY2(groups.isEmpty(), qPrintable(name + QStringLiteral(" group ") + (groups.isEmpty() ? QString() : groups.first()->title())));
        int panels = 0;
        for (QFrame* frame : page->findChildren<QFrame*>()) {
            if (frame->property("benchPanel").toBool()) ++panels;
        }
        QVERIFY2(panels > 0, qPrintable(name));
        QStringList primaries;
        for (QPushButton* button : page->findChildren<QPushButton*>()) {
            if (button->property("class").toString() == QStringLiteral("primaryActionButton")) primaries << button->objectName();
        }
        QVERIFY2(primaries.size() <= 1, qPrintable(name + QStringLiteral(" primaries: ") + primaries.join(QStringLiteral(", "))));
    }

    // Mine / Mint's mining state is label/value rows.
    for (const char* value : {"miningStatusValue", "solverStatusValue", "attemptsRateValue"}) {
        QLabel* label = mine.findChild<QLabel*>(QString::fromLatin1(value));
        QVERIFY2(label, value);
        QCOMPARE(label->property("class").toString(), QStringLiteral("benchValue"));
    }

    // Network: one panel, a row per network.
    QFrame* networks = network.findChild<QFrame*>(QStringLiteral("developerNetworkContext"));
    QVERIFY(networks);
    QCOMPARE(networks->property("benchPanel").toBool(), true);
    for (const char* row : {"developerNetworkMainCard", "developerNetworkPublicTestCard", "developerNetworkSandboxCard"}) {
        QWidget* card = network.findChild<QWidget*>(QString::fromLatin1(row));
        QVERIFY2(card, row);
        QVERIFY2(networks->isAncestorOf(card), row);
        QVERIFY2(!card->property("benchPanel").toBool(), row);
    }
}

void VaultTests::transferSolverTextNamesRealMenuAndSetting()
{
    TestChain100Setup test;
    ScopedNodeContext scoped_context(m_node, test.m_node);
    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    std::unique_ptr<const NetworkStyle> network_style(NetworkStyle::instantiate(Params().GetChainType()));
    QuicksilverGUI window(m_node, platform_style.get(), network_style.get());
    QStringList menus;
    for (QAction* action : window.menuBar()->actions()) {
        menus << QString(action->text()).remove(QLatin1Char('&'));
    }
    QVERIFY(menus.contains(QStringLiteral("Settings")));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
#ifdef Q_OS_WIN
    const QString solver_path = dir.filePath(QStringLiteral("solver.exe"));
#else
    const QString solver_path = dir.filePath(QStringLiteral("solver"));
#endif
    QFile solver(solver_path);
    QVERIFY(solver.open(QIODevice::WriteOnly));
    solver.close();
    QVERIFY(solver.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    QVERIFY(QFileInfo(solver_path).isExecutable());

    QStringList texts{
        SendCoinsDialog::sendWorkResourceTextForTesting(false, QString()),
        SendCoinsDialog::sendWorkResourceTextForTesting(true, QString()),
        SendCoinsDialog::sendWorkResourceTextForTesting(true, dir.filePath(QStringLiteral("absent"))),
        SendCoinsDialog::sendWorkResourceTextForTesting(true, dir.path()),
    };
    using Status = SendCoinsDialog::GpuSolverProbeStatus;
    for (const auto status : {Status::Unchecked, Status::Checking, Status::Available, Status::Failed, Status::TimedOut}) {
        texts << SendCoinsDialog::sendWorkResourceTextForTesting(true, solver_path, status);
    }
    texts << SendCoinsDialog::startupAccelerationTextForTesting(true)
          << SendCoinsDialog::startupAccelerationTextForTesting(false);
    const QRegularExpression path(QStringLiteral("([A-Za-z]+) (?:>|→) [A-Za-z]+ (?:>|→) [A-Za-z]+"));
    int paths = 0;
    for (const QString& text : texts) {
        auto matches = path.globalMatch(text);
        while (matches.hasNext()) {
            const auto match = matches.next();
            QVERIFY2(menus.contains(match.captured(1)), qPrintable(text));
            QCOMPARE(match.captured(0), QStringLiteral("Settings > Options > Main"));
            ++paths;
        }
        QVERIFY2(!text.contains(QStringLiteral("transfer helper")), qPrintable(text));
        // The menu was renamed from Controls to Settings (owner ruling 2026-10-10).
        QVERIFY2(!text.contains(QStringLiteral("Controls")), qPrintable(text));
    }
    QCOMPARE(paths, 5);
}

namespace {
class TransferPrecheckFixture
{
public:
    TestChain100Setup chain;
    std::unique_ptr<interfaces::VaultLoader> loader;
    ScopedNodeContext context;
    std::unique_ptr<const PlatformStyle> style;
    OptionsModel options;
    PollMarkerVault* vault;
    std::unique_ptr<VaultModel> model;
    std::unique_ptr<SendCoinsDialog> dialog;
    QString address;

    explicit TransferPrecheckFixture(interfaces::Node& node)
        : loader(interfaces::MakeVaultLoader(*chain.m_node.chain, *Assert(chain.m_node.args))),
          context(node, chain.m_node), style(PlatformStyle::instantiate("other")), options(node)
    {
        chain.m_node.vault_loader = loader.get();
        bilingual_str error;
        if (!options.Init(error)) throw std::runtime_error("transfer test options initialization failed");
        auto mock = std::make_unique<PollMarkerVault>();
        vault = mock.get();
        vault->crypted = true;
        vault->locked = true;
        vault->coin_listing_available = true;
        vault->transaction_creation_available = true;
        model = std::make_unique<VaultModel>(std::move(mock), node, &options, style.get());
        dialog = std::make_unique<SendCoinsDialog>(style.get());
        dialog->setModel(model.get());
        dialog->getCoinControl()->m_avoid_address_reuse = true;
        address = QString::fromStdString(EncodeDestination(PKHash(chain.coinbaseKey.GetPubKey().GetID())));
    }

    void recipient(CAmount amount)
    {
        SendCoinsRecipient recipient;
        recipient.address = address;
        recipient.amount = amount;
        dialog->pasteEntry(recipient);
    }

    void transmit()
    {
        QMetaObject::invokeMethod(dialog.get(), "sendButtonClicked", Qt::DirectConnection, Q_ARG(bool, false));
    }
};
} // namespace

void VaultTests::transferPrecheckRefusesOverBalanceBeforeUnlock()
{
    TransferPrecheckFixture fixture(m_node);
    fixture.recipient(COIN);
    QSignalSpy unlock(fixture.model.get(), &VaultModel::requireUnlock);
    QSignalSpy cpu_check(fixture.dialog.get(), &SendCoinsDialog::cpuFallbackCheckReachedForTesting);
    QSignalSpy messages(fixture.dialog.get(), &SendCoinsDialog::message);
    fixture.transmit();
    QCOMPARE(unlock.size(), 0);
    QCOMPARE(cpu_check.size(), 0);
    QVERIFY(!fixture.dialog->findChild<QMessageBox*>(QStringLiteral("cpuFallbackSendWarning")));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.at(0).at(1).toString(), QStringLiteral("The amount exceeds your balance."));
    QVERIFY(fixture.dialog->findChild<QWidget*>(QStringLiteral("sendWorkProgressPanel"))->isHidden());
    // An early refusal must leave another recipient/send attempt possible.
    fixture.recipient(COIN);
    QCOMPARE(fixture.dialog->findChildren<SendCoinsEntry*>().size(), 2);
}

void VaultTests::transferPrecheckRefusesDuplicateBeforeUnlock()
{
    TransferPrecheckFixture fixture(m_node);
    fixture.recipient(COIN);
    fixture.recipient(COIN);
    QSignalSpy unlock(fixture.model.get(), &VaultModel::requireUnlock);
    QSignalSpy cpu_check(fixture.dialog.get(), &SendCoinsDialog::cpuFallbackCheckReachedForTesting);
    QSignalSpy messages(fixture.dialog.get(), &SendCoinsDialog::message);
    fixture.transmit();
    QCOMPARE(unlock.size(), 0);
    QCOMPARE(cpu_check.size(), 0);
    QVERIFY(!fixture.dialog->findChild<QMessageBox*>(QStringLiteral("cpuFallbackSendWarning")));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.at(0).at(1).toString(), QStringLiteral("Duplicate address found: addresses should only be used once each."));
}

void VaultTests::transferPrecheckRefusesZeroBeforeUnlock()
{
    TransferPrecheckFixture fixture(m_node);
    fixture.recipient(0);
    QSignalSpy unlock(fixture.model.get(), &VaultModel::requireUnlock);
    QSignalSpy cpu_check(fixture.dialog.get(), &SendCoinsDialog::cpuFallbackCheckReachedForTesting);
    QSignalSpy messages(fixture.dialog.get(), &SendCoinsDialog::message);
    fixture.transmit();
    QCOMPARE(unlock.size(), 0);
    QCOMPARE(cpu_check.size(), 0);
    QVERIFY(!fixture.dialog->findChild<QMessageBox*>(QStringLiteral("cpuFallbackSendWarning")));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.at(0).at(1).toString(), QStringLiteral("The transfer amount must be larger than 0."));
}

void VaultTests::transferPrecheckAllowsStaleLowBalance()
{
    TransferPrecheckFixture fixture(m_node);
    fixture.recipient(COIN);
    fixture.vault->balance = 2 * COIN;
    QCOMPARE(fixture.model->getCachedBalance().balance, CAmount{0});
    QSignalSpy unlock(fixture.model.get(), &VaultModel::requireUnlock);
    QSignalSpy cpu_check(fixture.dialog.get(), &SendCoinsDialog::cpuFallbackCheckReachedForTesting);
    QSignalSpy finished(fixture.dialog.get(), &SendCoinsDialog::sendPreparationFinishedForTesting);
    // Complete the real unlock continuation and let the worker run its own
    // balance check. The mock fails creation only after that check has passed.
    QObject::connect(fixture.model.get(), &VaultModel::requireUnlock, fixture.dialog.get(), [&] {
        fixture.vault->locked = false;
    });
    fixture.transmit();
    QCOMPARE(unlock.size(), 1);
    QCOMPARE(cpu_check.size(), 1);
    QTRY_COMPARE(finished.size(), 1);
    QCOMPARE(finished.at(0).at(0).toInt(), static_cast<int>(VaultModel::TransactionCreationFailed));
    QCOMPARE(fixture.vault->create_calls, 1);
    QCOMPARE(fixture.vault->auto_selection_bound_calls, 1);
}

void VaultTests::transferPrecheckDefersUncertainBalance_data()
{
    QTest::addColumn<int>("uncertainty");
    QTest::newRow("uncertain-reuse-policy") << 0;
    QTest::newRow("selected-inputs") << 1;
    QTest::newRow("unsafe-inputs") << 2;
    QTest::newRow("busy-vault") << 3;
    QTest::newRow("uninitialized-vault") << 4;
}

void VaultTests::transferPrecheckDefersUncertainBalance()
{
    QFETCH(int, uncertainty);
    TransferPrecheckFixture fixture(m_node);
    fixture.recipient(COIN);
    if (uncertainty == 0) fixture.dialog->getCoinControl()->m_avoid_address_reuse = false;
    if (uncertainty == 1) fixture.dialog->getCoinControl()->Select(COutPoint{Txid::FromUint256(uint256::ONE), 0});
    if (uncertainty == 2) fixture.dialog->getCoinControl()->m_include_unsafe_inputs = true;
    if (uncertainty == 3) fixture.vault->auto_selection_bound_busy = true;
    if (uncertainty == 4) fixture.vault->auto_selection_bound_uninitialized = true;
    QSignalSpy unlock(fixture.model.get(), &VaultModel::requireUnlock);
    QSignalSpy messages(fixture.dialog.get(), &SendCoinsDialog::message);
    fixture.transmit();
    QCOMPARE(unlock.size(), 1);
    QCOMPARE(messages.size(), 0);
}

void VaultTests::transferPreparationFailureWithoutGraphsHidesProgress()
{
    TransferPrecheckFixture fixture(m_node);
    fixture.recipient(COIN);
    fixture.vault->locked = false;
    // Selected inputs defer the GUI check; the worker's unchanged balance
    // check refuses. This reproduces the old "Stopped" panel path itself.
    fixture.dialog->getCoinControl()->Select(COutPoint{Txid::FromUint256(uint256::ONE), 0});
    QSignalSpy finished(fixture.dialog.get(), &SendCoinsDialog::sendPreparationFinishedForTesting);
    QSignalSpy messages(fixture.dialog.get(), &SendCoinsDialog::message);
    fixture.transmit();
    QTRY_COMPARE(finished.size(), 1);
    QCOMPARE(finished.at(0).at(0).toInt(), static_cast<int>(VaultModel::AmountExceedsBalance));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.at(0).at(1).toString(), QStringLiteral("The amount exceeds your balance."));
    QVERIFY(fixture.dialog->findChild<QWidget*>(QStringLiteral("sendWorkProgressPanel"))->isHidden());
}

void VaultTests::requestPageNamesFormatsAndShowsGeneratedFormat()
{
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    MiniGUI mini_gui(m_node, platformStyle.get());
    mini_gui.initModelForVault(m_node, SetupDescriptorsVault(m_node, test), platformStyle.get());

    ReceiveCoinsDialog receive(platformStyle.get());
    receive.setModel(mini_gui.vaultModel.get());

    auto* types = receive.findChild<QComboBox*>(QStringLiteral("addressType"));
    auto* format_label = receive.findChild<QLabel*>(QStringLiteral("addressFormatLabel"));
    auto* advice = receive.findChild<QLabel*>(QStringLiteral("addressFormatAdvice"));
    QVERIFY(types && format_label && advice);
    QCOMPARE(format_label->text(), QStringLiteral("Address &format"));
    QCOMPARE(format_label->buddy(), types);
    QCOMPARE(types->count(), 3);
    QCOMPARE(types->itemText(types->findData(static_cast<int>(OutputType::BECH32))), QStringLiteral("Bech32 (recommended)"));
    receive.resize(980, 600);
    receive.show();
    QVERIFY(QTest::qWaitForWindowExposed(&receive));
    auto* amount = receive.findChild<QuicksilverAmountField*>(QStringLiteral("reqAmount"));
    QVERIFY(amount);
    QVERIFY(types->mapTo(&receive, QPoint(0, 0)).y() >= amount->mapTo(&receive, QPoint(0, amount->height())).y());
    for (const char* name : {"label", "label_2", "label_3"}) {
        auto* label = receive.findChild<QLabel*>(QString::fromLatin1(name));
        QVERIFY(label);
        QVERIFY(!label->text().endsWith(QLatin1Char(':')));
        QCOMPARE(label->property("class").toString(), QStringLiteral("benchKey"));
        QVERIFY(label->alignment().testFlag(Qt::AlignLeft));
    }
    auto* table = receive.findChild<QTableView*>(QStringLiteral("recentRequestsView"));
    QVERIFY(table);
    QCOMPARE(table->model()->columnCount(), 4);
    QCOMPARE(table->horizontalHeader()->defaultAlignment(), Qt::AlignLeft | Qt::AlignVCenter);
    for (int column = 0; column < table->model()->columnCount(); ++column) {
        QVERIFY(!table->model()->headerData(column, Qt::Horizontal).toString().isEmpty());
    }
    auto* generate = receive.findChild<QPushButton*>(QStringLiteral("receiveButton"));
    QVERIFY(generate);
    for (const auto type : {OutputType::BECH32, OutputType::BECH32M, OutputType::BASE58}) {
        types->setCurrentIndex(types->findData(static_cast<int>(type)));
        QVERIFY(advice->isVisible());
        QVERIFY(!advice->text().isEmpty());
        if (type == OutputType::BASE58) {
            QVERIFY(advice->text().startsWith(QStringLiteral("Not recommended:")));
        }
        const int count = table->model()->rowCount();
        generate->click();
        QCOMPARE(table->model()->rowCount(), count + 1);
        auto dialogs = receive.findChildren<ReceiveRequestDialog*>();
        QVERIFY(!dialogs.isEmpty());
        auto* generated = dialogs.last();
        auto* address = generated->findChild<QLabel*>(QStringLiteral("address_content"));
        auto* format = generated->findChild<QLabel*>(QStringLiteral("address_format_content"));
        QVERIFY(address && format);
        QCOMPARE(OutputTypeFromDestination(DecodeDestination(address->text().toStdString())).value(), type);
        const QString expected = type == OutputType::BASE58 ? QStringLiteral("Base58") :
            type == OutputType::BECH32 ? QStringLiteral("Bech32") : QStringLiteral("Bech32m");
        QCOMPARE(format->text(), expected);
        generated->close();
    }
}

void VaultTests::signVerifyMessageRejectsNonBase58WithoutBitcoinVocabulary()
{
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    MiniGUI mini_gui(m_node, platformStyle.get());
    mini_gui.initModelForVault(m_node, SetupDescriptorsVault(m_node, test), platformStyle.get());

    SignVerifyMessageDialog dialog(platformStyle.get(), nullptr);
    dialog.setModel(mini_gui.vaultModel.get());

    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("infoLabel_SM"))->text(),
             QStringLiteral("You can sign messages/agreements with your Base58 addresses to prove you can receive Hg sent to them. Be careful not to sign anything vague or random, as phishing attacks may try to trick you into signing your identity over to them. Only sign fully-detailed statements you agree to."));

    CKey key = GenerateRandomKey();
    const QString bech32 = QString::fromStdString(EncodeDestination(WitnessV0KeyHash(key.GetPubKey())));
    const QString expected = QStringLiteral("Message signing is only supported for Base58 addresses. Please check the address and try again.");

    // Signing needs a Base58 address, so the example in both address fields is one:
    // this chain's key-hash prefix and a key-hash length, with a checksum that fails.
    const std::vector<unsigned char>& prefix = Params().Base58Prefix(CChainParams::PUBKEY_ADDRESS);
    for (const char* field : {"addressIn_SM", "addressIn_VM"}) {
        const QString placeholder = dialog.findChild<QValidatedLineEdit*>(QString::fromLatin1(field))->placeholderText();
        const QRegularExpressionMatch example = QRegularExpression(QStringLiteral("\\(e\\.g\\. (\\S+)\\)")).match(placeholder);
        QVERIFY2(example.hasMatch(), qPrintable(placeholder));
        const std::string address = example.captured(1).toStdString();
        std::vector<unsigned char> raw;
        QVERIFY2(DecodeBase58(address, raw, 64), address.c_str());
        QCOMPARE(raw.size(), prefix.size() + 20 + 4);
        QVERIFY(std::equal(prefix.begin(), prefix.end(), raw.begin()));
        QVERIFY(!IsValidDestinationString(address));
    }

    dialog.findChild<QValidatedLineEdit*>(QStringLiteral("addressIn_SM"))->setText(bech32);
    dialog.findChild<QPushButton*>(QStringLiteral("signMessageButton_SM"))->click();
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("statusLabel_SM"))->text(), expected);

    dialog.findChild<QValidatedLineEdit*>(QStringLiteral("addressIn_VM"))->setText(bech32);
    dialog.findChild<QPushButton*>(QStringLiteral("verifyMessageButton_VM"))->click();
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("statusLabel_VM"))->text(), expected);
}

void VaultTests::sendEntryAddressBookFillsDestinationWithoutNestedEventLoop()
{
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);

    std::shared_ptr<CVault> vault = SetupDescriptorsVault(m_node, test);
    CKey send_key = GenerateRandomKey();
    CTxDestination send_dest{PKHash(send_key.GetPubKey())};
    const QString send_address = QString::fromStdString(EncodeDestination(send_dest));
    {
        LOCK(vault->cs_vault);
        vault->SetAddressBook(send_dest, "destination", vault::AddressPurpose::SEND);
    }

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    MiniGUI mini_gui(m_node, platformStyle.get());
    mini_gui.initModelForVault(m_node, vault, platformStyle.get());

    QVBoxLayout* entries = mini_gui.sendCoinsDialog.findChild<QVBoxLayout*>(QStringLiteral("entries"));
    QVERIFY(entries);
    SendCoinsEntry* entry = qobject_cast<SendCoinsEntry*>(entries->itemAt(0)->widget());
    QVERIFY(entry);
    QValidatedLineEdit* pay_to = entry->findChild<QValidatedLineEdit*>(QStringLiteral("payTo"));
    QVERIFY(pay_to);
    QCOMPARE(pay_to->text(), QString());

    bool caller_returned = false;
    bool nested_loop = false;
    QTimer::singleShot(0, [&]() {
        nested_loop = !caller_returned;
        QWidget* modal = QApplication::activeModalWidget();
        if (!modal || !modal->inherits("AddressBookPage")) {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                if (widget->inherits("AddressBookPage") && widget->isVisible()) {
                    modal = widget;
                    break;
                }
            }
        }
        QVERIFY(modal);
        QVERIFY(modal->inherits("AddressBookPage"));
        QTableView* table = modal->findChild<QTableView*>(QStringLiteral("tableView"));
        QVERIFY(table);
        QVERIFY(table->model()->rowCount() >= 1);
        table->selectRow(0);
        qobject_cast<QDialog*>(modal)->accept();
    });
    entry->findChild<QToolButton*>(QStringLiteral("addressBookButton"))->click();
    caller_returned = true;
    QTRY_VERIFY_WITH_TIMEOUT(pay_to->text() == send_address, 1000);
    QVERIFY2(!nested_loop, "address book used QDialog::exec() (nested event loop)");
    QCOMPARE(pay_to->text(), send_address);
}

void VaultTests::signVerifyAddressBookDoesNotNestEventLoop()
{
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);

    std::shared_ptr<CVault> vault = SetupDescriptorsVault(m_node, test);
    CKey send_key = GenerateRandomKey();
    CTxDestination send_dest{PKHash(send_key.GetPubKey())};
    {
        LOCK(vault->cs_vault);
        vault->SetAddressBook(send_dest, "destination", vault::AddressPurpose::SEND);
    }

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    MiniGUI mini_gui(m_node, platformStyle.get());
    mini_gui.initModelForVault(m_node, vault, platformStyle.get());

    SignVerifyMessageDialog dialog(platformStyle.get(), nullptr);
    dialog.setModel(mini_gui.vaultModel.get());
    ExpectModalWithoutNestedEventLoop("AddressBookPage", [&] {
        dialog.findChild<QPushButton*>(QStringLiteral("addressBookButton_SM"))->click();
    });
    ExpectModalWithoutNestedEventLoop("AddressBookPage", [&] {
        dialog.findChild<QPushButton*>(QStringLiteral("addressBookButton_VM"))->click();
    });
}

void VaultTests::closeVaultDoesNotNestEventLoop()
{
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    OptionsModel options_model(m_node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    VaultController controller(m_node, &options_model, platformStyle.get(), nullptr);
    VaultModel vault_model(std::make_unique<PollMarkerVault>(), m_node, &options_model, platformStyle.get());

    ExpectModalWithoutNestedEventLoop("QMessageBox", [&] {
        controller.closeVault(&vault_model, nullptr);
    });
}

void VaultTests::closeAllVaultsDoesNotNestEventLoop()
{
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    OptionsModel options_model(m_node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    VaultController controller(m_node, &options_model, platformStyle.get(), nullptr);

    ExpectModalWithoutNestedEventLoop("QMessageBox", [&] {
        controller.closeAllVaults(nullptr);
    });
}

void VaultTests::abandonProofOfWorkConfirmDoesNotNestEventLoop()
{
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    OptionsModel options_model(m_node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    VaultController controller(m_node, &options_model, platformStyle.get(), nullptr);

    ExpectModalWithoutNestedEventLoop("QMessageBox", [&] {
        controller.confirmAbandonProofOfWork(nullptr, [](bool) {});
    });
}

void VaultTests::encryptPassphraseConfirmDoesNotNestEventLoop()
{
    AskPassphraseDialog dialog(AskPassphraseDialog::Encrypt, nullptr);
    dialog.findChild<QLineEdit*>(QStringLiteral("passEdit2"))->setText(QStringLiteral("abcdefghij"));
    dialog.findChild<QLineEdit*>(QStringLiteral("passEdit3"))->setText(QStringLiteral("abcdefghij"));
    ExpectModalWithoutNestedEventLoop("QMessageBox", [&] {
        dialog.accept();
    });
}

void VaultTests::customChangeAddressConfirmDoesNotNestEventLoop()
{
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    MiniGUI mini_gui(m_node, platformStyle.get());
    mini_gui.initModelForVault(m_node, SetupDescriptorsVault(m_node, test), platformStyle.get());
    QVERIFY(mini_gui.optionsModel.setOption(OptionsModel::CoinControlFeatures, true));

    QCheckBox* check = mini_gui.sendCoinsDialog.findChild<QCheckBox*>(QStringLiteral("checkBoxCoinControlChange"));
    QVERIFY(check);
    check->setChecked(true);
    QValidatedLineEdit* edit = mini_gui.sendCoinsDialog.findChild<QValidatedLineEdit*>(QStringLiteral("lineEditCoinControlChange"));
    QVERIFY(edit);

    CKey change_key = GenerateRandomKey();
    const QString change_address = QString::fromStdString(EncodeDestination(PKHash(change_key.GetPubKey())));
    ExpectModalWithoutNestedEventLoop("QMessageBox", [&] {
        Q_EMIT edit->textEdited(change_address);
    });
}

void VaultTests::unlockVaultDoesNotNestEventLoop()
{
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    auto vault = std::make_unique<PollMarkerVault>();
    PollMarkerVault* vault_ptr = vault.get();
    OptionsModel options_model(m_node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    VaultModel vault_model(std::move(vault), m_node, &options_model, platformStyle.get());
    vault_ptr->crypted = true;
    vault_ptr->locked = true;
    VaultView view(&vault_model, platformStyle.get(), nullptr);

    ExpectModalWithoutNestedEventLoop("AskPassphraseDialog", [&] {
        view.unlockVault();
    });
}

void VaultTests::unlockFailedDoesNotNestEventLoop()
{
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    auto vault = std::make_unique<PollMarkerVault>();
    PollMarkerVault* vault_ptr = vault.get();
    OptionsModel options_model(m_node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    VaultModel vault_model(std::move(vault), m_node, &options_model, platformStyle.get());
    vault_ptr->crypted = true;
    vault_ptr->locked = true;
    vault_ptr->unlock_ok = false;

    AskPassphraseDialog dialog(AskPassphraseDialog::Unlock, nullptr);
    dialog.setModel(&vault_model);
    dialog.findChild<QLineEdit*>(QStringLiteral("passEdit1"))->setText(QStringLiteral("wrong"));
    ExpectModalWithoutNestedEventLoop("QMessageBox", [&] {
        dialog.accept();
    });
}

void VaultTests::unlockRuntimeErrorDoesNotNestEventLoop()
{
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    auto vault = std::make_unique<PollMarkerVault>();
    PollMarkerVault* vault_ptr = vault.get();
    OptionsModel options_model(m_node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    VaultModel vault_model(std::move(vault), m_node, &options_model, platformStyle.get());
    vault_ptr->crypted = true;
    vault_ptr->locked = true;
    vault_ptr->unlock_throws = true;

    AskPassphraseDialog dialog(AskPassphraseDialog::Unlock, nullptr);
    dialog.setModel(&vault_model);
    dialog.findChild<QLineEdit*>(QStringLiteral("passEdit1"))->setText(QStringLiteral("passphrase"));
    ExpectModalWithoutNestedEventLoop("QMessageBox", [&] {
        dialog.accept();
    });
}

void VaultTests::changePassMismatchDoesNotNestEventLoop()
{
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    auto vault = std::make_unique<PollMarkerVault>();
    OptionsModel options_model(m_node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    VaultModel vault_model(std::move(vault), m_node, &options_model, platformStyle.get());

    AskPassphraseDialog dialog(AskPassphraseDialog::ChangePass, nullptr);
    dialog.setModel(&vault_model);
    dialog.findChild<QLineEdit*>(QStringLiteral("passEdit1"))->setText(QStringLiteral("oldpassphrase"));
    dialog.findChild<QLineEdit*>(QStringLiteral("passEdit2"))->setText(QStringLiteral("newpassphrase"));
    dialog.findChild<QLineEdit*>(QStringLiteral("passEdit3"))->setText(QStringLiteral("mismatchpass"));
    ExpectModalWithoutNestedEventLoop("QMessageBox", [&] {
        dialog.accept();
    });
}

void VaultTests::unloadWithUnrelatedModalDoesNotDefer()
{
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    OptionsModel options_model(m_node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    VaultController controller(m_node, &options_model, platformStyle.get(), nullptr);
    VaultModel* model = controller.getOrCreateVault(std::make_unique<PollMarkerVault>());
    QVERIFY(model);

    QWidget dummy;
    QDialog dialog(&dummy);
    dialog.setWindowModality(Qt::ApplicationModal);
    dialog.show();
    QCOMPARE(QApplication::activeModalWidget(), static_cast<QWidget*>(&dialog));

    QPointer<VaultModel> live(model);
    QVERIFY(QMetaObject::invokeMethod(model, "unload"));
    QApplication::processEvents();
    QVERIFY(live.isNull());
}

void VaultTests::unloadWithVaultParentedModalDefers()
{
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    OptionsModel options_model(m_node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    VaultController controller(m_node, &options_model, platformStyle.get(), nullptr);
    VaultModel* model = controller.getOrCreateVault(std::make_unique<PollMarkerVault>());
    QVERIFY(model);

    std::unique_ptr<VaultView> view(new VaultView(model, platformStyle.get(), nullptr));
    auto* dialog = new QDialog(view.get());
    dialog->setWindowModality(Qt::ApplicationModal);
    dialog->show();
    QCOMPARE(QApplication::activeModalWidget(), static_cast<QWidget*>(dialog));

    QPointer<VaultModel> live(model);
    QVERIFY(QMetaObject::invokeMethod(model, "unload"));
    QApplication::processEvents();
    QVERIFY(!live.isNull());

    dialog->close();
    view.reset();
}
