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
#include <node/context.h>
#include <node/warnings.h>
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
#include <qt/fittingscrollarea.h>
#include <qt/gpusituation.h>
#include <crypto/cuckatoo/gpu_solver.h>
#include <crypto/cuckatoo/multi_device.h>
#include <qt/minemintpage.h>
#include <qt/miningmodel.h>
#include <qt/networkpage.h>
#include <qt/networkstyle.h>
#include <qt/notificator.h>
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
#include <qt/transactiondesc.h>
#include <qt/transactiontablemodel.h>
#include <qt/transactionview.h>
#include <qt/thinvaultheadersource.h>
#include <qt/vaultframe.h>
#include <qt/vaultmodel.h>
#include <qt/vaultview.h>
#include <qt/utilitydialog.h>
#include <QTextBrowser>
#include <QTextBlock>
#include <QTextDocumentFragment>
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
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>

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
#include <QHBoxLayout>
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
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QFontInfo>
#include <QStyleOption>
#include <QStyleOptionViewItem>
#include <QTextDocument>
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
QString Listing(const QStringList& lines)
{
    static int counter{0};
    const QString path{QDir::temp().filePath(QStringLiteral("qs_vaulttests_%1_%2.listing").arg(QCoreApplication::applicationPid()).arg(++counter))};
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) qFatal("cannot write %s", qPrintable(path));
    for (const QString& line : lines) file.write((line + QLatin1Char('\n')).toUtf8());
    return path;
}

cuckatoo::GpuDiscovery Discover(const QStringList& lines, const QByteArray& devices_env = {})
{
    qputenv("CUCKATOO_GPU_SOLVER", SolverProbeStubPath().toUtf8());
    qputenv("QS_PROBE_STUB_LISTING", Listing(lines).toUtf8());
    if (devices_env.isEmpty()) qunsetenv("CUCKATOO_GPU_DEVICES");
    else qputenv("CUCKATOO_GPU_DEVICES", devices_env);
    cuckatoo::ResetGpuDiscoveryForTesting();
    return cuckatoo::DiscoverGpus(19);
}

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
    //! Atomic: the transfer worker reads it while the GUI thread locks.
    std::atomic<bool> locked{false};
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
    std::vector<interfaces::VaultValueMap> committed_value_maps;
    std::vector<interfaces::VaultOrderForm> committed_order_forms;
    std::optional<interfaces::VaultTx> details_tx;
    int set_address_calls{0};
    int new_mining_destination_calls{0};
    std::optional<bilingual_str> commit_error;
    //! Set by the test so createTransaction can observe the model that is driving it.
    VaultModel* vault_model_under_test{nullptr};
    bool in_flight_during_create{false};
    bool cancel_requested_during_create{false};
    std::optional<bool> remove_load_on_start{true}; // an unset value the calls cannot produce
    //! Closed by a test to hold createTransaction until the GUI thread has run on.
    std::atomic<bool> create_gate_open{true};
    std::atomic<bool> locked_during_create{false};

    bool encryptVault(const SecureString&) override { return false; }
    bool isCrypted() override { return crypted; }
    bool lock() override
    {
        locked = true;
        return true;
    }
    bool unlock(const SecureString&) override
    {
        if (unlock_throws) throw std::runtime_error("unlock boom");
        if (unlock_ok) locked = false;
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
    util::Result<CTxDestination> getNewDestination(const OutputType, const std::string& label) override
    {
        if (label == "mining") ++new_mining_destination_calls;
        return CTxDestination{};
    }
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
        for (int i = 0; !create_gate_open && i < 1000; ++i) std::this_thread::sleep_for(std::chrono::milliseconds{10});
        // Signing needs the keys, so the vault must still be unlocked here.
        locked_during_create = locked.load();
        // Stands in for the grind: records what the vault model reported while a
        // transfer was in flight, which is what the close paths consult.
        in_flight_during_create = vault_model_under_test && vault_model_under_test->proofOfWorkInFlight();
        cancel_requested_during_create = tx_proof_cancel && tx_proof_cancel();
        return util::Error{Untranslated("unsupported")};
    }
    util::Result<void> commitTransaction(CTransactionRef, interfaces::VaultValueMap value_map, interfaces::VaultOrderForm order_form) override
    {
        ++commit_calls;
        if (commit_error) return util::Error{*commit_error};
        committed_value_maps.push_back(std::move(value_map));
        committed_order_forms.push_back(std::move(order_form));
        return {};
    }
    bool transactionCanBeAbandoned(const uint256&) override { return false; }
    bool abandonTransaction(const uint256&) override { return false; }
    CTransactionRef getTx(const uint256&) override { return {}; }
    interfaces::VaultTx getVaultTx(const uint256&) override { return {}; }
    std::set<interfaces::VaultTx> getVaultTxs() override { return {}; }
    bool tryGetTxStatus(const uint256&, interfaces::VaultTxStatus&, int&, int64_t&) override { return false; }
    bool spendsUnconfirmedChange(const CTransaction&) override { return false; }
    bool tryGetVaultTxDetails(const uint256&, interfaces::VaultTx& tx, interfaces::VaultTxStatus& status, interfaces::VaultOrderForm& order_form, bool& in_relaypool, int& num_blocks) override
    {
        if (!details_tx) return false;
        tx = *details_tx;
        status = {};
        order_form = committed_order_forms.empty() ? interfaces::VaultOrderForm{} : committed_order_forms.back();
        in_relaypool = false;
        num_blocks = 0;
        return true;
    }
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
    // SP3: the page is now live. With an address and the role idle, Start is
    // enabled, Stop is disabled, and the status
    // label reflects the polled state rather than the scaffold placeholder.
    mineMintPage->setPayoutAddress(QStringLiteral("hg1qexample"));
    QVERIFY(mineMintPage->findChild<QPushButton*>(QStringLiteral("startMiningButton"))->isEnabled());
    QVERIFY(!mineMintPage->findChild<QPushButton*>(QStringLiteral("stopMiningButton"))->isEnabled());
    QCOMPARE(mineMintPage->findChild<QLabel*>(QStringLiteral("miningStatusValue"))->text(), QStringLiteral("Idle"));
    interfaces::MiningStatus minted_status;
    minted_status.coins_minted_session = 123456789;
    vaultModel.getOptionsModel()->setDisplayUnit(QVariant::fromValue(QuicksilverUnit::HGS));
    mineMintPage->setStatus(minted_status);
    QCOMPARE(mineMintPage->findChild<QLabel*>(QStringLiteral("sessionMintedValue"))->text(), QStringLiteral("123,456,789🜓"));
    vaultModel.getOptionsModel()->setDisplayUnit(QVariant::fromValue(QuicksilverUnit::HG));
    QCOMPARE(mineMintPage->findChild<QLabel*>(QStringLiteral("sessionMintedValue"))->text(), QStringLiteral("1.23456789Hg"));
    vaultModel.getOptionsModel()->setDisplayUnit(QVariant::fromValue(QuicksilverUnit::HGS));

    // F-482: VaultView no longer has a network page. The same "Offline" with no
    // peers is read on VaultFrame's network page, with consensus running.
    {
        QSettings().setValue(QStringLiteral("Desktop/ConsensusEnabled"), false);
        VaultFrame offlineFrame(platformStyle.get(), nullptr);
        offlineFrame.setClientModel(mini_gui.clientModel.get());
        offlineFrame.gotoConsensusReviewPage();
        QStackedWidget* offline_stack = offlineFrame.findChild<QStackedWidget*>(QStringLiteral("vaultFrameStack"));
        QVERIFY(offline_stack);
        QPushButton* offline_continue = offline_stack->currentWidget()->findChild<QPushButton*>(QStringLiteral("consensusContinueButton"));
        QVERIFY(offline_continue);
        offline_continue->click();
        NetworkPage* offline_page = qobject_cast<NetworkPage*>(offline_stack->currentWidget());
        QVERIFY(offline_page);
        QCOMPARE(offline_page->objectName(), QStringLiteral("networkPage"));
        QCOMPARE(offline_page->findChild<QLabel*>(QStringLiteral("networkStatusValue"))->text(), QStringLiteral("Offline"));
    }

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
    noVaultFrame.gotoConsensusReviewPage();
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
    // The node's warnings read in Home's Node panel. The test raises one itself: a
    // release build has no unreleased-build warning, so the list can be empty.
    QVERIFY(node.context() && node.context()->warnings);
    const QString clock_warning = QStringLiteral("Test: the clock is out of sync.");
    QVERIFY(node.context()->warnings->Set(node::Warning::CLOCK_OUT_OF_SYNC, Untranslated(clock_warning.toStdString())));
    vaultFrame.setClientModel(mini_gui.clientModel.get());
    {
        QLabel* node_alerts = vaultFrame.findChild<QLabel*>(QStringLiteral("homeNodeAlerts"));
        QVERIFY(node_alerts);
        const QString warnings = mini_gui.clientModel->getStatusBarWarnings();
        QVERIFY(warnings.contains(clock_warning));
        QCOMPARE(node_alerts->text(), warnings);
        QVERIFY(!node_alerts->isHidden());
    }
    QVERIFY(node.context()->warnings->Unset(node::Warning::CLOCK_OUT_OF_SYNC));
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

    vaultFrame.gotoConsensusReviewPage();
    QStackedWidget* vault_stack = vaultFrame.findChild<QStackedWidget*>(QStringLiteral("vaultFrameStack"));
    QVERIFY(vault_stack);
    QCOMPARE(vault_stack->currentWidget()->objectName(), QStringLiteral("consensusReviewPage"));
    QPushButton* consensus_continue = vault_stack->currentWidget()->findChild<QPushButton*>(QStringLiteral("consensusContinueButton"));
    QVERIFY(consensus_continue);
    QCOMPARE(consensus_continue->text(), QStringLiteral("Enable consensus"));
    consensus_continue->click();
    NetworkPage* networkPage = qobject_cast<NetworkPage*>(vault_stack->currentWidget());
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
            QCOMPARE(vaultModel.getOptionsModel()->getDisplayUnit(), QuicksilverUnit::HGS);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("amount_content")->text(), QString::fromUtf8("1\xF0\x9F\x9C\x93"));

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

    // Each figure is one text run: the suffix shares the figure's size and
    // baseline. TOTAL is everything the vault holds.
    const auto plain_text = [](const QLabel* label) {
        return QTextDocumentFragment::fromHtml(label->text()).toPlainText();
    };
    const auto expect_ticker = [&](const QString& spendable, const QString& pending, const QString& maturing,
                                   const QString& delegated, const QString& total) {
        const interfaces::VaultBalances balances = model.getCachedBalance();
        QCOMPARE(plain_text(window.findChild<QLabel*>(QStringLiteral("benchTickerSpendable"))), spendable);
        QCOMPARE(plain_text(window.findChild<QLabel*>(QStringLiteral("benchTickerPending"))), pending);
        QCOMPARE(plain_text(window.findChild<QLabel*>(QStringLiteral("benchTickerMaturing"))), maturing);
        QCOMPARE(plain_text(window.findChild<QLabel*>(QStringLiteral("benchTickerDelegated"))), delegated);
        QCOMPARE(plain_text(window.findChild<QLabel*>(QStringLiteral("benchTickerTotal"))), total);
        QCOMPARE(window.findChild<QWidget*>(QStringLiteral("benchTickerDelegatedBox"))->isHidden(), balances.delegated_balance == 0);
        // One text run per figure: each figure box holds its key and its value, and no other label.
        for (const char* name : {"benchTickerSpendable", "benchTickerPending", "benchTickerMaturing", "benchTickerDelegated", "benchTickerTotal"}) {
            const QLabel* value = window.findChild<QLabel*>(QString::fromLatin1(name));
            QVERIFY(value);
            QCOMPARE(value->parentWidget()->findChildren<QLabel*>(QString(), Qt::FindDirectChildrenOnly).size(), 2);
        }
    };

    QVERIFY(model.getOptionsModel()->setOption(OptionsModel::DisplayUnit, QVariant::fromValue(QuicksilverUnit::HGS)));
    const interfaces::VaultBalances initial = model.getCachedBalance();
    QCOMPARE(initial.balance, 50 * COIN);
    QCOMPARE(initial.unconfirmed_balance, 2 * COIN);
    QCOMPARE(initial.immature_balance, 3 * COIN);
    QCOMPARE(initial.delegated_balance, 0 * COIN);
    expect_ticker(QStringLiteral("5,000,000,000🜓"), QStringLiteral("200,000,000🜓"),
                  QStringLiteral("300,000,000🜓"), QStringLiteral("0🜓"),
                  QStringLiteral("5,500,000,000🜓"));
    if (QTest::currentTestFailed()) return;
    const QString spendable_html = window.findChild<QLabel*>(QStringLiteral("benchTickerSpendable"))->text();
    const QString vermilion_span = QStringLiteral("<span style='color:%1;'>🜓</span>")
                                      .arg(QuicksilverStyle::Color(QuicksilverStyle::Token::Vermilion).name());
    QCOMPARE(spendable_html.count(QStringLiteral("color:")), 1);
    QVERIFY(spendable_html.contains(vermilion_span));
    QVERIFY(!spendable_html.contains(QStringLiteral("font-size")));
    QVERIFY(model.getOptionsModel()->setOption(OptionsModel::DisplayUnit, QVariant::fromValue(QuicksilverUnit::HG)));
    QCOMPARE(plain_text(window.findChild<QLabel*>(QStringLiteral("benchTickerSpendable"))), QStringLiteral("50.00000000Hg"));
    QVERIFY(!window.findChild<QLabel*>(QStringLiteral("benchTickerSpendable"))->text().contains(QStringLiteral("color:")));
    QVERIFY(model.getOptionsModel()->setOption(OptionsModel::DisplayUnit, QVariant::fromValue(QuicksilverUnit::HGS)));
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
    expect_ticker(QStringLiteral("4,000,000,000🜓"), QStringLiteral("200,000,000🜓"),
                  QStringLiteral("300,000,000🜓"), QStringLiteral("0🜓"),
                  QStringLiteral("4,500,000,000🜓"));
    if (QTest::currentTestFailed()) return;
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("benchTickerSpendable"))->text() != spendable_before);

    vault_ptr->delegated = 7 * COIN;
    vault_ptr->block_hash = uint256{3};
    model.pollBalanceChanged();
    expect_ticker(QStringLiteral("4,000,000,000🜓"), QStringLiteral("200,000,000🜓"),
                  QStringLiteral("300,000,000🜓"), QStringLiteral("700,000,000🜓"),
                  QStringLiteral("5,200,000,000🜓"));
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
    expect_ticker(QStringLiteral("#🜓"), QStringLiteral("#🜓"), QStringLiteral("#🜓"),
                  QStringLiteral("#🜓"), QStringLiteral("#🜓"));
    if (QTest::currentTestFailed()) return;
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("benchTickerSpendable"))->text().contains(QLatin1Char('#')));
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("benchTickerSpendable"))->text() != spendable_before);
    mask->setChecked(false);
    expect_ticker(QStringLiteral("4,000,000,000🜓"), QStringLiteral("200,000,000🜓"),
                  QStringLiteral("300,000,000🜓"), QStringLiteral("700,000,000🜓"),
                  QStringLiteral("5,200,000,000🜓"));
    if (QTest::currentTestFailed()) return;

    vault_ptr->delegated = 0;
    vault_ptr->block_hash = uint256{4};
    model.pollBalanceChanged();
    expect_ticker(QStringLiteral("4,000,000,000🜓"), QStringLiteral("200,000,000🜓"),
                  QStringLiteral("300,000,000🜓"), QStringLiteral("0🜓"),
                  QStringLiteral("4,500,000,000🜓"));

    QAction* transfer = window.findChild<QAction*>(QStringLiteral("sendCoinsAction"));
    QVERIFY(transfer);
    transfer->setChecked(true);
    QCOMPARE(crumb->text(), model.getDisplayName().toUpper() + QStringLiteral(" / TRANSFER"));
}

//! F-481: Home's card buttons and both Mine / Mint network links go through
//! the window navigation slots, so the page, rail, and breadcrumb stay together.
void VaultTests::homeAndMineLinksFollowRail()
{
    QSettings settings;
    const bool had_consensus_setting = settings.contains(QStringLiteral("Desktop/ConsensusEnabled"));
    const QVariant previous_consensus_setting = settings.value(QStringLiteral("Desktop/ConsensusEnabled"));
    struct RestoreConsensusSetting {
        bool had_value;
        QVariant value;
        ~RestoreConsensusSetting()
        {
            QSettings settings;
            if (had_value) settings.setValue(QStringLiteral("Desktop/ConsensusEnabled"), value);
            else settings.remove(QStringLiteral("Desktop/ConsensusEnabled"));
        }
    } restore_consensus_setting{had_consensus_setting, previous_consensus_setting};
    settings.setValue(QStringLiteral("Desktop/ConsensusEnabled"), true);

    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    QScopedPointer<const NetworkStyle> network_style(NetworkStyle::instantiate(Params().GetChainType()));
    OptionsModel options_model(m_node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    VaultModel model(std::make_unique<PollMarkerVault>(), m_node, &options_model, platform_style.get());

    QuicksilverGUI window(m_node, platform_style.get(), network_style.data());
    VaultFrame* frame = window.findChild<VaultFrame*>(QStringLiteral("vaultFrame"));
    QVERIFY(frame);
    auto* view = new VaultView(&model, platform_style.get(), frame);
    QVERIFY(frame->addView(view));
    frame->setCurrentVault(&model);

    QStackedWidget* frame_stack = frame->findChild<QStackedWidget*>(QStringLiteral("vaultFrameStack"));
    QLabel* breadcrumb = window.findChild<QLabel*>(QStringLiteral("benchBreadcrumb"));
    QAction* home_action = window.findChild<QAction*>(QStringLiteral("homeBootstrapAction"));
    QAction* mine_action = window.findChild<QAction*>(QStringLiteral("mineMintAction"));
    QAction* network_action = window.findChild<QAction*>(QStringLiteral("networkAction"));
    QVERIFY(frame_stack);
    QVERIFY(breadcrumb);
    QVERIFY(home_action);
    QVERIFY(mine_action);
    QVERIFY(network_action);

    const QString vault_crumb = model.getDisplayName().toUpper() + QStringLiteral(" / ");
    const auto expect_page = [&](const QString& page_name, QAction* action, const QString& crumb_page) {
        QWidget* page = frame_stack->currentWidget();
        if (page == view) page = view->currentWidget();
        QCOMPARE(page->objectName(), page_name);
        QVERIFY(action->isChecked());
        QCOMPARE(breadcrumb->text(), vault_crumb + crumb_page);
    };
    const auto home_button = [&](const char* object_name) {
        home_action->trigger();
        QPushButton* button = window.findChild<QPushButton*>(QString::fromLatin1(object_name));
        QVERIFY(button);
        button->click();
    };

    home_button("launchMiningCardButton");
    expect_page(QStringLiteral("mineMintPage"), mine_action, QStringLiteral("MINE / MINT"));

    home_button("launchConsensusCardButton");
    expect_page(QStringLiteral("networkPage"), network_action, QStringLiteral("NETWORK"));

    home_button("launchVaultCardButton");
    expect_page(QStringLiteral("desktopLaunchPage"), home_action, QStringLiteral("HOME"));

    home_button("launchMiningCardButton");
    QPushButton* vault_mine_network = view->findChild<QPushButton*>(QStringLiteral("mineMintIsolationNetworkButton"));
    QVERIFY(vault_mine_network);
    vault_mine_network->click();
    QVERIFY(frame_stack->currentWidget() != view);
    expect_page(QStringLiteral("networkPage"), network_action, QStringLiteral("NETWORK"));

    QuicksilverGUI no_vault_window(m_node, platform_style.get(), network_style.data());
    VaultFrame* no_vault_frame = no_vault_window.findChild<VaultFrame*>(QStringLiteral("vaultFrame"));
    QStackedWidget* no_vault_stack = no_vault_window.findChild<QStackedWidget*>(QStringLiteral("vaultFrameStack"));
    QAction* no_vault_mine_action = no_vault_window.findChild<QAction*>(QStringLiteral("mineMintAction"));
    QAction* no_vault_network_action = no_vault_window.findChild<QAction*>(QStringLiteral("networkAction"));
    QLabel* no_vault_breadcrumb = no_vault_window.findChild<QLabel*>(QStringLiteral("benchBreadcrumb"));
    QVERIFY(no_vault_frame);
    QVERIFY(no_vault_stack);
    QVERIFY(no_vault_mine_action);
    QVERIFY(no_vault_network_action);
    QVERIFY(no_vault_breadcrumb);
    QPushButton* no_vault_mining = no_vault_window.findChild<QPushButton*>(QStringLiteral("launchMiningCardButton"));
    QVERIFY(no_vault_mining);
    no_vault_mining->click();
    QCOMPARE(no_vault_stack->currentWidget()->objectName(), QStringLiteral("mineMintPage"));
    QVERIFY(no_vault_mine_action->isChecked());
    QCOMPARE(no_vault_breadcrumb->text(), QStringLiteral("NO VAULT OPEN / MINE / MINT"));
    QPushButton* no_vault_mine_network = no_vault_stack->currentWidget()->findChild<QPushButton*>(QStringLiteral("mineMintIsolationNetworkButton"));
    QVERIFY(no_vault_mine_network);
    no_vault_mine_network->click();
    QCOMPARE(no_vault_stack->currentWidget()->objectName(), QStringLiteral("networkPage"));
    QVERIFY(no_vault_network_action->isChecked());
    QCOMPARE(no_vault_breadcrumb->text(), QStringLiteral("NO VAULT OPEN / NETWORK"));
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
        // STATE last. No status-icon column. Below a 1200 px window Type is
        // hidden (F-459, owner ruling 2026-10-16). The window asks for 1200x800
        // but is bounded by the screen, which on Windows can be smaller.
        QHeaderView* header = table->horizontalHeader();
        QStringList columns;
        for (int visual = 0; visual < header->count(); ++visual) {
            const int logical = header->logicalIndex(visual);
            if (header->isSectionHidden(logical)) continue;
            columns << rows->headerData(logical, Qt::Horizontal).toString();
        }
        const QString unit = QuicksilverUnits::shortName(mini_gui.vaultModel->getOptionsModel()->getDisplayUnit());
        const bool wide_window = window.width() >= 1200;
        if (!wide_window) {
            const QRect screen = window.screen()->availableGeometry();
            qWarning("Type column expected hidden: below the 1200 px window width, window %dx%d on a %dx%d screen",
                     window.width(), window.height(), screen.width(), screen.height());
        }
        QCOMPARE(header->isSectionHidden(TransactionTableModel::Type), !wide_window);
        QStringList want_columns{QStringLiteral("DATE"), QStringLiteral("TYPE"), QStringLiteral("LABEL"),
                                 QStringLiteral("AMOUNT  ") + unit, QStringLiteral("STATE")};
        if (!wide_window) want_columns.removeOne(QStringLiteral("TYPE"));
        QCOMPARE(columns, want_columns);
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
    const QString known_label = QStringLiteral("household-budget-02");
    {
        LOCK(vault->cs_vault);
        vault->SetAddressBook(CTxDestination{WitnessV0KeyHash(test.coinbaseKey.GetPubKey())}, known_label.toStdString(), vault::AddressPurpose::RECEIVE);
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
        if (home_rows->index(row, TransactionTableModel::ToAddress).data().toString() == known_label) labelled = true;
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
    // The fit is a claim about the 1200x800 design size. Windows will not make a
    // window larger than its screen, and a CI runner's screen can be smaller.
    const bool design_size = window.width() >= 1200 && window.height() >= 800;
    if (!design_size) {
        const QRect screen = window.screen()->availableGeometry();
        qWarning("Label fit not checked: below the 1200x800 design size, window %dx%d on a %dx%d screen",
                 window.width(), window.height(), screen.width(), screen.height());
    }
    const int label_need = home_cells->sizeHintForColumn(TransactionTableModel::ToAddress);
    if (real_fonts && design_size) {
        QVERIFY2(home_table->columnWidth(TransactionTableModel::ToAddress) >= label_need,
                 qPrintable(QStringLiteral("Label %1 px of %2").arg(home_table->columnWidth(TransactionTableModel::ToAddress)).arg(label_need)));
        QVERIFY(home_table->fontMetrics().horizontalAdvance(known_label) < home_table->columnWidth(TransactionTableModel::ToAddress));
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
    launch->setSyncState(/*blocks=*/105, /*answered=*/8, /*behind_known_headers=*/false, /*progress=*/1.0);
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

void VaultTests::openingVaultDoesNotReplacePayoutOrMintAddress()
{
    const QString payout_key = QStringLiteral("Mining/PayoutAddress/") + QString::fromLatin1(QStringLiteral("poll-marker-vault").toUtf8().toHex());
    QSettings().remove(payout_key);
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);

    std::unique_ptr<const PlatformStyle> style(PlatformStyle::instantiate("other"));
    OptionsModel options(m_node);
    bilingual_str error;
    QVERIFY(options.Init(error));
    ClientModel client(m_node, &options);
    auto vault = std::make_unique<PollMarkerVault>();
    PollMarkerVault* vault_ptr = vault.get();
    VaultModel model(std::move(vault), m_node, &options, style.get());
    VaultView view(&model, style.get(), nullptr);
    auto* page = view.findChild<MineMintPage*>(QStringLiteral("mineMintPage"));
    QVERIFY(page);
    page->setPayoutAddress(QStringLiteral("hg1quserdraft"));
    const int minted_before = vault_ptr->new_mining_destination_calls;
    view.setClientModel(&client);
    QCOMPARE(page->payoutAddress(), QStringLiteral("hg1quserdraft"));
    QCOMPARE(vault_ptr->new_mining_destination_calls, minted_before);
    view.setClientModel(nullptr);
    QSettings().remove(payout_key);
}

void VaultTests::payoutDraftPersistsPerVaultOnlyOnUserAction()
{
    const QString vault_a = QStringLiteral("f549-payout-%1-a").arg(QCoreApplication::applicationPid());
    const QString vault_b = QStringLiteral("f549-payout-%1-b").arg(QCoreApplication::applicationPid());
    const auto key = [](const QString& name) -> QString {
        return QStringLiteral("Mining/PayoutAddress/") + QString::fromLatin1(name.toUtf8().toHex());
    };
    QSettings().remove(key(vault_a));
    QSettings().remove(key(vault_b));

    MineMintPage page;
    page.setPayoutVault(vault_a);
    QCOMPARE(page.payoutAddress(), QString());
    QVERIFY(!QSettings().contains(key(vault_a))); // attaching must not write
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));
    auto* edit = page.findChild<QLineEdit*>(QStringLiteral("payoutEdit"));
    QVERIFY(edit);
    edit->setFocus();
    QTest::keyClicks(edit, QStringLiteral("hg1quser"));
    QCOMPARE(QSettings().value(key(vault_a)).toString(), QStringLiteral("hg1quser"));

    MineMintPage reopened;
    reopened.setPayoutVault(vault_a);
    QCOMPARE(reopened.payoutAddress(), QStringLiteral("hg1quser"));
    reopened.setStatus(interfaces::MiningStatus{});
    QCOMPARE(QSettings().value(key(vault_a)).toString(), QStringLiteral("hg1quser"));

    MineMintPage other;
    other.setPayoutVault(vault_b);
    QCOMPARE(other.payoutAddress(), QString());
    other.setStatus(interfaces::MiningStatus{});
    QVERIFY(!QSettings().contains(key(vault_b)));

    QTest::keyClick(edit, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(edit, Qt::Key_Backspace);
    QCOMPARE(QSettings().value(key(vault_a)).toString(), QString());
    QSettings().remove(key(vault_a));
    QSettings().remove(key(vault_b));
}

void VaultTests::savedPayoutRequiresConfiguredAddress()
{
    MineMintPage page;
    auto* saved = page.findChild<QPushButton*>(QStringLiteral("savedAddressButton"));
    QVERIFY(saved);
    {
        TestChain100Setup test;
        ScopedNodeContext context(m_node, test.m_node);
        const QString address = QString::fromStdString(m_node.configuredMiningPayoutAddress());
        QCOMPARE(address, QString());
        page.setSavedPayoutAddress(address);
        QVERIFY(saved->isHidden());
    }
    {
        TestChain100Setup test{ChainType::SANDBOX, {.extra_args = {"-minescript=6a"}}};
        ScopedNodeContext context(m_node, test.m_node);
        const QString address = QString::fromStdString(m_node.configuredMiningPayoutAddress());
        QCOMPARE(address, QString());
        page.setSavedPayoutAddress(address);
        QVERIFY(saved->isHidden());
    }
    {
        TestChain100Setup test{ChainType::SANDBOX, {.extra_args = {"-mineaddress=hg1qsaved"}}};
        auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
        test.m_node.vault_loader = vault_loader.get();
        ScopedNodeContext context(m_node, test.m_node);
        const QString address = QString::fromStdString(m_node.configuredMiningPayoutAddress());
        QCOMPARE(address, QStringLiteral("hg1qsaved"));
        page.setSavedPayoutAddress(address);
        QVERIFY(!saved->isHidden());
        saved->click();
        QCOMPARE(page.payoutAddress(), address);

        std::unique_ptr<const PlatformStyle> style(PlatformStyle::instantiate("other"));
        OptionsModel options(m_node);
        bilingual_str error;
        QVERIFY(options.Init(error));
        ClientModel client(m_node, &options);
        VaultModel model(std::make_unique<PollMarkerVault>(), m_node, &options, style.get());
        VaultView view(&model, style.get(), nullptr);
        auto* view_page = view.findChild<MineMintPage*>(QStringLiteral("mineMintPage"));
        QVERIFY(view_page);
        auto* view_saved = view_page->findChild<QPushButton*>(QStringLiteral("savedAddressButton"));
        QVERIFY(view_saved->isHidden());
        view.setClientModel(&client);
        QVERIFY(!view_saved->isHidden());
        view_saved->click();
        QCOMPARE(view_page->payoutAddress(), address);
        view.setClientModel(nullptr);
        QSettings().remove(QStringLiteral("Mining/PayoutAddress/") + QString::fromLatin1(QStringLiteral("poll-marker-vault").toUtf8().toHex()));
    }
}

void VaultTests::reviewConsensusButtonKeepsNaturalWidth()
{
    NetworkPage page;
    page.setPeerCount(1); // the common running state
    page.resize(900, 700);
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));
    auto* panel = page.findChild<QFrame*>(QStringLiteral("networkConnectivityPanel"));
    auto* button = page.findChild<QPushButton*>(QStringLiteral("reviewConsensusButton"));
    QVERIFY(panel && button);
    QVERIFY(button->isVisible());
    // The property F-553 fixed is exactly this: the stretch beside the button leaves it
    // its own width. Half the panel is the symptom's bound, not the fix's -- a button
    // stretched to 440 px of a 900 px panel would pass that and still be the defect.
    QVERIFY2(button->width() == button->sizeHint().width(),
             qPrintable(QStringLiteral("button %1 px, sizeHint %2 px, panel %3 px")
                            .arg(button->width()).arg(button->sizeHint().width()).arg(panel->width())));
    QVERIFY2(button->width() < panel->width() / 2,
             qPrintable(QStringLiteral("button %1 px, panel %2 px").arg(button->width()).arg(panel->width())));
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

void VaultTests::aboutDialogLinksSourceOnce()
{
    HelpMessageDialog dialog(nullptr, /*about=*/true);
    auto* text = dialog.findChild<QTextBrowser*>(QStringLiteral("aboutMessage"));
    QVERIFY(text);
    const QString html = text->toHtml();
    QCOMPARE(html.count(QStringLiteral("<a href=\"https://github.com/quicksilver-hg/quicksilver\">")), 1);
    QVERIFY(html.contains(QStringLiteral("Quicksilver is open source. Source code, contribution guidelines, and further information are at")));
    QVERIFY(!html.contains(QStringLiteral("Please contribute")));
}

void VaultTests::mineMintPageRendersStatus()
{
    MineMintPage page;
    interfaces::MiningStatus st;
    st.active = true;
    st.address = "hg1qexample";
    st.blocks_found = 3;
    st.coins_minted_session = 123456789;
    st.attempts_per_second = 0.29;
    st.graphs_attempted = 7;
    st.congestion_multiplier = 1.0;
    st.gpu_solver = true;
    page.setStatus(st);

    QCOMPARE(page.findChild<QLabel*>("miningStatusValue")->text(), QString("Active"));
    QCOMPARE(page.findChild<QLabel*>("payoutTargetValue")->text(), QString("hg1qexample"));
    QCOMPARE(page.findChild<QLabel*>("solverStatusValue")->text(), QString("Graphics card"));
    QVERIFY(std::ranges::any_of(page.findChildren<QLabel*>(), [](const QLabel* label) { return label->text() == /*W-E5*/ QStringLiteral("Mining on"); }));
    QCOMPARE(page.findChild<QLabel*>("sessionMintedValue")->text(), QStringLiteral("123,456,789🜓"));
    page.setDisplayUnit(QuicksilverUnit::HG);
    QCOMPARE(page.findChild<QLabel*>("sessionMintedValue")->text(), QStringLiteral("1.23456789Hg"));
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
    st.last_solver_error = "No graphics card is in use; install the quicksilver-gpu package, set -cuckatoodevice, or allow processor mining with -allowcpumining";
    page.setStatus(st);
    QCOMPARE(health->text(),
             QStringLiteral("No graphics card is in use. Choose one in Settings > Options > Hardware."));
    QVERIFY(!health->text().contains(QStringLiteral("-cuckatoosolver")));
    // And the control itself, not just its address: this is the only health state
    // the user can act on from this page.
    QPushButton* configure = page.findChild<QPushButton*>("configureSolverButton");
    QVERIFY(configure);
    QVERIFY(configure->isVisibleTo(&page));
    QCOMPARE(configure->text(), /*W-E4*/ QStringLiteral("Open graphics settings"));
    QSignalSpy solver_settings_spy(&page, &MineMintPage::solverSettingsRequested);
    QVERIFY(solver_settings_spy.isValid());
    configure->click();
    QCOMPARE(solver_settings_spy.count(), 1);

    // A solver that ran and failed is a different thing, and its message is the
    // useful one: it names the card, the driver or the log. Keep passing it through.
    st.solver_missing = false;
    st.last_solver_error = "The graphics helper exited with an error; see the debug log";
    page.setStatus(st);
    QCOMPARE(health->text(),
             QStringLiteral("The graphics helper exited with an error; see the debug log"));
    // Nothing on this page fixes a dead card, so the button must go away again
    // rather than offer a setting that is already correct.
    QVERIFY(!configure->isVisibleTo(&page));
}

void VaultTests::solverConfigurationErrorUsesIdentity()
{
    QVERIFY(VaultModel::IsSolverConfigurationError(vault::TxProofNeedsGraphicsCardError()));
    QVERIFY(!VaultModel::IsSolverConfigurationError(_("Transfer proof-of-work canceled")));
    QVERIFY(!VaultModel::IsSolverConfigurationError(_("Could not produce per-tx proof-of-work")));
    QCOMPARE(vault::TxProofNeedsGraphicsCardError().original,
             std::string{"This transfer could not be proved. Quicksilver charges no fees, so every transfer is paid for with proof-of-work, and this network's work needs a graphics card. Choose one in Settings > Options > Hardware and restart. Nothing was sent and your funds are untouched."});
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
    QCOMPARE(idle.solver, QStringLiteral("Processor"));
    QCOMPARE(idle.attempts, QStringLiteral("Idle"));
    QCOMPARE(idle.health, QStringLiteral("Not running"));
    QVERIFY(!idle.show_configure_solver);

    // Stopped after mining: idle, with the graphs it attempted, and no rate.
    QCOMPARE(rows(false, false, true, true, false, 7, std::nullopt, nullptr).attempts, QStringLiteral("Idle (7 graphs attempted)"));
    QCOMPARE(rows(false, false, true, true, false, 1, 0.29, nullptr).attempts, QStringLiteral("Idle (1 graph attempted)"));

    const auto warming = rows(true, true, true, true, false, 0, std::nullopt, nullptr);
    QCOMPARE(warming.block_mining, QStringLiteral("Active"));
    QCOMPARE(warming.solver, QStringLiteral("Graphics card"));
    QCOMPARE(warming.attempts, QStringLiteral("Warming up"));
    QCOMPARE(warming.health, QStringLiteral("Working — no graph finished yet"));
    QVERIFY(!warming.show_configure_solver);

    const auto dead = rows(true, true, true, false, false, 4, 0.29,
                           "The graphics helper exited with an error; see the debug log");
    QCOMPARE(dead.block_mining, QStringLiteral("Active"));
    QCOMPARE(dead.solver, QStringLiteral("Graphics card"));
    QCOMPARE(dead.attempts, QStringLiteral("0.290 (4 graphs attempted)"));
    QCOMPARE(dead.health, QStringLiteral("The graphics helper exited with an error; see the debug log"));
    QVERIFY(!dead.show_configure_solver);

    // Before the first solve attempt the node has already refused the
    // configuration, and solver_ok is still the default. This is the reading
    // that used to say the miner was warming up on the CPU.
    const auto halted = rows(true, false, false, true, false, 0, std::nullopt, nullptr);
    QCOMPARE(halted.block_mining, QStringLiteral("Halted"));
    QCOMPARE(halted.solver, QStringLiteral("Nothing"));
    QCOMPARE(halted.attempts, QStringLiteral("Not solving"));
    QCOMPARE(halted.health, QStringLiteral("No graphics card is in use, and processor block mining is off. Choose a card, or allow processor block mining, in Settings > Options > Hardware."));
    QVERIFY(!halted.health.contains(QStringLiteral("-cuckatoosolver")));
    QVERIFY(!halted.health.contains(QStringLiteral("-allowcpumining")));
    QVERIFY(halted.show_configure_solver);

    // After the attempt that never ran, solver_missing becomes true. Same rows:
    // the page must not fall through to the older "choose a solver" sentence
    // or back to Warming up.
    const auto halted_after = rows(true, false, false, false, true, 0, std::nullopt,
                                   "No graphics card is in use; install the quicksilver-gpu package, set -cuckatoodevice, or allow processor mining with -allowcpumining");
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
void VaultTests::mineMintStartRequiresPayoutAddress()
{
    MineMintPage page;
    auto* start = page.findChild<QPushButton*>(QStringLiteral("startMiningButton"));
    auto* reason = page.findChild<QLabel*>(QStringLiteral("startMiningReason"));
    QVERIFY(start);
    QVERIFY(reason);
    QVERIFY(!start->isEnabled());
    QCOMPARE(reason->text(), QStringLiteral("Enter a payout address, or open a vault and press Use new."));
    page.resize(1366, 768);
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));
    QVERIFY(reason->height() >= reason->heightForWidth(reason->width()));
    QVERIFY(reason->mapTo(&page, QPoint(0, reason->height())).y() <= page.height());

    page.setAddressHelperAvailable(true);
    QVERIFY(!start->isEnabled());
    QCOMPARE(reason->text(), QStringLiteral("Enter a payout address, or press Use new."));

    page.setPayoutAddress(QStringLiteral("hg1qexample"));
    QVERIFY(start->isEnabled());
    QCOMPARE(reason->text(), QString());
    page.setPayoutAddress(QString());
    QVERIFY(!start->isEnabled());
    page.setAddressHelperAvailable(false);
    QCOMPARE(reason->text(), QStringLiteral("Enter a payout address, or open a vault and press Use new."));
}

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
    const QString sentence = QStringLiteral("No graphics card is in use, and processor block mining is off. Choose a card, or allow processor block mining, in Settings > Options > Hardware.");

    const auto cpu = rows(false, true);
    QCOMPARE(cpu.block_mining, QStringLiteral("Idle"));
    QCOMPARE(cpu.solver, QStringLiteral("Processor"));
    QCOMPARE(cpu.attempts, QStringLiteral("Idle"));
    QCOMPARE(cpu.health, QStringLiteral("Not running"));
    QVERIFY(!cpu.show_configure_solver);

    const auto gpu = rows(true, true);
    QCOMPARE(gpu.block_mining, QStringLiteral("Idle"));
    QCOMPARE(gpu.solver, QStringLiteral("Graphics card"));
    QCOMPARE(gpu.attempts, QStringLiteral("Idle"));
    QCOMPARE(gpu.health, QStringLiteral("Not running"));
    QVERIFY(!gpu.show_configure_solver);

    // A graphics helper is configured, so the permit bit is not what the row
    // is allowed to contradict. This input is not one BuildMiningStatus emits
    // while idle; the page must still not call the solver CPU.
    const auto gpu_denied = rows(true, false);
    QCOMPARE(gpu_denied.block_mining, QStringLiteral("Idle"));
    QCOMPARE(gpu_denied.solver, QStringLiteral("Graphics card"));
    QCOMPARE(gpu_denied.attempts, QStringLiteral("Idle"));
    QCOMPARE(gpu_denied.health, QStringLiteral("Not running"));
    QVERIFY(!gpu_denied.show_configure_solver);

    const auto denied = rows(false, false);
    QCOMPARE(denied.block_mining, QStringLiteral("Idle"));
    QCOMPARE(denied.solver, QStringLiteral("Nothing"));
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
    // Still idle: Start is available once a payout address is entered.
    page.setPayoutAddress(QStringLiteral("hg1qexample"));
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

    page.updateStatus(/*peers=*/0, ClientModel::SyncState::Synchronized, /*progress=*/1.0);
    QVERIFY(diagnosis->isVisibleTo(&page));
    // Sandbox ships no fixed seeds at all, which is a different obstacle from the one
    // main and publictest have, and it must not be described as a Tor problem.
    QVERIFY(diagnosis->text().contains(QStringLiteral("no seed addresses")));
    QVERIFY(!diagnosis->text().contains(QStringLiteral("Tor")));
    QVERIFY(peer_button->isEnabled());

    // A peer retires the diagnosis: zero is the only count that needs explaining.
    page.updateStatus(2, ClientModel::SyncState::Synchronized, 1.0);
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
        QVERIFY(color != QuicksilverStyle::Color(Token::Vermilion));
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

    // F-487: the pages follow the same rule as the dialogs, with images on
    // buttons enabled ("other"): a command with words carries no icon.
    const auto words_only = [](QWidget& page, const char* name) {
        QList<QAbstractButton*> buttons;
        for (QPushButton* button : page.findChildren<QPushButton*>()) buttons << button;
        for (QToolButton* button : page.findChildren<QToolButton*>()) buttons << button;
        for (QAbstractButton* button : buttons) {
            if (button->text().isEmpty()) continue;
            QVERIFY2(button->icon().isNull(), qPrintable(QStringLiteral("%1 %2 (%3) carries an icon").arg(QString::fromLatin1(name), button->text(), button->objectName())));
        }
    };
    {
        SendCoinsDialog page(platform_style.data());
        words_only(page, "Transfer");
    }
    {
        ReceiveCoinsDialog page(platform_style.data());
        words_only(page, "Request");
    }
    for (AddressBookPage::Tabs tab : {AddressBookPage::SendingTab, AddressBookPage::ReceivingTab}) {
        for (AddressBookPage::Mode mode : {AddressBookPage::ForSelection, AddressBookPage::ForEditing}) {
            AddressBookPage page(platform_style.data(), mode, tab);
            words_only(page, "Address book");
        }
    }
    {
        RPCConsole page(m_node, platform_style.data(), nullptr);
        words_only(page, "Node window");
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

void VaultTests::networkPageHidesDeveloperNetworksOnMain()
{
    // F-477: on the main network the panel describes that network alone; the
    // developer networks wait behind a checkbox that is not kept between launches.
    NetworkPage page(ChainType::MAIN);
    QVERIFY(page.findChild<QLabel*>("developerNetworkBanner")->isHidden());
    QCOMPARE(page.findChild<QLabel*>("developerNetworkMainCardName")->text(), QString("Quicksilver"));
    QCOMPARE(page.findChild<QLabel*>("developerNetworkMainCardPurpose")->text(),
             QString("The live Quicksilver network. Balances, transfers and mining rewards here are real."));
    QCOMPARE(page.findChild<QLabel*>("developerNetworkMainCardState")->text(), QString("Current"));
    QVERIFY(!page.findChild<QPushButton*>("developerNetworkMainCardButton"));

    QCheckBox* toggle = page.findChild<QCheckBox*>("developerNetworkToggle");
    QWidget* developer_list = page.findChild<QWidget*>("developerNetworkList");
    QVERIFY(toggle);
    QVERIFY(developer_list);
    QCOMPARE(toggle->text(), QString("Show developer networks"));
    QVERIFY(toggle->isEnabled());
    QVERIFY(!toggle->isChecked());
    QVERIFY(developer_list->isHidden());

    toggle->click();
    QVERIFY(!developer_list->isHidden());
    QCOMPARE(page.findChild<QPushButton*>("developerNetworkPublicTestCardButton")->text(), QString("Switch to Public Test"));
    QCOMPARE(page.findChild<QPushButton*>("developerNetworkSandboxCardButton")->text(), QString("Switch to Sandbox"));
    toggle->click();
    QVERIFY(developer_list->isHidden());

    // The command-line wording is gone from every label on the panel.
    for (QLabel* label : page.findChild<QFrame*>("developerNetworkContext")->findChildren<QLabel*>()) {
        QVERIFY2(!label->text().contains(QString("Token")), qPrintable(label->text()));
        QVERIFY2(!label->text().contains(QString("-chain")), qPrintable(label->text()));
        QVERIFY2(!label->text().contains(QString("Datadir")), qPrintable(label->text()));
    }
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
    QCOMPARE(main_restart->toolTip(), QString("Close Quicksilver and reopen it on this network."));
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
    // F-477: no -chain token on the page; the data folder is named in words.
    QVERIFY(!page.findChild<QLabel*>("developerNetworkMainCardToken"));
    QVERIFY(page.findChild<QLabel*>("developerNetworkMainCardDatadir")->text().startsWith(QString("Data folder: ")));
    // Running on the sandbox, the developer networks are shown and cannot be hidden.
    QCheckBox* developer_toggle = page.findChild<QCheckBox*>("developerNetworkToggle");
    QVERIFY(developer_toggle);
    QVERIFY(developer_toggle->isChecked());
    QVERIFY(!developer_toggle->isEnabled());
    QVERIFY(!page.findChild<QWidget*>("developerNetworkList")->isHidden());
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
    QCOMPARE(main_restart->text(), QString("Switch to main network"));
    QCOMPARE(publictest_restart->text(), QString("Switch to Public Test"));
    QVERIFY(main_restart->isEnabled());
    QVERIFY(publictest_restart->isEnabled());
    publictest_restart->click();
    QCOMPARE(restart_spy.count(), 1);
    const QList<QVariant> restart = restart_spy.takeFirst();
    QCOMPARE(restart.at(0).toString(), QString("publictest"));
    QCOMPARE(restart.at(1).toString(), QString("Public Test"));

    page.showInitializing();
    QCOMPARE(page.findChild<QLabel*>("networkStatusValue")->text(), QString("Starting consensus"));
    QCOMPARE(page.findChild<QLabel*>("peerCountValue")->text(), QString("Waiting"));
    QCOMPARE(page.findChild<QLabel*>("syncStatusValue")->text(), QString("Waiting for consensus"));

    page.showStartFailed();
    QCOMPARE(page.findChild<QLabel*>("networkIntro")->text(), QString("Consensus did not start. The saved opt-in was cleared; restart after fixing node settings to try again."));
    QCOMPARE(page.findChild<QLabel*>("networkStatusValue")->text(), QString("Startup failed"));
    QCOMPARE(page.findChild<QLabel*>("peerCountValue")->text(), QString("Unavailable"));
    QCOMPARE(page.findChild<QLabel*>("syncStatusValue")->text(), QString("Not running"));

    page.updateStatus(0, ClientModel::SyncState::CatchingUp, 0.5);
    QCOMPARE(page.findChild<QLabel*>("networkStatusValue")->text(), QString("Offline"));
    QCOMPARE(page.findChild<QLabel*>("peerCountValue")->text(), QString("0"));

    page.updateStatus(8, ClientModel::SyncState::Synchronized, 1.0);
    QCOMPARE(page.findChild<QLabel*>("networkStatusValue")->text(), QString("Online"));
    QCOMPARE(page.findChild<QLabel*>("peerCountValue")->text(), QString("8"));
    QCOMPARE(page.findChild<QLabel*>("syncStatusValue")->text(), QString("Synchronized"));
}

void VaultTests::consensusCancelDoesNotShowStarting()
{
    QSettings().setValue(QStringLiteral("Desktop/ConsensusEnabled"), false);
    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    {
        VaultFrame route(platform_style.get(), nullptr);
        QStackedWidget* stack = route.findChild<QStackedWidget*>(QStringLiteral("vaultFrameStack"));
        QVERIFY(stack);
        route.gotoNetworkPage();
        auto* network = qobject_cast<NetworkPage*>(stack->currentWidget());
        QVERIFY(network);
        auto* review = network->findChild<QPushButton*>(QStringLiteral("reviewConsensusButton"));
        QVERIFY(review);
        QVERIFY(!review->isHidden());
        review->click();
        QCOMPARE(stack->currentWidget()->objectName(), QStringLiteral("consensusReviewPage"));
        QCOMPARE(stack->currentWidget()->findChild<QLabel*>(QStringLiteral("consensusReviewTitle"))->text(), QStringLiteral("Consensus is optional"));
        auto* enable = stack->currentWidget()->findChild<QPushButton*>(QStringLiteral("consensusContinueButton"));
        QVERIFY(enable);
        enable->click();
        QVERIFY(route.consensusEnabled());
        QCOMPARE(stack->currentWidget(), network);
        QVERIFY(!review->isHidden());
        network->showStartFailed();
        QVERIFY(review->isHidden());
        network->showTorMissing();
        QVERIFY(review->isHidden());
        network->showInitializing();
        QVERIFY(!review->isHidden());
        network->setPeerCount(0);
        QVERIFY(!review->isHidden());
        review->click();
        QCOMPARE(stack->currentWidget()->objectName(), QStringLiteral("consensusReviewPage"));
        auto* title = stack->currentWidget()->findChild<QLabel*>(QStringLiteral("consensusReviewTitle"));
        QVERIFY(title);
        QCOMPARE(title->text(), QStringLiteral("Consensus is enabled"));
        auto* back = stack->currentWidget()->findChild<QPushButton*>(QStringLiteral("consensusDeclineButton"));
        auto* status = stack->currentWidget()->findChild<QPushButton*>(QStringLiteral("consensusContinueButton"));
        QVERIFY(back && status);
        QCOMPARE(back->text(), QStringLiteral("Back to home"));
        QCOMPARE(status->text(), QStringLiteral("Show node status"));
        status->click();
        QCOMPARE(stack->currentWidget(), network);
        route.gotoConsensusReviewPage();
        QSignalSpy home_spy(&route, &VaultFrame::homeRequested);
        QVERIFY(home_spy.isValid());
        back->click();
        QCOMPARE(home_spy.count(), 1);
    }
    QSettings().setValue(QStringLiteral("Desktop/ConsensusEnabled"), false);
    VaultFrame frame(platform_style.get(), nullptr);
    QObject::connect(&frame, &VaultFrame::consensusStateChanged, &frame, [&frame](bool enabled) {
        // Models the synchronous "Not now" path in QuicksilverGUI's handover
        // dialog: the opt-in is reverted before the click handler resumes.
        if (enabled) frame.revertConsensusOptIn();
    });

    frame.gotoConsensusReviewPage();
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
    auto* note_edit = send_dialog.findChild<QLineEdit*>(QStringLiteral("transferNote"));
    QVERIFY(note_edit);
    note_edit->setText(QStringLiteral("Retain in this vault"));

    QSignalSpy prepare_finished(&send_dialog, &SendCoinsDialog::sendPreparationFinishedForTesting);
    QString prepared_note;
    QObject::connect(&send_dialog, &SendCoinsDialog::sendPreparationFinishedForTesting,
                     &send_dialog, [&](int, const QString&) { prepared_note = send_dialog.currentTransferNoteForTesting(); });
    QSignalSpy dialog_messages(&send_dialog, &SendCoinsDialog::message);
    QVERIFY(QMetaObject::invokeMethod(&send_dialog, "sendButtonClicked", Q_ARG(bool, false)));
    QVERIFY(prepare_finished.wait(3000));
    QCOMPARE(prepare_finished.count(), 1);
    QCOMPARE(prepared_note, QStringLiteral("Retain in this vault"));
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
        QStringLiteral("URI message")));
    VaultModelTransaction transaction(recipients);
    CMutableTransaction mutable_tx;
    mutable_tx.vout.emplace_back(COIN, GetScriptForDestination(WitnessV0KeyHash(key.GetPubKey())));
    transaction.setWtx(MakeTransactionRef(std::move(mutable_tx)));

    std::vector<QByteArray> broadcast_bytes;
    QObject::connect(&vault_model, &VaultModel::coinsSent, &vault_model,
                     [&](VaultModel*, SendCoinsRecipient, const QByteArray& bytes) { broadcast_bytes.push_back(bytes); });
    const VaultModel::SendCoinsReturn status{vault_model.sendCoins(transaction)};

    QCOMPARE(status.status, VaultModel::TransactionCommitFailed);
    QCOMPARE(status.reason, QStringLiteral("Transaction is invalid by consensus rules: tx-pow-invalid"));
    QCOMPARE(vault_ptr->commit_calls, 1);
    QCOMPARE(vault_ptr->set_address_calls, 0);
    QVERIFY(broadcast_bytes.empty());

    // The same transaction bytes leave the model whether or not the sender
    // keeps a local note. URI messages retain their separate order-form path.
    vault_ptr->commit_error.reset();
    VaultModelTransaction without_note(recipients);
    without_note.setWtx(transaction.getWtx());
    QCOMPARE(vault_model.sendCoins(without_note).status, VaultModel::OK);
    QCOMPARE(vault_ptr->committed_value_maps.size(), size_t{1});
    QVERIFY(vault_ptr->committed_value_maps.back().empty());
    QCOMPARE(vault_ptr->committed_order_forms.back().size(), size_t{1});
    QCOMPARE(vault_ptr->committed_order_forms.back().front().first, std::string("Message"));
    QCOMPARE(vault_ptr->committed_order_forms.back().front().second, std::string("URI message"));

    VaultModelTransaction with_note(recipients);
    with_note.setWtx(transaction.getWtx());
    with_note.setTransferNote(QStringLiteral("Private <note> & reminder"));
    QCOMPARE(vault_model.sendCoins(with_note).status, VaultModel::OK);
    QCOMPARE(vault_ptr->committed_value_maps.back().at("quicksilver_transfer_note"), std::string("Private <note> & reminder"));
    QCOMPARE(vault_ptr->committed_order_forms.back().front().second, std::string("URI message"));
    QCOMPARE(broadcast_bytes.size(), size_t{2});
    QVERIFY2(broadcast_bytes.at(0) == broadcast_bytes.at(1), "serialized transaction bytes changed when only the local transfer note changed");

    interfaces::VaultTx details{};
    details.tx = transaction.getWtx();
    details.credit = COIN;
    details.is_coinbase = false;
    details.value_map.emplace("message", "older metadata");
    TransactionRecord record{details.tx->GetHash(), 0, TransactionRecord::RecvFromOther, "", 0, COIN};
    vault_ptr->details_tx = details;
    const QString old_details = TransactionDesc::toHTML(m_node, vault_model.vault(), &record, QuicksilverUnit::HG);
    QVERIFY(!old_details.contains(QStringLiteral("Transfer Note")));
    details.value_map = vault_ptr->committed_value_maps.back();
    details.value_map.emplace("message", "older metadata");
    vault_ptr->details_tx = details;
    const QString noted_details = TransactionDesc::toHTML(m_node, vault_model.vault(), &record, QuicksilverUnit::HG);
    QVERIFY(noted_details.contains(QStringLiteral("<b>Transfer Note:</b> Private &lt;note&gt; &amp; reminder")));
    QVERIFY(noted_details.contains(QStringLiteral("For your records only — it is not sent with the transfer.")));
    QCOMPARE(noted_details.count(QStringLiteral("<b>Message:</b>")), 2);
    QVERIFY(noted_details.contains(QStringLiteral("older metadata")));
    QVERIFY(noted_details.contains(QStringLiteral("URI message")));
    QVERIFY(noted_details.indexOf(QStringLiteral("<b>Transfer Note:</b>")) < noted_details.indexOf(QStringLiteral("<b>Net amount:</b>")));
    details.value_map.erase("quicksilver_transfer_note");
    vault_ptr->details_tx = details;
    QCOMPARE(TransactionDesc::toHTML(m_node, vault_model.vault(), &record, QuicksilverUnit::HG), old_details);
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

void VaultTests::gpuSituationWordingIsExact()
{
    using gpusituation::SendNoteFor;
    using gpusituation::StartupNoticeFor;
    const QString rx480{QStringLiteral("device backend=opencl id=pci:0000:04:00.0 type=gpu integrated=0 mem=0 status=ok name=AMD Radeon RX 480")};
    const QString rx580{QStringLiteral("device backend=opencl id=pci:0000:05:00.0 type=gpu integrated=0 mem=0 status=ok name=AMD Radeon RX 580")};
    const QString p106{QStringLiteral("device backend=opencl id=pci:0000:01:00.0 type=gpu integrated=0 mem=0 status=compile-failed reason=x name=NVIDIA P106-100")};
    const QString hd{QStringLiteral("device backend=opencl id=pci:0000:00:02.0 type=gpu integrated=1 mem=0 status=ok name=Intel(R) HD Graphics 4600")};
    const QString timing{QStringLiteral("Preparing a transfer usually takes one to two minutes, but the search is random: some transfers finish in seconds and some run past five minutes. You can stop the work at any time.")};
    const QString display{QStringLiteral("Your screen may respond slowly while transfers are being prepared.")};
    const QString fallback{QStringLiteral("Graphics acceleration isn't available, so this computer's processor will prepare transfers. This may take many minutes.")};

    auto discovery = Discover({rx480});
    QCOMPARE(SendNoteFor(discovery, false).text, /*W-B1*/ QStringLiteral("Graphics acceleration is ready (1 card).") + QLatin1Char(' ') + timing + QLatin1Char(' ') + display);
    QVERIFY(!SendNoteFor(discovery, false).processor_fallback);
    QVERIFY(!StartupNoticeFor(discovery, false, {}));
    discovery = Discover({rx480, rx580});
    QCOMPARE(SendNoteFor(discovery, false).text, /*W-B1*/ QStringLiteral("Graphics acceleration is ready (2 cards).") + QLatin1Char(' ') + timing + QLatin1Char(' ') + display);

    discovery = Discover({rx480, p106});
    QCOMPARE(SendNoteFor(discovery, false).text,
             /*W-B2*/ QStringLiteral("Using 1 of 2 graphics cards. NVIDIA P106-100 can't be used: its driver couldn't run Quicksilver's graphics code. See Settings.") + QLatin1Char(' ') + timing + QLatin1Char(' ') + display);

    discovery = Discover({});
    auto notice = StartupNoticeFor(discovery, false, {});
    QVERIFY(notice);
    QCOMPARE(notice->headline, /*W-A3*/ QStringLiteral("No graphics card was found on this computer."));
    QCOMPARE(notice->informative, /*W-A3*/ QStringLiteral("Quicksilver will prepare transfers on this computer's processor. You can stop the work at any time."));
    QVERIFY(!notice->offer_settings);
    QCOMPARE(SendNoteFor(discovery, false).text, fallback);
    QVERIFY(SendNoteFor(discovery, false).processor_fallback);

    notice = StartupNoticeFor(Discover({hd}), false, {});
    QCOMPARE(notice->headline, /*W-A4*/ QStringLiteral("This computer has only built-in graphics."));
    QCOMPARE(notice->informative, /*W-A4*/ QStringLiteral("By default Quicksilver will use the processor. Built-in graphics may be selected in Settings."));
    QVERIFY(notice->offer_settings);

    notice = StartupNoticeFor(Discover({p106}), false, {});
    QCOMPARE(notice->headline, /*W-A5*/ QStringLiteral("Quicksilver is unable to use this computer's graphics card."));
    QCOMPARE(notice->informative, /*W-A5c*/ QStringLiteral("This computer's graphics driver is not compatible with Quicksilver. Updating the driver may fix this."));
    const QString nodriver{QStringLiteral("device backend=opencl id=pci:0000:04:00.0 type=gpu integrated=0 mem=0 status=no-driver reason=no-compiler name=AMD Radeon RX 480")};
    QCOMPARE(StartupNoticeFor(Discover({nodriver}), false, {})->informative,
             /*W-A5a*/ QStringLiteral("Install your card's OpenCL driver. On Ubuntu, AMD cards use the <i>mesa-opencl-icd</i> package, Intel cards use <i>intel-opencl-icd</i>, and NVIDIA cards use NVIDIA's driver."));
    // The message box shows its informative text as rich text only when Qt detects
    // it as such, so the italic package names and command render instead of printing their tags.
    QVERIFY(Qt::mightBeRichText(StartupNoticeFor(Discover({nodriver}), false, {})->informative));
    QCOMPARE(StartupNoticeFor(Discover({nodriver}), true, {})->informative, /*W-A5b*/ QStringLiteral("Install the latest driver from your card's maker."));

    qputenv("QS_PROBE_STUB_EXIT", "4");
    notice = StartupNoticeFor(Discover({}), false, {});
    QCOMPARE(notice->headline, /*W-A5n*/ QStringLiteral("Quicksilver couldn't check this computer's graphics cards, because no OpenCL driver is installed."));
    QCOMPARE(notice->informative, /*W-A5nL*/ QStringLiteral("If this computer has a graphics card and you'd like Quicksilver to use it, install the card's OpenCL driver: <i>mesa-opencl-icd</i> for AMD, <i>intel-opencl-icd</i> for Intel, or NVIDIA's own driver for NVIDIA."));
    QVERIFY(Qt::mightBeRichText(notice->informative));
    QVERIFY(!notice->offer_settings);
    QCOMPARE(StartupNoticeFor(Discover({}), true, {})->informative, /*W-A5nW*/ QStringLiteral("If this computer has a graphics card and you'd like Quicksilver to use it, install the latest driver from the card's maker."));
    qunsetenv("QS_PROBE_STUB_EXIT");

    notice = StartupNoticeFor(Discover({rx480}, "pci:0000:09:00.0"), false, {{QStringLiteral("pci:0000:09:00.0"), QStringLiteral("NVIDIA P106-100")}});
    QCOMPARE(notice->headline, /*W-A6*/ QStringLiteral("The graphics card chosen in Settings, NVIDIA P106-100 (slot 09:00.0), wasn't found."));
    QCOMPARE(notice->informative, /*W-A6*/ QStringLiteral("It may have been removed or moved to another slot. Select a graphics card in Settings."));

    qunsetenv("CUCKATOO_GPU_SOLVER");
    cuckatoo::ResetGpuDiscoveryForTesting();
    const auto none{cuckatoo::DiscoverGpus(19)};
    QCOMPARE(StartupNoticeFor(none, false, {})->headline, /*W-A7L*/ QStringLiteral("Quicksilver's graphics helper isn't installed."));
    QCOMPARE(StartupNoticeFor(none, false, {})->informative, /*W-A7L*/ QStringLiteral("Install it with <i>sudo apt install quicksilver-gpu</i>."));
    QVERIFY(Qt::mightBeRichText(StartupNoticeFor(none, false, {})->informative));
    QCOMPARE(StartupNoticeFor(none, true, {})->informative, /*W-A7W*/ QStringLiteral("Reinstall Quicksilver."));

    discovery = Discover({rx480}, "cpu");
    QCOMPARE(SendNoteFor(discovery, false).text, /*W-B8*/ QStringLiteral("Graphics cards are turned off in Settings, so this computer's processor will prepare transfers. This may take many minutes."));
    QVERIFY(SendNoteFor(discovery, false).processor_fallback);
    QVERIFY(!StartupNoticeFor(discovery, false, {}));

    QCOMPARE(SendNoteFor(std::nullopt, false).text, /*W-B0*/ QStringLiteral("Checking this computer's graphics cards…"));
    QCOMPARE(SendNoteFor(discovery, true).text, /*W-BS*/ QStringLiteral("Sandbox transfers use quick built-in preparation and do not need graphics acceleration."));
    qputenv("QS_PROBE_STUB_EXIT", "1");
    discovery = Discover({rx480});
    QCOMPARE(SendNoteFor(discovery, false).text, /*W-BT*/ QStringLiteral("Quicksilver was unable to detect a graphics card on this computer, so this computer's processor will prepare transfers. This may take many minutes."));
    QVERIFY(!StartupNoticeFor(discovery, false, {}));
    qunsetenv("QS_PROBE_STUB_EXIT");
}

void VaultTests::discoveryRunsOffTheGuiThread()
{
    qputenv("QS_PROBE_STUB_SLEEP_MS", "3000");
    const QString listing{Listing({QStringLiteral("device backend=opencl id=pci:0000:04:00.0 type=gpu integrated=0 mem=0 status=ok name=AMD Radeon RX 480")})};
    qputenv("CUCKATOO_GPU_SOLVER", SolverProbeStubPath().toUtf8());
    qputenv("QS_PROBE_STUB_LISTING", listing.toUtf8());
    qunsetenv("CUCKATOO_GPU_DEVICES");
    cuckatoo::ResetGpuDiscoveryForTesting();
    QObject receiver;
    std::optional<cuckatoo::GpuDiscovery> got;
    QElapsedTimer timer;
    timer.start();
    gpusituation::RunDiscovery(19, &receiver, [&](const cuckatoo::GpuDiscovery& result) { got = result; });
    QVERIFY2(timer.elapsed() < 500, "RunDiscovery blocked its caller");
    QVERIFY(!got);
    QTRY_VERIFY_WITH_TIMEOUT(got.has_value(), 30000);
    QVERIFY(got->situation == cuckatoo::GpuSituation::kReady);
    qunsetenv("QS_PROBE_STUB_SLEEP_MS");
}

void VaultTests::cpuFallbackWarningFollowsDiscovery()
{
    QVERIFY(!gpusituation::CardWillWork(std::nullopt));
    QVERIFY(gpusituation::CardWillWork(Discover({QStringLiteral("device backend=opencl id=pci:0000:04:00.0 type=gpu integrated=0 mem=0 status=ok name=X")})));
    QVERIFY(!gpusituation::CardWillWork(Discover({})));
    QVERIFY(!SendCoinsDialog::cpuFallbackWarningRequiredForTesting(false, false, true));
    QVERIFY(!SendCoinsDialog::cpuFallbackWarningRequiredForTesting(true, true, true));
    QVERIFY(!SendCoinsDialog::cpuFallbackWarningRequiredForTesting(true, false, false));
    QVERIFY(SendCoinsDialog::cpuFallbackWarningRequiredForTesting(true, false, true));

    // F-453: when the processor will take over, the acceleration line is its own
    // line in the warning tone, under the notes, so it is noticed before Review;
    // a note that is not a warning stays with them.
    const SendCoinsDialog::SendWorkNotes fallback = SendCoinsDialog::sendWorkNotesForTesting(Discover({}), false);
    QVERIFY(fallback.processor_fallback);
    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    SendCoinsDialog page(platform_style.get());
    QLabel* notes = page.findChild<QLabel*>(QStringLiteral("sendWorkDisclosureLabel"));
    QLabel* warning = page.findChild<QLabel*>(QStringLiteral("sendWorkAccelerationWarning"));
    QVERIFY(notes);
    QVERIFY(warning);
    QCOMPARE(warning->property("class").toString(), QStringLiteral("benchNote"));
    QCOMPARE(warning->property("benchTone").toString(), QStringLiteral("warn"));
    page.showSendWorkNotesForTesting(fallback);
    QVERIFY(!warning->isHidden());
    QCOMPARE(warning->text(), fallback.acceleration);
    QCOMPARE(notes->text(), fallback.intro);
    QVERIFY(notes->parentWidget() == warning->parentWidget());
    const SendCoinsDialog::SendWorkNotes ready = SendCoinsDialog::sendWorkNotesForTesting(
        Discover({QStringLiteral("device backend=opencl id=pci:0000:04:00.0 type=gpu integrated=0 mem=0 status=ok name=X")}), false);
    QVERIFY(!ready.processor_fallback);
    page.showSendWorkNotesForTesting(ready);
    QVERIFY(warning->isHidden());
    QCOMPARE(notes->text(), ready.intro + QLatin1Char('\n') + ready.acceleration);
}

void VaultTests::faultedCardsShowSendWarningDetails()
{
    const auto discovery = Discover({
        QStringLiteral("device backend=stub id=pci:0000:04:00.0 type=gpu integrated=0 mem=0 status=ok name=First Card"),
        QStringLiteral("device backend=stub id=pci:0000:05:00.0 type=gpu integrated=0 mem=0 status=ok name=Second Card")});
    QVERIFY(gpusituation::CardWillWork(discovery));
    cuckatoo::SetGpuDeviceStatsForTesting({
        {"pci:0000:04:00.0", cuckatoo::GpuDeviceState::kFaulted, cuckatoo::GpuSolveStatus::kDeviceFault},
        {"pci:0000:05:00.0", cuckatoo::GpuDeviceState::kFaulted, cuckatoo::GpuSolveStatus::kTimedOut},
    });
    QVERIFY(!gpusituation::CardWillWork(discovery));
    QVERIFY(SendCoinsDialog::cpuFallbackWarningRequiredForTesting(true, gpusituation::CardWillWork(discovery), true));
    std::unique_ptr<QMessageBox> box{SendCoinsDialog::createCpuFallbackWarning(nullptr, discovery)};
    box->show();
    QVERIFY(box->isVisible());
    QCOMPARE(box->objectName(), QStringLiteral("cpuFallbackSendWarning"));
    const QString expected = QStringLiteral("First Card (slot 04:00.0): %1\nSecond Card (slot 05:00.0): %2")
        .arg(QString::fromStdString(cuckatoo::SolverFault(cuckatoo::GpuSolveStatus::kDeviceFault, false).value()),
             QString::fromStdString(cuckatoo::SolverFault(cuckatoo::GpuSolveStatus::kTimedOut, false).value()));
    QCOMPARE(box->detailedText(), expected);
    box->close();

    cuckatoo::SetGpuDeviceStatsForTesting({
        {"pci:0000:04:00.0", cuckatoo::GpuDeviceState::kFaulted, cuckatoo::GpuSolveStatus::kDeviceFault},
        {"pci:0000:05:00.0", cuckatoo::GpuDeviceState::kSolving, cuckatoo::GpuSolveStatus::kNoCycle},
    });
    QVERIFY(gpusituation::CardWillWork(discovery));
    QVERIFY(!SendCoinsDialog::cpuFallbackWarningRequiredForTesting(true, gpusituation::CardWillWork(discovery), true));
    cuckatoo::ResetGpuDeviceStatsForTesting();
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
    const QDateTime stamp = QDateTime::currentDateTime();
    bare.setKnownBestHeight(1, /*presync=*/false);
    bare.setNumBlocks(1, stamp, 1.0, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
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

//! F-486: coin control paints its item-view checks with the same visible
//! contrast as the dialog's ordinary checks, including a distinct mixed state.
void VaultTests::coinControlIndicatorsMatchDialogCheckboxes()
{
    CoinControlFormSetup setup(m_node);
    if (!setup.ok) return;

    const RestoreApplicationStyle restore;
    QuicksilverStyle::Apply(*qApp);

    vault::CCoinControl coin_control;
    CoinControlDialog dialog(coin_control, setup.model.get(), setup.style.get());
    auto* tree = dialog.findChild<QTreeWidget*>(QStringLiteral("treeWidget"));
    auto* list_mode = dialog.findChild<QRadioButton*>(QStringLiteral("radioListMode"));
    auto* tree_mode = dialog.findChild<QRadioButton*>(QStringLiteral("radioTreeMode"));
    QVERIFY(tree);
    QVERIFY(list_mode);
    QVERIFY(tree_mode);
    dialog.resize(1000, 620);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));

    const auto populate_tree = [&] {
        tree->clear();
        tree->setAlternatingRowColors(false);
        std::array<QTreeWidgetItem*, 3> parents{};
        const std::array<QString, 3> labels{
            QStringLiteral("No outputs selected"),
            QStringLiteral("Some outputs selected"),
            QStringLiteral("All outputs selected"),
        };
        for (int row = 0; row < 3; ++row) {
            auto* parent = new QTreeWidgetItem(tree);
            parent->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
            parent->setText(2, labels[row]);
            for (int child_index = 0; child_index < 2; ++child_index) {
                auto* child = new QTreeWidgetItem(parent);
                child->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
                child->setText(2, QStringLiteral("Output %1").arg(child_index + 1));
                child->setCheckState(0, row == 2 || (row == 1 && child_index == 0) ? Qt::Checked : Qt::Unchecked);
            }
            parent->setExpanded(true);
            parents[row] = parent;
        }
        QCoreApplication::processEvents();
        return parents;
    };

    tree_mode->click();
    QVERIFY(tree_mode->isChecked());
    tree->setEnabled(true);
    const auto parents = populate_tree();
    QCOMPARE(parents[0]->checkState(0), Qt::Unchecked);
    QCOMPARE(parents[1]->checkState(0), Qt::PartiallyChecked);
    QCOMPARE(parents[2]->checkState(0), Qt::Checked);

    QCheckBox reference;
    reference.setTristate(true);
    reference.resize(84, tree->sizeHintForRow(0));
    reference.show();
    QVERIFY(QTest::qWaitForWindowExposed(&reference));

    const auto tree_indicator = [&](QTreeWidgetItem* item) {
        QStyleOptionViewItem option;
        option.initFrom(tree->viewport());
        option.rect = tree->visualItemRect(item);
        option.features = QStyleOptionViewItem::HasCheckIndicator;
        option.checkState = item->checkState(0);
        const QRect rect = tree->style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &option, tree->viewport()).adjusted(-1, -1, 1, 1);
        return tree->viewport()->grab(rect).toImage();
    };
    const auto reference_indicator = [&](Qt::CheckState state) {
        reference.setCheckState(state);
        QCoreApplication::processEvents();
        QStyleOptionButton option;
        option.initFrom(&reference);
        const QRect rect = reference.style()->subElementRect(QStyle::SE_CheckBoxIndicator, &option, &reference);
        return reference.grab(rect).toImage();
    };
    const auto pixel_contrast = [](const QImage& image) {
        int best{0};
        for (int y1 = 0; y1 < image.height(); ++y1) {
            for (int x1 = 0; x1 < image.width(); ++x1) {
                const QColor first = image.pixelColor(x1, y1);
                for (int y2 = 0; y2 < image.height(); ++y2) {
                    for (int x2 = 0; x2 < image.width(); ++x2) {
                        const QColor second = image.pixelColor(x2, y2);
                        best = std::max(best, std::abs(first.red() - second.red()) +
                                                  std::abs(first.green() - second.green()) +
                                                  std::abs(first.blue() - second.blue()));
                    }
                }
            }
        }
        return best;
    };

    std::array<QImage, 3> rendered{};
    const std::array<Qt::CheckState, 3> states{Qt::Unchecked, Qt::PartiallyChecked, Qt::Checked};
    for (size_t i = 0; i < states.size(); ++i) {
        rendered[i] = tree_indicator(parents[i]);
        const QImage expected = reference_indicator(states[i]);
        const int actual_contrast = pixel_contrast(rendered[i]);
        const int expected_contrast = pixel_contrast(expected);
        QVERIFY2(actual_contrast >= expected_contrast,
                 qPrintable(QStringLiteral("state %1: coin-control indicator contrast %2 vs dialog checkbox %3")
                                .arg(static_cast<int>(states[i]))
                                .arg(actual_contrast)
                                .arg(expected_contrast)));
    }
    QVERIFY2(rendered[1] != rendered[2], "partial and checked indicators render the same glyph pixels");

    const QString screenshot_dir = qEnvironmentVariable("QS_F486_SCREENSHOTS");
    if (!screenshot_dir.isEmpty()) {
        QVERIFY(QDir().mkpath(screenshot_dir));
        list_mode->click();
        QVERIFY(list_mode->isChecked());
        tree->clear();
        tree->setAlternatingRowColors(true);
        for (int row = 0; row < 3; ++row) {
            auto* item = new QTreeWidgetItem(tree);
            item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            item->setCheckState(0, row == 1 ? Qt::Checked : Qt::Unchecked);
            item->setText(1, QStringLiteral("%1.00000000 Hg").arg(row + 1));
            item->setText(2, QStringLiteral("List output %1").arg(row + 1));
        }
        QCoreApplication::processEvents();
        QVERIFY(dialog.grab().save(QDir(screenshot_dir).filePath(QStringLiteral("input-selection-list.png"))));

        tree_mode->click();
        QVERIFY(tree_mode->isChecked());
        populate_tree();
        QVERIFY(dialog.grab().save(QDir(screenshot_dir).filePath(QStringLiteral("input-selection-tree.png"))));
    }
}

//! Each copy action reads the stored amount, not the label. The label is a
//! different number on purpose, so a cut-at-the-space or a comma strip still fails.
void VaultTests::copyAmountActionsCopyTheBareNumber()
{
    CoinControlFormSetup setup(m_node);
    if (!setup.ok) return;
    QVERIFY(setup.options.setOption(OptionsModel::DisplayUnit, QVariant::fromValue(QuicksilverUnit::HG)));

    const CAmount one_thousand = 1234 * COIN;
    const CAmount one_thousand_one = 1235 * COIN;
    const CAmount row_amount = 12345 * COIN + COIN / 4;
    const CAmount change_amount = 50 * COIN + COIN / 2;
    const QString misleading = QStringLiteral("9,999.99999999 Hg");
    QStringList failures;

    const auto check = [&](const char* name, const QString& expected) {
        const QString actual = QApplication::clipboard()->text();
        if (actual != expected) {
            failures << QStringLiteral("%1: clipboard \"%2\", expected \"%3\"").arg(QString::fromLatin1(name), actual, expected);
        }
    };

    vault::CCoinControl coin_control;
    CoinControlDialog dialog(coin_control, setup.model.get(), setup.style.get());
    auto* amount = dialog.findChild<QLabel*>(QStringLiteral("labelCoinControlAmount"));
    auto* change = dialog.findChild<QLabel*>(QStringLiteral("labelCoinControlChange"));
    QVERIFY(amount);
    QVERIFY(change);

    amount->setText(misleading);
    amount->setProperty(CoinControlDialog::kRawAmountProperty, static_cast<qlonglong>(one_thousand));
    change->setText(misleading);
    change->setProperty(CoinControlDialog::kRawAmountProperty, static_cast<qlonglong>(one_thousand_one));
    QVERIFY(QMetaObject::invokeMethod(&dialog, "clipboardAmount"));
    check("coin control amount", QStringLiteral("1234.00000000"));
    QVERIFY(QMetaObject::invokeMethod(&dialog, "clipboardChange"));
    check("coin control change", QStringLiteral("1235.00000000"));

    auto* row = new QTreeWidgetItem();
    row->setText(1, QStringLiteral("9,999.99999999"));
    row->setData(1, Qt::UserRole, QVariant(static_cast<qlonglong>(row_amount)));
    dialog.copyContextAmountForTesting(row);
    check("coin control row", QStringLiteral("12345.25000000"));
    delete row;

    SendCoinsDialog transfer(setup.style.get());
    transfer.setModel(setup.model.get());
    auto* transfer_amount = transfer.findChild<QLabel*>(QStringLiteral("labelCoinControlAmount"));
    auto* transfer_change = transfer.findChild<QLabel*>(QStringLiteral("labelCoinControlChange"));
    QVERIFY(transfer_amount);
    QVERIFY(transfer_change);
    transfer_amount->setText(misleading);
    transfer_amount->setProperty(CoinControlDialog::kRawAmountProperty, static_cast<qlonglong>(one_thousand));
    transfer_change->setText(misleading);
    transfer_change->setProperty(CoinControlDialog::kRawAmountProperty, static_cast<qlonglong>(change_amount));
    QVERIFY(QMetaObject::invokeMethod(&transfer, "coinControlClipboardAmount"));
    check("transfer amount", QStringLiteral("1234.00000000"));
    QVERIFY(QMetaObject::invokeMethod(&transfer, "coinControlClipboardChange"));
    check("transfer change", QStringLiteral("50.50000000"));

    QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QLatin1Char('\n'))));
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
    QCOMPARE(first->findChild<QLabel*>(QStringLiteral("recipientLabelKey"))->text(), QStringLiteral("Address Label"));
    QCOMPARE(first->findChild<QLineEdit*>(QStringLiteral("addAsLabel"))->placeholderText(), QStringLiteral("Address Label"));
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
    QCOMPARE(dialog.findChildren<QLineEdit*>(QStringLiteral("transferNote")).size(), 1);
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
    const QString known_label = QStringLiteral("household-budget-02");
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

//! F-493: the label's paint room holds an address-book label even when the
//! font is wider than the one the row was first laid out with. The check is
//! the same sum as transferKeepsTheAddressReadableAndReviewInReach.
void VaultTests::transferLabelFieldHoldsAReferenceLabel()
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

    // Letter spacing widens the reference and leaves the pixel constants
    // (index, Max, spacing) where they are. The bench sheet matches QLineEdit,
    // and Qt then does not carry a parent font down to that field, so the
    // wide face is set on the field itself. That is the FontChange the
    // minimum has to follow.
    QLineEdit* label = page->findChild<QLineEdit*>(QStringLiteral("addAsLabel"));
    QVERIFY(label);
    QFont wide = label->font();
    wide.setLetterSpacing(QFont::AbsoluteSpacing, 4);
    page->setFont(wide);
    label->setFont(wide);
    if (page->layout()) page->layout()->activate();
    QCoreApplication::processEvents();
    const QString known_label = QStringLiteral("household-budget-02");
    label->setText(known_label);
    QCoreApplication::processEvents();
    QStyleOptionFrame label_option;
    label_option.initFrom(label);
    label_option.rect = label->rect();
    label_option.lineWidth = label->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, &label_option, label);
    const int label_room = label->style()->subElementRect(QStyle::SE_LineEditContents, &label_option, label).width() - 2 * 2 - 1;
    const int text_width = label->fontMetrics().horizontalAdvance(known_label);
    QVERIFY2(text_width <= label_room,
             qPrintable(QStringLiteral("label field has %1 px of room for a %2 px label").arg(label_room).arg(text_width)));
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
    for (QAction* action : window.menuBar()->actions()) menus << QString(action->text()).remove(QLatin1Char('&'));
    QVERIFY(menus.contains(QStringLiteral("Settings")));

    const QString rx480{QStringLiteral("device backend=opencl id=pci:0000:04:00.0 type=gpu integrated=0 mem=0 status=ok name=AMD Radeon RX 480")};
    const QString p106{QStringLiteral("device backend=opencl id=pci:0000:01:00.0 type=gpu integrated=0 mem=0 status=compile-failed reason=x name=NVIDIA P106-100")};
    const QString hd{QStringLiteral("device backend=opencl id=pci:0000:00:02.0 type=gpu integrated=1 mem=0 status=ok name=Intel(R) HD Graphics 4600")};
    QStringList texts;
    const auto add = [&texts](const cuckatoo::GpuDiscovery& discovery) {
        texts << gpusituation::SendNoteFor(discovery, false).text;
        for (bool windows : {false, true}) {
            if (const auto notice = gpusituation::StartupNoticeFor(discovery, windows, {{QStringLiteral("pci:0000:09:00.0"), QStringLiteral("NVIDIA P106-100")}})) {
                texts << notice->headline << notice->informative;
            }
        }
    };
    add(Discover({rx480}));
    add(Discover({rx480, p106}));
    add(Discover({}));
    add(Discover({hd}));
    add(Discover({p106}));
    add(Discover({rx480}, "pci:0000:09:00.0"));
    add(Discover({rx480}, "cpu"));
    qputenv("QS_PROBE_STUB_EXIT", "4");
    add(Discover({}));
    qunsetenv("QS_PROBE_STUB_EXIT");
    qunsetenv("CUCKATOO_GPU_SOLVER");
    cuckatoo::ResetGpuDiscoveryForTesting();
    add(cuckatoo::DiscoverGpus(19));
    texts << gpusituation::SendNoteFor(std::nullopt, false).text
          << gpusituation::SendNoteFor(Discover({rx480}), true).text;
    qputenv("QS_PROBE_STUB_EXIT", "1");
    add(Discover({rx480}));
    qunsetenv("QS_PROBE_STUB_EXIT");

    interfaces::MiningStatus mining;
    mining.active = true;
    mining.block_solving_possible = false;
    texts << MineMintPage::statusTextForTesting(mining).health;
    mining.block_solving_possible = true;
    mining.solver_missing = true;
    mining.solver_ok = false;
    texts << MineMintPage::statusTextForTesting(mining).health;
    texts << QString::fromStdString(vault::TxProofNeedsGraphicsCardError().original);

    const QRegularExpression path(QStringLiteral("([A-Za-z]+) (?:>|→) [A-Za-z]+ (?:>|→) [A-Za-z]+"));
    int paths = 0;
    for (const QString& text : texts) {
        auto matches = path.globalMatch(text);
        while (matches.hasNext()) {
            const auto match = matches.next();
            QVERIFY2(menus.contains(match.captured(1)), qPrintable(text));
            QCOMPARE(match.captured(0), QStringLiteral("Settings > Options > Hardware"));
            ++paths;
        }
        QVERIFY2(!text.contains(QStringLiteral("transfer helper")), qPrintable(text));
        QVERIFY2(!text.contains(QStringLiteral("Controls")), qPrintable(text));
    }
    QCOMPARE(paths, 3);
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

void VaultTests::transferHoldsUnlockThroughPreparation()
{
    // F-519: the worker signs, so the unlock must outlive the button handler.
    TransferPrecheckFixture fixture(m_node);
    fixture.recipient(COIN);
    fixture.vault->balance = 2 * COIN;
    QSignalSpy finished(fixture.dialog.get(), &SendCoinsDialog::sendPreparationFinishedForTesting);
    QObject::connect(fixture.model.get(), &VaultModel::requireUnlock, fixture.dialog.get(), [&] {
        fixture.vault->locked = false;
    });
    fixture.vault->create_gate_open = false;
    fixture.transmit();
    fixture.vault->create_gate_open = true;
    QTRY_COMPARE(finished.size(), 1);
    QCOMPARE(fixture.vault->create_calls, 1);
    QVERIFY(!fixture.vault->locked_during_create);
    // Relocked once the preparation is over: the vault was locked before the transfer.
    QTRY_VERIFY(fixture.vault->locked);
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

//! F-497: the way out of a halted mining configuration belongs with the mining
//! commands, not at the far edge of the explanatory health sentence above them.
void VaultTests::mineGraphicsSettingsSitsBesideStop()
{
    MineMintPage page;
    page.resize(1000, 700);
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));

    auto* configure = page.findChild<QPushButton*>(QStringLiteral("configureSolverButton"));
    auto* stop = page.findChild<QPushButton*>(QStringLiteral("stopMiningButton"));
    QVERIFY(configure);
    QVERIFY(stop);

    interfaces::MiningStatus status;
    status.active = true;
    status.gpu_solver = false;
    status.block_solving_possible = false;
    page.setStatus(status);
    QVERIFY(configure->isVisibleTo(&page));

    QHBoxLayout* command_row{nullptr};
    for (QHBoxLayout* layout : page.findChildren<QHBoxLayout*>()) {
        if (layout->indexOf(stop) >= 0) {
            command_row = layout;
            break;
        }
    }
    QVERIFY(command_row);
    QCOMPARE(command_row->indexOf(configure), command_row->indexOf(stop) + 1);
    QCOMPARE(configure->geometry().center().y(), stop->geometry().center().y());
    QVERIFY(configure->geometry().left() > stop->geometry().right());

    status.active = false;
    status.block_solving_possible = true;
    page.setStatus(status);
    QVERIFY(!configure->isVisibleTo(&page));
}

//! F-499: the recipient area is as tall as its rows. It is capped at its own
//! size hint, and the stock hint kept the height the contents had when they
//! were set: a row that grew (the invalid border after Review) or a horizontal
//! bar (a narrow window) was clipped behind a vertical bar.
void VaultTests::transferRecipientAreaFitsItsRow()
{
    const RestoreApplicationStyle restore;
    QuicksilverStyle::Apply(*qApp);
    CoinControlFormSetup setup(m_node);
    if (!setup.ok) return;
    for (const QSize size : {QSize(1600, 900), QSize(1366, 768)}) {
        SendCoinsDialog dialog(setup.style.get());
        dialog.setModel(setup.model.get());
        dialog.resize(size);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        auto* area = dialog.findChild<QScrollArea*>(QStringLiteral("scrollArea"));
        QVERIFY(area);
        const auto rows = [&] {
            QList<SendCoinsEntry*> list;
            for (SendCoinsEntry* entry : area->widget()->findChildren<SendCoinsEntry*>()) {
                if (entry->isVisible()) list << entry;
            }
            return list;
        };
        // A style change reaches the row's layout, then the contents', and so on
        // up, a round of posted LayoutRequests each.
        const auto settle = [] {
            for (int round = 0; round < 16; ++round) QCoreApplication::processEvents();
        };
        // Narrower than the row, the fields may lie past the viewport's sides
        // (the horizontal bar reaches them) but not past its top or bottom.
        const auto fits = [&](const QString& stage, bool across = true) {
            settle();
            const QString where = QStringLiteral("%1x%2 %3").arg(size.width()).arg(size.height()).arg(stage);
            auto* note_key = dialog.findChild<QLabel*>(QStringLiteral("transferNoteKey"));
            auto* note_field = dialog.findChild<QLineEdit*>(QStringLiteral("transferNote"));
            auto* note_hint = dialog.findChild<QLabel*>(QStringLiteral("transferNoteHint"));
            QVERIFY(note_key && note_field && note_hint);
            QCOMPARE(note_key->text(), QStringLiteral("Transfer Note"));
            QCOMPARE(note_hint->text(), QStringLiteral("For your records only — it is not sent with the transfer."));
            for (QWidget* note_widget : {static_cast<QWidget*>(note_key), static_cast<QWidget*>(note_field), static_cast<QWidget*>(note_hint)}) {
                QVERIFY2(note_widget->isVisibleTo(&dialog), qPrintable(where));
                const QRect seen(note_widget->mapTo(&dialog, QPoint{}), note_widget->size());
                QVERIFY2(dialog.rect().contains(seen),
                         qPrintable(QStringLiteral("%1: %2 at %3,%4 %5x%6 outside the Transfer page %7x%8")
                                        .arg(where, note_widget->objectName()).arg(seen.x()).arg(seen.y())
                                        .arg(seen.width()).arg(seen.height()).arg(dialog.width()).arg(dialog.height())));
            }
            QVERIFY2(!rows().isEmpty(), qPrintable(where));
            QVERIFY2(!area->verticalScrollBar()->isVisible(),
                     qPrintable(QStringLiteral("%1: vertical bar shown, viewport %2 px for contents %3 px")
                                    .arg(where).arg(area->viewport()->height()).arg(area->widget()->sizeHint().height())));
            for (SendCoinsEntry* entry : rows()) {
                for (const char* name : {"payAmount", "addAsLabel"}) {
                    QWidget* field = entry->findChild<QWidget*>(QString::fromLatin1(name));
                    QVERIFY2(field, name);
                    const QRect seen(field->mapTo(area->viewport(), QPoint(0, 0)), field->size());
                    const QRect viewport = area->viewport()->rect();
                    const bool inside = across ? viewport.contains(seen) : seen.top() >= viewport.top() && seen.bottom() <= viewport.bottom();
                    QVERIFY2(inside,
                             qPrintable(QStringLiteral("%1: %2 at %3,%4 %5x%6 outside the %7x%8 viewport").arg(where, QString::fromLatin1(name))
                                            .arg(seen.x()).arg(seen.y()).arg(seen.width()).arg(seen.height())
                                            .arg(area->viewport()->width()).arg(area->viewport()->height())));
                }
            }
        };

        fits(QStringLiteral("fresh"));

        // Review with an empty row marks its fields invalid; their border is wider.
        QPushButton* review = dialog.findChild<QPushButton*>(QStringLiteral("sendButton"));
        QVERIFY(review);
        review->click();
        auto* address = rows().at(0)->findChild<QValidatedLineEdit*>(QStringLiteral("payTo"));
        QVERIFY(address);
        QVERIFY(!address->isValid());
        fits(QStringLiteral("after Review"));

        dialog.resize(1366, size.height());
        fits(QStringLiteral("at 1366 px"));

        // Narrower than the row: the horizontal bar takes room from the viewport.
        // The area lays its contents out at least as wide as their minimum, an
        // explicit one if set, else the hint.
        const auto contents_minimum = [&] {
            QWidget* contents = area->widget();
            return qMax(contents->minimumWidth(), contents->minimumSizeHint().width());
        };
        const auto narrower_than_row = [&](const QString& stage) {
            QVERIFY2(area->viewport()->width() < contents_minimum(),
                     qPrintable(QStringLiteral("%1: viewport %2 px is not narrower than the row's %3 px").arg(stage).arg(area->viewport()->width()).arg(contents_minimum())));
            QVERIFY2(area->horizontalScrollBar()->isVisible(),
                     qPrintable(QStringLiteral("%1: no horizontal bar at %2 px").arg(stage).arg(dialog.width())));
            fits(stage, /*across=*/false);
        };
        const int short_by = area->viewport()->width() - area->widget()->minimumSizeHint().width() + 2 * area->fontMetrics().height();
        dialog.resize(qMax(dialog.minimumSizeHint().width(), dialog.width() - short_by), size.height());
        settle();
        if (area->viewport()->width() < contents_minimum()) {
            narrower_than_row(QStringLiteral("narrower than its row"));
        } else {
            // The dialog's own minimum stopped the window first (on Windows,
            // 910 px), so the row still fits it: no bar, every field in view.
            qWarning("Row not narrowed by the window: at the dialog's %d px minimum the viewport is %d px for a %d px row",
                     dialog.width(), area->viewport()->width(), contents_minimum());
            QVERIFY2(!area->horizontalScrollBar()->isVisible(),
                     qPrintable(QStringLiteral("horizontal bar at %1 px with a %2 px viewport for a %3 px row").arg(dialog.width()).arg(area->viewport()->width()).arg(contents_minimum())));
            fits(QStringLiteral("at the dialog's minimum"));
        }
        // On every platform, whatever the window's minimum: a row wider than
        // the viewport, from the contents' side, brings the bar in the same way.
        area->widget()->setMinimumWidth(area->viewport()->width() + 2 * area->fontMetrics().height());
        settle();
        narrower_than_row(QStringLiteral("contents wider than the viewport"));
        area->widget()->setMinimumWidth(0);

        // Enough rows to pass the cap: the area stops growing and scrolls, in
        // a window with room for more, so the cap and not the window stops it.
        // A window the screen holds shorter (on Windows) gets its height in a
        // later resize event, so the height is read once that has arrived.
        const int cap = FittingScrollArea::MAX_LINES * area->fontMetrics().height();
        const int asked = dialog.height() + 2 * cap;
        dialog.resize(size.width(), asked);
        settle();
        const int window_height = dialog.height();
        while (area->widget()->sizeHint().height() <= 2 * cap && rows().size() < 40) {
            dialog.addEntry();
            settle();
        }
        const int capped = area->height();
        // Room for more: past every part's preferred height, the area's at the
        // cap among them, the window has another row's height to give.
        auto* entries = area->widget()->findChild<QVBoxLayout*>(QStringLiteral("entries"));
        QVERIFY(entries);
        const int spare = dialog.height() - dialog.sizeHint().height();
        const int one_more_row = rows().last()->height() + entries->spacing();
        if (spare >= one_more_row) {
            QVERIFY2(capped == area->sizeHint().height(),
                     qPrintable(QStringLiteral("%1x%2 at the cap: area %3 px of its %4 px hint with %5 px spare in the window")
                                    .arg(size.width()).arg(size.height()).arg(capped).arg(area->sizeHint().height()).arg(spare)));
        } else {
            // A screen too short for the cap: the window, not the cap, stops
            // the area. Only the screen may take that room.
            qWarning("Window has no room past the cap: %d px spare for a %d px row in a %d px window, %d px asked",
                     spare, one_more_row, window_height, asked);
            QVERIFY2(window_height < asked,
                     qPrintable(QStringLiteral("%1x%2: %3 px spare for a %4 px row, yet the window got the %5 px it asked")
                                    .arg(size.width()).arg(size.height()).arg(spare).arg(one_more_row).arg(asked)));
        }
        dialog.addEntry();
        settle();
        QVERIFY2(area->height() == capped, qPrintable(QStringLiteral("area grew from %1 to %2 px").arg(capped).arg(area->height())));
        QVERIFY2(area->height() <= cap, qPrintable(QStringLiteral("area %1 px, cap %2 px").arg(area->height()).arg(cap)));
        QVERIFY(area->verticalScrollBar()->isVisible());
        QCOMPARE(dialog.height(), window_height);
    }
}

//! F-501: Clear All leaves one empty row, and the summary counts it.
void VaultTests::transferSummaryCountsTheRowClearAllLeaves()
{
    CoinControlFormSetup setup(m_node);
    if (!setup.ok) return;
    SendCoinsDialog dialog(setup.style.get());
    dialog.setModel(setup.model.get());
    dialog.resize(1200, 800);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    QLabel* recipients = dialog.findChild<QLabel*>(QStringLiteral("transferSummaryRecipients"));
    QVERIFY(recipients);
    QCOMPARE(recipients->text(), QStringLiteral("1"));

    dialog.addEntry();
    QCoreApplication::processEvents();
    QCOMPARE(recipients->text(), QStringLiteral("2"));

    QPushButton* clear = dialog.findChild<QPushButton*>(QStringLiteral("clearButton"));
    QVERIFY(clear);
    clear->click();
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    int shown = 0;
    for (SendCoinsEntry* entry : dialog.findChildren<SendCoinsEntry*>()) shown += entry->isVisible() ? 1 : 0;
    QCOMPARE(shown, 1);
    QCOMPARE(recipients->text(), QStringLiteral("1"));
}

void VaultTests::transactionDescriptionCountsConfirmationsFromFirstBlock()
{
    TestChain100Setup test;
    ScopedNodeContext scoped_context(m_node, test.m_node);
    vault::VaultContext context;
    context.args = &test.m_args;
    context.chain = test.m_node.chain.get();
    std::shared_ptr<vault::CVault> raw_vault = vault::TestLoadVault(context);
    std::unique_ptr<interfaces::Vault> vault;
    // Release the vault before `test` removes its data dir, on every exit path,
    // a failed check included: Windows cannot remove the database file the
    // vault still holds open, and the exception ends the whole run. With a
    // chain the vault is also held by its chain notifications, so a plain
    // WaitForDeleteVault would wait forever; TestUnloadVault drops those first.
    struct VaultRelease {
        std::shared_ptr<vault::CVault>& raw;
        std::unique_ptr<interfaces::Vault>& wrapped;
        ~VaultRelease()
        {
            wrapped.reset();
            vault::TestUnloadVault(std::move(raw));
        }
    } release{raw_vault, vault};

    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vout.emplace_back(COIN, CScript() << OP_TRUE);
    const CTransactionRef transaction = MakeTransactionRef(coinbase);
    raw_vault->AddToVault(transaction, vault::TxStateConfirmed{
        Params().GenesisBlock().GetHash(), /*height=*/0, /*index=*/0});
    vault = interfaces::MakeVault(context, raw_vault);
    TransactionRecord record{transaction->GetHash(), /*time=*/0, TransactionRecord::Generated, "", /*debit=*/0, /*credit=*/COIN};

    QString description;
    for (const auto& [depth, expected] : std::initializer_list<std::pair<int, QString>>{
             {1, QStringLiteral("1 confirmation")},
             {2, QStringLiteral("2 confirmations")},
             {5, QStringLiteral("5 confirmations")},
             {6, QStringLiteral("6 confirmations")},
         }) {
        {
            LOCK(raw_vault->cs_vault);
            raw_vault->SetLastBlockProcessed(depth - 1, Params().GenesisBlock().GetHash());
        }
        description = TransactionDesc::toHTML(m_node, *vault, &record, QuicksilverUnit::HG);
        const QRegularExpressionMatch status_line = QRegularExpression(QStringLiteral("<b>Status:</b> ([^<]+)<br>")).match(description);
        QVERIFY(status_line.hasMatch());
        QCOMPARE(status_line.captured(1), expected);
    }

    QVERIFY(!description.contains(QStringLiteral("broadcast to the network")));
    QVERIFY(description.contains(QStringLiteral("If the block fails to get into the chain")));
}

//! F-459 established Home's ledger fit at 1024x768. On 2026-10-17, F-511
//! moved the supported floor to 1366x768. Type is shown there, the label column
//! stays at least as wide as the reference label, Date, Amount and Status keep
//! their content width, a longer label elides, and the tooltip is that full
//! label. The instruments column gives the label what it lacks, so no note on
//! Home may be shorter than the height its own width needs. All of it holds
//! in each of Home's states: consensus off, consensus on, isolated, and
//! isolated and mining.
void VaultTests::homeLedgerKeepsALabelAtFloor()
{
    const RestoreApplicationStyle restore;
    QuicksilverStyle::Apply(*qApp);
    TestChain100Setup test{ChainType::SANDBOX, {.extra_args = {"-txpownocycle=1"}}};
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);
    // Start with the same labelled reward as homeAndLedgerReadTransactionsAlike.
    // The longer label is chosen after Home has its actual font and width.
    test.CreateAndProcessBlock({}, GetScriptForDestination(WitnessV0KeyHash(test.coinbaseKey.GetPubKey())));
    std::shared_ptr<CVault> vault = SetupDescriptorsVault(m_node, test);
    const QString reference = QStringLiteral("household-budget-02");
    QString long_label = reference;
    {
        LOCK(vault->cs_vault);
        vault->SetAddressBook(CTxDestination{WitnessV0KeyHash(test.coinbaseKey.GetPubKey())}, long_label.toStdString(), vault::AddressPurpose::RECEIVE);
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
    window.resize(1366, 768);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    QAction* home = window.findChild<QAction*>(QStringLiteral("homeBootstrapAction"));
    QVERIFY(home);
    home->trigger();
    QTableView* home_table = window.findChild<QTableView*>(QStringLiteral("homeLedgerTable"));
    QVERIFY(home_table);
    QTRY_VERIFY(home_table->isVisible() && home_table->model() && home_table->model()->rowCount() > 0);
    QCoreApplication::processEvents();

    QAbstractItemModel* rows = home_table->model();
    int labelled = -1;
    for (int row = 0; row < rows->rowCount(); ++row) {
        if (rows->index(row, TransactionTableModel::ToAddress).data(Qt::DisplayRole).toString() == long_label) labelled = row;
    }
    QVERIFY(labelled >= 0);
    const QModelIndex label_index = rows->index(labelled, TransactionTableModel::ToAddress);
    QCOMPARE(label_index.data(Qt::DisplayRole).toString(), long_label);

    const bool real_fonts = !QFontDatabase().families().isEmpty();
    QWidget* page = window.findChild<QWidget*>(QStringLiteral("desktopLaunchPage"));
    QVERIFY(page);
    QWidget* instruments = page->findChild<QWidget*>(QStringLiteral("homeInstruments"));
    QVERIFY(instruments);
    // Home fits the instruments column after each layout, so a resize takes
    // a few rounds of events. Settled is a round that changes nothing; a
    // layout that keeps moving fails here rather than being sampled mid-swing.
    const auto settle = [&] {
        QString last;
        for (int round = 0; round < 20; ++round) {
            QCoreApplication::processEvents();
            QString now = QStringLiteral("%1 %2 %3").arg(instruments->width()).arg(home_table->viewport()->width()).arg(home_table->columnWidth(TransactionTableModel::ToAddress));
            for (QLabel* note : page->findChildren<QLabel*>()) {
                if (note->isVisible() && note->wordWrap()) now += QStringLiteral(" %1x%2").arg(note->width()).arg(note->height());
            }
            if (now == last) return true;
            last = now;
        }
        return false;
    };
    QVERIFY2(settle(), "Home's layout did not settle at 1366x768");
    QFont label_font = home_table->font();
    const QVariant font_role = label_index.data(Qt::FontRole);
    if (font_role.canConvert<QFont>()) label_font = font_role.value<QFont>();
    const QFontMetrics metrics(label_font);
    // A column in this window cannot exceed the window's measured width.
    // Grow the same purpose until even that upper bound cannot hold it.
    const int column_ceiling = window.width();
    while (metrics.horizontalAdvance(long_label) <= column_ceiling) {
        long_label += QStringLiteral("-plus-a-purpose-too-long-for-the-column");
    }
    {
        LOCK(vault->cs_vault);
        QVERIFY(vault->SetAddressBook(CTxDestination{WitnessV0KeyHash(test.coinbaseKey.GetPubKey())}, long_label.toStdString(), vault::AddressPurpose::RECEIVE));
    }
    QTRY_COMPARE(label_index.data(Qt::DisplayRole).toString(), long_label);
    QVERIFY2(settle(), "Home's layout did not settle after the label changed");
    const int long_advance = metrics.horizontalAdvance(long_label);
    const QRect screen = window.screen()->availableGeometry();
    QStringList widths;
    for (int column = 0; column < home_table->horizontalHeader()->count(); ++column) {
        widths << QStringLiteral("%1:%2").arg(column).arg(home_table->columnWidth(column));
    }
    qWarning("F-521 screen=%dx%d window=%dx%d label_advance=%d column_ceiling=%d viewport=%d columns=%s",
             screen.width(), screen.height(), window.width(), window.height(), long_advance,
             column_ceiling, home_table->viewport()->width(), qPrintable(widths.join(QLatin1Char(','))));
    // The first word-wrapped note on Home painted shorter than its own width
    // needs, or empty when none is.
    const auto short_note = [&]() -> QString {
        for (QLabel* note : page->findChildren<QLabel*>()) {
            if (!note->isVisible() || !note->wordWrap()) continue;
            const int need = note->heightForWidth(note->width());
            if (note->height() < need) {
                return QStringLiteral("%1 is %2x%3, needs height %4").arg(note->objectName()).arg(note->width()).arg(note->height()).arg(need);
            }
        }
        return {};
    };
    const auto sized = [&](int width, int height) {
        if (window.width() >= width && window.height() >= height) return true;
        const QRect screen = window.screen()->availableGeometry();
        qWarning("Label fit not checked: below the %dx%d size, window %dx%d on a %dx%d screen",
                 width, height, window.width(), window.height(), screen.width(), screen.height());
        return false;
    };
    if (real_fonts && sized(1366, 768)) {
        QVERIFY2(long_advance > column_ceiling,
                 qPrintable(QStringLiteral("premise: label advance %1 px, column can reach %2 px")
                                .arg(long_advance).arg(column_ceiling)));
        QAbstractItemView* cells = home_table;
        const int content = cells->sizeHintForColumn(TransactionTableModel::ToAddress);
        const int pad = content > long_advance ? content - long_advance : 0;
        const int minimum = metrics.horizontalAdvance(reference) + pad;
        // At 1366x768, on first show and again after the window has been
        // wider: Type shown, Label at its minimum or more, the rest at their
        // content width, nothing past the viewport, and no note clipped.
        const auto check_floor = [&](const QString& where) {
            QVERIFY2(!home_table->isColumnHidden(TransactionTableModel::Type), qPrintable(where + QStringLiteral(": Type is hidden")));
            QVERIFY2(home_table->columnWidth(TransactionTableModel::ToAddress) >= minimum,
                     qPrintable(where + QStringLiteral(": Label %1 px, minimum %2").arg(home_table->columnWidth(TransactionTableModel::ToAddress)).arg(minimum)));
            for (int column : {TransactionTableModel::Date, TransactionTableModel::Amount, TransactionTableModel::Status}) {
                const int need = cells->sizeHintForColumn(column);
                QVERIFY2(home_table->columnWidth(column) >= need,
                         qPrintable(where + QStringLiteral(": Column %1 is %2 px of %3").arg(column).arg(home_table->columnWidth(column)).arg(need)));
            }
            QVERIFY2(home_table->columnWidth(TransactionTableModel::ToAddress) < long_advance,
                     qPrintable(where + QStringLiteral(": Label column %1 px holds the %2 px label")
                                    .arg(home_table->columnWidth(TransactionTableModel::ToAddress))
                                    .arg(long_advance)));
            int shown = 0;
            for (int column = 0; column < home_table->horizontalHeader()->count(); ++column) {
                if (!home_table->isColumnHidden(column)) shown += home_table->columnWidth(column);
            }
            QVERIFY2(shown <= home_table->viewport()->width() && home_table->horizontalScrollBar()->maximum() == 0,
                     qPrintable(where + QStringLiteral(": Columns %1 px in a %2 px viewport, bar range %3")
                                    .arg(shown)
                                    .arg(home_table->viewport()->width())
                                    .arg(home_table->horizontalScrollBar()->maximum())));
            const QString note = short_note();
            QVERIFY2(note.isEmpty(), qPrintable(where + QStringLiteral(": ") + note));
        };
        // Home's states differ in height, and the isolated two scroll the
        // page, whose vertical bar takes ledger width.
        auto* launch = qobject_cast<DesktopLaunchPage*>(page);
        QVERIFY(launch);
        const auto check_states = [&](const QString& where) {
            launch->setConsensusEnabled(false);
            launch->clearMiningStatus();
            QVERIFY2(settle(), qPrintable(where + QStringLiteral(", consensus off: layout did not settle")));
            check_floor(where + QStringLiteral(", consensus off"));
            if (QTest::currentTestFailed()) return;
            launch->setConsensusEnabled(true);
            launch->setPeerCount(8);
            launch->setChainTip(105, QDateTime::currentDateTime());
            launch->setSyncState(105, 8, false, 1.0);
            QVERIFY2(settle(), qPrintable(where + QStringLiteral(", consensus on: layout did not settle")));
            check_floor(where + QStringLiteral(", consensus on"));
            if (QTest::currentTestFailed()) return;
            launch->setPeerCount(0);
            QVERIFY2(settle(), qPrintable(where + QStringLiteral(", isolated: layout did not settle")));
            check_floor(where + QStringLiteral(", isolated"));
            if (QTest::currentTestFailed()) return;
            interfaces::MiningStatus mining;
            mining.active = true;
            mining.attempts_per_second = 1.0;
            launch->setMiningStatus(mining);
            QVERIFY2(settle(), qPrintable(where + QStringLiteral(", isolated and mining: layout did not settle")));
            check_floor(where + QStringLiteral(", isolated and mining"));
            if (QTest::currentTestFailed()) return;
            launch->setConsensusEnabled(false);
            launch->clearMiningStatus();
            QVERIFY2(settle(), qPrintable(where + QStringLiteral(", consensus off again: layout did not settle")));
        };
        check_floor(QStringLiteral("first shown at 1366"));
        if (QTest::currentTestFailed()) return;
        check_states(QStringLiteral("at 1366"));
        if (QTest::currentTestFailed()) return;
        window.resize(1600, 900);
        QVERIFY2(settle(), "Home's layout did not settle at 1600x900");
        if (sized(1600, 900)) {
            QVERIFY(!home_table->isColumnHidden(TransactionTableModel::Type));
            const QString at_1600 = short_note();
            QVERIFY2(at_1600.isEmpty(), qPrintable(at_1600));
        }
        window.resize(1366, 768);
        QVERIFY2(settle(), "Home's layout did not settle back at 1366x768");
        check_floor(QStringLiteral("back at 1366 from 1600x900"));
        if (QTest::currentTestFailed()) return;
        check_states(QStringLiteral("back at 1366 from 1600x900"));
        if (QTest::currentTestFailed()) return;
    }
    QCOMPARE(label_index.data(Qt::ToolTipRole).toString(), long_label);
}

//! F-506: the ID-specific primary-button rules must not mask the shared
//! disabled state used by primary actions.
void VaultTests::idStyledPrimaryButtonsShowDisabledState()
{
    const RestoreApplicationStyle restore;
    QuicksilverStyle::Apply(*qApp);

    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto make_button = [&](const QString& name) {
        auto* button = new QPushButton(QStringLiteral("Action"), &host);
        button->setObjectName(name);
        button->setProperty("class", QStringLiteral("primaryActionButton"));
        button->setFixedSize(180, 48);
        layout->addWidget(button);
        return button;
    };
    QPushButton* reference = make_button(QStringLiteral("disabledPrimaryReference"));
    const std::array<QPushButton*, 4> buttons{
        make_button(QStringLiteral("receiveButton")),
        make_button(QStringLiteral("consensusContinueButton")),
        make_button(QStringLiteral("startMiningButton")),
        make_button(QStringLiteral("agentAllotmentCreateButton")),
    };
    host.show();
    QVERIFY(QTest::qWaitForWindowExposed(&host));

    const auto background = [](QPushButton* button) {
        QCoreApplication::processEvents();
        const QImage image = button->grab().toImage();
        const QPoint sample = QPoint(8, button->height() / 2) * image.devicePixelRatio();
        return image.pixelColor(sample);
    };
    reference->setEnabled(false);
    const QColor disabled_primary = background(reference);
    for (QPushButton* button : buttons) {
        button->setEnabled(true);
        const QColor enabled = background(button);
        button->setEnabled(false);
        const QColor disabled = background(button);
        QVERIFY2(disabled != enabled,
                 qPrintable(QStringLiteral("%1 keeps its enabled background %2 while disabled")
                                .arg(button->objectName(), enabled.name())));
        QCOMPARE(disabled, disabled_primary);
    }
}

//! F-507: an intentionally stopped consensus service is not a network
//! connection attempt.
void VaultTests::statusStripNamesConsensusOff()
{
    QSettings().setValue(QStringLiteral("Desktop/ConsensusEnabled"), false);
    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    QScopedPointer<const NetworkStyle> network_style(NetworkStyle::instantiate(Params().GetChainType()));
    QuicksilverGUI window(m_node, platform_style.get(), network_style.data());

    QLabel* sync = window.findChild<QLabel*>(QStringLiteral("benchStatusSync"));
    QVERIFY(sync);
    QCOMPARE(sync->text(), QStringLiteral("Consensus off"));
    QCOMPARE(sync->property("benchTone").toString(), QStringLiteral("plain"));
    QVERIFY(sync->toolTip().isEmpty());

    VaultFrame* frame = window.findChild<VaultFrame*>(QStringLiteral("vaultFrame"));
    QVERIFY(frame);
    QVERIFY(QMetaObject::invokeMethod(frame, "consensusStateChanged", Q_ARG(bool, true)));
    QVERIFY(sync->text() != QStringLiteral("Consensus off"));
}

void VaultTests::notificationBurstPolicyCoalescesTransactions()
{
    NotificationBurst policy;
    auto decision = policy.record(0);
    QVERIFY(decision.notify_transaction);
    QCOMPARE(decision.summary_count, 0);
    QCOMPARE(policy.takeSummary(NotificationBurst::INTERVAL_MILLIS), 0);

    NotificationBurst fifty;
    QVERIFY(fifty.record(0).notify_transaction);
    for (int i = 1; i < 50; ++i) QVERIFY(!fifty.record(i).notify_transaction);
    QCOMPARE(fifty.takeSummary(NotificationBurst::INTERVAL_MILLIS), 49);

    NotificationBurst spaced;
    for (int i = 0; i < 4; ++i) {
        decision = spaced.record(i * (NotificationBurst::INTERVAL_MILLIS + 1));
        QVERIFY(decision.notify_transaction);
        QCOMPARE(decision.summary_count, 0);
    }

    NotificationBurst sustained;
    int notified = sustained.record(0).notify_transaction ? 1 : 0;
    int summarized = 0;
    for (int second = 1; second <= 30; ++second) {
        const int64_t now = second * 1000;
        if (second % 10 == 0) summarized += sustained.takeSummary(now);
        const auto event = sustained.record(now);
        notified += event.notify_transaction ? 1 : 0;
        summarized += event.summary_count;
    }
    summarized += sustained.takeSummary(40'000);
    QCOMPARE(notified, 1);
    QCOMPARE(summarized, 30);
}

void VaultTests::transactionBurstPostsOneImmediateNotification()
{
    TestChain100Setup test{ChainType::SANDBOX, {.extra_args = {"-txpownocycle=1"}}};
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
        SetMockTime(GetTime() + 1);
    }
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);
    std::shared_ptr<CVault> vault = SetupDescriptorsVault(m_node, test);

    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    QScopedPointer<const NetworkStyle> network_style(NetworkStyle::instantiate(Params().GetChainType()));
    MiniGUI mini_gui(m_node, platform_style.get());
    mini_gui.initModelForVault(m_node, vault, platform_style.get());
    QuicksilverGUI window(m_node, platform_style.get(), network_style.data());
    QCOMPARE(QuicksilverGUI::transactionSummaryTextForTesting(1), QStringLiteral("1 more transaction. See Ledger for details."));
    QCOMPARE(QuicksilverGUI::transactionSummaryTextForTesting(3), QStringLiteral("3 more transactions. See Ledger for details."));
    VaultFrame* frame = window.findChild<VaultFrame*>(QStringLiteral("vaultFrame"));
    QVERIFY(frame);
    auto* view = new VaultView(mini_gui.vaultModel.get(), platform_style.get(), frame);
    QVERIFY(frame->addView(view));
    frame->setCurrentVault(mini_gui.vaultModel.get());
    view->setClientModel(mini_gui.clientModel.get());
    connect(view, &VaultView::incomingTransaction, &window, &QuicksilverGUI::incomingTransaction);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    Notificator* notificator = window.findChild<Notificator*>();
    QVERIFY(notificator);
    qRegisterMetaType<Notificator::Class>("Class");
    QSignalSpy notifications(notificator, &Notificator::notified);
    TransactionTableModel* transactions = mini_gui.vaultModel->getTransactionTableModel();
    QVERIFY(transactions);
    QTRY_VERIFY(!transactions->processingQueuedTransactions());
    const int rows_before = transactions->rowCount({});
    // The burst policy reads the window's clock. Drive it, so the outcome is the
    // same however long this machine takes to connect the blocks (F-515).
    int64_t now_millis{0};
    window.setNotificationClockForTesting([&now_millis] { return now_millis; });
    const auto connect_block = [&](int height) {
        const CBlock block = test.CreateAndProcessBlock({}, GetScriptForDestination(WitnessV0KeyHash(test.coinbaseKey.GetPubKey())));
        const uint256 block_hash{block.GetHash()};
        interfaces::BlockInfo block_info{block_hash};
        block_info.height = height;
        block_info.data = &block;
        block_info.chain_time_max = block.GetBlockTime();
        vault->blockConnected(block_info);
        QCoreApplication::processEvents();
        SetMockTime(GetTime() + 1);
    };
    constexpr int BLOCKS{20};
    constexpr int64_t STEP_MILLIS{100};
    static_assert(BLOCKS * STEP_MILLIS < NotificationBurst::INTERVAL_MILLIS);
    for (int i = 0; i < BLOCKS; ++i) {
        connect_block(106 + i);
        now_millis += STEP_MILLIS;
    }
    QTRY_COMPARE(transactions->rowCount({}), rows_before + BLOCKS);
    QCOMPARE(notifications.count(), 1);
    QCOMPARE(notifications.at(0).at(1).toString(), QStringLiteral("Quicksilver - Incoming transaction"));

    // The first transaction after the window closes posts one summary of the
    // rest of the burst, and no notification of its own.
    now_millis = NotificationBurst::INTERVAL_MILLIS;
    connect_block(106 + BLOCKS);
    QTRY_COMPARE(transactions->rowCount({}), rows_before + BLOCKS + 1);
    QCOMPARE(notifications.count(), 2);
    QCOMPARE(notifications.at(1).at(1).toString(), QStringLiteral("Quicksilver - New transactions"));
    QCOMPARE(notifications.at(1).at(2).toString(), QuicksilverGUI::transactionSummaryTextForTesting(BLOCKS - 1));
}

void VaultTests::homeLedgerBurstCoalescesColumnFits()
{
    TestChain100Setup test{ChainType::SANDBOX, {.extra_args = {"-txpownocycle=1"}}};
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
        SetMockTime(GetTime() + 1);
    }
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    ScopedNodeContext scoped_context(m_node, test.m_node);
    // Both vaults exist before the burst is mined. A model loads history when
    // it is created, and a vault cannot be replayed, so the two windows have
    // to watch the same new blocks arrive.
    std::shared_ptr<CVault> vault_settled = SetupDescriptorsVault(m_node, test);
    std::shared_ptr<CVault> vault_burst = SetupDescriptorsVault(m_node, test);

    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    QScopedPointer<const NetworkStyle> network_style(NetworkStyle::instantiate(Params().GetChainType()));
    MiniGUI mini_settled(m_node, platform_style.get());
    mini_settled.initModelForVault(m_node, vault_settled, platform_style.get());
    MiniGUI mini_burst(m_node, platform_style.get());
    mini_burst.initModelForVault(m_node, vault_burst, platform_style.get());

    QuicksilverGUI settled_window(m_node, platform_style.get(), network_style.data());
    QuicksilverGUI burst_window(m_node, platform_style.get(), network_style.data());
    int64_t settled_now{0};
    int64_t burst_now{0};
    settled_window.setNotificationClockForTesting([&settled_now] { return settled_now; });
    burst_window.setNotificationClockForTesting([&burst_now] { return burst_now; });

    const auto attach = [&](MiniGUI& mini, QuicksilverGUI& window) -> QString {
        VaultFrame* frame = window.findChild<VaultFrame*>(QStringLiteral("vaultFrame"));
        if (!frame) return QStringLiteral("no vault frame");
        auto* view = new VaultView(mini.vaultModel.get(), platform_style.get(), frame);
        if (!frame->addView(view)) return QStringLiteral("addView failed");
        frame->setCurrentVault(mini.vaultModel.get());
        view->setClientModel(mini.clientModel.get());
        connect(view, &VaultView::incomingTransaction, &window, &QuicksilverGUI::incomingTransaction);
        window.resize(1200, 800);
        window.show();
        QAction* home = window.findChild<QAction*>(QStringLiteral("homeBootstrapAction"));
        if (!home) return QStringLiteral("no home action");
        home->trigger();
        return {};
    };
    const QString settled_attached = attach(mini_settled, settled_window);
    QVERIFY2(settled_attached.isEmpty(), qPrintable(settled_attached));
    const QString burst_attached = attach(mini_burst, burst_window);
    QVERIFY2(burst_attached.isEmpty(), qPrintable(burst_attached));
    QVERIFY(QTest::qWaitForWindowExposed(&settled_window));
    QVERIFY(QTest::qWaitForWindowExposed(&burst_window));
    settled_window.resize(1200, 800);
    burst_window.resize(1200, 800);

    TransactionTableModel* settled_tx = mini_settled.vaultModel->getTransactionTableModel();
    TransactionTableModel* burst_tx = mini_burst.vaultModel->getTransactionTableModel();
    QVERIFY(settled_tx && burst_tx);
    QTRY_VERIFY(!settled_tx->processingQueuedTransactions());
    QTRY_VERIFY(!burst_tx->processingQueuedTransactions());
    const int settled_before = settled_tx->rowCount({});
    const int burst_before = burst_tx->rowCount({});
    QCOMPARE(settled_before, burst_before);
    QVERIFY(settled_before > 10);

    constexpr int BLOCKS{200};
    std::vector<CBlock> blocks;
    blocks.reserve(BLOCKS);
    const auto feed = [&](const std::shared_ptr<CVault>& vault, int index) {
        const CBlock& block = blocks[index];
        const uint256 hash{block.GetHash()};
        interfaces::BlockInfo block_info{hash};
        block_info.height = 106 + index;
        block_info.data = &block;
        block_info.chain_time_max = block.GetBlockTime();
        vault->blockConnected(block_info);
    };
    for (int i = 0; i < BLOCKS; ++i) {
        blocks.push_back(test.CreateAndProcessBlock({}, GetScriptForDestination(WitnessV0KeyHash(test.coinbaseKey.GetPubKey()))));
        SetMockTime(GetTime() + 1);
        feed(vault_settled, i);
        settled_now += 1;
        QTRY_COMPARE_WITH_TIMEOUT(settled_tx->rowCount({}), settled_before + i + 1, 30000);
        for (int n = 0; n < 4; ++n) QCoreApplication::processEvents();
    }
    for (int n = 0; n < 8; ++n) QCoreApplication::processEvents();

    struct HomeSnap {
        QStringList hashes;
        QString count;
        QList<int> widths;
        QList<bool> hidden;
    };
    const auto snapOf = [](QuicksilverGUI& window) {
        HomeSnap snap;
        auto* count = window.findChild<QLabel*>(QStringLiteral("homeLedgerCount"));
        auto* table = window.findChild<QTableView*>(QStringLiteral("homeLedgerTable"));
        snap.count = count->text();
        QAbstractItemModel* rows = table->model();
        for (int row = 0; row < rows->rowCount(); ++row) {
            snap.hashes << rows->index(row, 0).data(TransactionTableModel::TxHashRole).toString();
        }
        for (int column = 0; column < table->horizontalHeader()->count(); ++column) {
            snap.widths << table->columnWidth(column);
            snap.hidden << table->isColumnHidden(column);
        }
        return snap;
    };
    const HomeSnap settled_snap = snapOf(settled_window);
    QCOMPARE(settled_snap.hashes.size(), 10);

    auto* burst_page = burst_window.findChild<DesktopLaunchPage*>(QStringLiteral("desktopLaunchPage"));
    QVERIFY(burst_page);
    for (int n = 0; n < 8; ++n) QCoreApplication::processEvents();
    const int fits_before = burst_page->ledgerColumnFitsForTesting();

    QElapsedTimer clock;
    clock.start();
    qint64 last = clock.elapsed();
    qint64 longest = 0;
    QTimer heartbeat;
    heartbeat.setInterval(0);
    QObject::connect(&heartbeat, &QTimer::timeout, [&] {
        const qint64 now = clock.elapsed();
        longest = std::max(longest, now - last);
        last = now;
    });
    heartbeat.start();
    for (int i = 0; i < BLOCKS; ++i) feed(vault_burst, i);
    // blockConnected's own time is not an event-loop stall. The gap that
    // starts here is the drain of the queued inserts.
    last = clock.elapsed();
    QTRY_COMPARE_WITH_TIMEOUT(burst_tx->rowCount({}), burst_before + BLOCKS, 60000);
    for (int n = 0; n < 12; ++n) QCoreApplication::processEvents();
    heartbeat.stop();

    const int fits = burst_page->ledgerColumnFitsForTesting() - fits_before;
    qWarning("F-512 burst fits=%d longest_event_loop_stall_ms=%lld", fits, static_cast<long long>(longest));
    QVERIFY2(fits >= 1 && fits <= 6, qPrintable(QStringLiteral("fits=%1").arg(fits)));

    const HomeSnap burst_snap = snapOf(burst_window);
    QCOMPARE(burst_snap.hashes, settled_snap.hashes);
    QCOMPARE(burst_snap.count, settled_snap.count);
    QCOMPARE(burst_snap.hidden, settled_snap.hidden);
    QCOMPARE(burst_snap.widths, settled_snap.widths);
}
