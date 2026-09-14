// Copyright (c) 2012-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <vault/vault.h>

#include <chrono>
#include <future>
#include <memory>
#include <stdint.h>
#include <vector>

#include <addresstype.h>
#include <chainparams.h>
#include <interfaces/chain.h>
#include <interfaces/vault.h>
#include <key_io.h>
#include <node/blockstorage.h>
#include <policy/policy.h>
#include <rpc/server.h>
#include <script/solver.h>
#include <streams.h>
#include <sync.h>
#include <test/util/logging.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <util/translation.h>
#include <validation.h>
#include <validationinterface.h>
#include <vault/archive.h>
#include <vault/coincontrol.h>
#include <vault/context.h>
#include <vault/receive.h>
#include <vault/spend.h>
#include <vault/test/util.h>
#include <vault/test/vault_test_fixture.h>

#include <boost/test/unit_test.hpp>
#include <univalue.h>

using node::MAX_BLOCKFILE_SIZE;

namespace vault {

class RejectingChain final : public interfaces::Chain
{
public:
    RejectingChain(interfaces::Chain& chain, node::TransactionError error, std::string reason)
        : m_chain{chain}, m_error{error}, m_reason{std::move(reason)}
    {
    }

    std::optional<int> getHeight() override { return m_chain.getHeight(); }
    uint256 getBlockHash(int height) override { return m_chain.getBlockHash(height); }
    uint256 txPowTarget(int anchor_height, uint64_t bytes, size_t nout, size_t nin) override { return m_chain.txPowTarget(anchor_height, bytes, nout, nin); }
    bool haveBlockOnDisk(int height) override { return m_chain.haveBlockOnDisk(height); }
    CBlockLocator getTipLocator() override { return m_chain.getTipLocator(); }
    CBlockLocator getActiveChainLocator(const uint256& block_hash) override { return m_chain.getActiveChainLocator(block_hash); }
    std::optional<int> findLocatorFork(const CBlockLocator& locator) override { return m_chain.findLocatorFork(locator); }
    bool hasBlockFilterIndex(BlockFilterType filter_type) override { return m_chain.hasBlockFilterIndex(filter_type); }
    std::optional<bool> blockFilterMatchesAny(BlockFilterType filter_type, const uint256& block_hash, const GCSFilter::ElementSet& filter_set) override { return m_chain.blockFilterMatchesAny(filter_type, block_hash, filter_set); }
    bool findBlock(const uint256& hash, const interfaces::FoundBlock& block = {}) override { return m_chain.findBlock(hash, block); }
    bool findFirstBlockWithTimeAndHeight(int64_t min_time, int min_height, const interfaces::FoundBlock& block = {}) override { return m_chain.findFirstBlockWithTimeAndHeight(min_time, min_height, block); }
    bool findAncestorByHeight(const uint256& block_hash, int ancestor_height, const interfaces::FoundBlock& ancestor_out = {}) override { return m_chain.findAncestorByHeight(block_hash, ancestor_height, ancestor_out); }
    bool findAncestorByHash(const uint256& block_hash, const uint256& ancestor_hash, const interfaces::FoundBlock& ancestor_out = {}) override { return m_chain.findAncestorByHash(block_hash, ancestor_hash, ancestor_out); }
    bool findCommonAncestor(const uint256& block_hash1, const uint256& block_hash2, const interfaces::FoundBlock& ancestor_out = {}, const interfaces::FoundBlock& block1_out = {}, const interfaces::FoundBlock& block2_out = {}) override { return m_chain.findCommonAncestor(block_hash1, block_hash2, ancestor_out, block1_out, block2_out); }
    void findCoins(std::map<COutPoint, Coin>& coins) override { m_chain.findCoins(coins); }
    double guessVerificationProgress(const uint256& block_hash) override { return m_chain.guessVerificationProgress(block_hash); }
    bool hasBlocks(const uint256& block_hash, int min_height = 0, std::optional<int> max_height = {}) override { return m_chain.hasBlocks(block_hash, min_height, max_height); }
    bool isInRelayPool(const uint256& txid) override { return m_chain.isInRelayPool(txid); }
    node::TransactionError broadcastTransaction(const CTransactionRef&, bool, std::string& err_string) override
    {
        err_string = m_reason;
        return m_error;
    }
    void getTransactionAncestry(const uint256& txid, size_t& ancestors, size_t& descendants, size_t* ancestorsize = nullptr) override { m_chain.getTransactionAncestry(txid, ancestors, descendants, ancestorsize); }
    void getPackageLimits(unsigned int& limit_ancestor_count, unsigned int& limit_descendant_count) override { m_chain.getPackageLimits(limit_ancestor_count, limit_descendant_count); }
    util::Result<void> checkChainLimits(const CTransactionRef& tx) override { return m_chain.checkChainLimits(tx); }
    bool havePruned() override { return m_chain.havePruned(); }
    std::optional<int> getPruneHeight() override { return m_chain.getPruneHeight(); }
    bool isReadyToBroadcast() override { return m_chain.isReadyToBroadcast(); }
    bool isInitialBlockDownload() override { return m_chain.isInitialBlockDownload(); }
    bool shutdownRequested() override { return m_chain.shutdownRequested(); }
    void initMessage(const std::string& message) override { m_chain.initMessage(message); }
    void initWarning(const bilingual_str& message) override { m_chain.initWarning(message); }
    void initError(const bilingual_str& message) override { m_chain.initError(message); }
    void showProgress(const std::string& title, int progress, bool resume_possible) override { m_chain.showProgress(title, progress, resume_possible); }
    std::unique_ptr<interfaces::Handler> handleNotifications(std::shared_ptr<Notifications> notifications) override { return m_chain.handleNotifications(std::move(notifications)); }
    void waitForNotificationsIfTipChanged(const uint256& old_tip) override { m_chain.waitForNotificationsIfTipChanged(old_tip); }
    std::unique_ptr<interfaces::Handler> handleRpc(const CRPCCommand& command) override { return m_chain.handleRpc(command); }
    void rpcRunLater(const std::string& name, std::function<void()> fn, int64_t seconds) override { m_chain.rpcRunLater(name, std::move(fn), seconds); }
    std::vector<common::SettingsValue> getSettingsList(const std::string& arg) override { return m_chain.getSettingsList(arg); }
    common::SettingsValue getRwSetting(const std::string& name) override { return m_chain.getRwSetting(name); }
    bool updateRwSetting(const std::string& name, const interfaces::SettingsUpdate& update_function) override { return m_chain.updateRwSetting(name, update_function); }
    void requestRelayPoolTransactions(Notifications& notifications) override { m_chain.requestRelayPoolTransactions(notifications); }
    bool hasChainstate() override { return m_chain.hasChainstate(); }
    node::NodeContext* context() override { return m_chain.context(); }

private:
    interfaces::Chain& m_chain;
    node::TransactionError m_error;
    std::string m_reason;
};

BOOST_FIXTURE_TEST_SUITE(vault_tests, VaultTestingSetup)

BOOST_FIXTURE_TEST_CASE(transaction_creation_readiness_distinguishes_lock_contention, TestChain100Setup)
{
    std::unique_ptr<interfaces::VaultLoader> vault_loader = interfaces::MakeVaultLoader(*m_node.chain, *Assert(m_node.args));
    std::shared_ptr<CVault> vault = CreateSyncedVault(
        *m_node.chain,
        WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain()),
        coinbaseKey);
    std::unique_ptr<interfaces::Vault> vault_interface = interfaces::MakeVault(*vault_loader->context(), vault);
    BOOST_REQUIRE(vault_interface);
    BOOST_REQUIRE(vault_interface->canCreateTransactionsNow());
    BOOST_REQUIRE(vault_interface->canCreateTransactions());

    std::promise<void> lock_acquired;
    std::promise<void> release_lock;
    std::shared_future<void> release_future{release_lock.get_future()};
    std::future<void> lock_holder = std::async(std::launch::async, [&] {
        LOCK(vault->cs_vault);
        lock_acquired.set_value();
        release_future.wait();
    });
    lock_acquired.get_future().wait();

    // GUI probes must remain nonblocking, but worker preparation must not turn
    // this ordinary contention into a false "header sync unavailable" result.
    BOOST_CHECK(!vault_interface->canCreateTransactionsNow());
    std::future<bool> authoritative_check = std::async(std::launch::async, [&] {
        return vault_interface->canCreateTransactions();
    });
    BOOST_CHECK(authoritative_check.wait_for(std::chrono::milliseconds{50}) == std::future_status::timeout);

    release_lock.set_value();
    lock_holder.get();
    BOOST_CHECK(authoritative_check.get());
}

static CMutableTransaction TestSimpleSpend(const CTransaction& from, uint32_t index, const CKey& key, const CScript& pubkey)
{
    CMutableTransaction mtx;
    mtx.vout.emplace_back(from.vout[index].nValue, pubkey);
    mtx.vin.push_back({CTxIn{from.GetHash(), index}});
    FillableSigningProvider keystore;
    keystore.AddKey(key);
    std::map<COutPoint, Coin> coins;
    coins[mtx.vin[0].prevout].out = from.vout[index];
    std::map<int, bilingual_str> input_errors;
    BOOST_CHECK(SignTransaction(mtx, &keystore, coins, SIGHASH_ALL, input_errors));
    return mtx;
}

static void AddKey(CVault& vault, const CKey& key)
{
    LOCK(vault.cs_vault);
    FlatSigningProvider provider;
    std::string error;
    auto descs = Parse("combo(" + EncodeSecret(key) + ")", provider, error, /* require_checksum=*/false);
    assert(descs.size() == 1);
    auto& desc = descs.at(0);
    VaultDescriptor w_desc(std::move(desc), 0, 0, 1, 1);
    if (!vault.AddVaultDescriptor(w_desc, provider, "", false)) assert(false);
}

struct VaultTxPowTestSetup : TestChain100Setup {
    VaultTxPowTestSetup()
    {
        const_cast<Consensus::Params&>(Params().GetConsensus()).fTxPowNoCycle = true;
    }

    void ProveTxPowAtTip(CMutableTransaction& tx) EXCLUSIVE_LOCKS_REQUIRED(!::cs_main)
    {
        LOCK(::cs_main);
        ProveTxPowForTest(tx, *Assert(m_node.chainman->ActiveChain().Tip()), Params().GetConsensus());
    }
};

BOOST_FIXTURE_TEST_CASE(commit_relaypool_rejection_removes_transaction_and_releases_inputs, VaultTxPowTestSetup)
{
    CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    RejectingChain rejecting_chain{*m_node.chain, node::TransactionError::CONSENSUS_INVALID, "tx-pow-invalid"};
    std::unique_ptr<CVault> vault{CreateSyncedVault(
        rejecting_chain,
        WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain()),
        coinbaseKey)};
    vault->SetBroadcastTransactions(true);

    const CAmount available_before{WITH_LOCK(vault->cs_vault, return AvailableCoins(*vault).GetTotalAmount())};
    const CKey destination_key{GenerateRandomKey()};
    CCoinControl coin_control;
    auto created{CreateTransaction(*vault,
                                   {CRecipient{PKHash(destination_key.GetPubKey()), COIN}},
                                   /*change_pos=*/std::nullopt,
                                   coin_control)};
    BOOST_REQUIRE_MESSAGE(created, util::ErrorString(created).original);
    const CTransactionRef tx{created->tx};
    BOOST_REQUIRE(!tx->vin.empty());

    const CommitTransactionResult committed{vault->CommitTransaction(tx, {}, {})};
    BOOST_CHECK(!committed);
    BOOST_CHECK_EQUAL(committed.error, node::TransactionError::CONSENSUS_INVALID);
    BOOST_CHECK_EQUAL(committed.reject_reason, "tx-pow-invalid");

    LOCK(vault->cs_vault);
    BOOST_CHECK_EQUAL(vault->mapVault.count(tx->GetHash()), 0U);
    for (const CTxIn& txin : tx->vin) {
        BOOST_CHECK(!vault->IsSpent(txin.prevout));
    }
    BOOST_CHECK_EQUAL(AvailableCoins(*vault).GetTotalAmount(), available_before);
}

BOOST_FIXTURE_TEST_CASE(commit_retryable_rejection_keeps_transaction, VaultTxPowTestSetup)
{
    // A transaction the pool will not take *yet* -- an unmatured locktime is the
    // everyday case -- must survive the commit. Dropping it here would discard a
    // transaction the caller deliberately built ahead of time, and would report a
    // transfer as failed that the vault is still going to make.
    CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    RejectingChain rejecting_chain{*m_node.chain, node::TransactionError::RELAYPOOL_REJECTED, "non-final"};
    std::unique_ptr<CVault> vault{CreateSyncedVault(
        rejecting_chain,
        WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain()),
        coinbaseKey)};
    vault->SetBroadcastTransactions(true);

    const CKey destination_key{GenerateRandomKey()};
    CCoinControl coin_control;
    auto created{CreateTransaction(*vault,
                                   {CRecipient{PKHash(destination_key.GetPubKey()), COIN}},
                                   /*change_pos=*/std::nullopt,
                                   coin_control)};
    BOOST_REQUIRE_MESSAGE(created, util::ErrorString(created).original);
    const CTransactionRef tx{created->tx};
    BOOST_REQUIRE(!tx->vin.empty());

    const CommitTransactionResult committed{vault->CommitTransaction(tx, {}, {})};
    BOOST_CHECK(committed);
    BOOST_CHECK_EQUAL(committed.error, node::TransactionError::OK);

    LOCK(vault->cs_vault);
    BOOST_CHECK_EQUAL(vault->mapVault.count(tx->GetHash()), 1U);
    for (const CTxIn& txin : tx->vin) {
        BOOST_CHECK(vault->IsSpent(txin.prevout));
    }
}

BOOST_FIXTURE_TEST_CASE(scan_for_vault_transactions, TestChain100Setup)
{
    // Cap last block file size, and mine new block in a new block file.
    CBlockIndex* oldTip = WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain().Tip());
    const CAmount old_tip_coinbase_value{m_coinbase_txns.back()->vout[0].nValue};
    WITH_LOCK(::cs_main, m_node.chainman->m_blockman.GetBlockFileInfo(oldTip->GetBlockPos().nFile)->nSize = MAX_BLOCKFILE_SIZE);
    const CBlock new_block{CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()))};
    const CAmount new_tip_coinbase_value{new_block.vtx[0]->vout[0].nValue};
    CBlockIndex* newTip = WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain().Tip());

    // Verify ScanForVaultTransactions fails to read an unknown start block.
    {
        CVault vault(m_node.chain.get(), "", CreateMockableVaultDatabase());
        {
            LOCK(vault.cs_vault);
            LOCK(Assert(m_node.chainman)->GetMutex());
            vault.SetVaultFlag(VAULT_FLAG_DESCRIPTORS);
            vault.SetLastBlockProcessed(m_node.chainman->ActiveChain().Height(), m_node.chainman->ActiveChain().Tip()->GetBlockHash());
        }
        AddKey(vault, coinbaseKey);
        VaultRescanReserver reserver(vault);
        reserver.reserve();
        CVault::ScanResult result = vault.ScanForVaultTransactions(/*start_block=*/{}, /*start_height=*/0, /*max_height=*/{}, reserver, /*fUpdate=*/false, /*save_progress=*/false);
        BOOST_CHECK_EQUAL(result.status, CVault::ScanResult::FAILURE);
        BOOST_CHECK(result.last_failed_block.IsNull());
        BOOST_CHECK(result.last_scanned_block.IsNull());
        BOOST_CHECK(!result.last_scanned_height);
        BOOST_CHECK_EQUAL(GetBalance(vault).m_mine_immature, 0);
    }

    // Verify ScanForVaultTransactions picks up transactions in both the old
    // and new block files.
    {
        CVault vault(m_node.chain.get(), "", CreateMockableVaultDatabase());
        {
            LOCK(vault.cs_vault);
            LOCK(Assert(m_node.chainman)->GetMutex());
            vault.SetVaultFlag(VAULT_FLAG_DESCRIPTORS);
            vault.SetLastBlockProcessed(m_node.chainman->ActiveChain().Height(), m_node.chainman->ActiveChain().Tip()->GetBlockHash());
        }
        AddKey(vault, coinbaseKey);
        VaultRescanReserver reserver(vault);
        std::chrono::steady_clock::time_point fake_time;
        reserver.setNow([&] { fake_time += 60s; return fake_time; });
        reserver.reserve();

        {
            CBlockLocator locator;
            BOOST_CHECK(!VaultBatch{vault.GetDatabase()}.ReadBestBlock(locator));
            BOOST_CHECK(locator.IsNull());
        }

        CVault::ScanResult result = vault.ScanForVaultTransactions(/*start_block=*/oldTip->GetBlockHash(), /*start_height=*/oldTip->nHeight, /*max_height=*/{}, reserver, /*fUpdate=*/false, /*save_progress=*/true);
        BOOST_CHECK_EQUAL(result.status, CVault::ScanResult::SUCCESS);
        BOOST_CHECK(result.last_failed_block.IsNull());
        BOOST_CHECK_EQUAL(result.last_scanned_block, newTip->GetBlockHash());
        BOOST_CHECK_EQUAL(*result.last_scanned_height, newTip->nHeight);
        BOOST_CHECK_EQUAL(GetBalance(vault).m_mine_immature, old_tip_coinbase_value + new_tip_coinbase_value);

        {
            CBlockLocator locator;
            BOOST_CHECK(VaultBatch{vault.GetDatabase()}.ReadBestBlock(locator));
            BOOST_CHECK(!locator.IsNull());
        }
    }

    // Prune the older block file.
    int file_number;
    {
        LOCK(cs_main);
        file_number = oldTip->GetBlockPos().nFile;
        Assert(m_node.chainman)->m_blockman.PruneOneBlockFile(file_number);
    }
    m_node.chainman->m_blockman.UnlinkPrunedFiles({file_number});

    // Verify ScanForVaultTransactions only picks transactions in the new block
    // file.
    {
        CVault vault(m_node.chain.get(), "", CreateMockableVaultDatabase());
        {
            LOCK(vault.cs_vault);
            LOCK(Assert(m_node.chainman)->GetMutex());
            vault.SetVaultFlag(VAULT_FLAG_DESCRIPTORS);
            vault.SetLastBlockProcessed(m_node.chainman->ActiveChain().Height(), m_node.chainman->ActiveChain().Tip()->GetBlockHash());
        }
        AddKey(vault, coinbaseKey);
        VaultRescanReserver reserver(vault);
        reserver.reserve();
        CVault::ScanResult result = vault.ScanForVaultTransactions(/*start_block=*/oldTip->GetBlockHash(), /*start_height=*/oldTip->nHeight, /*max_height=*/{}, reserver, /*fUpdate=*/false, /*save_progress=*/false);
        BOOST_CHECK_EQUAL(result.status, CVault::ScanResult::FAILURE);
        BOOST_CHECK_EQUAL(result.last_failed_block, oldTip->GetBlockHash());
        BOOST_CHECK_EQUAL(result.last_scanned_block, newTip->GetBlockHash());
        BOOST_CHECK_EQUAL(*result.last_scanned_height, newTip->nHeight);
        BOOST_CHECK_EQUAL(GetBalance(vault).m_mine_immature, new_tip_coinbase_value);
    }

    // Prune the remaining block file.
    {
        LOCK(cs_main);
        file_number = newTip->GetBlockPos().nFile;
        Assert(m_node.chainman)->m_blockman.PruneOneBlockFile(file_number);
    }
    m_node.chainman->m_blockman.UnlinkPrunedFiles({file_number});

    // Verify ScanForVaultTransactions scans no blocks.
    {
        CVault vault(m_node.chain.get(), "", CreateMockableVaultDatabase());
        {
            LOCK(vault.cs_vault);
            LOCK(Assert(m_node.chainman)->GetMutex());
            vault.SetVaultFlag(VAULT_FLAG_DESCRIPTORS);
            vault.SetLastBlockProcessed(m_node.chainman->ActiveChain().Height(), m_node.chainman->ActiveChain().Tip()->GetBlockHash());
        }
        AddKey(vault, coinbaseKey);
        VaultRescanReserver reserver(vault);
        reserver.reserve();
        CVault::ScanResult result = vault.ScanForVaultTransactions(/*start_block=*/oldTip->GetBlockHash(), /*start_height=*/oldTip->nHeight, /*max_height=*/{}, reserver, /*fUpdate=*/false, /*save_progress=*/false);
        BOOST_CHECK_EQUAL(result.status, CVault::ScanResult::FAILURE);
        BOOST_CHECK_EQUAL(result.last_failed_block, newTip->GetBlockHash());
        BOOST_CHECK(result.last_scanned_block.IsNull());
        BOOST_CHECK(!result.last_scanned_height);
        BOOST_CHECK_EQUAL(GetBalance(vault).m_mine_immature, 0);
    }
}

// This test verifies that vault settings can be added and removed
// concurrently, ensuring no race conditions occur during either process.
BOOST_FIXTURE_TEST_CASE(write_vault_settings_concurrently, TestingSetup)
{
    auto chain = m_node.chain.get();
    const auto NUM_VAULTS{5};

    // Since we're counting the number of vaults, ensure we start without any.
    BOOST_REQUIRE(chain->getRwSetting("vault").isNull());

    const auto& check_concurrent_vault = [&](const auto& settings_function, int num_expected_vaults) {
        std::vector<std::thread> threads;
        threads.reserve(NUM_VAULTS);
        for (auto i{0}; i < NUM_VAULTS; ++i)
            threads.emplace_back(settings_function, i);
        for (auto& t : threads)
            t.join();

        auto vaults = chain->getRwSetting("vault");
        BOOST_CHECK_EQUAL(vaults.getValues().size(), num_expected_vaults);
    };

    // Add NUM_VAULTS vaults concurrently, ensure we end up with NUM_VAULTS stored.
    check_concurrent_vault([&chain](int i) {
        Assert(AddVaultSetting(*chain, strprintf("vault_%d", i)));
    },
                           /*num_expected_vaults=*/NUM_VAULTS);

    // Remove NUM_VAULTS vaults concurrently, ensure we end up with 0 vaults.
    check_concurrent_vault([&chain](int i) {
        Assert(RemoveVaultSetting(*chain, strprintf("vault_%d", i)));
    },
                           /*num_expected_vaults=*/0);
}

// `Close vault` unlists a vault so it stays closed. Handing one over to node
// initialisation must not: that list is exactly how the node finds it again, so
// clearing it would turn an intended reopen into a disappearance.
BOOST_FIXTURE_TEST_CASE(remove_vault_load_on_start_controls_the_startup_list, TestingSetup)
{
    VaultContext context;
    context.args = &m_args;
    context.chain = m_node.chain.get();

    // Ask whether the vault is listed rather than how many names are listed: this
    // test module shares a process with others that write the same setting, and the
    // question here is about one vault's membership, not the size of the list.
    const auto listed = [&context] {
        const common::SettingsValue setting = context.chain->getRwSetting("vault");
        if (!setting.isArray()) return false;
        for (const auto& value : setting.getValues()) {
            if (value.isStr() && value.get_str().empty()) return true;
        }
        return false;
    };

    BOOST_REQUIRE(AddVaultSetting(*context.chain, ""));
    BOOST_REQUIRE(listed());

    {
        auto vault = TestLoadVault(context);
        BOOST_REQUIRE(AddVault(context, vault));
        std::vector<bilingual_str> warnings;
        BOOST_CHECK(RemoveVault(context, vault, /*load_on_start=*/std::nullopt, warnings));
        WaitForDeleteVault(std::move(vault));
    }
    // Unloaded, and still named in the startup list for whoever loads it next.
    BOOST_CHECK(listed());

    {
        auto vault = TestLoadVault(context);
        BOOST_REQUIRE(AddVault(context, vault));
        std::vector<bilingual_str> warnings;
        BOOST_CHECK(RemoveVault(context, vault, /*load_on_start=*/false, warnings));
        WaitForDeleteVault(std::move(vault));
    }
    BOOST_CHECK(!listed());
}

// A cancelled grind has to say it was cancelled. The solver returns false for a
// cancel exactly as it does for a missing GPU, and telling someone who pressed
// Cancel that they need to install a solver sends them hunting for a fault that is
// not there.
BOOST_FIXTURE_TEST_CASE(grind_transaction_pow_reports_cancellation, VaultTxPowTestSetup)
{
    VaultContext context;
    context.args = &m_args;
    context.chain = m_node.chain.get();
    auto vault = TestLoadVault(context);

    const auto anchor_height = TxPowAnchorHeight(*vault);
    BOOST_REQUIRE(anchor_height);

    CMutableTransaction tx;
    tx.vin.emplace_back();
    tx.vout.emplace_back(CENT, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));

    const auto error = GrindTransactionPow(*vault, tx, /*bytes=*/1000, *anchor_height,
                                           /*progress=*/{}, /*cancel=*/[] { return true; });
    BOOST_REQUIRE(error.has_value());
    BOOST_CHECK_EQUAL(error->original, "Transfer proof-of-work canceled");

    TestUnloadVault(std::move(vault));
}

BOOST_AUTO_TEST_CASE(tx_pow_cpu_fallback_covers_shipped_networks)
{
    BOOST_CHECK(AllowsTxPowCpuFallback(/*sandbox=*/19));
    BOOST_CHECK(AllowsTxPowCpuFallback(/*main/publictest=*/28));
    BOOST_CHECK(!AllowsTxPowCpuFallback(/*unsupported=*/29));
}

// Check that GetImmatureCredit() returns a newly calculated value instead of
// the cached value after a MarkDirty() call.
//
// This is a regression test written to verify a bugfix for the immature credit
// function. Similar tests probably should be written for the other credit and
// debit functions.
BOOST_FIXTURE_TEST_CASE(coin_mark_dirty_immature_credit, TestChain100Setup)
{
    CVault vault(m_node.chain.get(), "", CreateMockableVaultDatabase());

    LOCK(vault.cs_vault);
    LOCK(Assert(m_node.chainman)->GetMutex());
    CVaultTx wtx{m_coinbase_txns.back(), TxStateConfirmed{m_node.chainman->ActiveChain().Tip()->GetBlockHash(), m_node.chainman->ActiveChain().Height(), /*index=*/0}};
    vault.SetVaultFlag(VAULT_FLAG_DESCRIPTORS);
    vault.SetupDescriptorScriptPubKeyMans();

    vault.SetLastBlockProcessed(m_node.chainman->ActiveChain().Height(), m_node.chainman->ActiveChain().Tip()->GetBlockHash());

    // Call GetImmatureCredit() once before adding the key to the vault to
    // cache the current immature credit amount, which is 0.
    BOOST_CHECK_EQUAL(CachedTxGetImmatureCredit(vault, wtx, ISMINE_SPENDABLE), 0);

    // Invalidate the cached value, add the key, and make sure a new immature
    // credit amount is calculated.
    wtx.MarkDirty();
    AddKey(vault, coinbaseKey);
    BOOST_CHECK_EQUAL(CachedTxGetImmatureCredit(vault, wtx, ISMINE_SPENDABLE), m_coinbase_txns.back()->vout[0].nValue);
}

static int64_t AddTx(ChainstateManager& chainman, CVault& vault, uint32_t lockTime, int64_t mockTime, int64_t blockTime)
{
    CMutableTransaction tx;
    TxState state = TxStateInactive{};
    tx.nLockTime = lockTime;
    SetMockTime(mockTime);
    CBlockIndex* block = nullptr;
    if (blockTime > 0) {
        LOCK(cs_main);
        auto inserted = chainman.BlockIndex().emplace(std::piecewise_construct, std::make_tuple(GetRandHash()), std::make_tuple());
        assert(inserted.second);
        const uint256& hash = inserted.first->first;
        block = &inserted.first->second;
        block->nTime = blockTime;
        block->phashBlock = &hash;
        state = TxStateConfirmed{hash, block->nHeight, /*index=*/0};
    }
    return vault.AddToVault(MakeTransactionRef(tx), state, [&](CVaultTx& wtx, bool /* new_tx */) {
                    // Assign wtx.m_state to simplify test and avoid the need to simulate
                    // reorg events. Without this, AddToVault asserts false when the same
                    // transaction is confirmed in different blocks.
                    wtx.m_state = state;
                    return true;
                })
        ->nTimeSmart;
}

// Simple test to verify assignment of CVaultTx::nSmartTime value. Could be
// expanded to cover more corner cases of smart time logic.
BOOST_AUTO_TEST_CASE(ComputeTimeSmart)
{
    // New transaction should use clock time if lower than block time.
    BOOST_CHECK_EQUAL(AddTx(*m_node.chainman, m_vault, 1, 100, 120), 100);

    // Test that updating existing transaction does not change smart time.
    BOOST_CHECK_EQUAL(AddTx(*m_node.chainman, m_vault, 1, 200, 220), 100);

    // New transaction should use clock time if there's no block time.
    BOOST_CHECK_EQUAL(AddTx(*m_node.chainman, m_vault, 2, 300, 0), 300);

    // New transaction should use block time if lower than clock time.
    BOOST_CHECK_EQUAL(AddTx(*m_node.chainman, m_vault, 3, 420, 400), 400);

    // New transaction should use latest entry time if higher than
    // min(block time, clock time).
    BOOST_CHECK_EQUAL(AddTx(*m_node.chainman, m_vault, 4, 500, 390), 400);

    // If there are future entries, new transaction should use time of the
    // newest entry that is no more than 300 seconds ahead of the clock time.
    BOOST_CHECK_EQUAL(AddTx(*m_node.chainman, m_vault, 5, 50, 600), 300);
}

BOOST_AUTO_TEST_CASE(backup_records_vault_metadata)
{
    CVault vault(m_node.chain.get(), "", CreateMockableVaultDatabase());
    BOOST_CHECK(!vault.IsBackupRecorded());

    BOOST_CHECK(vault.BackupVault("successful-backup.dat"));
    BOOST_CHECK(vault.IsBackupRecorded());

    BOOST_CHECK(vault.SetBackupRecorded(false));
    GetMockableDatabase(vault).m_backup_pass = false;
    BOOST_CHECK(!vault.BackupVault("failed-first-backup.dat"));
    BOOST_CHECK(!vault.IsBackupRecorded());

    BOOST_CHECK(vault.SetBackupRecorded(true));
    BOOST_CHECK(!vault.BackupVault("failed-later-backup.dat"));
    BOOST_CHECK(vault.IsBackupRecorded());
}

BOOST_AUTO_TEST_CASE(agent_allotment_setup_records_vault_metadata)
{
    CVault vault(m_node.chain.get(), "", CreateMockableVaultDatabase());
    BOOST_CHECK(vault.ListAgentAllotmentRecords().empty());

    BOOST_CHECK(!vault.RecordAgentAllotmentSetup("", COIN, 0));
    BOOST_CHECK(!vault.RecordAgentAllotmentSetup("test-agent", 0, 0));
    BOOST_CHECK(!vault.RecordAgentAllotmentSetup("test-agent", COIN, -1));
    BOOST_CHECK(vault.ListAgentAllotmentRecords().empty());

    {
        LOCK(vault.cs_vault);
        vault.SetVaultFlag(VAULT_FLAG_DESCRIPTORS);
        vault.SetupDescriptorScriptPubKeyMans();
    }

    auto first = vault.RecordAgentAllotmentSetup(" test-agent ", COIN, COIN / 2);
    BOOST_REQUIRE(first);
    BOOST_CHECK_EQUAL(first->version, AgentAllotmentRecord::CURRENT_VERSION);
    BOOST_CHECK_EQUAL(first->id, "agent-1");
    BOOST_CHECK_EQUAL(first->label, "test-agent");
    BOOST_CHECK_EQUAL(first->funding_limit, COIN);
    BOOST_CHECK_EQUAL(first->daily_limit, COIN / 2);
    BOOST_CHECK_GT(first->risk_accepted_time, 0);
    BOOST_CHECK(!first->backend_created);
    BOOST_CHECK(IsValidDestinationString(first->funding_address));
    BOOST_CHECK(first->policy_status == AgentAllotmentPolicyStatus::PendingIntegration);
    const std::string policy_request{vault.AgentAllotmentPolicyRequest(*first, COIN)};
    BOOST_CHECK_NE(policy_request.find(R"("type":"quicksilver.agent_allotment_policy_request")"), std::string::npos);
    BOOST_CHECK_NE(policy_request.find(R"("version":1)"), std::string::npos);
    BOOST_CHECK_NE(policy_request.find(strprintf(R"("chain":"%s")", Params().GetChainTypeString())), std::string::npos);
    BOOST_CHECK_NE(policy_request.find(strprintf(R"("genesis_hash":"%s")", Params().GenesisBlock().GetHash().ToString())), std::string::npos);
    BOOST_CHECK_NE(policy_request.find(R"("id":"agent-1")"), std::string::npos);
    BOOST_CHECK_NE(policy_request.find(R"("label":"test-agent")"), std::string::npos);
    BOOST_CHECK_NE(policy_request.find(strprintf(R"("funding_address":"%s")", first->funding_address)), std::string::npos);
    BOOST_CHECK_NE(policy_request.find(R"("funding_limit_cinnabar":"100000000")"), std::string::npos);
    BOOST_CHECK_NE(policy_request.find(R"("funding_available_cinnabar":"100000000")"), std::string::npos);
    BOOST_CHECK_NE(policy_request.find(R"("daily_limit_cinnabar":"50000000")"), std::string::npos);
    BOOST_CHECK_NE(policy_request.find(R"("request_created_time":")"), std::string::npos);
    BOOST_CHECK_NE(policy_request.find(R"("policy_status":"pending_integration")"), std::string::npos);
    BOOST_CHECK_NE(policy_request.find(R"("backend_created":false)"), std::string::npos);
    auto validated_policy_request = vault.ValidateAgentAllotmentPolicyRequest(policy_request);
    BOOST_REQUIRE(validated_policy_request);
    BOOST_CHECK_EQUAL(validated_policy_request->id, first->id);
    BOOST_CHECK_EQUAL(validated_policy_request->label, first->label);
    BOOST_CHECK_EQUAL(validated_policy_request->funding_address, first->funding_address);
    BOOST_CHECK_EQUAL(validated_policy_request->funding_limit, first->funding_limit);
    BOOST_CHECK_EQUAL(validated_policy_request->funding_available, COIN);
    BOOST_CHECK_EQUAL(validated_policy_request->daily_limit, first->daily_limit);
    BOOST_CHECK_EQUAL(validated_policy_request->risk_accepted_time, first->risk_accepted_time);
    BOOST_CHECK_GT(validated_policy_request->request_created_time, 0);
    BOOST_CHECK(!validated_policy_request->backend_created);
    BOOST_CHECK(validated_policy_request->policy_status == AgentAllotmentPolicyStatus::PendingIntegration);
    auto policy_bundle = vault.ExportAgentAllotmentPolicyBundle(policy_request);
    BOOST_REQUIRE_MESSAGE(policy_bundle, util::ErrorString(policy_bundle).original);
    BOOST_CHECK_EQUAL(policy_bundle->metadata.id, first->id);
    BOOST_CHECK_EQUAL(policy_bundle->metadata.funding_address, first->funding_address);
    BOOST_CHECK_EQUAL(policy_bundle->policy_request, policy_request);
    BOOST_CHECK(!policy_bundle->funding_secret.empty());
    CKey funding_secret{DecodeSecret(policy_bundle->funding_secret)};
    BOOST_REQUIRE(funding_secret.IsValid());
    const CKeyID funding_secret_id{funding_secret.GetPubKey().GetID()};
    const CTxDestination funding_dest{DecodeDestination(first->funding_address)};
    bool funding_secret_matches{false};
    if (const auto* pkhash{std::get_if<PKHash>(&funding_dest)}) {
        funding_secret_matches = ToKeyID(*pkhash) == funding_secret_id;
    } else if (const auto* witness_hash{std::get_if<WitnessV0KeyHash>(&funding_dest)}) {
        funding_secret_matches = ToKeyID(*witness_hash) == funding_secret_id;
    }
    BOOST_CHECK(funding_secret_matches);
    UniValue bundle_json{UniValue::VOBJ};
    BOOST_REQUIRE(bundle_json.read(policy_bundle->bundle_json));
    BOOST_CHECK_EQUAL(bundle_json.find_value("type").get_str(), "quicksilver.agent_allotment_key_bundle");
    BOOST_CHECK_EQUAL(bundle_json.find_value("version").getInt<int>(), 1);
    BOOST_CHECK(bundle_json.find_value("policy_request").isObject());
    BOOST_CHECK_EQUAL(bundle_json.find_value("funding_address").get_str(), first->funding_address);
    BOOST_CHECK_EQUAL(bundle_json.find_value("funding_secret_wif").get_str(), policy_bundle->funding_secret);
    const UniValue& bundle_funding_outputs{bundle_json.find_value("funding_outputs")};
    BOOST_REQUIRE(bundle_funding_outputs.isArray());
    BOOST_CHECK_EQUAL(policy_bundle->funding_outputs.size(), bundle_funding_outputs.size());
    BOOST_CHECK_EQUAL(bundle_json.find_value("policy_enforcement").get_str(), "pending_integration");

    auto replace_first = [](std::string value, const std::string& from, const std::string& to) {
        const size_t pos{value.find(from)};
        BOOST_REQUIRE_NE(pos, std::string::npos);
        value.replace(pos, from.size(), to);
        return value;
    };
    BOOST_CHECK(!vault.ValidateAgentAllotmentPolicyRequest("[]"));
    BOOST_CHECK(!vault.ValidateAgentAllotmentPolicyRequest(replace_first(policy_request, R"("version":1)", R"("version":2)")));
    BOOST_CHECK(!vault.ValidateAgentAllotmentPolicyRequest(replace_first(
        policy_request,
        strprintf(R"("chain":"%s")", Params().GetChainTypeString()),
        R"("chain":"wrong-chain")")));
    BOOST_CHECK(!vault.ValidateAgentAllotmentPolicyRequest(replace_first(
        policy_request,
        strprintf(R"("genesis_hash":"%s")", Params().GenesisBlock().GetHash().ToString()),
        R"("genesis_hash":"00")")));
    BOOST_CHECK(!vault.ValidateAgentAllotmentPolicyRequest(replace_first(
        policy_request,
        strprintf(R"("funding_address":"%s")", first->funding_address),
        R"("funding_address":"not-a-quicksilver-address")")));
    BOOST_CHECK(!vault.ValidateAgentAllotmentPolicyRequest(replace_first(
        policy_request,
        R"("funding_limit_cinnabar":"100000000")",
        R"("funding_limit_cinnabar":"-1")")));
    BOOST_CHECK(!vault.ValidateAgentAllotmentPolicyRequest(replace_first(
        policy_request,
        R"("id":"agent-1")",
        R"("id":"agent-unknown")")));
    BOOST_CHECK(!vault.ExportAgentAllotmentPolicyBundle(replace_first(
        policy_request,
        R"("id":"agent-1")",
        R"("id":"agent-unknown")")));
    BOOST_CHECK(!vault.ValidateAgentAllotmentPolicyRequest(replace_first(
        policy_request,
        R"("daily_limit_cinnabar":"50000000")",
        R"("daily_limit_cinnabar":"25000000")")));
    {
        LOCK(vault.cs_vault);
        const CTxDestination dest{DecodeDestination(first->funding_address)};
        const CAddressBookData* entry{vault.FindAddressBookEntry(dest)};
        BOOST_REQUIRE(entry);
        BOOST_CHECK_EQUAL(entry->GetLabel(), "agent:test-agent");
        BOOST_REQUIRE(entry->purpose);
        BOOST_CHECK(*entry->purpose == vault::AddressPurpose::RECEIVE);
        BOOST_CHECK(vault.IsMine(dest) & vault::ISMINE_SPENDABLE);
    }

    auto second = vault.RecordAgentAllotmentSetup("second-agent", 2 * COIN, 0);
    BOOST_REQUIRE(second);
    BOOST_CHECK_EQUAL(second->id, "agent-2");
    BOOST_CHECK(IsValidDestinationString(second->funding_address));
    BOOST_CHECK_NE(second->funding_address, first->funding_address);

    const auto records = vault.ListAgentAllotmentRecords();
    BOOST_REQUIRE_EQUAL(records.size(), 2U);
    BOOST_CHECK_EQUAL(records[0].label, "test-agent");
    BOOST_CHECK_EQUAL(records[0].funding_address, first->funding_address);
    BOOST_CHECK(records[0].policy_status == AgentAllotmentPolicyStatus::PendingIntegration);
    BOOST_CHECK_EQUAL(records[1].label, "second-agent");
    BOOST_CHECK_EQUAL(records[1].funding_address, second->funding_address);
    BOOST_CHECK(records[1].policy_status == AgentAllotmentPolicyStatus::PendingIntegration);

}

void TestLoadVault(const std::string& name, DatabaseFormat format, std::function<void(std::shared_ptr<CVault>)> f)
{
    node::NodeContext node;
    auto chain{interfaces::MakeChain(node)};
    DatabaseOptions options;
    options.require_format = format;
    DatabaseStatus status;
    bilingual_str error;
    std::vector<bilingual_str> warnings;
    auto database{MakeVaultDatabase(name, options, status, error)};
    auto vault{std::make_shared<CVault>(chain.get(), "", std::move(database))};
    BOOST_CHECK_EQUAL(vault->LoadVault(), DBErrors::LOAD_OK);
    WITH_LOCK(vault->cs_vault, f(vault));
}

BOOST_FIXTURE_TEST_CASE(LoadReceiveRequests, TestingSetup)
{
    for (DatabaseFormat format : DATABASE_FORMATS) {
        const std::string name{strprintf("receive-requests-%i", format)};
        TestLoadVault(name, format, [](std::shared_ptr<CVault> vault) EXCLUSIVE_LOCKS_REQUIRED(vault->cs_vault) {
            BOOST_CHECK(!vault->IsAddressPreviouslySpent(PKHash()));
            VaultBatch batch{vault->GetDatabase()};
            BOOST_CHECK(batch.WriteAddressPreviouslySpent(PKHash(), true));
            BOOST_CHECK(batch.WriteAddressPreviouslySpent(ScriptHash(), true));
            BOOST_CHECK(vault->SetAddressReceiveRequest(batch, PKHash(), "0", "val_rr00"));
            BOOST_CHECK(vault->EraseAddressReceiveRequest(batch, PKHash(), "0"));
            BOOST_CHECK(vault->SetAddressReceiveRequest(batch, PKHash(), "1", "val_rr10"));
            BOOST_CHECK(vault->SetAddressReceiveRequest(batch, PKHash(), "1", "val_rr11"));
            BOOST_CHECK(vault->SetAddressReceiveRequest(batch, ScriptHash(), "2", "val_rr20"));
        });
        TestLoadVault(name, format, [](std::shared_ptr<CVault> vault) EXCLUSIVE_LOCKS_REQUIRED(vault->cs_vault) {
            BOOST_CHECK(vault->IsAddressPreviouslySpent(PKHash()));
            BOOST_CHECK(vault->IsAddressPreviouslySpent(ScriptHash()));
            auto requests = vault->GetAddressReceiveRequests();
            auto erequests = {"val_rr11", "val_rr20"};
            BOOST_CHECK_EQUAL_COLLECTIONS(requests.begin(), requests.end(), std::begin(erequests), std::end(erequests));
            RunWithinTxn(vault->GetDatabase(), /*process_desc*/ "test", [](VaultBatch& batch) {
                BOOST_CHECK(batch.WriteAddressPreviouslySpent(PKHash(), false));
                BOOST_CHECK(batch.EraseAddressData(ScriptHash()));
                return true;
            });
        });
        TestLoadVault(name, format, [](std::shared_ptr<CVault> vault) EXCLUSIVE_LOCKS_REQUIRED(vault->cs_vault) {
            BOOST_CHECK(!vault->IsAddressPreviouslySpent(PKHash()));
            BOOST_CHECK(!vault->IsAddressPreviouslySpent(ScriptHash()));
            auto requests = vault->GetAddressReceiveRequests();
            auto erequests = {"val_rr11"};
            BOOST_CHECK_EQUAL_COLLECTIONS(requests.begin(), requests.end(), std::begin(erequests), std::end(erequests));
        });
    }
}

class ListCoinsTestingSetup : public TestChain100Setup
{
public:
    ListCoinsTestingSetup()
    {
        CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
        vault = CreateSyncedVault(*m_node.chain, WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain()), coinbaseKey);
    }

    ~ListCoinsTestingSetup()
    {
        vault.reset();
    }

    CVaultTx& AddTx(CRecipient recipient)
    {
        CTransactionRef tx;
        CCoinControl dummy;
        {
            auto res = CreateTransaction(*vault, {recipient}, /*change_pos=*/std::nullopt, dummy);
            BOOST_CHECK(res);
            tx = res->tx;
        }
        vault->CommitTransaction(tx, {}, {});
        CMutableTransaction blocktx;
        {
            LOCK(vault->cs_vault);
            blocktx = CMutableTransaction(*vault->mapVault.at(tx->GetHash()).tx);
        }
        CreateAndProcessBlock({CMutableTransaction(blocktx)}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));

        LOCK(vault->cs_vault);
        LOCK(Assert(m_node.chainman)->GetMutex());
        vault->SetLastBlockProcessed(vault->GetLastBlockHeight() + 1, m_node.chainman->ActiveChain().Tip()->GetBlockHash());
        auto it = vault->mapVault.find(tx->GetHash());
        BOOST_CHECK(it != vault->mapVault.end());
        it->second.m_state = TxStateConfirmed{m_node.chainman->ActiveChain().Tip()->GetBlockHash(), m_node.chainman->ActiveChain().Height(), /*index=*/1};
        return it->second;
    }

    std::unique_ptr<CVault> vault;
};

BOOST_FIXTURE_TEST_CASE(ListCoinsTest, ListCoinsTestingSetup)
{
    std::string coinbaseAddress = coinbaseKey.GetPubKey().GetID().ToString();

    // Confirm ListCoins initially returns 1 coin grouped under coinbaseKey
    // address.
    std::map<CTxDestination, std::vector<COutput>> list;
    {
        LOCK(vault->cs_vault);
        list = ListCoins(*vault);
    }
    BOOST_CHECK_EQUAL(list.size(), 1U);
    BOOST_CHECK_EQUAL(std::get<PKHash>(list.begin()->first).ToString(), coinbaseAddress);
    BOOST_CHECK_EQUAL(list.begin()->second.size(), 1U);

    // Check initial balance from one mature coinbase transaction.
    BOOST_CHECK_EQUAL(m_coinbase_txns[0]->vout[0].nValue, WITH_LOCK(vault->cs_vault, return AvailableCoins(*vault).GetTotalAmount()));

    // Add a transaction creating a change address, and confirm ListCoins still
    // returns the coin associated with the change address underneath the
    // coinbaseKey pubkey, even though the change address has a different
    // pubkey.
    AddTx(CRecipient{PubKeyDestination{{}}, 1 * COIN});
    {
        LOCK(vault->cs_vault);
        list = ListCoins(*vault);
    }
    BOOST_CHECK_EQUAL(list.size(), 1U);
    BOOST_CHECK_EQUAL(std::get<PKHash>(list.begin()->first).ToString(), coinbaseAddress);
    BOOST_CHECK_EQUAL(list.begin()->second.size(), 2U);

    // Lock both coins. Confirm number of available coins drops to 0.
    {
        LOCK(vault->cs_vault);
        BOOST_CHECK_EQUAL(AvailableCoins(*vault).Size(), 2U);
    }
    for (const auto& group : list) {
        for (const auto& coin : group.second) {
            LOCK(vault->cs_vault);
            vault->LockCoin(coin.outpoint);
        }
    }
    {
        LOCK(vault->cs_vault);
        BOOST_CHECK_EQUAL(AvailableCoins(*vault).Size(), 0U);
    }
    // Confirm ListCoins still returns same result as before, despite coins
    // being locked.
    {
        LOCK(vault->cs_vault);
        list = ListCoins(*vault);
    }
    BOOST_CHECK_EQUAL(list.size(), 1U);
    BOOST_CHECK_EQUAL(std::get<PKHash>(list.begin()->first).ToString(), coinbaseAddress);
    BOOST_CHECK_EQUAL(list.begin()->second.size(), 2U);
}

void TestCoinsResult(ListCoinsTest& context, OutputType out_type, CAmount amount,
                     std::map<OutputType, size_t>& expected_coins_sizes)
{
    LOCK(context.vault->cs_vault);
    util::Result<CTxDestination> dest = Assert(context.vault->GetNewDestination(out_type, ""));
    CVaultTx& wtx = context.AddTx(CRecipient{*dest, amount});
    CoinFilterParams filter;
    filter.skip_locked = false;
    CoinsResult available_coins = AvailableCoins(*context.vault, nullptr, filter);
    // Lock outputs so they are not spent in follow-up transactions
    for (uint32_t i = 0; i < wtx.tx->vout.size(); i++)
        context.vault->LockCoin({wtx.GetHash(), i});
    for (const auto& [type, size] : expected_coins_sizes)
        BOOST_CHECK_EQUAL(size, available_coins.coins[type].size());
}

BOOST_FIXTURE_TEST_CASE(BasicOutputTypesTest, ListCoinsTest)
{
    std::map<OutputType, size_t> expected_coins_sizes;
    for (const auto& out_type : OUTPUT_TYPES) {
        expected_coins_sizes[out_type] = 0U;
    }

    // Verify our vault has one usable coinbase UTXO before starting
    // This UTXO is a P2PK, so it should show up in the Other bucket
    expected_coins_sizes[OutputType::UNKNOWN] = 1U;
    CoinsResult available_coins = WITH_LOCK(vault->cs_vault, return AvailableCoins(*vault));
    BOOST_CHECK_EQUAL(available_coins.Size(), expected_coins_sizes[OutputType::UNKNOWN]);
    BOOST_CHECK_EQUAL(available_coins.coins[OutputType::UNKNOWN].size(), expected_coins_sizes[OutputType::UNKNOWN]);

    // We will create a self transfer for each of the OutputTypes and
    // verify it is put in the correct bucket after running GetAvailablecoins
    //
    // For each OutputType, We expect 2 UTXOs in our vault following the self transfer:
    //   1. One UTXO as the recipient
    //   2. One UTXO from the change, due to payment address matching logic

    for (const auto& out_type : OUTPUT_TYPES) {
        if (out_type == OutputType::UNKNOWN) continue;
        expected_coins_sizes[out_type] = 2U;
        TestCoinsResult(*this, out_type, 1 * COIN, expected_coins_sizes);
    }
}

// A thin vault cannot rescan, so the archive is the only thing standing between a lost
// vault file and lost history. See doc/design/vault-backup.md.
BOOST_FIXTURE_TEST_CASE(vault_archive_round_trip, ListCoinsTestingSetup)
{
    const fs::path archive_path{m_args.GetDataDirNet() / "history.qsva"};
    const SecureString passphrase{"correct horse battery staple"};

    auto dest{vault->GetNewDestination(OutputType::BECH32, "archived")};
    BOOST_REQUIRE(dest);
    AddTx(CRecipient{*dest, COIN / 8});

    auto archive{BuildVaultArchive(*vault)};
    BOOST_REQUIRE_MESSAGE(archive, util::ErrorString(archive).original);
    BOOST_CHECK(!archive->descriptors.empty());
    BOOST_CHECK(!archive->transactions.empty());
    // The address book distinguishes a payment from change, so it is history too.
    BOOST_CHECK(!archive->addresses.empty());
    BOOST_CHECK_EQUAL(archive->chain, Params().GetChainTypeString());
    // The archive must never carry spending material: that is the vault-file backup's
    // job, and a history backup that also unlocks the money is a different, worse artifact.
    for (const VaultArchiveDescriptor& desc : archive->descriptors) {
        BOOST_CHECK_EQUAL(desc.descriptor.find("xprv"), std::string::npos);
        BOOST_CHECK_EQUAL(desc.descriptor.find("qprv"), std::string::npos);
        BOOST_CHECK_EQUAL(desc.descriptor.find("tqrv"), std::string::npos);
    }
    const size_t exported_transactions{archive->transactions.size()};

    BOOST_REQUIRE(WriteVaultArchive(*archive, archive_path, passphrase));
    BOOST_CHECK(fs::exists(archive_path));

    // A wrong passphrase is reported as a wrong passphrase, not as a corrupt file.
    auto wrong{ReadVaultArchive(archive_path, SecureString{"correct horse battery stapl"})};
    BOOST_CHECK(!wrong);
    BOOST_CHECK_NE(util::ErrorString(wrong).original.find("passphrase is incorrect"), std::string::npos);

    auto reread{ReadVaultArchive(archive_path, passphrase)};
    BOOST_REQUIRE_MESSAGE(reread, util::ErrorString(reread).original);
    BOOST_CHECK_EQUAL(reread->transactions.size(), exported_transactions);
    BOOST_CHECK_EQUAL(reread->descriptors.size(), archive->descriptors.size());
    BOOST_CHECK_EQUAL(reread->chain, archive->chain);
    BOOST_CHECK(reread->genesis_hash == archive->genesis_hash);
    BOOST_CHECK_EQUAL(reread->birth_time, archive->birth_time);
    for (size_t i = 0; i < exported_transactions; ++i) {
        BOOST_CHECK(reread->transactions[i].tx->GetHash() == archive->transactions[i].tx->GetHash());
        BOOST_CHECK_EQUAL(reread->transactions[i].confirmed, archive->transactions[i].confirmed);
        BOOST_CHECK_EQUAL(reread->transactions[i].block_height, archive->transactions[i].block_height);
        BOOST_CHECK_EQUAL(reread->transactions[i].time_received, archive->transactions[i].time_received);
    }

    // Restoring into a fresh vault reproduces the history.
    CVault restored(m_node.chain.get(), "restored", CreateMockableVaultDatabase());
    {
        LOCK(restored.cs_vault);
        restored.SetVaultFlag(VAULT_FLAG_DESCRIPTORS);
    }
    auto imported{ImportVaultArchive(restored, *reread)};
    BOOST_REQUIRE_MESSAGE(imported, util::ErrorString(imported).original);
    BOOST_CHECK_EQUAL(imported->transactions_imported, exported_transactions);
    BOOST_CHECK_EQUAL(imported->transactions_skipped, 0U);
    BOOST_CHECK_EQUAL(imported->descriptors_imported, archive->descriptors.size());
    BOOST_CHECK_EQUAL(imported->addresses_imported, archive->addresses.size());
    BOOST_CHECK_EQUAL(WITH_LOCK(restored.cs_vault, return restored.mapVault.size()), exported_transactions);

    // Importing the same archive again must change nothing rather than duplicate history.
    auto reimported{ImportVaultArchive(restored, *reread)};
    BOOST_REQUIRE_MESSAGE(reimported, util::ErrorString(reimported).original);
    BOOST_CHECK_EQUAL(reimported->transactions_imported, 0U);
    BOOST_CHECK_EQUAL(reimported->transactions_skipped, exported_transactions);
    BOOST_CHECK_EQUAL(reimported->descriptors_imported, 0U);
    BOOST_CHECK_EQUAL(WITH_LOCK(restored.cs_vault, return restored.m_address_book.size()),
                      archive->addresses.size());
    BOOST_CHECK_EQUAL(WITH_LOCK(restored.cs_vault, return restored.mapVault.size()), exported_transactions);
}

// An archive names the chain it came from, so history cannot be restored onto the wrong
// network -- where the block heights it carries would refer to different blocks.
BOOST_FIXTURE_TEST_CASE(vault_archive_rejects_foreign_chain, ListCoinsTestingSetup)
{
    auto archive{BuildVaultArchive(*vault)};
    BOOST_REQUIRE_MESSAGE(archive, util::ErrorString(archive).original);

    archive->chain = "some-other-network";

    CVault restored(m_node.chain.get(), "restored", CreateMockableVaultDatabase());
    {
        LOCK(restored.cs_vault);
        restored.SetVaultFlag(VAULT_FLAG_DESCRIPTORS);
    }
    auto imported{ImportVaultArchive(restored, *archive)};
    BOOST_CHECK(!imported);
    BOOST_CHECK_NE(util::ErrorString(imported).original.find("some-other-network"), std::string::npos);
}

// An archive encodes "purpose was never recorded" as 0 (see VaultArchiveAddress), so an
// import can be handed a labelled address with no purpose. Restoring it must classify the
// address rather than refuse the archive: purpose is what tells one of our own receiving
// addresses from somebody we paid, and the address book has no second source for it.
BOOST_FIXTURE_TEST_CASE(vault_archive_restores_address_without_purpose, ListCoinsTestingSetup)
{
    auto mine{vault->GetNewDestination(OutputType::BECH32, "ours")};
    BOOST_REQUIRE(mine);
    const CTxDestination theirs{PKHash(GenerateRandomKey().GetPubKey())};
    BOOST_REQUIRE(vault->SetAddressBook(theirs, "payee", AddressPurpose::SEND));

    auto archive{BuildVaultArchive(*vault)};
    BOOST_REQUIRE_MESSAGE(archive, util::ErrorString(archive).original);

    // Strip the purpose from both, exactly as an archive written before the address ever
    // had one carries it.
    const std::string mine_encoded{EncodeDestination(*mine)};
    const std::string theirs_encoded{EncodeDestination(theirs)};
    size_t stripped{0};
    for (VaultArchiveAddress& addr : archive->addresses) {
        if (addr.destination == mine_encoded || addr.destination == theirs_encoded) {
            BOOST_REQUIRE(addr.has_label);
            addr.purpose = 0;
            ++stripped;
        }
    }
    BOOST_REQUIRE_EQUAL(stripped, 2U);

    CVault restored(m_node.chain.get(), "restored", CreateMockableVaultDatabase());
    {
        LOCK(restored.cs_vault);
        restored.SetVaultFlag(VAULT_FLAG_DESCRIPTORS);
    }
    auto imported{ImportVaultArchive(restored, *archive)};
    BOOST_REQUIRE_MESSAGE(imported, util::ErrorString(imported).original);
    BOOST_CHECK_EQUAL(imported->addresses_imported, archive->addresses.size());

    LOCK(restored.cs_vault);
    const CAddressBookData* ours{restored.FindAddressBookEntry(*mine, /*allow_change=*/false)};
    BOOST_REQUIRE(ours);
    BOOST_REQUIRE(ours->purpose);
    BOOST_CHECK(*ours->purpose == AddressPurpose::RECEIVE);

    const CAddressBookData* payee{restored.FindAddressBookEntry(theirs, /*allow_change=*/false)};
    BOOST_REQUIRE(payee);
    BOOST_REQUIRE(payee->purpose);
    BOOST_CHECK(*payee->purpose == AddressPurpose::SEND);
}

// Exporting an agent bundle hands the agent the funding key AND the list of outputs it
// may spend. Both sides can sign for those outputs from that moment on, so the vault has
// to stop selecting them: a collision does not double-spend, but it does throw away a
// full transaction grind. See doc/design/agent-client.md.
BOOST_FIXTURE_TEST_CASE(agent_bundle_export_locks_funding_outputs, ListCoinsTestingSetup)
{
    auto record{vault->RecordAgentAllotmentSetup("locking-agent", COIN, COIN / 2)};
    BOOST_REQUIRE(record);
    const CTxDestination funding_dest{DecodeDestination(record->funding_address)};
    BOOST_REQUIRE(IsValidDestination(funding_dest));
    const CScript funding_script{GetScriptForDestination(funding_dest)};

    AddTx(CRecipient{funding_dest, COIN / 4});

    std::vector<COutPoint> funding_outpoints;
    {
        LOCK(vault->cs_vault);
        for (const COutput& coin : AvailableCoins(*vault).All()) {
            if (coin.txout.scriptPubKey == funding_script) funding_outpoints.push_back(coin.outpoint);
        }
        BOOST_REQUIRE(!funding_outpoints.empty());
        for (const COutPoint& outpoint : funding_outpoints) {
            BOOST_CHECK(!vault->IsLockedCoin(outpoint));
        }
    }

    const std::string policy_request{vault->AgentAllotmentPolicyRequest(*record, COIN / 4)};
    auto policy_bundle{vault->ExportAgentAllotmentPolicyBundle(policy_request)};
    BOOST_REQUIRE_MESSAGE(policy_bundle, util::ErrorString(policy_bundle).original);
    BOOST_CHECK_EQUAL(policy_bundle->funding_outputs.size(), funding_outpoints.size());

    {
        LOCK(vault->cs_vault);
        for (const COutPoint& outpoint : funding_outpoints) {
            BOOST_CHECK(vault->IsLockedCoin(outpoint));
        }
        // The point of the lock: ordinary coin selection can no longer reach them.
        for (const COutput& coin : AvailableCoins(*vault).All()) {
            BOOST_CHECK(coin.txout.scriptPubKey != funding_script);
        }
    }

    // Re-exporting must still enumerate the agent's outputs, or handing the same agent a
    // refreshed bundle would silently tell it that it has nothing to spend.
    auto second_bundle{vault->ExportAgentAllotmentPolicyBundle(policy_request)};
    BOOST_REQUIRE_MESSAGE(second_bundle, util::ErrorString(second_bundle).original);
    BOOST_CHECK_EQUAL(second_bundle->funding_outputs.size(), funding_outpoints.size());
}

// The lock takes the agent's outputs out of coin selection, so the balance reported as
// spendable has to drop by exactly that much and reappear as delegated. Reporting them as
// available would show a figure that a send then refuses to honour.
// See doc/design/agent-client.md.
BOOST_FIXTURE_TEST_CASE(agent_bundle_export_moves_balance_to_delegated, ListCoinsTestingSetup)
{
    constexpr CAmount funding_amount{COIN / 4};

    auto record{vault->RecordAgentAllotmentSetup("delegating-agent", COIN, COIN / 2)};
    BOOST_REQUIRE(record);
    const CTxDestination funding_dest{DecodeDestination(record->funding_address)};
    BOOST_REQUIRE(IsValidDestination(funding_dest));

    AddTx(CRecipient{funding_dest, funding_amount});

    const Balance before{GetBalance(*vault)};
    BOOST_CHECK_EQUAL(before.m_mine_delegated, 0);

    const std::string policy_request{vault->AgentAllotmentPolicyRequest(*record, funding_amount)};
    auto policy_bundle{vault->ExportAgentAllotmentPolicyBundle(policy_request)};
    BOOST_REQUIRE_MESSAGE(policy_bundle, util::ErrorString(policy_bundle).original);

    const Balance after{GetBalance(*vault)};
    BOOST_CHECK_EQUAL(after.m_mine_delegated, funding_amount);
    BOOST_CHECK_EQUAL(after.m_mine_trusted, before.m_mine_trusted - funding_amount);
    // Nothing left the vault, so what it owns in total is unchanged. Quicksilver is
    // feeless, so a self-send moves the money without shrinking it.
    BOOST_CHECK_EQUAL(after.m_mine_trusted + after.m_mine_delegated, before.m_mine_trusted);
}

BOOST_FIXTURE_TEST_CASE(vault_disableprivkeys, TestChain100Setup)
{
    const std::shared_ptr<CVault> vault = std::make_shared<CVault>(m_node.chain.get(), "", CreateMockableVaultDatabase());
    LOCK(vault->cs_vault);
    vault->SetVaultFlag(VAULT_FLAG_DESCRIPTORS);
    vault->SetMinVersion();
    vault->SetVaultFlag(VAULT_FLAG_DISABLE_PRIVATE_KEYS);
    BOOST_CHECK(!vault->GetNewDestination(OutputType::BECH32, ""));
}

bool malformed_descriptor(std::ios_base::failure e)
{
    std::string s(e.what());
    return s.find("Missing checksum") != std::string::npos;
}

BOOST_FIXTURE_TEST_CASE(vault_descriptor_test, BasicTestingSetup)
{
    std::vector<unsigned char> malformed_record;
    VectorWriter vw{malformed_record, 0};
    vw << std::string("notadescriptor");
    vw << uint64_t{0};
    vw << int32_t{0};
    vw << int32_t{0};
    vw << int32_t{1};

    SpanReader vr{malformed_record};
    VaultDescriptor w_desc;
    BOOST_CHECK_EXCEPTION(vr >> w_desc, std::ios_base::failure, malformed_descriptor);
}

//! Test CVault::Create() and its behavior handling potential race
//! conditions if it's called the same time an incoming transaction shows up in
//! the relaypool or a new block.
//!
//! It isn't possible to verify there aren't race condition in every case, so
//! this test just checks two specific cases and ensures that timing of
//! notifications in these cases doesn't prevent the vault from detecting
//! transactions.
//!
//! In the first case, block and relaypool transactions are created before the
//! vault is loaded, but notifications about these transactions are delayed
//! until after it is loaded. The notifications are superfluous in this case, so
//! the test verifies the transactions are detected before they arrive.
//!
//! In the second case, block and relaypool transactions are created after the
//! vault rescan and notifications are immediately synced, to verify the vault
//! must already have a handler in place for them, and there's no gap after
//! rescanning where new transactions in new blocks could be lost.
BOOST_FIXTURE_TEST_CASE(CreateVault, VaultTxPowTestSetup)
{
    m_args.ForceSetArg("-unsafesqlitesync", "1");
    // Create new vault with known key and unload it.
    VaultContext context;
    context.args = &m_args;
    context.chain = m_node.chain.get();
    auto vault = TestLoadVault(context);
    CKey key = GenerateRandomKey();
    AddKey(*vault, key);
    TestUnloadVault(std::move(vault));


    // Add log hook to detect AddToVault events from rescans, blockConnected,
    // and transactionAddedToRelayPool notifications
    int addtx_count = 0;
    DebugLogHelper addtx_counter("saved tx=", [&](const std::string* s) {
        if (s && s->find(" vault=default") != std::string::npos) ++addtx_count;
        return false;
    });


    bool rescan_completed = false;
    DebugLogHelper rescan_check("rescanned ok=1", [&](const std::string* s) {
        if (s && s->find(" vault=default") != std::string::npos) rescan_completed = true;
        return false;
    });


    // Block the queue to prevent the vault receiving blockConnected and
    // transactionAddedToRelayPool notifications, and create block and relaypool
    // transactions paying to the vault
    std::promise<void> promise;
    m_node.validation_signals->CallFunctionInValidationInterfaceQueue([&promise] {
        promise.get_future().wait();
    });
    std::string error;
    m_coinbase_txns.push_back(CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey())).vtx[0]);
    auto block_tx = TestSimpleSpend(*m_coinbase_txns[0], 0, coinbaseKey, GetScriptForRawPubKey(key.GetPubKey()));
    ProveTxPowAtTip(block_tx);
    m_coinbase_txns.push_back(CreateAndProcessBlock({block_tx}, GetScriptForRawPubKey(coinbaseKey.GetPubKey())).vtx[0]);
    auto relaypool_tx = TestSimpleSpend(*m_coinbase_txns[1], 0, coinbaseKey, GetScriptForRawPubKey(key.GetPubKey()));
    ProveTxPowAtTip(relaypool_tx);
    BOOST_CHECK_EQUAL(m_node.chain->broadcastTransaction(MakeTransactionRef(relaypool_tx), false, error), node::TransactionError::OK);


    // Reload vault and make sure new transactions are detected despite events
    // being blocked
    // Loading will also ask for current relaypool transactions
    vault = TestLoadVault(context);
    BOOST_CHECK(rescan_completed);
    // AddToVault events for block_tx and relaypool_tx (x2)
    BOOST_CHECK_EQUAL(addtx_count, 3);
    {
        LOCK(vault->cs_vault);
        BOOST_CHECK_EQUAL(vault->mapVault.count(block_tx.GetHash()), 1U);
        BOOST_CHECK_EQUAL(vault->mapVault.count(relaypool_tx.GetHash()), 1U);
    }


    // Unblock notification queue and make sure stale blockConnected and
    // transactionAddedToRelayPool events are processed
    promise.set_value();
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    // AddToVault events for block_tx and relaypool_tx events are counted a
    // second time as the notification queue is processed
    BOOST_CHECK_EQUAL(addtx_count, 5);


    TestUnloadVault(std::move(vault));


    // Load vault again, this time creating new block and relaypool transactions
    // paying to the vault as the vault finishes loading and syncing the
    // queue so the events have to be handled immediately. Releasing the vault
    // lock during the sync is a little artificial but is needed to avoid a
    // deadlock during the sync and simulates a new block notification happening
    // as soon as possible.
    addtx_count = 0;
    auto handler = HandleLoadVault(context, [&](std::unique_ptr<interfaces::Vault> vault) {
        BOOST_CHECK(rescan_completed);
        m_coinbase_txns.push_back(CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey())).vtx[0]);
        block_tx = TestSimpleSpend(*m_coinbase_txns[2], 0, coinbaseKey, GetScriptForRawPubKey(key.GetPubKey()));
        ProveTxPowAtTip(block_tx);
        m_coinbase_txns.push_back(CreateAndProcessBlock({block_tx}, GetScriptForRawPubKey(coinbaseKey.GetPubKey())).vtx[0]);
        relaypool_tx = TestSimpleSpend(*m_coinbase_txns[3], 0, coinbaseKey, GetScriptForRawPubKey(key.GetPubKey()));
        ProveTxPowAtTip(relaypool_tx);
        BOOST_CHECK_EQUAL(m_node.chain->broadcastTransaction(MakeTransactionRef(relaypool_tx), false, error), node::TransactionError::OK);
        m_node.validation_signals->SyncWithValidationInterfaceQueue();
    });
    vault = TestLoadVault(context);
    // Since relaypool transactions are requested at the end of loading, there will
    // be 2 additional AddToVault calls, one from the previous test, and a duplicate for relaypool_tx
    BOOST_CHECK_EQUAL(addtx_count, 2 + 2);
    {
        LOCK(vault->cs_vault);
        BOOST_CHECK_EQUAL(vault->mapVault.count(block_tx.GetHash()), 1U);
        BOOST_CHECK_EQUAL(vault->mapVault.count(relaypool_tx.GetHash()), 1U);
    }


    TestUnloadVault(std::move(vault));
}

BOOST_FIXTURE_TEST_CASE(CreateVaultWithoutChain, BasicTestingSetup)
{
    VaultContext context;
    context.args = &m_args;
    auto vault = TestLoadVault(context);
    BOOST_CHECK(vault);
    WaitForDeleteVault(std::move(vault));
}

/**
 * A vault that files a descriptor under the wrong address type must say so.
 *
 * The active-ScriptPubKeyMan record stores the type as a raw ordinal. If a
 * corrupt registration files a manager under a different type, destination
 * generation must return a useful error instead of throwing through the GUI.
 */
BOOST_FIXTURE_TEST_CASE(get_new_destination_reports_a_misfiled_descriptor, TestingSetup)
{
    CVault vault(m_node.chain.get(), "", CreateMockableVaultDatabase());
    {
        LOCK(vault.cs_vault);
        vault.SetVaultFlag(VAULT_FLAG_DESCRIPTORS);
        vault.SetupDescriptorScriptPubKeyMans();
    }

    // Sanity: as set up, both types serve their own addresses.
    BOOST_REQUIRE(vault.GetNewDestination(OutputType::BECH32, ""));
    BOOST_REQUIRE(vault.GetNewDestination(OutputType::BECH32M, ""));

    ScriptPubKeyMan* bech32m{vault.GetScriptPubKeyMan(OutputType::BECH32M, /*internal=*/false)};
    BOOST_REQUIRE(bech32m);
    // Re-file it under BECH32 to model a corrupt active-manager registration.
    WITH_LOCK(vault.cs_vault, vault.LoadActiveScriptPubKeyMan(bech32m->GetID(), OutputType::BECH32, /*internal=*/false));

    const auto misfiled{vault.GetNewDestination(OutputType::BECH32, "")};
    BOOST_CHECK(!misfiled);
    // The message has to name both types because "inconsistent" alone does not
    // identify the damaged registration.
    const std::string message{util::ErrorString(misfiled).original};
    BOOST_CHECK_NE(message.find("bech32"), std::string::npos);
    BOOST_CHECK_NE(message.find("bech32m"), std::string::npos);
    BOOST_CHECK_NE(message.find("numbered address types differently"), std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(RemoveTxs, VaultTxPowTestSetup)
{
    m_args.ForceSetArg("-unsafesqlitesync", "1");
    VaultContext context;
    context.args = &m_args;
    context.chain = m_node.chain.get();
    auto vault = TestLoadVault(context);
    CKey key = GenerateRandomKey();
    AddKey(*vault, key);

    std::string error;
    m_coinbase_txns.push_back(CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey())).vtx[0]);
    auto block_tx = TestSimpleSpend(*m_coinbase_txns[0], 0, coinbaseKey, GetScriptForRawPubKey(key.GetPubKey()));
    ProveTxPowAtTip(block_tx);
    CreateAndProcessBlock({block_tx}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));

    m_node.validation_signals->SyncWithValidationInterfaceQueue();

    {
        auto block_hash = block_tx.GetHash();
        auto prev_tx = m_coinbase_txns[0];

        LOCK(vault->cs_vault);
        BOOST_CHECK(vault->HasVaultSpend(prev_tx));
        BOOST_CHECK_EQUAL(vault->mapVault.count(block_hash), 1u);

        std::vector<uint256> vHashIn{block_hash};
        BOOST_CHECK(vault->RemoveTxs(vHashIn));

        BOOST_CHECK(!vault->HasVaultSpend(prev_tx));
        BOOST_CHECK_EQUAL(vault->mapVault.count(block_hash), 0u);
    }

    TestUnloadVault(std::move(vault));
}

/**
 * Checks a vault invalid state where the inputs (prev-txs) of a new arriving transaction are not marked dirty,
 * while the transaction that spends them exist inside the in-memory vault tx map (not stored on db due a db write failure).
 */
BOOST_FIXTURE_TEST_CASE(vault_sync_tx_invalid_state_test, TestingSetup)
{
    CVault vault(m_node.chain.get(), "", CreateMockableVaultDatabase());
    {
        LOCK(vault.cs_vault);
        vault.SetVaultFlag(VAULT_FLAG_DESCRIPTORS);
        vault.SetupDescriptorScriptPubKeyMans();
    }

    // Add tx to vault
    const auto op_dest{*Assert(vault.GetNewDestination(OutputType::BECH32M, ""))};

    CMutableTransaction mtx;
    mtx.vout.emplace_back(COIN, GetScriptForDestination(op_dest));
    mtx.vin.emplace_back(Txid::FromUint256(m_rng.rand256()), 0);
    const auto& tx_id_to_spend = vault.AddToVault(MakeTransactionRef(mtx), TxStateInRelayPool{})->GetHash();

    {
        // Cache and verify available balance for the wtx
        LOCK(vault.cs_vault);
        const CVaultTx* wtx_to_spend = vault.GetVaultTx(tx_id_to_spend);
        BOOST_CHECK_EQUAL(CachedTxGetAvailableCredit(vault, *wtx_to_spend), 1 * COIN);
    }

    // Now the good case:
    // 1) Add a transaction that spends the previously created transaction
    // 2) Verify that the available balance of this new tx and the old one is updated (prev tx is marked dirty)

    mtx.vin.clear();
    mtx.vin.emplace_back(tx_id_to_spend, 0);
    vault.transactionAddedToRelayPool(MakeTransactionRef(mtx));
    const auto good_tx_id{mtx.GetHash()};

    {
        // Verify balance update for the new tx and the old one
        LOCK(vault.cs_vault);
        const CVaultTx* new_wtx = vault.GetVaultTx(good_tx_id.ToUint256());
        BOOST_CHECK_EQUAL(CachedTxGetAvailableCredit(vault, *new_wtx), 1 * COIN);

        // Now the old wtx
        const CVaultTx* wtx_to_spend = vault.GetVaultTx(tx_id_to_spend);
        BOOST_CHECK_EQUAL(CachedTxGetAvailableCredit(vault, *wtx_to_spend), 0 * COIN);
    }

    // Now the bad case:
    // 1) Make db always fail
    // 2) Try to add a transaction that spends the previously created transaction and
    //    verify that we are not moving forward if the vault cannot store it
    GetMockableDatabase(vault).m_pass = false;
    mtx.vin.clear();
    mtx.vin.emplace_back(good_tx_id, 0);
    BOOST_CHECK_EXCEPTION(vault.transactionAddedToRelayPool(MakeTransactionRef(mtx)),
                          std::runtime_error,
                          HasReason("DB error adding transaction to vault, write failed"));
}

BOOST_FIXTURE_TEST_CASE(create_vault_without_chainstate, BasicTestingSetup)
{
    // The GUI's bootstrap screen offers Vault as the primary action and Consensus
    // as optional. Taking it at its word leaves m_node.chainman null while the
    // vault still holds a live Chain pointer, and every Chain method asserts on
    // chainman. Creating a vault in that state must succeed, not abort.
    BOOST_REQUIRE(!m_node.chainman);
    std::unique_ptr<interfaces::Chain> chain = interfaces::MakeChain(m_node);
    BOOST_CHECK(!chain->hasChainstate());
    // Every chain-backed query used by vault startup must fail closed while
    // AppInitMain has not installed a ChainstateManager. A load error should be
    // reported to the user, never converted into an Assert(m_node.chainman).
    BOOST_CHECK(!chain->getHeight());
    BOOST_CHECK(chain->getBlockHash(0).IsNull());
    BOOST_CHECK(chain->getTipLocator().IsNull());
    BOOST_CHECK(!chain->findBlock(uint256::ZERO, interfaces::FoundBlock{}));
    BOOST_CHECK(!chain->haveBlockOnDisk(0));
    BOOST_CHECK(!chain->havePruned());
    BOOST_CHECK(!chain->getPruneHeight());
    BOOST_CHECK(!chain->isReadyToBroadcast());
    BOOST_CHECK(chain->isInitialBlockDownload());

    VaultContext context;
    context.chain = chain.get();
    context.args = &m_args;

    bilingual_str error;
    std::vector<bilingual_str> warnings;
    std::shared_ptr<CVault> vault = CVault::Create(
        context, "", CreateMockableVaultDatabase(), VAULT_FLAG_DESCRIPTORS, error, warnings);

    BOOST_REQUIRE_MESSAGE(vault != nullptr, error.original);
    // No chainstate means no birth locator: the vault must later rescan from
    // genesis. That is the documented cost of creating one before consensus.
    //
    // Asserted through TryGetLastBlockProcessed rather than GetLastBlockHash:
    // the latter asserts m_last_block_processed_height >= 0, which is exactly
    // the condition this test creates, so calling it here aborts on its own
    // precondition instead of measuring anything. The Try form is the accessor
    // for the may-be-absent case, and it is the stronger check -- a null hash
    // recorded against a real height would satisfy "hash is null" but not this.
    int last_height{0};
    uint256 last_hash;
    BOOST_CHECK(!WITH_LOCK(vault->cs_vault,
                           return vault->TryGetLastBlockProcessed(last_height, last_hash)));
}

BOOST_FIXTURE_TEST_CASE(load_confirmed_tx_without_chainstate_keeps_depth_unresolved, BasicTestingSetup)
{
    BOOST_REQUIRE(!m_node.chainman);
    std::unique_ptr<interfaces::Chain> chain = interfaces::MakeChain(m_node);
    BOOST_REQUIRE(!chain->hasChainstate());

    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vout.emplace_back(COIN, CScript() << OP_TRUE);
    CVaultTx stored{MakeTransactionRef(coinbase),
                    TxStateConfirmed{Params().GenesisBlock().GetHash(), /*height=*/0, /*index=*/0}};
    stored.nOrderPos = 0;

    std::unique_ptr<VaultDatabase> database = CreateMockableVaultDatabase();
    BOOST_REQUIRE(VaultBatch(*database).WriteTx(stored));

    CVault vault{chain.get(), "", std::move(database)};
    BOOST_REQUIRE_EQUAL(vault.LoadVault(), DBErrors::LOAD_OK);

    LOCK(vault.cs_vault);
    const CVaultTx* loaded = vault.GetVaultTx(stored.GetHash().ToUint256());
    BOOST_REQUIRE(loaded);
    // A serialized block reference is only a historical claim until an active
    // chain resolves it. It must not enter the confirmed/conflicted variants
    // with their height invariant violated.
    BOOST_REQUIRE(!loaded->state<TxStateConfirmed>());
    BOOST_REQUIRE(!loaded->state<TxStateBlockConflicted>());
    BOOST_CHECK(loaded->isBlockUnresolved());
    BOOST_CHECK(!loaded->isUnconfirmed());
    BOOST_CHECK_EQUAL(TxStateSerializedBlockHash(loaded->m_state), Params().GenesisBlock().GetHash());
    BOOST_CHECK_EQUAL(TxStateSerializedIndex(loaded->m_state), 0);

    // This is the balance path the GUI transaction model takes while opening a
    // startup vault before AppInitMain has installed a chainstate.
    BOOST_CHECK_EQUAL(CachedTxGetCredit(vault, *loaded, ISMINE_SPENDABLE), 0);
    BOOST_CHECK_EQUAL(vault.GetTxDepthInMainChain(*loaded), 0);
    BOOST_CHECK_EQUAL(vault.GetTxBlocksToMaturity(*loaded), COINBASE_MATURITY + 1);
    BOOST_CHECK(!vault.AbandonTransaction(stored.GetHash()));

    vault.SetVaultFlag(VAULT_FLAG_DESCRIPTORS);
    auto archive = BuildVaultArchive(vault);
    BOOST_CHECK(!archive);
    BOOST_CHECK_NE(util::ErrorString(archive).original.find("unresolved block confirmations"), std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(load_confirmed_tx_with_chainstate_resolves_stored_block, TestChain100Setup)
{
    BOOST_REQUIRE(m_node.chainman);
    BOOST_REQUIRE(m_node.chain->hasChainstate());

    const CBlockIndex* tip = m_node.chainman->ActiveChain().Tip();
    BOOST_REQUIRE(tip);
    CVaultTx stored{m_coinbase_txns.back(),
                    TxStateConfirmed{tip->GetBlockHash(), tip->nHeight, /*index=*/0}};
    stored.nOrderPos = 0;

    std::unique_ptr<VaultDatabase> database = CreateMockableVaultDatabase();
    BOOST_REQUIRE(VaultBatch(*database).WriteTx(stored));

    CVault vault{m_node.chain.get(), "", std::move(database)};
    BOOST_REQUIRE_EQUAL(vault.LoadVault(), DBErrors::LOAD_OK);

    LOCK(vault.cs_vault);
    const CVaultTx* loaded = vault.GetVaultTx(stored.GetHash().ToUint256());
    BOOST_REQUIRE(loaded);
    const TxStateConfirmed* confirmed = loaded->state<TxStateConfirmed>();
    BOOST_REQUIRE(confirmed);
    BOOST_CHECK_EQUAL(confirmed->confirmed_block_hash, tip->GetBlockHash());
    BOOST_CHECK_EQUAL(confirmed->confirmed_block_height, tip->nHeight);
    BOOST_CHECK_EQUAL(confirmed->position_in_block, 0);
}

BOOST_FIXTURE_TEST_CASE(thin_header_tip_refreshes_vault_without_enabling_spends, BasicTestingSetup)
{
    BOOST_REQUIRE(!m_node.chainman);
    std::unique_ptr<interfaces::Chain> chain = interfaces::MakeChain(m_node);
    std::unique_ptr<interfaces::VaultLoader> loader = interfaces::MakeVaultLoader(*chain, m_args);
    VaultContext& context = *loader->context();

    bilingual_str error;
    std::vector<bilingual_str> warnings;
    std::shared_ptr<CVault> vault = CVault::Create(
        context, "", CreateMockableVaultDatabase(), VAULT_FLAG_DESCRIPTORS, error, warnings);
    BOOST_REQUIRE_MESSAGE(vault != nullptr, error.original);
    BOOST_REQUIRE(AddVault(context, vault));

    std::unique_ptr<interfaces::Vault> vault_interface = interfaces::MakeVault(context, vault);
    const CBlockHeader& genesis{Params().GenesisBlock()};
    CMutableTransaction confirmed_tx;
    confirmed_tx.vout.emplace_back(COIN, CScript() << OP_TRUE);
    vault->AddToVault(MakeTransactionRef(confirmed_tx), TxStateConfirmed{genesis.GetHash(), 0, 0});
    BOOST_REQUIRE(loader->setHeaderTip(/*height=*/0, genesis.GetHash(), genesis.GetBlockTime()));

    uint256 refresh_hash;
    interfaces::VaultBalances balances;
    BOOST_CHECK(vault_interface->tryGetBalanceUpdateBlockHash(refresh_hash));
    BOOST_CHECK_EQUAL(refresh_hash, genesis.GetHash());
    BOOST_CHECK(vault_interface->tryGetBalances(balances, refresh_hash));
    BOOST_CHECK_EQUAL(refresh_hash, genesis.GetHash());
    BOOST_CHECK(!vault_interface->canCreateTransactionsNow());
    BOOST_CHECK(!vault_interface->canCreateTransactions());

    interfaces::VaultTxStatus tx_status;
    int num_blocks{-1};
    int64_t block_time{-1};
    interfaces::Chain* context_chain{context.chain};
    context.chain = nullptr;
    BOOST_CHECK(vault_interface->tryGetTxStatus(confirmed_tx.GetHash(), tx_status, num_blocks, block_time));
    context.chain = context_chain;
    BOOST_CHECK_EQUAL(tx_status.depth_in_main_chain, 1);
    BOOST_CHECK_EQUAL(num_blocks, 0);
    BOOST_CHECK_EQUAL(block_time, genesis.GetBlockTime());

    BOOST_CHECK(!loader->setHeaderTip(/*height=*/0, uint256::ONE, genesis.GetBlockTime()));
    BOOST_CHECK(!loader->setHeaderTip(/*height=*/-1, genesis.GetHash(), genesis.GetBlockTime()));

    CVault ahead_vault{chain.get(), "ahead", CreateMockableVaultDatabase()};
    BOOST_REQUIRE_EQUAL(ahead_vault.LoadVault(), DBErrors::LOAD_OK);
    ahead_vault.AddToVault(MakeTransactionRef(confirmed_tx), TxStateConfirmed{uint256::ONE, 1, 0});
    BOOST_CHECK(!WITH_LOCK(ahead_vault.cs_vault, return ahead_vault.ApplyHeaderTip(
        /*block_height=*/0, genesis.GetHash(), genesis.GetBlockTime())));

    BOOST_REQUIRE(RemoveVault(context, vault, /*load_on_start=*/std::nullopt));
    vault_interface.reset();
    WaitForDeleteVault(std::move(vault));
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace vault
