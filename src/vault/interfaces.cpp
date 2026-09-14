// Copyright (c) 2018-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/vault.h>

#include <common/args.h>
#include <common/messages.h>
#include <consensus/amount.h>
#include <interfaces/chain.h>
#include <interfaces/handler.h>
#include <node/types.h>
#include <primitives/transaction.h>
#include <rpc/server.h>
#include <scheduler.h>
#include <support/allocators/secure.h>
#include <sync.h>
#include <uint256.h>
#include <util/check.h>
#include <util/translation.h>
#include <util/ui_change_type.h>
#include <vault/coincontrol.h>
#include <vault/context.h>
#include <vault/load.h>
#include <vault/receive.h>
#include <vault/rpc/vault.h>
#include <vault/spend.h>
#include <vault/types.h>
#include <vault/vault.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

using common::PSQTError;
using interfaces::Chain;
using interfaces::FoundBlock;
using interfaces::Handler;
using interfaces::MakeSignalHandler;
using interfaces::Vault;
using interfaces::VaultAddress;
using interfaces::VaultBalances;
using interfaces::VaultLoader;
using interfaces::VaultOrderForm;
using interfaces::VaultTx;
using interfaces::VaultTxOut;
using interfaces::VaultTxStatus;
using interfaces::VaultValueMap;

namespace vault {
// All members of the classes in this namespace are intentionally public, as the
// classes themselves are private.
namespace {
//! Construct vault tx struct.
VaultTx MakeVaultTx(CVault& vault, const CVaultTx& wtx)
{
    LOCK(vault.cs_vault);
    VaultTx result;
    result.tx = wtx.tx;
    result.txin_is_mine.reserve(wtx.tx->vin.size());
    for (const auto& txin : wtx.tx->vin) {
        result.txin_is_mine.emplace_back(InputIsMine(vault, txin));
    }
    result.txout_is_mine.reserve(wtx.tx->vout.size());
    result.txout_address.reserve(wtx.tx->vout.size());
    result.txout_address_is_mine.reserve(wtx.tx->vout.size());
    for (const auto& txout : wtx.tx->vout) {
        result.txout_is_mine.emplace_back(vault.IsMine(txout));
        result.txout_is_change.push_back(OutputIsChange(vault, txout));
        result.txout_address.emplace_back();
        result.txout_address_is_mine.emplace_back(ExtractDestination(txout.scriptPubKey, result.txout_address.back()) ?
                                                      vault.IsMine(result.txout_address.back()) :
                                                      ISMINE_NO);
    }
    result.credit = CachedTxGetCredit(vault, wtx, ISMINE_SPENDABLE);
    result.debit = CachedTxGetDebit(vault, wtx, ISMINE_SPENDABLE);
    result.change = CachedTxGetChange(vault, wtx);
    result.time = wtx.GetTxTime();
    result.value_map = wtx.mapValue;
    result.is_coinbase = wtx.IsCoinBase();
    return result;
}

//! Construct vault tx status struct.
VaultTxStatus MakeVaultTxStatus(const CVault& vault, const CVaultTx& wtx)
    EXCLUSIVE_LOCKS_REQUIRED(vault.cs_vault)
{
    AssertLockHeld(vault.cs_vault);

    VaultTxStatus result;
    result.block_height =
        wtx.state<TxStateConfirmed>()       ? wtx.state<TxStateConfirmed>()->confirmed_block_height :
        wtx.state<TxStateBlockConflicted>() ? wtx.state<TxStateBlockConflicted>()->conflicting_block_height :
                                              std::numeric_limits<int>::max();
    result.blocks_to_maturity = vault.GetTxBlocksToMaturity(wtx);
    result.depth_in_main_chain = vault.GetTxDepthInMainChain(wtx);
    result.time_received = wtx.nTimeReceived;
    result.lock_time = wtx.tx->nLockTime;
    result.is_trusted = CachedTxIsTrusted(vault, wtx);
    result.is_abandoned = wtx.isAbandoned();
    result.is_coinbase = wtx.IsCoinBase();
    result.is_in_main_chain = wtx.isConfirmed();
    return result;
}

//! Construct vault TxOut struct.
VaultTxOut MakeVaultTxOut(const CVault& vault,
                          const CVaultTx& wtx,
                          int n,
                          int depth) EXCLUSIVE_LOCKS_REQUIRED(vault.cs_vault)
{
    VaultTxOut result;
    result.txout = wtx.tx->vout[n];
    result.time = wtx.GetTxTime();
    result.depth_in_main_chain = depth;
    result.is_spent = vault.IsSpent(COutPoint(wtx.GetHash(), n));
    return result;
}

VaultTxOut MakeVaultTxOut(const CVault& vault,
                          const COutput& output) EXCLUSIVE_LOCKS_REQUIRED(vault.cs_vault)
{
    VaultTxOut result;
    result.txout = output.txout;
    result.time = output.time;
    result.depth_in_main_chain = output.depth;
    result.is_spent = vault.IsSpent(output.outpoint);
    return result;
}

class VaultImpl : public Vault
{
public:
    explicit VaultImpl(VaultContext& context, const std::shared_ptr<CVault>& vault) : m_context(context), m_vault(vault) {}

    bool encryptVault(const SecureString& vault_passphrase) override
    {
        return m_vault->EncryptVault(vault_passphrase);
    }
    bool isCrypted() override { return m_vault->IsCrypted(); }
    bool lock() override { return m_vault->Lock(); }
    bool unlock(const SecureString& vault_passphrase) override { return m_vault->Unlock(vault_passphrase); }
    bool isLocked() override { return m_vault->IsLocked(); }
    bool changeVaultPassphrase(const SecureString& old_vault_passphrase,
                               const SecureString& new_vault_passphrase) override
    {
        return m_vault->ChangeVaultPassphrase(old_vault_passphrase, new_vault_passphrase);
    }
    void abortRescan() override { m_vault->AbortRescan(); }
    bool backupVault(const std::string& filename) override { return m_vault->BackupVault(filename); }
    bool isBackupRecorded() override { return m_vault->IsBackupRecorded(); }
    bool setBackupRecorded(bool recorded) override { return m_vault->SetBackupRecorded(recorded); }
    util::Result<AgentAllotmentRecord> recordAgentAllotmentSetup(const std::string& label, CAmount funding_limit, CAmount daily_limit) override
    {
        return m_vault->RecordAgentAllotmentSetup(label, funding_limit, daily_limit);
    }
    std::vector<AgentAllotmentRecord> listAgentAllotmentRecords() override { return m_vault->ListAgentAllotmentRecords(); }
    std::string agentAllotmentPolicyRequest(const AgentAllotmentRecord& record, CAmount funding_available) override
    {
        return m_vault->AgentAllotmentPolicyRequest(record, funding_available);
    }
    util::Result<AgentAllotmentPolicyRequestMetadata> validateAgentAllotmentPolicyRequest(const std::string& request_json) override
    {
        return m_vault->ValidateAgentAllotmentPolicyRequest(request_json);
    }
    util::Result<AgentAllotmentPolicyBundle> agentAllotmentPolicyBundle(const std::string& request_json) override
    {
        return m_vault->ExportAgentAllotmentPolicyBundle(request_json);
    }
    std::string getVaultName() override { return m_vault->GetName(); }
    util::Result<CTxDestination> getNewDestination(const OutputType type, const std::string& label) override
    {
        LOCK(m_vault->cs_vault);
        return m_vault->GetNewDestination(type, label);
    }
    bool getPubKey(const CScript& script, const CKeyID& address, CPubKey& pub_key) override
    {
        std::unique_ptr<SigningProvider> provider = m_vault->GetSolvingProvider(script);
        if (provider) {
            return provider->GetPubKey(address, pub_key);
        }
        return false;
    }
    SigningResult signMessage(const std::string& message, const PKHash& pkhash, std::string& str_sig) override
    {
        return m_vault->SignMessage(message, pkhash, str_sig);
    }
    bool isSpendable(const CTxDestination& dest) override
    {
        LOCK(m_vault->cs_vault);
        return m_vault->IsMine(dest) & ISMINE_SPENDABLE;
    }
    bool setAddressBook(const CTxDestination& dest, const std::string& name, const std::optional<AddressPurpose>& purpose) override
    {
        return m_vault->SetAddressBook(dest, name, purpose);
    }
    bool delAddressBook(const CTxDestination& dest) override
    {
        return m_vault->DelAddressBook(dest);
    }
    bool getAddress(const CTxDestination& dest,
                    std::string* name,
                    isminetype* is_mine,
                    AddressPurpose* purpose) override
    {
        LOCK(m_vault->cs_vault);
        const auto& entry = m_vault->FindAddressBookEntry(dest, /*allow_change=*/false);
        if (!entry) return false; // addr not found
        if (name) {
            *name = entry->GetLabel();
        }
        std::optional<isminetype> dest_is_mine;
        if (is_mine || purpose) {
            dest_is_mine = m_vault->IsMine(dest);
        }
        if (is_mine) {
            *is_mine = *dest_is_mine;
        }
        if (purpose) {
            *purpose = *CHECK_NONFATAL(entry->purpose);
        }
        return true;
    }
    std::vector<VaultAddress> getAddresses() override
    {
        LOCK(m_vault->cs_vault);
        std::vector<VaultAddress> result;
        m_vault->ForEachAddrBookEntry([&](const CTxDestination& dest, const std::string& label, bool is_change, const std::optional<AddressPurpose>& purpose) EXCLUSIVE_LOCKS_REQUIRED(m_vault->cs_vault) {
            if (is_change) return;
            isminetype is_mine = m_vault->IsMine(dest);
            result.emplace_back(dest, is_mine, *CHECK_NONFATAL(purpose), label);
        });
        return result;
    }
    std::vector<std::string> getAddressReceiveRequests() override
    {
        LOCK(m_vault->cs_vault);
        return m_vault->GetAddressReceiveRequests();
    }
    bool setAddressReceiveRequest(const CTxDestination& dest, const std::string& id, const std::string& value) override
    {
        // Note: The setAddressReceiveRequest interface used by the GUI to store
        // receive requests is a little awkward and could be improved in the
        // future:
        //
        // - The same method is used to save requests and erase them, but
        //   having separate methods could be clearer and prevent bugs.
        //
        // - Request ids are passed as strings even though they are generated as
        //   integers.
        //
        // - Multiple requests can be stored for the same address, but it might
        //   be better to only allow one request or only keep the current one.
        LOCK(m_vault->cs_vault);
        VaultBatch batch{m_vault->GetDatabase()};
        return value.empty() ? m_vault->EraseAddressReceiveRequest(batch, dest, id) : m_vault->SetAddressReceiveRequest(batch, dest, id, value);
    }
    util::Result<void> displayAddress(const CTxDestination& dest) override
    {
        LOCK(m_vault->cs_vault);
        return m_vault->DisplayAddress(dest);
    }
    bool lockCoin(const COutPoint& output, const bool write_to_db) override
    {
        LOCK(m_vault->cs_vault);
        std::unique_ptr<VaultBatch> batch = write_to_db ? std::make_unique<VaultBatch>(m_vault->GetDatabase()) : nullptr;
        return m_vault->LockCoin(output, batch.get());
    }
    bool unlockCoin(const COutPoint& output) override
    {
        LOCK(m_vault->cs_vault);
        std::unique_ptr<VaultBatch> batch = std::make_unique<VaultBatch>(m_vault->GetDatabase());
        return m_vault->UnlockCoin(output, batch.get());
    }
    bool isLockedCoin(const COutPoint& output) override
    {
        LOCK(m_vault->cs_vault);
        return m_vault->IsLockedCoin(output);
    }
    void listLockedCoins(std::vector<COutPoint>& outputs) override
    {
        LOCK(m_vault->cs_vault);
        return m_vault->ListLockedCoins(outputs);
    }
    bool canCreateTransactionsNow() override
    {
        TRY_LOCK(m_vault->cs_vault, locked_vault);
        if (!locked_vault || !m_vault->HaveChain() || !m_vault->chain().hasChainstate()) {
            return false;
        }

        uint256 block_hash;
        return m_vault->TryGetLastBlockHash(block_hash);
    }
    bool canCreateTransactions() override
    {
        LOCK(m_vault->cs_vault);
        if (!m_vault->HaveChain() || !m_vault->chain().hasChainstate()) {
            return false;
        }

        uint256 block_hash;
        return m_vault->TryGetLastBlockHash(block_hash);
    }
    util::Result<CTransactionRef> createTransaction(const std::vector<CRecipient>& recipients,
                                                    const CCoinControl& coin_control,
                                                    bool sign,
                                                    int& change_pos,
                                                    const std::function<void(uint32_t nonce)>& tx_proof_progress = {},
                                                    const std::function<bool()>& tx_proof_cancel = {}) override
    {
        // No cs_vault here on purpose. CreateTransaction takes the lock for the phases
        // that need it and releases it around the proof-of-work grind; wrapping the
        // call would hold it across the grind again — cs_vault is recursive, so the
        // release inside would be silently ineffective and the vault would freeze for
        // the grind's full duration. This is the desktop's and the agent's send path.
        auto res = CreateTransaction(*m_vault, recipients, change_pos == -1 ? std::nullopt : std::make_optional(change_pos),
                                     coin_control, sign, tx_proof_progress, tx_proof_cancel);
        if (!res) return util::Error{util::ErrorString(res)};
        const auto& txr = *res;
        change_pos = txr.change_pos ? int(*txr.change_pos) : -1;

        return txr.tx;
    }
    util::Result<void> commitTransaction(CTransactionRef tx,
                                         VaultValueMap value_map,
                                         VaultOrderForm order_form) override
    {
        LOCK(m_vault->cs_vault);
        const CommitTransactionResult committed{m_vault->CommitTransaction(std::move(tx), std::move(value_map), std::move(order_form))};
        if (!committed) {
            const bilingual_str error{common::TransactionErrorString(committed.error)};
            return util::Error{committed.reject_reason.empty()
                ? error
                : Untranslated(strprintf("%s: %s", error.original, committed.reject_reason))};
        }
        return {};
    }
    bool transactionCanBeAbandoned(const uint256& txid) override { return m_vault->TransactionCanBeAbandoned(txid); }
    bool abandonTransaction(const uint256& txid) override
    {
        LOCK(m_vault->cs_vault);
        return m_vault->AbandonTransaction(txid);
    }
    CTransactionRef getTx(const uint256& txid) override
    {
        LOCK(m_vault->cs_vault);
        auto mi = m_vault->mapVault.find(txid);
        if (mi != m_vault->mapVault.end()) {
            return mi->second.tx;
        }
        return {};
    }
    VaultTx getVaultTx(const uint256& txid) override
    {
        LOCK(m_vault->cs_vault);
        auto mi = m_vault->mapVault.find(txid);
        if (mi != m_vault->mapVault.end()) {
            return MakeVaultTx(*m_vault, mi->second);
        }
        return {};
    }
    std::set<VaultTx> getVaultTxs() override
    {
        LOCK(m_vault->cs_vault);
        std::set<VaultTx> result;
        for (const auto& entry : m_vault->mapVault) {
            result.emplace(MakeVaultTx(*m_vault, entry.second));
        }
        return result;
    }
    bool tryGetTxStatus(const uint256& txid,
                        interfaces::VaultTxStatus& tx_status,
                        int& num_blocks,
                        int64_t& block_time) override
    {
        TRY_LOCK(m_vault->cs_vault, locked_vault);
        if (!locked_vault) {
            return false;
        }
        auto mi = m_vault->mapVault.find(txid);
        if (mi == m_vault->mapVault.end()) {
            return false;
        }
        uint256 last_block_hash;
        if (!m_vault->TryGetLastBlockProcessed(num_blocks, last_block_hash)) {
            return false;
        }
        block_time = -1;
        if (m_context.chain && m_context.chain->hasChainstate()) {
            CHECK_NONFATAL(m_context.chain->findBlock(last_block_hash, FoundBlock().time(block_time)));
        } else {
            m_vault->TryGetLastBlockTime(block_time);
        }
        tx_status = MakeVaultTxStatus(*m_vault, mi->second);
        return true;
    }
    bool tryGetVaultTxDetails(const uint256& txid,
                              VaultTx& tx,
                              VaultTxStatus& tx_status,
                              VaultOrderForm& order_form,
                              bool& in_relaypool,
                              int& num_blocks) override
    {
        TRY_LOCK(m_vault->cs_vault, locked_vault);
        if (!locked_vault) {
            return false;
        }
        auto mi = m_vault->mapVault.find(txid);
        if (mi == m_vault->mapVault.end()) {
            return false;
        }
        uint256 last_block_hash;
        if (!m_vault->TryGetLastBlockProcessed(num_blocks, last_block_hash)) {
            return false;
        }
        in_relaypool = mi->second.InRelayPool();
        order_form = mi->second.vOrderForm;
        tx_status = MakeVaultTxStatus(*m_vault, mi->second);
        tx = MakeVaultTx(*m_vault, mi->second);
        return true;
    }
    std::optional<PSQTError> fillPSQT(int sighash_type,
                                      bool sign,
                                      bool bip32derivs,
                                      size_t* n_signed,
                                      PartiallySignedQuicksilverTransaction& psqtx,
                                      bool& complete) override
    {
        return m_vault->FillPSQT(psqtx, complete, sighash_type, sign, bip32derivs, n_signed);
    }
    VaultBalances getBalances() override
    {
        const auto bal = GetBalance(*m_vault);
        VaultBalances result;
        result.balance = bal.m_mine_trusted;
        result.unconfirmed_balance = bal.m_mine_untrusted_pending;
        result.immature_balance = bal.m_mine_immature;
        result.delegated_balance = bal.m_mine_delegated;
        return result;
    }
    bool tryGetBalances(VaultBalances& balances, uint256& block_hash) override
    {
        TRY_LOCK(m_vault->cs_vault, locked_vault);
        if (!locked_vault) {
            return false;
        }
        if (!m_vault->TryGetLastBlockHash(block_hash)) {
            return false;
        }
        balances = getBalances();
        return true;
    }
    bool tryGetBalanceUpdateBlockHash(uint256& block_hash) override
    {
        TRY_LOCK(m_vault->cs_vault, locked_vault);
        if (!locked_vault) {
            return false;
        }
        if (!m_vault->TryGetLastBlockHash(block_hash)) {
            return false;
        }
        return true;
    }
    CAmount getAvailableBalance(const CCoinControl& coin_control) override
    {
        LOCK(m_vault->cs_vault);
        CAmount total_amount = 0;
        // Fetch selected coins total amount
        if (coin_control.HasSelected()) {
            // Note: for now, swallow any error.
            if (auto res = FetchSelectedInputs(*m_vault, coin_control)) {
                total_amount += res->total_amount;
            }
        }

        // And fetch the vault available coins
        if (coin_control.m_allow_other_inputs) {
            total_amount += AvailableCoins(*m_vault, &coin_control).GetTotalAmount();
        }

        return total_amount;
    }
    isminetype txinIsMine(const CTxIn& txin) override
    {
        LOCK(m_vault->cs_vault);
        return InputIsMine(*m_vault, txin);
    }
    isminetype txoutIsMine(const CTxOut& txout) override
    {
        LOCK(m_vault->cs_vault);
        return m_vault->IsMine(txout);
    }
    CAmount getDebit(const CTxIn& txin, isminefilter filter) override
    {
        LOCK(m_vault->cs_vault);
        return m_vault->GetDebit(txin, filter);
    }
    CAmount getCredit(const CTxOut& txout, isminefilter filter) override
    {
        LOCK(m_vault->cs_vault);
        return OutputGetCredit(*m_vault, txout, filter);
    }
    bool tryListCoins(CoinsList& coins) override
    {
        TRY_LOCK(m_vault->cs_vault, locked_vault);
        if (!locked_vault) {
            return false;
        }
        uint256 block_hash;
        if (!m_vault->TryGetLastBlockHash(block_hash)) {
            return false;
        }

        coins.clear();
        for (const auto& entry : ListCoins(*m_vault)) {
            auto& group = coins[entry.first];
            for (const auto& coin : entry.second) {
                group.emplace_back(coin.outpoint,
                                   MakeVaultTxOut(*m_vault, coin));
            }
        }
        return true;
    }
    bool tryGetCoins(const std::vector<COutPoint>& outputs, std::vector<VaultTxOut>& coins) override
    {
        TRY_LOCK(m_vault->cs_vault, locked_vault);
        if (!locked_vault) {
            return false;
        }
        uint256 block_hash;
        if (!m_vault->TryGetLastBlockHash(block_hash)) {
            return false;
        }

        coins.clear();
        coins.reserve(outputs.size());
        for (const auto& output : outputs) {
            coins.emplace_back();
            auto it = m_vault->mapVault.find(output.hash);
            if (it != m_vault->mapVault.end()) {
                int depth = m_vault->GetTxDepthInMainChain(it->second);
                if (depth >= 0) {
                    coins.back() = MakeVaultTxOut(*m_vault, it->second, output.n, depth);
                }
            }
        }
        return true;
    }
    bool hdEnabled() override { return m_vault->IsHDEnabled(); }
    bool canGetAddresses() override { return m_vault->CanGetAddresses(); }
    bool hasExternalSigner() override { return m_vault->IsVaultFlagSet(VAULT_FLAG_EXTERNAL_SIGNER); }
    bool privateKeysDisabled() override { return m_vault->IsVaultFlagSet(VAULT_FLAG_DISABLE_PRIVATE_KEYS); }
    bool taprootEnabled() override
    {
        auto spk_man = m_vault->GetScriptPubKeyMan(OutputType::BECH32M, /*internal=*/false);
        return spk_man != nullptr;
    }
    OutputType getDefaultAddressType() override { return m_vault->m_default_address_type; }
    void remove(std::optional<bool> load_on_start = false) override
    {
        RemoveVault(m_context, m_vault, load_on_start);
    }

    std::unique_ptr<Handler> handleUnload(UnloadFn fn) override
    {
        return MakeSignalHandler(m_vault->NotifyUnload.connect(fn));
    }
    std::unique_ptr<Handler> handleShowProgress(ShowProgressFn fn) override
    {
        return MakeSignalHandler(m_vault->ShowProgress.connect(fn));
    }
    std::unique_ptr<Handler> handleStatusChanged(StatusChangedFn fn) override
    {
        return MakeSignalHandler(m_vault->NotifyStatusChanged.connect([fn](CVault*) { fn(); }));
    }
    std::unique_ptr<Handler> handleAddressBookChanged(AddressBookChangedFn fn) override
    {
        return MakeSignalHandler(m_vault->NotifyAddressBookChanged.connect(
            [fn](const CTxDestination& address, const std::string& label, bool is_mine,
                 AddressPurpose purpose, ChangeType status) { fn(address, label, is_mine, purpose, status); }));
    }
    std::unique_ptr<Handler> handleTransactionChanged(TransactionChangedFn fn) override
    {
        return MakeSignalHandler(m_vault->NotifyTransactionChanged.connect(
            [fn](const uint256& txid, ChangeType status) { fn(txid, status); }));
    }
    std::unique_ptr<Handler> handleCanGetAddressesChanged(CanGetAddressesChangedFn fn) override
    {
        return MakeSignalHandler(m_vault->NotifyCanGetAddressesChanged.connect(fn));
    }
    CVault* vault() override { return m_vault.get(); }

    VaultContext& m_context;
    std::shared_ptr<CVault> m_vault;
};

class VaultLoaderImpl : public VaultLoader
{
public:
    VaultLoaderImpl(Chain& chain, ArgsManager& args)
    {
        m_context.chain = &chain;
        m_context.args = &args;
    }
    ~VaultLoaderImpl() override { UnloadVaults(m_context); }

    //! ChainClient methods
    void registerRpcs() override
    {
        for (const CRPCCommand& command : GetVaultRPCCommands()) {
            m_rpc_commands.emplace_back(command.category, command.name, [this, &command](const JSONRPCRequest& request, UniValue& result, bool last_handler) {
                JSONRPCRequest vault_request = request;
                vault_request.context = &m_context;
                return command.actor(vault_request, result, last_handler); }, command.argNames, command.unique_id);
            m_rpc_handlers.emplace_back(m_context.chain->handleRpc(m_rpc_commands.back()));
        }
    }
    bool verify() override { return VerifyVaults(m_context); }
    bool load() override { return LoadVaults(m_context); }
    void start(CScheduler& scheduler) override
    {
        m_context.scheduler = &scheduler;
        return StartVaults(m_context);
    }
    void flush() override { return FlushVaults(m_context); }
    void stop() override { return StopVaults(m_context); }
    void setMockTime(int64_t time) override { return SetMockTime(time); }
    void schedulerMockForward(std::chrono::seconds delta) override { Assert(m_context.scheduler)->MockForward(delta); }

    //! VaultLoader methods
    bool setHeaderTip(int height, const uint256& hash, int64_t block_time) override
    {
        if (height < 0 || hash.IsNull() || block_time < 0) return false;

        std::vector<std::shared_ptr<CVault>> vaults;
        {
            LOCK(m_context.vaults_mutex);
            if (m_context.header_tip) {
                const HeaderTip& current{*m_context.header_tip};
                if (height < current.height || (height == current.height && hash != current.hash)) return false;
                if (height == current.height) return true;
            }
            m_context.header_tip = HeaderTip{height, hash, block_time};
            vaults = m_context.vaults;
        }

        if (m_context.chain && m_context.chain->hasChainstate()) return true;
        for (const std::shared_ptr<CVault>& vault : vaults) {
            LOCK(vault->cs_vault);
            vault->ApplyHeaderTip(height, hash, block_time);
        }
        return true;
    }

    util::Result<std::unique_ptr<Vault>> createVault(const std::string& name, const SecureString& passphrase, uint64_t vault_creation_flags, std::vector<bilingual_str>& warnings) override
    {
        DatabaseOptions options;
        DatabaseStatus status;
        ReadDatabaseArgs(*m_context.args, options);
        options.require_create = true;
        options.create_flags = vault_creation_flags;
        options.create_passphrase = passphrase;
        bilingual_str error;
        std::unique_ptr<Vault> vault{MakeVault(m_context, CreateVault(m_context, name, /*load_on_start=*/true, options, status, error, warnings))};
        if (vault) {
            return vault;
        } else {
            return util::Error{error};
        }
    }
    util::Result<std::unique_ptr<Vault>> loadVault(const std::string& name, std::vector<bilingual_str>& warnings) override
    {
        DatabaseOptions options;
        DatabaseStatus status;
        ReadDatabaseArgs(*m_context.args, options);
        options.require_existing = true;
        bilingual_str error;
        std::unique_ptr<Vault> vault{MakeVault(m_context, LoadVault(m_context, name, /*load_on_start=*/true, options, status, error, warnings))};
        if (vault) {
            return vault;
        } else {
            return util::Error{error};
        }
    }
    util::Result<std::unique_ptr<Vault>> restoreVault(const fs::path& backup_file, const std::string& vault_name, std::vector<bilingual_str>& warnings) override
    {
        DatabaseStatus status;
        bilingual_str error;
        std::unique_ptr<Vault> vault{MakeVault(m_context, RestoreVault(m_context, backup_file, vault_name, /*load_on_start=*/true, status, error, warnings))};
        if (vault) {
            return vault;
        } else {
            return util::Error{error};
        }
    }
    std::vector<std::pair<std::string, std::string>> listVaultDir() override
    {
        std::vector<std::pair<std::string, std::string>> paths;
        for (auto& [path, format] : ListDatabases(GetVaultDir())) {
            paths.emplace_back(fs::PathToString(path), format);
        }
        return paths;
    }
    std::vector<std::unique_ptr<Vault>> getVaults() override
    {
        std::vector<std::unique_ptr<Vault>> vaults;
        for (const auto& vault : GetVaults(m_context)) {
            vaults.emplace_back(MakeVault(m_context, vault));
        }
        return vaults;
    }
    std::unique_ptr<Handler> handleLoadVault(LoadVaultFn fn) override
    {
        return HandleLoadVault(m_context, std::move(fn));
    }
    VaultContext* context() override { return &m_context; }

    VaultContext m_context;
    std::vector<std::unique_ptr<Handler>> m_rpc_handlers;
    std::list<CRPCCommand> m_rpc_commands;
};
} // namespace
} // namespace vault

namespace interfaces {
std::unique_ptr<Vault> MakeVault(vault::VaultContext& context, const std::shared_ptr<vault::CVault>& vault) { return vault ? std::make_unique<vault::VaultImpl>(context, vault) : nullptr; }

std::unique_ptr<VaultLoader> MakeVaultLoader(Chain& chain, ArgsManager& args)
{
    return std::make_unique<vault::VaultLoaderImpl>(chain, args);
}
} // namespace interfaces
