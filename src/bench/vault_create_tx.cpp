// Copyright (c) 2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <addresstype.h>
#include <bench/bench.h>
#include <chain.h>
#include <chainparams.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <interfaces/chain.h>
#include <kernel/chain.h>
#include <node/blockstorage.h>
#include <outputtype.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/result.h>
#include <util/time.h>
#include <validation.h>
#include <versionbits.h>
#include <vault/coincontrol.h>
#include <vault/coinselection.h>
#include <vault/spend.h>
#include <vault/test/util.h>
#include <vault/vault.h>
#include <vault/vaultutil.h>

#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

using vault::CVault;
using vault::CreateMockableVaultDatabase;
using vault::VAULT_FLAG_DESCRIPTORS;

struct TipBlock
{
    uint256 prev_block_hash;
    int64_t prev_block_time;
    int tip_height;
};

TipBlock getTip(const CChainParams& params, const node::NodeContext& context)
{
    auto tip = WITH_LOCK(::cs_main, return context.chainman->ActiveTip());
    return (tip) ? TipBlock{tip->GetBlockHash(), tip->GetBlockTime(), tip->nHeight} :
           TipBlock{params.GenesisBlock().GetHash(), params.GenesisBlock().GetBlockTime(), 0};
}

void generateFakeBlock(const CChainParams& params,
                       const node::NodeContext& context,
                       CVault& vault,
                       const CScript& coinbase_out_script)
{
    TipBlock tip{getTip(params, context)};

    // Create block
    CBlock block;
    CMutableTransaction coinbase_tx;
    coinbase_tx.vin.resize(1);
    coinbase_tx.vin[0].prevout.SetNull();
    coinbase_tx.vout.resize(2);
    coinbase_tx.vout[0].scriptPubKey = coinbase_out_script;
    coinbase_tx.vout[0].nValue = 49 * COIN;
    coinbase_tx.vin[0].scriptSig = CScript() << ++tip.tip_height << OP_0;
    coinbase_tx.vout[1].scriptPubKey = coinbase_out_script; // extra output
    coinbase_tx.vout[1].nValue = 1 * COIN;
    block.vtx = {MakeTransactionRef(std::move(coinbase_tx))};

    block.nVersion = VERSIONBITS_LAST_OLD_BLOCK_VERSION;
    block.hashPrevBlock = tip.prev_block_hash;
    block.hashMerkleRoot = BlockMerkleRoot(block);
    block.nTime = ++tip.prev_block_time;
    block.nBits = params.GenesisBlock().nBits;
    block.nNonce = 0;

    {
        LOCK(::cs_main);
        // Add it to the index
        CBlockIndex* pindex{context.chainman->m_blockman.AddToBlockIndex(block, context.chainman->m_best_header)};
        // add it to the chain
        context.chainman->ActiveChain().SetTip(*pindex);
    }

    // notify vault
    const auto& pindex = WITH_LOCK(::cs_main, return context.chainman->ActiveChain().Tip());
    vault.blockConnected(kernel::MakeBlockInfo(pindex, &block));
}

struct PreSelectInputs {
    // How many coins from the vault the process should select
    int num_of_internal_inputs;
    // future: this could have external inputs as well.
};

static void VaultCreateTx(benchmark::Bench& bench, const OutputType output_type, bool allow_other_inputs, std::optional<PreSelectInputs> preset_inputs)
{
    const auto test_setup = MakeNoLogFileContext<const TestingSetup>();

    // Set clock to genesis block, so the descriptors/keys creation time don't interfere with the blocks scanning process.
    SetMockTime(test_setup->m_node.chainman->GetParams().GenesisBlock().nTime);
    CVault vault{test_setup->m_node.chain.get(), "", CreateMockableVaultDatabase()};
    {
        LOCK(vault.cs_vault);
        vault.SetVaultFlag(VAULT_FLAG_DESCRIPTORS);
        vault.SetupDescriptorScriptPubKeyMans();
    }

    // Generate destinations
    const auto dest{getNewDestination(vault, output_type)};

    // Generate chain; each coinbase will have two outputs to fill-up the vault
    const auto& params = Params();
    const CScript coinbase_out{GetScriptForDestination(dest)};
    unsigned int chain_size = 5000; // 5k blocks means 10k UTXO for the vault (minus 200 due COINBASE_MATURITY)
    for (unsigned int i = 0; i < chain_size; ++i) {
        generateFakeBlock(params, test_setup->m_node, vault, coinbase_out);
    }

    // Check available balance
    auto bal = WITH_LOCK(vault.cs_vault, return vault::AvailableCoins(vault).GetTotalAmount()); // Cache
    assert(bal == 50 * COIN * (chain_size - COINBASE_MATURITY));

    vault::CCoinControl coin_control;
    coin_control.m_allow_other_inputs = allow_other_inputs;

    CAmount target = 0;
    if (preset_inputs) {
        // Select inputs, each has 49 Hg
        vault::CoinFilterParams filter_coins;
        filter_coins.max_count = preset_inputs->num_of_internal_inputs;
        const auto& res = WITH_LOCK(vault.cs_vault,
                                    return vault::AvailableCoins(vault, /*coinControl=*/nullptr, filter_coins));
        for (int i=0; i < preset_inputs->num_of_internal_inputs; i++) {
            const auto& coin{res.coins.at(output_type)[i]};
            target += coin.txout.nValue;
            coin_control.Select(coin.outpoint);
        }
    }

    // If automatic coin selection is enabled, add the value of another UTXO to the target
    if (coin_control.m_allow_other_inputs) target += 50 * COIN;
    std::vector<vault::CRecipient> recipients = {{dest, target}};

    bench.epochIterations(5).run([&] {
        LOCK(vault.cs_vault);
        const auto& tx_res = CreateTransaction(vault, recipients, /*change_pos=*/std::nullopt, coin_control);
        assert(tx_res);
    });
}

static void AvailableCoins(benchmark::Bench& bench, const std::vector<OutputType>& output_type)
{
    const auto test_setup = MakeNoLogFileContext<const TestingSetup>();
    // Set clock to genesis block, so the descriptors/keys creation time don't interfere with the blocks scanning process.
    SetMockTime(test_setup->m_node.chainman->GetParams().GenesisBlock().nTime);
    CVault vault{test_setup->m_node.chain.get(), "", CreateMockableVaultDatabase()};
    {
        LOCK(vault.cs_vault);
        vault.SetVaultFlag(VAULT_FLAG_DESCRIPTORS);
        vault.SetupDescriptorScriptPubKeyMans();
    }

    // Generate destinations
    std::vector<CScript> dest_vault;
    dest_vault.reserve(output_type.size());
    for (auto type : output_type) {
        dest_vault.emplace_back(GetScriptForDestination(getNewDestination(vault, type)));
    }

    // Generate chain; each coinbase will have two outputs to fill-up the vault
    const auto& params = Params();
    unsigned int chain_size = 1000;
    for (unsigned int i = 0; i < chain_size / dest_vault.size(); ++i) {
        for (const auto& dest : dest_vault) {
            generateFakeBlock(params, test_setup->m_node, vault, dest);
        }
    }

    // Check available balance
    auto bal = WITH_LOCK(vault.cs_vault, return vault::AvailableCoins(vault).GetTotalAmount()); // Cache
    assert(bal == 50 * COIN * (chain_size - COINBASE_MATURITY));

    bench.epochIterations(2).run([&] {
        LOCK(vault.cs_vault);
        const auto& res = vault::AvailableCoins(vault);
        assert(res.All().size() == (chain_size - COINBASE_MATURITY) * 2);
    });
}

static void VaultCreateTxUseOnlyPresetInputs(benchmark::Bench& bench) { VaultCreateTx(bench, OutputType::BECH32, /*allow_other_inputs=*/false,
                                                                                        {{/*num_of_internal_inputs=*/4}}); }

static void VaultCreateTxUsePresetInputsAndCoinSelection(benchmark::Bench& bench) { VaultCreateTx(bench, OutputType::BECH32, /*allow_other_inputs=*/true,
                                                                                                    {{/*num_of_internal_inputs=*/4}}); }

static void VaultAvailableCoins(benchmark::Bench& bench) { AvailableCoins(bench, {OutputType::BECH32M}); }

BENCHMARK(VaultCreateTxUseOnlyPresetInputs, benchmark::PriorityLevel::LOW)
BENCHMARK(VaultCreateTxUsePresetInputsAndCoinSelection, benchmark::PriorityLevel::LOW)
BENCHMARK(VaultAvailableCoins, benchmark::PriorityLevel::LOW);
