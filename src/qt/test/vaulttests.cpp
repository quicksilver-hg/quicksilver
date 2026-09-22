// Copyright (c) 2015-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/util.h>
#include <qt/test/vaulttests.h>

#include <addresstype.h>
#include <agent/headerstore.h>
#include <agent/allotmentstore.h>
#include <chainparams.h>
#include <common/args.h>
#include <core_io.h>
#include <interfaces/chain.h>
#include <interfaces/handler.h>
#include <interfaces/node.h>
#include <key.h>
#include <key_io.h>
#include <outputtype.h>
#include <qt/agentallotmentpage.h>
#include <qt/askpassphrasedialog.h>
#include <qt/clientmodel.h>
#include <qt/desktoplaunchpage.h>
#include <qt/minemintpage.h>
#include <qt/miningmodel.h>
#include <qt/networkpage.h>
#include <qt/optionsmodel.h>
#include <qt/overviewpage.h>
#include <qt/platformstyle.h>
#include <qt/rpcconsole.h>
#include <qt/vaultcontroller.h>
#include <qt/quicksilveramountfield.h>
#include <qt/quicksilverunits.h>
#include <qt/qvalidatedlineedit.h>
#include <qt/receivecoinsdialog.h>
#include <qt/signverifymessagedialog.h>
#include <qt/receiverequestdialog.h>
#include <qt/recentrequeststablemodel.h>
#include <qt/sendcoinsdialog.h>
#include <qt/sendcoinsentry.h>
#include <qt/transactiontablemodel.h>
#include <qt/transactionview.h>
#include <qt/thinvaultheadersource.h>
#include <qt/vaultframe.h>
#include <qt/vaultmodel.h>
#include <qt/vaultview.h>
#include <script/solver.h>
#include <test/util/setup_common.h>
#include <util/fs.h>
#include <util/readwritefile.h>
#include <univalue.h>
#include <validation.h>
#include <vault/coincontrol.h>
#include <vault/context.h>
#include <vault/test/util.h>
#include <vault/vault.h>
#include <vault/vaultdb.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QCheckBox>
#include <QComboBox>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScopedPointer>
#include <QSettings>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QToolButton>
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
    std::vector<vault::AgentAllotmentRecord> listAgentAllotmentRecords() override { return {}; }
    std::string agentAllotmentPolicyRequest(const vault::AgentAllotmentRecord&, CAmount) override { return {}; }
    util::Result<vault::AgentAllotmentPolicyRequestMetadata> validateAgentAllotmentPolicyRequest(const std::string&) override { return util::Error{Untranslated("unsupported")}; }
    util::Result<vault::AgentAllotmentPolicyBundle> agentAllotmentPolicyBundle(const std::string&) override { return util::Error{Untranslated("unsupported")}; }
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
    bool tryGetVaultTxDetails(const uint256&, interfaces::VaultTx&, interfaces::VaultTxStatus&, interfaces::VaultOrderForm&, bool&, int&) override { return false; }
    std::optional<common::PSQTError> fillPSQT(int, bool, bool, size_t*, PartiallySignedQuicksilverTransaction&, bool&) override { return {}; }
    interfaces::VaultBalances getBalances() override
    {
        interfaces::VaultBalances balances;
        balances.balance = balance;
        return balances;
    }
    bool tryGetBalances(interfaces::VaultBalances& balances, uint256& hash) override
    {
        ++balance_calls;
        hash = block_hash;
        balances = getBalances();
        return true;
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
    CompareBalance(vaultModel, vaultModel.getCachedBalance().balance, sendCoinsDialog.findChild<QLabel*>("labelBalance"));

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

    // Check current balance on OverviewPage
    OverviewPage overviewPage(platformStyle.get());
    QLabel* home_title = overviewPage.findChild<QLabel*>(QStringLiteral("homeBootstrapTitle"));
    QVERIFY(home_title);
    QCOMPARE(home_title->text(), QStringLiteral("Quicksilver HUD"));
    QFrame* overview_backup_panel = overviewPage.findChild<QFrame*>(QStringLiteral("overviewBackupPanel"));
    QVERIFY(overview_backup_panel);
    QCOMPARE(overviewPage.findChild<QLabel*>(QStringLiteral("overviewBackupState"))->text(), QStringLiteral("Needed"));
    QLabel* overview_backup_summary = overviewPage.findChild<QLabel*>(QStringLiteral("overviewBackupSummary"));
    QVERIFY(overview_backup_summary);
    // The banner must name the vault-file backup as the complete recovery artifact;
    // the history export deliberately does not contain spending keys.
    QVERIFY(overview_backup_summary->text().contains(QStringLiteral("only a current vault-file backup restores both spending keys and transaction history")));
    QPushButton* overview_backup_button = overviewPage.findChild<QPushButton*>(QStringLiteral("overviewBackupButton"));
    QVERIFY(overview_backup_button);
    QCOMPARE(overview_backup_button->text(), QStringLiteral("Back up vault"));
    overviewPage.setBackupState(true);
    QCOMPARE(overviewPage.findChild<QLabel*>(QStringLiteral("overviewBackupState"))->text(), QStringLiteral("Done"));
    QCOMPARE(overview_backup_button->text(), QStringLiteral("Back up again"));
    overviewPage.setVaultModel(&vaultModel);
    vaultModel.pollBalanceChanged(); // Manual balance polling update
    CompareBalance(vaultModel, vaultModel.getCachedBalance().balance, overviewPage.findChild<QLabel*>("labelBalance"));

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

    QSettings().setValue(QStringLiteral("Desktop/ConsensusEnabled"), false);
    VaultFrame vaultFrame(platformStyle.get(), nullptr);
    vaultFrame.setClientModel(mini_gui.clientModel.get());
    auto* routedVaultView = new VaultView(&vaultModel, platformStyle.get(), &vaultFrame);
    QVERIFY(vaultFrame.addView(routedVaultView));
    vaultFrame.setCurrentVault(&vaultModel);
    QCOMPARE(vaultModel.backupRecorded(), false);

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
    QLabel* launch_summary = vaultFrame.findChild<QLabel*>(QStringLiteral("launchVaultCardBody"));
    QVERIFY(launch_summary);
    const QString launch_summary_text = launch_summary->text();
    QVERIFY(launch_summary_text.contains(vaultModel.getDisplayName()));
    QVERIFY(launch_summary_text.contains(QStringLiteral("Balance: ")));
    QVERIFY(launch_summary_text.contains(QuicksilverUnits::formatWithUnit(
        vaultModel.getOptionsModel()->getDisplayUnit(),
        vaultModel.getCachedBalance().balance,
        false,
        QuicksilverUnits::SeparatorStyle::ALWAYS)));
    QVERIFY(launch_summary_text.contains(QStringLiteral("Agent setups: none")));
    QVERIFY(!launch_summary_text.contains(QStringLiteral("Funded")));
    QVERIFY(launch_summary_text.contains(QStringLiteral("Backup: check required")));
    QVERIFY(launch_summary_text.contains(QStringLiteral("Recent activity: ")));
    QCOMPARE(vaultFrame.findChild<QPushButton*>(QStringLiteral("launchVaultCardButton"))->text(), QStringLiteral("Open vault"));
    QPushButton* launch_privacy = vaultFrame.findChild<QPushButton*>(QStringLiteral("launchVaultBalancePrivacyButton"));
    QVERIFY(launch_privacy);
    QVERIFY(!launch_privacy->isHidden());
    QCOMPARE(launch_privacy->text(), QStringLiteral("Hide balance"));
    QCOMPARE(launch_privacy->property("class").toString(), QStringLiteral("secondaryActionButton"));
    QSignalSpy launch_privacy_spy(&vaultFrame, &VaultFrame::privacyRequested);
    QVERIFY(launch_privacy_spy.isValid());
    launch_privacy->click();
    QCOMPARE(launch_privacy_spy.count(), 1);
    QCOMPARE(launch_privacy_spy.takeFirst().at(0).toBool(), true);
    vaultFrame.setPrivacy(true);
    const QString masked_launch_summary = launch_summary->text();
    QVERIFY(masked_launch_summary.contains(QStringLiteral("Balance: ")));
    QVERIFY(masked_launch_summary.contains(QStringLiteral("#")));
    QVERIFY(!masked_launch_summary.contains(QuicksilverUnits::formatWithUnit(
        vaultModel.getOptionsModel()->getDisplayUnit(),
        vaultModel.getCachedBalance().balance,
        false,
        QuicksilverUnits::SeparatorStyle::ALWAYS)));
    QCOMPARE(launch_privacy->text(), QStringLiteral("Show balance"));
    launch_privacy->click();
    QCOMPARE(launch_privacy_spy.count(), 1);
    QCOMPARE(launch_privacy_spy.takeFirst().at(0).toBool(), false);
    vaultFrame.setPrivacy(false);
    QVERIFY(launch_summary->text().contains(QuicksilverUnits::formatWithUnit(
        vaultModel.getOptionsModel()->getDisplayUnit(),
        vaultModel.getCachedBalance().balance,
        false,
        QuicksilverUnits::SeparatorStyle::ALWAYS)));
    QCOMPARE(launch_privacy->text(), QStringLiteral("Hide balance"));
    QFrame* backup_panel = vaultFrame.findChild<QFrame*>(QStringLiteral("desktopLaunchBackupPanel"));
    QVERIFY(backup_panel);
    QVERIFY(!backup_panel->isHidden());
    QCOMPARE(vaultFrame.findChild<QLabel*>(QStringLiteral("desktopLaunchBackupState"))->text(), QStringLiteral("Needed"));
    QLabel* backup_summary = vaultFrame.findChild<QLabel*>(QStringLiteral("desktopLaunchBackupSummary"));
    QVERIFY(backup_summary);
    QVERIFY(backup_summary->text().contains(QStringLiteral("only a current vault-file backup restores both spending keys and transaction history")));
    QPushButton* backup_button = vaultFrame.findChild<QPushButton*>(QStringLiteral("desktopLaunchBackupButton"));
    QVERIFY(backup_button);
    QCOMPARE(backup_button->text(), QStringLiteral("Back up vault"));
    QCOMPARE(backup_button->property("class").toString(), QStringLiteral("primaryActionButton"));
    QPushButton* launch_mining = vaultFrame.findChild<QPushButton*>(QStringLiteral("launchMiningCardButton"));
    QVERIFY(launch_mining);
    QVERIFY(launch_mining->isEnabled());

    vaultFrame.gotoOverviewPage();
    QLabel* routed_backup_state = vaultFrame.findChild<QLabel*>(QStringLiteral("overviewBackupState"));
    QVERIFY(routed_backup_state);
    QCOMPARE(routed_backup_state->text(), QStringLiteral("Needed"));
    QLabel* routed_backup_summary = vaultFrame.findChild<QLabel*>(QStringLiteral("overviewBackupSummary"));
    QVERIFY(routed_backup_summary);
    QVERIFY(routed_backup_summary->text().contains(QStringLiteral("does not issue a recovery phrase")));
    QVERIFY(routed_backup_summary->text().contains(QStringLiteral("restores both spending keys and transaction history")));
    QPushButton* routed_backup_button = vaultFrame.findChild<QPushButton*>(QStringLiteral("overviewBackupButton"));
    QVERIFY(routed_backup_button);
    QCOMPARE(routed_backup_button->text(), QStringLiteral("Back up vault"));

    QTemporaryDir backup_dir;
    QVERIFY(backup_dir.isValid());
    const QString backup_file = backup_dir.filePath(QStringLiteral("vault-backup.dat"));
    QVERIFY(vaultModel.vault().backupVault(backup_file.toLocal8Bit().data()));
    QCOMPARE(vaultModel.backupRecorded(), true);
    vaultFrame.setCurrentVault(&vaultModel);
    vaultFrame.gotoLaunchPage();
    QCOMPARE(vaultFrame.findChild<QLabel*>(QStringLiteral("desktopLaunchBackupState"))->text(), QStringLiteral("Done"));
    QVERIFY(vaultFrame.findChild<QLabel*>(QStringLiteral("desktopLaunchBackupSummary"))->text().contains(QStringLiteral("vault records a completed backup")));
    QCOMPARE(vaultFrame.findChild<QPushButton*>(QStringLiteral("desktopLaunchBackupButton"))->text(), QStringLiteral("Back up again"));
    vaultFrame.gotoOverviewPage();
    QCOMPARE(vaultFrame.findChild<QLabel*>(QStringLiteral("overviewBackupState"))->text(), QStringLiteral("Done"));
    QVERIFY(vaultFrame.findChild<QLabel*>(QStringLiteral("overviewBackupSummary"))->text().contains(QStringLiteral("vault records a completed backup")));
    QCOMPARE(vaultFrame.findChild<QPushButton*>(QStringLiteral("overviewBackupButton"))->text(), QStringLiteral("Back up again"));

    vaultFrame.gotoAgentAllotmentPage();
    QVERIFY(vaultFrame.currentVaultView());
    QWidget* agent_allotment_page = vaultFrame.currentVaultView()->currentWidget();
    QVERIFY(agent_allotment_page);
    QCOMPARE(agent_allotment_page->objectName(), QStringLiteral("agentAllotmentPage"));
    QCOMPARE(agent_allotment_page->findChild<QLabel*>(QStringLiteral("agentAllotmentTitle"))->text(), QStringLiteral("Agent allotments"));
    QVERIFY(agent_allotment_page->findChild<QLabel*>(QStringLiteral("agentAllotmentDishonestAgentRisk"))->text().contains(QStringLiteral("dishonest agent")));
    QVERIFY(agent_allotment_page->findChild<QLabel*>(QStringLiteral("agentAllotmentCompromisedHostRisk"))->text().contains(QStringLiteral("compromised host")));
    QLabel* guarantee_risk = agent_allotment_page->findChild<QLabel*>(QStringLiteral("agentAllotmentGuaranteeRisk"));
    QVERIFY(guarantee_risk);
    QVERIFY(guarantee_risk->text().contains(QStringLiteral("no guarantee")));
    QLabel* agent_utxo_state = agent_allotment_page->findChild<QLabel*>(QStringLiteral("agentAllotmentUtxoState"));
    QVERIFY(agent_utxo_state);
    QVERIFY(agent_utxo_state->text().contains(QStringLiteral("Spendable agent UTXOs: 0")));
    QPushButton* agent_utxo_refresh_button = agent_allotment_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentRefreshUtxosButton"));
    QVERIFY(agent_utxo_refresh_button);
    QVERIFY(agent_utxo_refresh_button->isEnabled());
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
    QCOMPARE(agent_create->text(), QStringLiteral("Record shared-key risk and setup"));
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
    QVERIFY(backend_state->text().contains(QStringLiteral("marks agent policy integration pending")));
    agent_create->click();
    const auto agent_records = vaultModel.listAgentAllotmentRecords();
    QCOMPARE(agent_records.size(), size_t{1});
    QCOMPARE(QString::fromStdString(agent_records[0].label), QStringLiteral("test-agent"));
    QCOMPARE(agent_records[0].funding_limit, COIN);
    QCOMPARE(agent_records[0].daily_limit, COIN / 2);
    QVERIFY(agent_records[0].risk_accepted_time > 0);
    QVERIFY(!agent_records[0].backend_created);
    QVERIFY(IsValidDestinationString(agent_records[0].funding_address));
    QVERIFY(agent_records[0].policy_status == vault::AgentAllotmentPolicyStatus::PendingIntegration);
    QVERIFY(backend_state->text().contains(QStringLiteral("Setup recorded with a vault funding address")));
    QVERIFY(recorded_setups->text().contains(QStringLiteral("test-agent")));
    QVERIFY(recorded_setups->text().contains(QStringLiteral("daily guardrail")));
    QVERIFY(recorded_setups->text().contains(QStringLiteral("policy pending")));
    QVERIFY(recorded_setups->text().contains(QString::fromStdString(agent_records[0].funding_address)));
    QCOMPARE(vaultModel.agentAllotmentFundingAvailable(agent_records[0]), CAmount{0});
    QLabel* agent_funding_state = agent_allotment_page->findChild<QLabel*>(QStringLiteral("agentAllotmentFundingState"));
    QVERIFY(agent_funding_state);
    QCOMPARE(agent_funding_state->text(), QStringLiteral("Awaiting funding"));
    QLabel* agent_policy_state = agent_allotment_page->findChild<QLabel*>(QStringLiteral("agentAllotmentPolicyState"));
    QVERIFY(agent_policy_state);
    QCOMPARE(agent_policy_state->text(), QStringLiteral("Policy pending"));
    QLabel* agent_funding_row = agent_allotment_page->findChild<QLabel*>(QStringLiteral("agentAllotmentFundingRowLabel"));
    QVERIFY(agent_funding_row);
    QVERIFY(agent_funding_row->text().contains(QStringLiteral("awaits funding")));
    QVERIFY(agent_funding_row->text().contains(QStringLiteral("Policy integration remains pending")));
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
    QCOMPARE(agent_policy_state->text(), QStringLiteral("Policy pending"));
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
    QCOMPARE(agent_policy_state->text(), QStringLiteral("Policy pending"));
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
    QVERIFY(agent_bundle.contains(QStringLiteral("\"type\":\"quicksilver.agent_allotment_key_bundle\"")));
    QVERIFY(agent_bundle.contains(QStringLiteral("\"version\":1")));
    QVERIFY(agent_bundle.contains(QStringLiteral("\"policy_request\":{")));
    QVERIFY(agent_bundle.contains(QStringLiteral("\"funding_secret_wif\":\"")));
    QVERIFY(agent_bundle.contains(QStringLiteral("\"funding_outputs\":[")));
    QVERIFY(agent_bundle.contains(QStringLiteral("\"amount_cinnabar\":\"")));
    QVERIFY(agent_bundle.contains(QStringLiteral("\"policy_enforcement\":\"pending_integration\"")));
    QVERIFY(agent_bundle.contains(QStringLiteral("\"funding_address\":\"%1\"").arg(QString::fromStdString(agent_records[0].funding_address))));
    QVERIFY(backend_state->text().contains(QStringLiteral("Agent bundle copied")));
    QPlainTextEdit* agent_spend_bundle_edit = agent_allotment_page->findChild<QPlainTextEdit*>(QStringLiteral("agentAllotmentSpendBundleEdit"));
    QVERIFY(agent_spend_bundle_edit);
    QLineEdit* agent_spend_destination_edit = agent_allotment_page->findChild<QLineEdit*>(QStringLiteral("agentAllotmentSpendDestinationEdit"));
    QVERIFY(agent_spend_destination_edit);
    QuicksilverAmountField* agent_spend_amount = agent_allotment_page->findChild<QuicksilverAmountField*>(QStringLiteral("agentAllotmentSpendAmount"));
    QVERIFY(agent_spend_amount);
    QuicksilverAmountField* agent_spent_today = agent_allotment_page->findChild<QuicksilverAmountField*>(QStringLiteral("agentAllotmentSpentToday"));
    QVERIFY(agent_spent_today);
    QPushButton* agent_copy_spend_command_button = agent_allotment_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentCopySpendCommandButton"));
    QVERIFY(agent_copy_spend_command_button);
    QVERIFY(!agent_copy_spend_command_button->isEnabled());
    QPushButton* agent_sign_spend_button = agent_allotment_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentSignSpendButton"));
    QVERIFY(agent_sign_spend_button);
    QVERIFY(!agent_sign_spend_button->isEnabled());
    QLabel* agent_spend_command_state = agent_allotment_page->findChild<QLabel*>(QStringLiteral("agentAllotmentSpendCommandState"));
    QVERIFY(agent_spend_command_state);
    QVERIFY(agent_spend_command_state->text().contains(QStringLiteral("Paste an agent bundle")));
    QCOMPARE(agent_spend_command_state->property("class").toString(), QStringLiteral("policyReviewIdle"));
    agent_spend_bundle_edit->setPlainText(agent_bundle);
    agent_spend_destination_edit->setText(QString::fromStdString(agent_records[0].funding_address));
    agent_spend_amount->setValue(COIN / 3);
    QVERIFY(agent_copy_spend_command_button->isEnabled());
    QVERIFY(agent_sign_spend_button->isEnabled());
    QCOMPARE(agent_spend_command_state->property("class").toString(), QStringLiteral("policyReviewReady"));
    agent_copy_spend_command_button->click();
    const fs::path policy_bundle_path = gArgs.GetDataDirNet() / "agent" / "policy-bundles.d" / "agent-1.json";
    const auto saved_policy_bundle = ReadBinaryFile(policy_bundle_path);
    QVERIFY(saved_policy_bundle.first);
    QCOMPARE(QString::fromStdString(saved_policy_bundle.second), agent_bundle + QStringLiteral("\n"));
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("quicksilver-agent -chain=%1 -policybundle=\"$(cat '%2')\" -destination='%3' -spendamount=33333333 signbundle")
                                                 .arg(QString::fromStdString(Params().GetChainTypeString()),
                                                      QString::fromStdString(fs::PathToString(policy_bundle_path)),
                                                      QString::fromStdString(agent_records[0].funding_address)));
    QVERIFY(agent_spend_command_state->text().contains(QStringLiteral("Agent spend command copied")));
    QVERIFY(agent_spend_command_state->text().contains(QStringLiteral("agent-1")));
    QCOMPARE(agent_spend_command_state->property("class").toString(), QStringLiteral("policyReviewValid"));
    agent_spent_today->setValue(COIN / 10);
    QVERIFY(agent_copy_spend_command_button->isEnabled());
    QVERIFY(agent_sign_spend_button->isEnabled());
    QCOMPARE(agent_spend_command_state->property("class").toString(), QStringLiteral("policyReviewReady"));
    agent_copy_spend_command_button->click();
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("quicksilver-agent -chain=%1 -policybundle=\"$(cat '%2')\" -destination='%3' -spendamount=33333333 -spenttoday=10000000 signbundle")
                                                 .arg(QString::fromStdString(Params().GetChainTypeString()),
                                                      QString::fromStdString(fs::PathToString(policy_bundle_path)),
                                                      QString::fromStdString(agent_records[0].funding_address)));
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
    QVERIFY(backend_state->text().contains(QStringLiteral("Agent funding receipts saved and imported")));
    QVERIFY(backend_state->text().contains(QString::number(exported_bundle->funding_outputs.size())));
    QVERIFY(agent_utxo_state->text().contains(QStringLiteral("Spendable agent UTXOs: %1").arg(exported_bundle->funding_outputs.size())));
    QVERIFY(agent_utxo_state->text().contains(QStringLiteral("Imported %1 new receipt").arg(exported_bundle->funding_outputs.size())));
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
    QVERIFY(policy_request.contains(QStringLiteral("\"policy_status\":\"pending_integration\"")));
    QVERIFY(policy_request.contains(QStringLiteral("\"backend_created\":false")));
    QPlainTextEdit* agent_policy_review_edit = agent_allotment_page->findChild<QPlainTextEdit*>(QStringLiteral("agentAllotmentPolicyRequestEdit"));
    QVERIFY(agent_policy_review_edit);
    QPushButton* agent_policy_review_button = agent_allotment_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentReviewPolicyButton"));
    QVERIFY(agent_policy_review_button);
    QVERIFY(!agent_policy_review_button->isEnabled());
    QLabel* agent_policy_review_state = agent_allotment_page->findChild<QLabel*>(QStringLiteral("agentAllotmentPolicyReviewState"));
    QVERIFY(agent_policy_review_state);
    QVERIFY(agent_policy_review_state->text().contains(QStringLiteral("Paste a policy request")));
    QCOMPARE(agent_policy_review_state->property("class").toString(), QStringLiteral("policyReviewIdle"));
    agent_policy_review_edit->setPlainText(policy_request);
    QVERIFY(agent_policy_review_button->isEnabled());
    QVERIFY(agent_policy_review_state->text().contains(QStringLiteral("Ready to review")));
    QCOMPARE(agent_policy_review_state->property("class").toString(), QStringLiteral("policyReviewReady"));
    agent_policy_review_button->click();
    QVERIFY(agent_policy_review_state->text().contains(QStringLiteral("Valid request for test-agent")));
    QVERIFY(agent_policy_review_state->text().contains(QString::fromStdString(agent_records[0].funding_address)));
    QVERIFY(agent_policy_review_state->text().contains(QStringLiteral("Review does not activate enforcement")));
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
    QCOMPARE(agent_payment_receipt_state->property("class").toString(), QStringLiteral("policyReviewIdle"));
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
    QVERIFY(agent_payment_receipt_state->text().contains(QStringLiteral("imported into the durable agent output store")));
    QVERIFY(agent_payment_receipt_state->text().contains(QStringLiteral("External-agent scan command copied")));
    QVERIFY(agent_utxo_state->text().contains(QStringLiteral("Imported 1 new receipt")));
    QCOMPARE(agent_payment_receipt_state->property("class").toString(), QStringLiteral("policyReviewValid"));
    agent_payment_receipt_edit->setPlainText(QStringLiteral("[]"));
    QVERIFY(agent_payment_receipt_review_button->isEnabled());
    QVERIFY(agent_payment_receipt_save_button->isEnabled());
    QCOMPARE(agent_payment_receipt_state->property("class").toString(), QStringLiteral("policyReviewReady"));
    agent_payment_receipt_review_button->click();
    QVERIFY(agent_payment_receipt_state->text().contains(QStringLiteral("must be a JSON object")));
    QCOMPARE(agent_payment_receipt_state->property("class").toString(), QStringLiteral("policyReviewError"));
    QPlainTextEdit* agent_signed_spend_edit = agent_allotment_page->findChild<QPlainTextEdit*>(QStringLiteral("agentAllotmentSignedSpendEdit"));
    QVERIFY(agent_signed_spend_edit);
    QPushButton* agent_signed_spend_review_button = agent_allotment_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentReviewSignedSpendButton"));
    QVERIFY(agent_signed_spend_review_button);
    QVERIFY(!agent_signed_spend_review_button->isEnabled());
    QPushButton* agent_signed_spend_copy_relay_button = agent_allotment_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentCopyRelayPayloadsButton"));
    QVERIFY(agent_signed_spend_copy_relay_button);
    QVERIFY(!agent_signed_spend_copy_relay_button->isEnabled());
    QLineEdit* agent_signed_spend_relay_peer_edit = agent_allotment_page->findChild<QLineEdit*>(QStringLiteral("agentAllotmentRelayPeerEdit"));
    QVERIFY(agent_signed_spend_relay_peer_edit);
    QPushButton* agent_signed_spend_copy_add_peer_command_button = agent_allotment_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentCopyAddPeerCommandButton"));
    QVERIFY(agent_signed_spend_copy_add_peer_command_button);
    QVERIFY(!agent_signed_spend_copy_add_peer_command_button->isEnabled());
    QPushButton* agent_signed_spend_copy_discover_peers_command_button = agent_allotment_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentCopyDiscoverPeersCommandButton"));
    QVERIFY(agent_signed_spend_copy_discover_peers_command_button);
    QVERIFY(agent_signed_spend_copy_discover_peers_command_button->isEnabled());
    QPushButton* agent_signed_spend_import_node_peers_button = agent_allotment_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentImportNodePeersButton"));
    QVERIFY(agent_signed_spend_import_node_peers_button);
    QVERIFY(agent_signed_spend_import_node_peers_button->isEnabled());
    QPushButton* agent_signed_spend_copy_node_address_import_command_button = agent_allotment_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentCopyNodeAddressImportCommandButton"));
    QVERIFY(agent_signed_spend_copy_node_address_import_command_button);
    QVERIFY(agent_signed_spend_copy_node_address_import_command_button->isEnabled());
    QPushButton* agent_signed_spend_copy_sync_headers_command_button = agent_allotment_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentCopySyncHeadersCommandButton"));
    QVERIFY(agent_signed_spend_copy_sync_headers_command_button);
    QVERIFY(agent_signed_spend_copy_sync_headers_command_button->isEnabled());
    QPushButton* agent_signed_spend_copy_peer_command_button = agent_allotment_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentCopyPeerRelayCommandButton"));
    QVERIFY(agent_signed_spend_copy_peer_command_button);
    QVERIFY(!agent_signed_spend_copy_peer_command_button->isEnabled());
    QPushButton* agent_signed_spend_copy_stored_peer_command_button = agent_allotment_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentCopyStoredPeerRelayCommandButton"));
    QVERIFY(agent_signed_spend_copy_stored_peer_command_button);
    QVERIFY(!agent_signed_spend_copy_stored_peer_command_button->isEnabled());
    QPushButton* agent_signed_spend_relay_peer_button = agent_allotment_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentRelayPeerButton"));
    QVERIFY(agent_signed_spend_relay_peer_button);
    QVERIFY(!agent_signed_spend_relay_peer_button->isEnabled());
    QPushButton* agent_signed_spend_submit_button = agent_allotment_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentSubmitSignedSpendButton"));
    QVERIFY(agent_signed_spend_submit_button);
    QVERIFY(!agent_signed_spend_submit_button->isEnabled());
    QLabel* agent_signed_spend_state = agent_allotment_page->findChild<QLabel*>(QStringLiteral("agentAllotmentSignedSpendState"));
    QVERIFY(agent_signed_spend_state);
    QVERIFY(agent_signed_spend_state->text().contains(QStringLiteral("Paste signed spend output")));
    QCOMPARE(agent_signed_spend_state->property("class").toString(), QStringLiteral("policyReviewIdle"));
    agent_sign_spend_button->click();
    // The spend is prepared off the GUI thread. The signed result is the same
    // string as before; it arrives on a later turn instead of inside the click.
    //
    // The wait is long on purpose, and is not a tolerance on the assertion. What
    // it bounds is a real sandbox Cuckatoo grind: the prove loop searches nonces
    // until a cycle beats the target, so its cost is a random variable, not a
    // fixed one. Before this test went asynchronous the click() blocked and no
    // time bound existed at all, so any duration passed; QTRY_VERIFY's 5 s
    // default introduced one that the work has always exceeded -- it was
    // measured at 11.4 s under `ctest -j10`. A tight bound here would fail on an
    // unlucky nonce or a loaded box while the code was correct. A genuine hang
    // still fails, in finite time.
    QTRY_VERIFY_WITH_TIMEOUT(agent_spend_command_state->text().contains(QStringLiteral("Agent spend signed locally")), 120000);
    QCOMPARE(agent_spend_command_state->property("class").toString(), QStringLiteral("policyReviewValid"));
    QVERIFY(agent_signed_spend_edit->toPlainText().contains(QStringLiteral("tx_payload=")));
    QVERIFY(agent_signed_spend_edit->toPlainText().contains(QStringLiteral("inv_payload=")));
    QVERIFY(agent_signed_spend_edit->toPlainText().contains(QStringLiteral("change_paymentreceipt=")));
    QVERIFY(agent_signed_spend_edit->toPlainText().contains(QStringLiteral("receipt_store_saved=true")));
    QVERIFY(agent_signed_spend_edit->toPlainText().contains(QStringLiteral("receipt_store_change_added=1")));
    QVERIFY(agent_signed_spend_state->text().contains(QStringLiteral("Valid signed spend")));
    QVERIFY(agent_signed_spend_state->text().contains(QStringLiteral("Change receipt saved")));
    QCOMPARE(agent_signed_spend_state->property("class").toString(), QStringLiteral("policyReviewValid"));
    QVERIFY(agent_signed_spend_copy_relay_button->isEnabled());
    QVERIFY(agent_signed_spend_copy_stored_peer_command_button->isEnabled());
    QVERIFY(agent_signed_spend_relay_peer_button->isEnabled());
    QVERIFY(agent_signed_spend_submit_button->isEnabled());
    const agent::AllotmentReceiptStoreLoadResult local_sign_store{agent::LoadAllotmentPaymentReceipts(gArgs.GetDataDirNet() / "agent" / "payment-receipts.dat", Params().GenesisBlock().GetHash())};
    QVERIFY(local_sign_store.ok());
    QCOMPARE(local_sign_store.receipts.size(), exported_bundle->funding_outputs.size() + size_t{1});
    QVERIFY(std::any_of(local_sign_store.receipts.begin(), local_sign_store.receipts.end(), [&](const auto& receipt) {
        return receipt.funding_address == discovered_funding_address && receipt.payment_id == "desk-42";
    }));
    QVERIFY(std::any_of(local_sign_store.receipts.begin(), local_sign_store.receipts.end(), [&](const auto& receipt) {
        return receipt.funding_address == agent_records[0].funding_address && receipt.memo == "agent spend change";
    }));
    QVERIFY(std::any_of(local_sign_store.activities.begin(), local_sign_store.activities.end(), [](const auto& activity) {
        return activity.type == agent::AllotmentReceiptActivityType::CHANGE;
    }));

    CMutableTransaction mutable_agent_spend;
    mutable_agent_spend.version = 2;
    mutable_agent_spend.vin.emplace_back(COutPoint{Txid::FromUint256(ArithToUint256(700)), 0});
    mutable_agent_spend.vout.emplace_back(COIN / 3, GetScriptForDestination(agent_funding_dest));
    const CTransaction agent_spend_tx{mutable_agent_spend};
    const QString agent_spend_hex = QString::fromStdString(EncodeHexTx(agent_spend_tx));
    const QString change_receipt = QStringLiteral(R"({"type":"quicksilver.agent_payment_receipt","version":1,"chain":"%1","genesis_hash":"%2","funding_address":"%3","txid":"%4","vout":0,"amount_cinnabar":"33333333","received_time":"790","payment_id":"agent-1:change","label":"test-agent","memo":"agent spend change","payer":"desktop vault"})")
                                       .arg(QString::fromStdString(Params().GetChainTypeString()),
                                            QString::fromStdString(Params().GenesisBlock().GetHash().ToString()),
                                            QString::fromStdString(agent_records[0].funding_address),
                                            QString::fromStdString(agent_spend_tx.GetHash().ToString()));
    agent_signed_spend_edit->setPlainText(QStringLiteral("policy_id=agent-1\nhex=%1\nchange_paymentreceipt=%2\nreceipt_store_saved=true")
                                              .arg(agent_spend_hex, change_receipt));
    QVERIFY(agent_signed_spend_review_button->isEnabled());
    QVERIFY(!agent_signed_spend_copy_relay_button->isEnabled());
    QVERIFY(!agent_signed_spend_copy_stored_peer_command_button->isEnabled());
    QVERIFY(!agent_signed_spend_submit_button->isEnabled());
    QCOMPARE(agent_signed_spend_state->property("class").toString(), QStringLiteral("policyReviewReady"));
    agent_signed_spend_review_button->click();
    QVERIFY(agent_signed_spend_state->text().contains(QStringLiteral("Valid signed spend")));
    QVERIFY(agent_signed_spend_state->text().contains(QStringLiteral("1 input")));
    QVERIFY(agent_signed_spend_state->text().contains(QStringLiteral("1 output")));
    QVERIFY(agent_signed_spend_state->text().contains(QStringLiteral("Submit starts consensus-backed relay")));
    QVERIFY(agent_signed_spend_state->text().contains(QStringLiteral("Change receipt saved")));
    QCOMPARE(agent_signed_spend_state->property("class").toString(), QStringLiteral("policyReviewValid"));
    const fs::path change_receipt_path = receipt_inbox / fs::PathFromString(agent_spend_tx.GetHash().ToString() + "-0.json");
    const auto saved_change_receipt = ReadBinaryFile(change_receipt_path);
    QVERIFY(saved_change_receipt.first);
    QCOMPARE(QString::fromStdString(saved_change_receipt.second), change_receipt + QStringLiteral("\n"));
    QVERIFY(agent_signed_spend_copy_relay_button->isEnabled());
    QVERIFY(agent_signed_spend_copy_stored_peer_command_button->isEnabled());
    QVERIFY(agent_signed_spend_relay_peer_button->isEnabled());
    QVERIFY(!agent_signed_spend_copy_peer_command_button->isEnabled());
    QVERIFY(agent_signed_spend_submit_button->isEnabled());
    agent_signed_spend_copy_discover_peers_command_button->click();
    const QString stored_discovery_command = QApplication::clipboard()->text();
    QCOMPARE(stored_discovery_command, QStringLiteral("quicksilver-agent -chain=%1 discoverpeers").arg(QString::fromStdString(Params().GetChainTypeString())));
    QVERIFY(agent_signed_spend_state->text().contains(QStringLiteral("Stored-peer discovery command copied")));
    QCOMPARE(agent_signed_spend_state->property("class").toString(), QStringLiteral("policyReviewValid"));
    agent_signed_spend_copy_node_address_import_command_button->click();
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("quicksilver-cli -chain=%1 getnodeaddresses 64 | quicksilver-agent -chain=%1 -peeraddresses=- importnodeaddresses")
                                             .arg(QString::fromStdString(Params().GetChainTypeString())));
    QVERIFY(agent_signed_spend_state->text().contains(QStringLiteral("Local-node peer import command copied")));
    QCOMPARE(agent_signed_spend_state->property("class").toString(), QStringLiteral("policyReviewValid"));
    agent_signed_spend_copy_sync_headers_command_button->click();
    const QString stored_header_sync_command = QApplication::clipboard()->text();
    QCOMPARE(stored_header_sync_command, QStringLiteral("quicksilver-agent -chain=%1 syncheaderspeer").arg(QString::fromStdString(Params().GetChainTypeString())));
    QVERIFY(agent_signed_spend_state->text().contains(QStringLiteral("Stored-peer header sync command copied")));
    QCOMPARE(agent_signed_spend_state->property("class").toString(), QStringLiteral("policyReviewValid"));
    const QString chain = QString::fromStdString(Params().GetChainTypeString());
    const QString relay_peer = QStringLiteral("127.0.0.1:%1").arg(Params().GetDefaultPort());
    agent_signed_spend_relay_peer_edit->setText(relay_peer);
    QVERIFY(agent_signed_spend_copy_add_peer_command_button->isEnabled());
    QVERIFY(agent_signed_spend_copy_discover_peers_command_button->isEnabled());
    QVERIFY(agent_signed_spend_copy_sync_headers_command_button->isEnabled());
    QVERIFY(agent_signed_spend_copy_peer_command_button->isEnabled());
    agent_signed_spend_copy_add_peer_command_button->click();
    const QString add_peer_command = QApplication::clipboard()->text();
    QCOMPARE(add_peer_command, QStringLiteral("quicksilver-agent -chain=%1 -peer='%2' addpeer").arg(chain, relay_peer));
    QVERIFY(agent_signed_spend_state->text().contains(QStringLiteral("Relay peer save command copied")));
    QCOMPARE(agent_signed_spend_state->property("class").toString(), QStringLiteral("policyReviewValid"));
    agent_signed_spend_copy_discover_peers_command_button->click();
    const QString peer_discovery_command = QApplication::clipboard()->text();
    QCOMPARE(peer_discovery_command, QStringLiteral("quicksilver-agent -chain=%1 -peer='%2' discoverpeers").arg(chain, relay_peer));
    QVERIFY(agent_signed_spend_state->text().contains(QStringLiteral("Relay peer discovery command copied")));
    QCOMPARE(agent_signed_spend_state->property("class").toString(), QStringLiteral("policyReviewValid"));
    agent_signed_spend_copy_sync_headers_command_button->click();
    const QString peer_header_sync_command = QApplication::clipboard()->text();
    QCOMPARE(peer_header_sync_command, QStringLiteral("quicksilver-agent -chain=%1 -peer='%2' syncheaderspeer").arg(chain, relay_peer));
    QVERIFY(agent_signed_spend_state->text().contains(QStringLiteral("Relay peer header sync command copied")));
    QCOMPARE(agent_signed_spend_state->property("class").toString(), QStringLiteral("policyReviewValid"));
    agent_signed_spend_copy_relay_button->click();
    const QString relay_payloads = QApplication::clipboard()->text();
    QVERIFY(relay_payloads.contains(QStringLiteral("tx_payload=")));
    QVERIFY(relay_payloads.contains(QStringLiteral("inv_payload=")));
    QVERIFY(agent_signed_spend_state->text().contains(QStringLiteral("Relay payloads copied")));
    QCOMPARE(agent_signed_spend_state->property("class").toString(), QStringLiteral("policyReviewValid"));
    QVERIFY(agent_signed_spend_copy_relay_button->isEnabled());
    QVERIFY(agent_signed_spend_copy_stored_peer_command_button->isEnabled());
    QVERIFY(agent_signed_spend_relay_peer_button->isEnabled());
    QVERIFY(agent_signed_spend_copy_peer_command_button->isEnabled());
    QVERIFY(agent_signed_spend_submit_button->isEnabled());
    agent_signed_spend_edit->setPlainText(relay_payloads);
    QVERIFY(agent_signed_spend_review_button->isEnabled());
    QCOMPARE(agent_signed_spend_state->property("class").toString(), QStringLiteral("policyReviewReady"));
    agent_signed_spend_review_button->click();
    QVERIFY(agent_signed_spend_state->text().contains(QStringLiteral("Valid signed spend")));
    QVERIFY(agent_signed_spend_state->text().contains(QStringLiteral("1 input")));
    QVERIFY(agent_signed_spend_state->text().contains(QStringLiteral("1 output")));
    QCOMPARE(agent_signed_spend_state->property("class").toString(), QStringLiteral("policyReviewValid"));
    QVERIFY(agent_signed_spend_copy_relay_button->isEnabled());
    QVERIFY(agent_signed_spend_copy_stored_peer_command_button->isEnabled());
    QVERIFY(agent_signed_spend_relay_peer_button->isEnabled());
    QVERIFY(agent_signed_spend_copy_peer_command_button->isEnabled());
    QVERIFY(agent_signed_spend_submit_button->isEnabled());
    agent_signed_spend_copy_peer_command_button->click();
    const QString relay_command = QApplication::clipboard()->text();
    QVERIFY(relay_command.startsWith(QStringLiteral("quicksilver-agent -chain=%1 ").arg(chain)));
    QVERIFY(relay_command.contains(QStringLiteral("-peer='%1'").arg(relay_peer)));
    QVERIFY(relay_command.contains(QStringLiteral(" -message=")));
    QVERIFY(relay_command.endsWith(QStringLiteral(" sendtxpeer")));
    QVERIFY(agent_signed_spend_state->text().contains(QStringLiteral("Peer relay command copied")));
    QCOMPARE(agent_signed_spend_state->property("class").toString(), QStringLiteral("policyReviewValid"));
    agent_signed_spend_copy_stored_peer_command_button->click();
    const QString stored_peer_relay_command = QApplication::clipboard()->text();
    QVERIFY(stored_peer_relay_command.startsWith(QStringLiteral("quicksilver-agent -chain=%1 ").arg(QString::fromStdString(Params().GetChainTypeString()))));
    QVERIFY(!stored_peer_relay_command.contains(QStringLiteral(" -peer=")));
    QVERIFY(stored_peer_relay_command.contains(QStringLiteral(" -message=")));
    QVERIFY(stored_peer_relay_command.endsWith(QStringLiteral(" sendtxpeer")));
    QVERIFY(agent_signed_spend_state->text().contains(QStringLiteral("Stored-peer relay command copied")));
    QCOMPARE(agent_signed_spend_state->property("class").toString(), QStringLiteral("policyReviewValid"));
    agent_signed_spend_submit_button->click();
    QVERIFY(agent_signed_spend_state->text().contains(QStringLiteral("Signed spend submission failed")));
    QCOMPARE(agent_signed_spend_state->property("class").toString(), QStringLiteral("policyReviewError"));
    agent_signed_spend_edit->setPlainText(QStringLiteral("hex=not-a-transaction"));
    QVERIFY(agent_signed_spend_review_button->isEnabled());
    QVERIFY(!agent_signed_spend_copy_relay_button->isEnabled());
    QVERIFY(!agent_signed_spend_copy_stored_peer_command_button->isEnabled());
    QVERIFY(!agent_signed_spend_copy_peer_command_button->isEnabled());
    QVERIFY(!agent_signed_spend_submit_button->isEnabled());
    QCOMPARE(agent_signed_spend_state->property("class").toString(), QStringLiteral("policyReviewReady"));
    agent_signed_spend_review_button->click();
    QVERIFY(agent_signed_spend_state->text().contains(QStringLiteral("not a valid serialized Quicksilver transaction")));
    QCOMPARE(agent_signed_spend_state->property("class").toString(), QStringLiteral("policyReviewError"));
    vaultFrame.setCurrentVault(&vaultModel);
    vaultFrame.gotoLaunchPage();
    launch_summary = vaultFrame.findChild<QLabel*>(QStringLiteral("launchVaultCardBody"));
    QVERIFY(launch_summary);
    QVERIFY(launch_summary->text().contains(QStringLiteral("Agent funding: 1 of 1 confirmed")));

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
    QLabel* requests_heading = receiveCoinsDialog.findChild<QLabel*>(QStringLiteral("recentRequestsHeading"));
    QVERIFY(requests_heading);
    QCOMPARE(requests_heading->text(), QStringLiteral("Recent requests"));
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

void VaultTests::agentAllotmentImportsNodePeers()
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
    AgentAllotmentPage agent_allotment_page;
    agent_allotment_page.setModel(&vault_model);

    QPushButton* import_button = agent_allotment_page.findChild<QPushButton*>(QStringLiteral("agentAllotmentImportNodePeersButton"));
    QVERIFY(import_button);
    QVERIFY(import_button->isEnabled());
    QLabel* state = agent_allotment_page.findChild<QLabel*>(QStringLiteral("agentAllotmentSignedSpendState"));
    QVERIFY(state);

    UniValue add_peer_params{UniValue::VARR};
    add_peer_params.push_back("1.2.3.4");
    add_peer_params.push_back(Params().GetDefaultPort());
    add_peer_params.push_back(false);
    const UniValue add_peer_result{m_node.executeRpc("addpeeraddress", add_peer_params, /*uri=*/{})};
    const bool added{add_peer_result.find_value("success").isBool() && add_peer_result.find_value("success").get_bool()};
    const std::vector<CService> node_addresses{m_node.getNodeAddresses(64)};
    QVERIFY(added || std::any_of(node_addresses.begin(), node_addresses.end(), [](const CService& peer) {
        return peer.ToStringAddrPort() == strprintf("1.2.3.4:%u", Params().GetDefaultPort());
    }));

    import_button->click();

    const fs::path relay_peer_store = gArgs.GetDataDirNet() / "agent" / "relay-peers.json";
    const auto saved_relay_peers = ReadBinaryFile(relay_peer_store);
    QVERIFY(saved_relay_peers.first);
    const QString saved_json = QString::fromStdString(saved_relay_peers.second);
    QVERIFY(saved_json.contains(QStringLiteral("\"type\": \"quicksilver.agent_relay_peers\"")));
    QVERIFY(saved_json.contains(QStringLiteral("1.2.3.4:%1").arg(Params().GetDefaultPort())));
    QVERIFY(state->text().contains(QStringLiteral("Local-node peers imported")));
    QCOMPARE(state->property("class").toString(), QStringLiteral("policyReviewValid"));
}

void VaultTests::agentAllotmentRelaysInBackgroundWithPeerFallback()
{
    const fs::path relay_peer_store{gArgs.GetDataDirNet() / "agent" / "relay-peers.json"};
    fs::create_directories(relay_peer_store.parent_path());
    const QString peer_store_json = QStringLiteral(R"({
  "type": "quicksilver.agent_relay_peers",
  "version": 1,
  "chain": "%1",
  "genesis_hash": "%2",
  "peers": ["127.0.0.1:%3", "127.0.0.2:%3"]
})")
                                        .arg(QString::fromStdString(Params().GetChainTypeString()),
                                             QString::fromStdString(Params().GenesisBlock().GetHash().ToString()),
                                             QString::number(Params().GetDefaultPort()));
    QVERIFY(WriteBinaryFile(relay_peer_store, peer_store_json.toStdString() + "\n"));

    std::atomic<int> relay_attempts{0};
    std::atomic<bool> ran_off_gui_thread{false};
    std::atomic<int64_t> observed_timeout_ms{0};
    QThread* const gui_thread{QThread::currentThread()};
    AgentAllotmentPage page{nullptr, [&](const CService& peer,
                                      const CSerializedNetMsg&,
                                      std::chrono::milliseconds timeout) {
        observed_timeout_ms = timeout.count();
        ran_off_gui_thread = QThread::currentThread() != gui_thread;
        std::this_thread::sleep_for(std::chrono::milliseconds{200});

        agent::PeerTransactionRelayResult result;
        result.peer = peer;
        if (relay_attempts.fetch_add(1) == 0) {
            result.error = "test peer unavailable";
        } else {
            result.connected = true;
            result.peer_version_received = true;
            result.peer_verack_received = true;
            result.local_verack_sent = true;
            result.sent_tx = true;
        }
        return result;
    }};

    QPlainTextEdit* signed_spend_edit{page.findChild<QPlainTextEdit*>(QStringLiteral("agentAllotmentSignedSpendEdit"))};
    QLineEdit* relay_peer_edit{page.findChild<QLineEdit*>(QStringLiteral("agentAllotmentRelayPeerEdit"))};
    QPushButton* review_button{page.findChild<QPushButton*>(QStringLiteral("agentAllotmentReviewSignedSpendButton"))};
    QPushButton* relay_button{page.findChild<QPushButton*>(QStringLiteral("agentAllotmentRelayPeerButton"))};
    QLabel* state{page.findChild<QLabel*>(QStringLiteral("agentAllotmentSignedSpendState"))};
    QVERIFY(signed_spend_edit);
    QVERIFY(relay_peer_edit);
    QVERIFY(review_button);
    QVERIFY(relay_button);
    QVERIFY(state);

    CMutableTransaction mutable_spend;
    mutable_spend.version = 2;
    mutable_spend.vin.emplace_back(COutPoint{Txid::FromUint256(ArithToUint256(901)), 0});
    mutable_spend.vout.emplace_back(COIN / 4, CScript{});
    signed_spend_edit->setPlainText(QStringLiteral("hex=%1").arg(QString::fromStdString(EncodeHexTx(CTransaction{mutable_spend}))));
    review_button->click();
    QVERIFY(relay_button->isEnabled());
    QVERIFY(relay_peer_edit->text().isEmpty());

    QElapsedTimer click_timer;
    click_timer.start();
    relay_button->click();
    QVERIFY2(click_timer.elapsed() < 150, "peer relay blocked the GUI thread");
    QVERIFY(!relay_button->isEnabled());
    QVERIFY(!signed_spend_edit->isEnabled());
    QVERIFY(state->text().contains(QStringLiteral("in the background")));

    QElapsedTimer relay_timer;
    relay_timer.start();
    while (!state->text().contains(QStringLiteral("sent to 1 of 2 peer(s)")) && relay_timer.elapsed() < 3000) {
        // Deliver only the relay's queued result. Processing every application
        // timer here can wake unrelated models retained by earlier GUI tests.
        QCoreApplication::sendPostedEvents(&page);
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    QVERIFY(state->text().contains(QStringLiteral("sent to 1 of 2 peer(s)")));
    QCOMPARE(relay_attempts.load(), 2);
    QVERIFY(ran_off_gui_thread.load());
    QCOMPARE(observed_timeout_ms.load(), agent::DEFAULT_AGENT_PEER_TIMEOUT_MS);
    QVERIFY(state->text().contains(QStringLiteral("test peer unavailable")));
    QCOMPARE(state->property("class").toString(), QStringLiteral("policyReviewValid"));
    QVERIFY(relay_button->isEnabled());
    QVERIFY(signed_spend_edit->isEnabled());

    std::atomic<bool> orphaned_worker_finished{false};
    auto closing_page{std::make_unique<AgentAllotmentPage>(nullptr, [&](const CService& peer,
                                                                     const CSerializedNetMsg&,
                                                                     std::chrono::milliseconds) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        orphaned_worker_finished = true;
        agent::PeerTransactionRelayResult result;
        result.peer = peer;
        result.sent_tx = true;
        return result;
    })};
    QPlainTextEdit* closing_spend_edit{closing_page->findChild<QPlainTextEdit*>(QStringLiteral("agentAllotmentSignedSpendEdit"))};
    QLineEdit* closing_peer_edit{closing_page->findChild<QLineEdit*>(QStringLiteral("agentAllotmentRelayPeerEdit"))};
    QPushButton* closing_review_button{closing_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentReviewSignedSpendButton"))};
    QPushButton* closing_relay_button{closing_page->findChild<QPushButton*>(QStringLiteral("agentAllotmentRelayPeerButton"))};
    QVERIFY(closing_spend_edit);
    QVERIFY(closing_peer_edit);
    QVERIFY(closing_review_button);
    QVERIFY(closing_relay_button);
    closing_spend_edit->setPlainText(QStringLiteral("hex=%1").arg(QString::fromStdString(EncodeHexTx(CTransaction{mutable_spend}))));
    closing_review_button->click();
    closing_peer_edit->setText(QStringLiteral("127.0.0.1:%1").arg(Params().GetDefaultPort()));
    closing_relay_button->click();
    closing_page.reset();

    QElapsedTimer orphaned_worker_timer;
    orphaned_worker_timer.start();
    while (!orphaned_worker_finished.load() && orphaned_worker_timer.elapsed() < 1000) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    QVERIFY(orphaned_worker_finished.load());
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
             QStringLiteral("Warming up (0 graphs attempted)"));

    st.active = false;
    page.setStatus(st);
    QCOMPARE(page.findChild<QLabel*>("attemptsRateValue")->text(),
             QStringLiteral("None (0 graphs attempted)"));

    // F-109: the core fault string is written for quicksilverd and names a
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
             QStringLiteral("No GPU solver is configured. Choose one in Controls > Options > Main."));
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
    QCOMPARE(idle.attempts, QStringLiteral("None (0 graphs attempted)"));
    QCOMPARE(idle.health, QStringLiteral("Not running"));
    QVERIFY(!idle.show_configure_solver);

    const auto warming = rows(true, true, true, true, false, 0, std::nullopt, nullptr);
    QCOMPARE(warming.block_mining, QStringLiteral("Active"));
    QCOMPARE(warming.solver, QStringLiteral("GPU bridge"));
    QCOMPARE(warming.attempts, QStringLiteral("Warming up (0 graphs attempted)"));
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
    QCOMPARE(halted.attempts, QStringLiteral("Not solving (0 graphs attempted)"));
    QCOMPARE(halted.health, QStringLiteral("No graphics solver is configured, and processor block mining is off. Choose a solver, or allow processor block mining, in Controls > Options > Main."));
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
    QGroupBox* panel = page.findChild<QGroupBox*>(QStringLiteral("torSetupPanel"));
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
    QVERIFY(working->text().contains(QStringLiteral("none of the manual setup below is needed")));

    // And it is reversible: a Tor that dies must bring the recipe back.
    page.applyTorSetupAdvice(/*onion_proxy_configured=*/false);
    QVERIFY(steps->isVisibleTo(&page));
    QVERIFY(!working->isVisibleTo(&page));
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
    QCOMPARE(page.findChild<QLabel*>("developerNetworkCurrentValue")->text(), QString("Quicksilver Sandbox (sandbox)"));
    QLabel* developer_banner = page.findChild<QLabel*>("developerNetworkBanner");
    QVERIFY(developer_banner);
    QVERIFY(!developer_banner->isHidden());
    QVERIFY(developer_banner->text().contains(QString("not using the live ledger")));
    QCOMPARE(page.findChild<QLabel*>("developerNetworkSandboxCardState")->text(), QString("Current"));
    QCOMPARE(page.findChild<QLabel*>("developerNetworkMainCardState")->text(), QString("Restart to use"));
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
    QVERIFY(tor_steps->text().contains(QStringLiteral("Controls > Options > Network")));
    QPushButton* main_restart = page.findChild<QPushButton*>("developerNetworkMainCardButton");
    QPushButton* publictest_restart = page.findChild<QPushButton*>("developerNetworkPublicTestCardButton");
    QPushButton* sandbox_restart = page.findChild<QPushButton*>("developerNetworkSandboxCardButton");
    QVERIFY(main_restart);
    QVERIFY(publictest_restart);
    QVERIFY(sandbox_restart);
    QCOMPARE(main_restart->text(), QString("Restart"));
    QVERIFY(main_restart->isEnabled());
    QVERIFY(publictest_restart->isEnabled());
    QCOMPARE(sandbox_restart->text(), QString("Current network"));
    QVERIFY(!sandbox_restart->isEnabled());
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

void VaultTests::overviewPageMasksValuesWithoutClientModel()
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

    // Opening a vault with consensus off leaves the client model unset, and the overview
    // page is handed the privacy setting the moment the vault is added.
    OverviewPage overview_page(platformStyle.get());
    overview_page.setVaultModel(&vault_model);

    options_model.setOption(OptionsModel::OptionID::MaskValues, false);
    overview_page.setPrivacy(true);
    QVERIFY(options_model.getOption(OptionsModel::OptionID::MaskValues).toBool());

    overview_page.setPrivacy(false);
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
    QVERIFY(missing_arg_text.contains(QStringLiteral("Settings → Options → Main")));

    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    const QString absent_solver_path = temp_dir.filePath(QStringLiteral("missing-solver"));
    const QString absent_solver_text = SendCoinsDialog::sendWorkResourceTextForTesting(true, absent_solver_path);
    QVERIFY(absent_solver_text.contains(QStringLiteral("could not be found")));
    QVERIFY(absent_solver_text.contains(QStringLiteral("Settings → Options → Main")));

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
    QVERIFY(executable_solver_text.contains(QStringLiteral("transfer helper is configured")));
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

void VaultTests::requestPageAddressTypeCopyHasNoBitcoinVocabulary()
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

    QComboBox* types = receive.findChild<QComboBox*>(QStringLiteral("addressType"));
    QVERIFY(types);
    QVERIFY(mini_gui.vaultModel->vault().taprootEnabled());
    QCOMPARE(types->count(), 3);
    QCOMPARE(types->itemText(0), QStringLiteral("Base58"));
    QCOMPARE(types->itemData(0, Qt::ToolTipRole).toString(),
             QStringLiteral("Not recommended due to larger transactions and less protection against typos."));
    QCOMPARE(types->itemText(1), QStringLiteral("Bech32"));
    QCOMPARE(types->itemData(1, Qt::ToolTipRole).toString(),
             QStringLiteral("Recommended. Smaller transfers and better protection against mistyped addresses."));
    QCOMPARE(types->itemText(2), QStringLiteral("Bech32m"));
    QCOMPARE(types->itemData(2, Qt::ToolTipRole).toString(),
             QStringLiteral("Same benefits as Bech32, with a stronger checksum."));
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
