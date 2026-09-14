// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/vaultmodel.h>

#include <qt/addresstablemodel.h>
#include <qt/clientmodel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/paymentserver.h>
#include <qt/recentrequeststablemodel.h>
#include <qt/sendcoinsdialog.h>
#include <qt/transactiontablemodel.h>

#include <addresstype.h>
#include <common/args.h> // for GetBoolArg
#include <common/messages.h>
#include <interfaces/handler.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <node/interface_ui.h>
#include <node/types.h>
#include <psqt.h>
#include <util/translation.h>
#include <vault/coincontrol.h>
#include <vault/vault.h> // for CRecipient

#include <cassert>
#include <functional>
#include <memory>
#include <stdint.h>

#include <QDebug>
#include <QMessageBox>
#include <QSet>
#include <QTimer>

using common::TransactionErrorString;
using vault::CCoinControl;
using vault::CRecipient;
using vault::DEFAULT_DISABLE_VAULT;

VaultModel::VaultModel(std::unique_ptr<interfaces::Vault> vault, ClientModel& client_model, const PlatformStyle* platformStyle, QObject* parent) : VaultModel(std::move(vault), client_model.node(), client_model.getOptionsModel(), platformStyle, parent)
{
    setClientModel(&client_model);
}

VaultModel::VaultModel(std::unique_ptr<interfaces::Vault> vault, interfaces::Node& node, OptionsModel* options_model, const PlatformStyle* platformStyle, QObject* parent) : QObject(parent),
                                                                                                                                                                             m_vault(std::move(vault)),
                                                                                                                                                                             m_client_model(nullptr),
                                                                                                                                                                             m_node(node),
                                                                                                                                                                             optionsModel(options_model),
                                                                                                                                                                             timer(new QTimer(this))
{
    addressTableModel = new AddressTableModel(this);
    transactionTableModel = new TransactionTableModel(platformStyle, this);
    recentRequestsTableModel = new RecentRequestsTableModel(this);

    subscribeToCoreSignals();
}

VaultModel::~VaultModel()
{
    unsubscribeFromCoreSignals();
}

void VaultModel::startPollBalance()
{
    // Update the cached balance right away, so every view can make use of it,
    // so them don't need to waste resources recalculating it.
    pollBalanceChanged();

    // This timer will be fired repeatedly to update the balance
    // Since the QTimer::timeout is a private signal, it cannot be used
    // in the GUIUtil::ExceptionSafeConnect directly.
    connect(timer, &QTimer::timeout, this, &VaultModel::timerTimeout);
    GUIUtil::ExceptionSafeConnect(this, &VaultModel::timerTimeout, this, &VaultModel::pollBalanceChanged);
    timer->start(MODEL_UPDATE_DELAY);
}

void VaultModel::setClientModel(ClientModel* client_model)
{
    m_client_model = client_model;
}

void VaultModel::updateStatus()
{
    EncryptionStatus newEncryptionStatus = getEncryptionStatus();

    if (cachedEncryptionStatus != newEncryptionStatus) {
        cachedEncryptionStatus = newEncryptionStatus;
        Q_EMIT encryptionStatusChanged();
    }
}

void VaultModel::pollBalanceChanged()
{
    uint256 update_tip;
    if (!m_vault->tryGetBalanceUpdateBlockHash(update_tip)) {
        return;
    }

    // Avoid recomputing vault balances unless a TransactionChanged or
    // vault-tracked block tip notification was received.
    if (!fForceCheckBalanceChanged && m_cached_last_update_tip == update_tip) return;

    // Try to get balances and return early if locks can't be acquired. This
    // avoids the GUI from getting stuck on periodical polls if the core is
    // holding the locks for a longer time - for example, during a vault
    // rescan.
    interfaces::VaultBalances new_balances;
    uint256 block_hash;
    if (!m_vault->tryGetBalances(new_balances, block_hash)) {
        return;
    }

    if (fForceCheckBalanceChanged || block_hash != m_cached_last_update_tip) {
        fForceCheckBalanceChanged = false;

        // Balance and number of transactions might have changed
        m_cached_last_update_tip = block_hash;

        checkBalanceChanged(new_balances);
        if (transactionTableModel)
            transactionTableModel->updateConfirmations();
    }
}

void VaultModel::checkBalanceChanged(const interfaces::VaultBalances& new_balances)
{
    if (new_balances.balanceChanged(m_cached_balances)) {
        m_cached_balances = new_balances;
        Q_EMIT balanceChanged(new_balances);
    }
}

interfaces::VaultBalances VaultModel::getCachedBalance() const
{
    return m_cached_balances;
}

void VaultModel::updateTransaction()
{
    // Balance and number of transactions might have changed
    fForceCheckBalanceChanged = true;
}

void VaultModel::updateAddressBook(const QString& address, const QString& label,
                                   bool isMine, vault::AddressPurpose purpose, int status)
{
    if (addressTableModel)
        addressTableModel->updateEntry(address, label, isMine, purpose, status);
}

bool VaultModel::validateAddress(const QString& address) const
{
    return IsValidDestinationString(address.toStdString());
}

VaultModel::SendCoinsReturn VaultModel::prepareTransaction(VaultModelTransaction& transaction, const CCoinControl& coinControl, const std::function<void(uint32_t nonce)>& tx_proof_progress)
{
    CAmount total = 0;
    QList<SendCoinsRecipient> recipients = transaction.getRecipients();
    std::vector<CRecipient> vecSend;

    if (recipients.empty()) {
        return OK;
    }

    QSet<QString> setAddress; // Used to detect duplicates
    int nAddresses = 0;

    // Pre-check input data for validity
    for (const SendCoinsRecipient& rcp : recipients) {
        { // User-entered Quicksilver address / amount:
            if (!validateAddress(rcp.address)) {
                return InvalidAddress;
            }
            if (rcp.amount <= 0) {
                return InvalidAmount;
            }
            setAddress.insert(rcp.address);
            ++nAddresses;

            CRecipient recipient{DecodeDestination(rcp.address.toStdString()), rcp.amount};
            vecSend.push_back(recipient);

            total += rcp.amount;
        }
    }
    if (setAddress.size() != nAddresses) {
        return DuplicateAddress;
    }

    // This method runs on the send worker. Wait out ordinary cs_vault contention
    // instead of using the GUI's nonblocking readiness probe and misreporting a
    // busy vault as missing chain/header state.
    if (!m_vault->canCreateTransactions()) {
        return VaultSyncUnavailable;
    }

    // Preparation runs on a worker thread. The cached balance belongs to the GUI
    // thread and is refreshed by its timer, so reading it here would race that
    // refresh and could also make a send decision from stale state. Ask the vault
    // for an authoritative balance under its own lock instead.
    CAmount nBalance = vault().getAvailableBalance(coinControl);

    if (total > nBalance) {
        return AmountExceedsBalance;
    }

    try {
        int nChangePosRet = -1;

        auto& newTx = transaction.getWtx();
        // Marked in flight for the whole call, not merely the grind: coin selection
        // and signing hold cs_vault too, and an unload is no safer during those.
        ProofOfWorkScope proof_scope(*this);
        const auto& res = m_vault->createTransaction(vecSend, coinControl, /*sign=*/!vault().privateKeysDisabled(), nChangePosRet, tx_proof_progress,
                                                     [this] { return proofOfWorkCancelRequested(); });
        newTx = res ? *res : nullptr;

        if (!newTx) {
            const bilingual_str error = util::ErrorString(res);
            return SendCoinsReturn(TransactionCreationFailed,
                                   QString::fromStdString(error.translated),
                                   error.original.find("GPU solver") != std::string::npos);
        }

    } catch (const std::runtime_error& err) {
        // Something unexpected happened, instruct user to report this bug.
        return SendCoinsReturn(TransactionCreationFailed, QString::fromStdString(err.what()));
    }

    return SendCoinsReturn(OK);
}

VaultModel::SendCoinsReturn VaultModel::sendCoins(VaultModelTransaction& transaction)
{
    QByteArray transaction_array; /* store serialized transaction */

    {
        std::vector<std::pair<std::string, std::string>> vOrderForm;
        for (const SendCoinsRecipient& rcp : transaction.getRecipients()) {
            if (!rcp.message.isEmpty()) // Message from normal quicksilver:URI (quicksilver:123...?message=example)
                vOrderForm.emplace_back("Message", rcp.message.toStdString());
        }

        auto& newTx = transaction.getWtx();
        const auto committed{vault().commitTransaction(newTx, /*value_map=*/{}, std::move(vOrderForm))};
        if (!committed) {
            return SendCoinsReturn(TransactionCommitFailed, QString::fromStdString(util::ErrorString(committed).translated));
        }

        DataStream ssTx;
        ssTx << TX_WITH_WITNESS(*newTx);
        transaction_array.append((const char*)ssTx.data(), ssTx.size());
    }

    // Add addresses / update labels that we've sent to the address book,
    // and emit coinsSent signal for each recipient
    for (const SendCoinsRecipient& rcp : transaction.getRecipients()) {
        {
            std::string strAddress = rcp.address.toStdString();
            CTxDestination dest = DecodeDestination(strAddress);
            std::string strLabel = rcp.label.toStdString();
            {
                // Check if we have a new address or an updated label
                std::string name;
                if (!m_vault->getAddress(
                        dest, &name, /* is_mine= */ nullptr, /* purpose= */ nullptr)) {
                    m_vault->setAddressBook(dest, strLabel, vault::AddressPurpose::SEND);
                } else if (name != strLabel) {
                    m_vault->setAddressBook(dest, strLabel, {}); // {} means don't change purpose
                }
            }
        }
        Q_EMIT coinsSent(this, rcp, transaction_array);
    }

    checkBalanceChanged(m_vault->getBalances()); // update balance immediately, otherwise there could be a short noticeable delay until pollBalanceChanged hits
    return SendCoinsReturn(OK);
}

OptionsModel* VaultModel::getOptionsModel() const
{
    return optionsModel;
}

ClientModel& VaultModel::clientModel() const
{
    assert(m_client_model);
    return *m_client_model;
}

AddressTableModel* VaultModel::getAddressTableModel() const
{
    return addressTableModel;
}

TransactionTableModel* VaultModel::getTransactionTableModel() const
{
    return transactionTableModel;
}

RecentRequestsTableModel* VaultModel::getRecentRequestsTableModel() const
{
    return recentRequestsTableModel;
}

VaultModel::EncryptionStatus VaultModel::getEncryptionStatus() const
{
    if (!m_vault->isCrypted()) {
        // A key-disabled vault has nothing to encrypt, so report that rather than
        // implying the vault is merely left unencrypted.
        if (m_vault->privateKeysDisabled()) {
            return NoKeys;
        }
        return Unencrypted;
    } else if (m_vault->isLocked()) {
        return Locked;
    } else {
        return Unlocked;
    }
}

bool VaultModel::setVaultEncrypted(const SecureString& passphrase)
{
    return m_vault->encryptVault(passphrase);
}

bool VaultModel::setVaultLocked(bool locked, const SecureString& passPhrase)
{
    if (locked) {
        // Lock
        return m_vault->lock();
    } else {
        // Unlock
        return m_vault->unlock(passPhrase);
    }
}

bool VaultModel::changePassphrase(const SecureString& oldPass, const SecureString& newPass)
{
    m_vault->lock(); // Make sure vault is locked before attempting pass change
    return m_vault->changeVaultPassphrase(oldPass, newPass);
}

// Handlers for core signals
static void NotifyUnload(VaultModel* vaultModel)
{
    qDebug() << "NotifyUnload";
    bool invoked = QMetaObject::invokeMethod(vaultModel, "unload");
    assert(invoked);
}

static void NotifyKeyStoreStatusChanged(VaultModel* vaultmodel)
{
    qDebug() << "NotifyKeyStoreStatusChanged";
    bool invoked = QMetaObject::invokeMethod(vaultmodel, "updateStatus", Qt::QueuedConnection);
    assert(invoked);
}

static void NotifyAddressBookChanged(VaultModel* vaultmodel,
                                     const CTxDestination& address, const std::string& label, bool isMine,
                                     vault::AddressPurpose purpose, ChangeType status)
{
    QString strAddress = QString::fromStdString(EncodeDestination(address));
    QString strLabel = QString::fromStdString(label);

    qDebug() << "NotifyAddressBookChanged: " + strAddress + " " + strLabel + " isMine=" + QString::number(isMine) + " purpose=" + QString::number(static_cast<uint8_t>(purpose)) + " status=" + QString::number(status);
    bool invoked = QMetaObject::invokeMethod(vaultmodel, "updateAddressBook",
                                             Q_ARG(QString, strAddress),
                                             Q_ARG(QString, strLabel),
                                             Q_ARG(bool, isMine),
                                             Q_ARG(vault::AddressPurpose, purpose),
                                             Q_ARG(int, status));
    assert(invoked);
}

static void NotifyTransactionChanged(VaultModel* vaultmodel, const uint256& hash, ChangeType status)
{
    Q_UNUSED(hash);
    Q_UNUSED(status);
    bool invoked = QMetaObject::invokeMethod(vaultmodel, "updateTransaction", Qt::QueuedConnection);
    assert(invoked);
}

static void ShowProgress(VaultModel* vaultmodel, const std::string& title, int nProgress)
{
    // emits signal "showProgress"
    bool invoked = QMetaObject::invokeMethod(vaultmodel, "showProgress", Qt::QueuedConnection,
                                             Q_ARG(QString, QString::fromStdString(title)),
                                             Q_ARG(int, nProgress));
    assert(invoked);
}

static void NotifyCanGetAddressesChanged(VaultModel* vaultmodel)
{
    bool invoked = QMetaObject::invokeMethod(vaultmodel, "canGetAddressesChanged");
    assert(invoked);
}

void VaultModel::subscribeToCoreSignals()
{
    // Connect signals to vault
    m_handler_unload = m_vault->handleUnload(std::bind(&NotifyUnload, this));
    m_handler_status_changed = m_vault->handleStatusChanged(std::bind(&NotifyKeyStoreStatusChanged, this));
    m_handler_address_book_changed = m_vault->handleAddressBookChanged(std::bind(NotifyAddressBookChanged, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));
    m_handler_transaction_changed = m_vault->handleTransactionChanged(std::bind(NotifyTransactionChanged, this, std::placeholders::_1, std::placeholders::_2));
    m_handler_show_progress = m_vault->handleShowProgress(std::bind(ShowProgress, this, std::placeholders::_1, std::placeholders::_2));
    m_handler_can_get_addrs_changed = m_vault->handleCanGetAddressesChanged(std::bind(NotifyCanGetAddressesChanged, this));
}

void VaultModel::unsubscribeFromCoreSignals()
{
    // Disconnect signals from vault
    m_handler_unload->disconnect();
    m_handler_status_changed->disconnect();
    m_handler_address_book_changed->disconnect();
    m_handler_transaction_changed->disconnect();
    m_handler_show_progress->disconnect();
    m_handler_can_get_addrs_changed->disconnect();
}

// VaultModel::UnlockContext implementation
void VaultModel::requestUnlock(std::function<void(std::shared_ptr<UnlockContext>)> done)
{
    // Bugs in earlier versions may have resulted in vaults with private keys disabled to become "encrypted"
    // (encryption keys are present, but not actually doing anything).
    // To avoid issues with such vaults, check if the vault has private keys disabled, and if so, return a context
    // that indicates the vault is not encrypted.
    if (m_vault->privateKeysDisabled()) {
        done(std::make_shared<UnlockContext>(this, /*valid=*/true, /*relock=*/false));
        return;
    }
    const bool was_locked = getEncryptionStatus() == Locked;
    if (!was_locked) {
        done(std::make_shared<UnlockContext>(this, /*valid=*/true, /*relock=*/false));
        return;
    }
    if (m_pending_unlock) {
        done(std::make_shared<UnlockContext>(this, /*valid=*/false, /*relock=*/false));
        return;
    }
    m_unlock_was_locked = true;
    m_unlock_dialog_shown = false;
    m_pending_unlock = std::move(done);
    Q_EMIT requireUnlock();
    if (!m_unlock_dialog_shown) {
        completePendingUnlock();
    }
}

void VaultModel::notifyUnlockDialogShown()
{
    m_unlock_dialog_shown = true;
}

void VaultModel::completePendingUnlock()
{
    if (!m_pending_unlock) return;
    auto done = std::move(m_pending_unlock);
    m_unlock_dialog_shown = false;
    const bool valid = getEncryptionStatus() != Locked;
    done(std::make_shared<UnlockContext>(this, valid, m_unlock_was_locked));
}

VaultModel::UnlockContext::UnlockContext(VaultModel* _vault, bool _valid, bool _relock) : vault(_vault),
                                                                                          valid(_valid),
                                                                                          relock(_relock)
{
}

VaultModel::UnlockContext::~UnlockContext()
{
    if (valid && relock) {
        vault->setVaultLocked(true);
    }
}

VaultModel::ProofOfWorkScope::ProofOfWorkScope(VaultModel& model) : m_model(model)
{
    // Clearing on the first grind rather than on the last one to end means a cancel
    // requested while nothing was running cannot poison the transfer that follows.
    if (m_model.m_proofs_in_flight.fetch_add(1) == 0) {
        m_model.m_proof_cancel_requested = false;
    }
}

VaultModel::ProofOfWorkScope::~ProofOfWorkScope()
{
    m_model.m_proofs_in_flight.fetch_sub(1);
}

void VaultModel::displayAddress(std::string sAddress) const
{
    CTxDestination dest = DecodeDestination(sAddress);
    try {
        util::Result<void> result = m_vault->displayAddress(dest);
        if (!result) {
            auto* box = new QMessageBox(QMessageBox::Warning, tr("Signer error"),
                QString::fromStdString(util::ErrorString(result).translated), QMessageBox::Ok, nullptr);
            box->setObjectName(QStringLiteral("signerError"));
            GUIUtil::ShowModalDialogAsynchronously(box);
        }
    } catch (const std::runtime_error& e) {
        auto* box = new QMessageBox(QMessageBox::Critical, tr("Can't display address"),
            e.what(), QMessageBox::Ok, nullptr);
        box->setObjectName(QStringLiteral("displayAddressFailed"));
        GUIUtil::ShowModalDialogAsynchronously(box);
    }
}

bool VaultModel::isVaultEnabled()
{
    return !gArgs.GetBoolArg("-disablevault", DEFAULT_DISABLE_VAULT);
}

QString VaultModel::getVaultName() const
{
    return QString::fromStdString(m_vault->getVaultName());
}

QString VaultModel::getDisplayName() const
{
    return GUIUtil::VaultDisplayName(getVaultName());
}

bool VaultModel::backupRecorded() const
{
    return m_vault->isBackupRecorded();
}

bool VaultModel::setBackupRecorded(bool recorded)
{
    return m_vault->setBackupRecorded(recorded);
}

util::Result<vault::AgentAllotmentRecord> VaultModel::recordAgentAllotmentSetup(const QString& label, CAmount funding_limit, CAmount daily_limit)
{
    return m_vault->recordAgentAllotmentSetup(label.toStdString(), funding_limit, daily_limit);
}

std::vector<vault::AgentAllotmentRecord> VaultModel::listAgentAllotmentRecords() const
{
    return m_vault->listAgentAllotmentRecords();
}

QString VaultModel::agentAllotmentPolicyRequest(const vault::AgentAllotmentRecord& record, CAmount funding_available) const
{
    return QString::fromStdString(m_vault->agentAllotmentPolicyRequest(record, funding_available));
}

util::Result<vault::AgentAllotmentPolicyRequestMetadata> VaultModel::validateAgentAllotmentPolicyRequest(const QString& request_json) const
{
    return m_vault->validateAgentAllotmentPolicyRequest(request_json.toStdString());
}

util::Result<vault::AgentAllotmentPolicyBundle> VaultModel::agentAllotmentPolicyBundle(const QString& request_json) const
{
    return m_vault->agentAllotmentPolicyBundle(request_json.toStdString());
}

CAmount VaultModel::agentAllotmentFundingAvailable(const vault::AgentAllotmentRecord& record) const
{
    if (record.funding_address.empty()) return 0;

    const CTxDestination dest = DecodeDestination(record.funding_address);
    if (!IsValidDestination(dest)) return 0;

    interfaces::Vault::CoinsList coins_by_address;
    if (!tryListCoins(coins_by_address)) return 0;

    const auto coins_it = coins_by_address.find(dest);
    if (coins_it == coins_by_address.end()) return 0;

    CAmount available{0};
    for (const auto& coin : coins_it->second) {
        const auto& txout = std::get<1>(coin);
        if (!txout.is_spent && MoneyRange(txout.txout.nValue) && MoneyRange(available + txout.txout.nValue)) {
            available += txout.txout.nValue;
        }
    }
    return available;
}

VaultModel::AgentSignedSpendBroadcastResult VaultModel::broadcastAgentAllotmentSignedSpend(CTransactionRef tx) const
{
    if (!tx) {
        return {.accepted = false, .txid = {}, .error = tr("Signed spend is empty.")};
    }
    if (!m_client_model) {
        return {.accepted = false, .txid = QString::fromStdString(tx->GetHash().ToString()), .error = tr("Start consensus before submitting a signed agent spend.")};
    }

    const QString txid{QString::fromStdString(tx->GetHash().ToString())};
    std::string err_string;
    const node::TransactionError error{m_node.broadcastTransaction(std::move(tx), err_string)};
    if (error == node::TransactionError::OK) {
        return {.accepted = true, .txid = txid, .error = {}};
    }

    QString error_text = QString::fromStdString(TransactionErrorString(error).translated);
    if (!err_string.empty()) {
        error_text += QStringLiteral(": ") + QString::fromStdString(err_string);
    }
    return {.accepted = false, .txid = txid, .error = error_text};
}

bool VaultModel::tryListCoins(interfaces::Vault::CoinsList& coins) const
{
    return m_vault->tryListCoins(coins);
}

bool VaultModel::tryGetCoins(const std::vector<COutPoint>& outputs, std::vector<interfaces::VaultTxOut>& coins) const
{
    return m_vault->tryGetCoins(outputs, coins);
}

bool VaultModel::canCreateTransactionsNow()
{
    return m_vault->canCreateTransactionsNow();
}

bool VaultModel::isMultivault() const
{
    return m_node.vaultLoader().getVaults().size() > 1;
}

void VaultModel::refresh(bool pk_hash_only)
{
    addressTableModel = new AddressTableModel(this, pk_hash_only);
}

uint256 VaultModel::getLastBlockProcessed() const
{
    return m_client_model ? m_client_model->getBestBlockHash() : uint256{};
}

CAmount VaultModel::getAvailableBalance(const CCoinControl* control)
{
    if (!canCreateTransactionsNow()) {
        return 0;
    }

    // No selected coins, return the cached balance
    if (!control || !control->HasSelected()) {
        const interfaces::VaultBalances& balances = getCachedBalance();
        return balances.balance;
    }
    // Fetch balance from the vault, taking into account the selected coins
    return vault().getAvailableBalance(*control);
}
