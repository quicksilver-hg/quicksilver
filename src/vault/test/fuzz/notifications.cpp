// Copyright (c) 2021-present The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <addresstype.h>
#include <consensus/amount.h>
#include <interfaces/chain.h>
#include <kernel/chain.h>
#include <outputtype.h>
#include <policy/policy.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/descriptor.h>
#include <script/script.h>
#include <script/signingprovider.h>
#include <sync.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <test/fuzz/util/vault.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/check.h>
#include <util/result.h>
#include <util/time.h>
#include <util/translation.h>
#include <vault/coincontrol.h>
#include <vault/context.h>
#include <vault/receive.h>
#include <vault/spend.h>
#include <vault/test/util.h>
#include <vault/vault.h>
#include <vault/vaultutil.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace vault {
namespace {
const TestingSetup* g_setup;

void initialize_setup()
{
    static const auto testing_setup = MakeNoLogFileContext<const TestingSetup>();
    g_setup = testing_setup.get();
}

FUZZ_TARGET(vault_notifications, .init = initialize_setup)
{
    SeedRandomStateForTest(SeedRand::ZEROS);
    FuzzedDataProvider fuzzed_data_provider{buffer.data(), buffer.size()};
    SetMockTime(ConsumeTime(fuzzed_data_provider));
    // The total amount, to be distributed to the vaults a and b in txs
    // without fee. Thus, the balance of the vaults should always equal the
    // total amount.
    const auto total_amount{ConsumeMoney(fuzzed_data_provider, /*max=*/MAX_MONEY / 100000)};
    FuzzedVault a{
        *g_setup->m_node.chain,
        "fuzzed_vault_a",
        "sqrv1wkfAGt6m8urozK3u8rQGJ72EqcN9rDXqwoNKuMbZBLKbk2gy9dhmzErD84KDNHrwCCAuaGSbvN6zrNDaMdDHrgCWAvwiiq6MLYENBN553A",
    };
    FuzzedVault b{
        *g_setup->m_node.chain,
        "fuzzed_vault_b",
        "sqrv1wkfAGt6m8urrBotaKQQjGJxsRom6W1fRRbcfrCLyFJ9ZpPFMKd7qyrZoPC8C9MNhipy6BckCkCikcBJpCsqtzu7bPipinLhsgiHppNejUM",
    };

    // Keep track of all coins in this test.
    // Each tuple in the chain represents the coins and the block created with
    // those coins. Once the block is mined, the next tuple will have an empty
    // block and the freshly mined coins.
    using Coins = std::set<std::tuple<CAmount, COutPoint>>;
    std::vector<std::tuple<Coins, CBlock>> chain;
    {
        // Add the initial entry
        chain.emplace_back();
        auto& [coins, block]{chain.back()};
        coins.emplace(total_amount, COutPoint{Txid::FromUint256(uint256::ONE), 1});
    }
    LIMITED_WHILE(fuzzed_data_provider.ConsumeBool(), 20)
    {
        CallOneOf(
            fuzzed_data_provider,
            [&] {
                auto& [coins_orig, block]{chain.back()};
                // Copy the coins for this block and consume all of them
                Coins coins = coins_orig;
                while (!coins.empty()) {
                    // Create a new tx
                    CMutableTransaction tx{};
                    // Add some coins as inputs to it
                    auto num_inputs{fuzzed_data_provider.ConsumeIntegralInRange<int>(1, coins.size())};
                    CAmount in{0};
                    while (num_inputs-- > 0) {
                        const auto& [coin_amt, coin_outpoint]{*coins.begin()};
                        in += coin_amt;
                        tx.vin.emplace_back(coin_outpoint);
                        coins.erase(coins.begin());
                    }
                    // Create some outputs spending all inputs, without fee
                    LIMITED_WHILE(in > 0 && fuzzed_data_provider.ConsumeBool(), 10)
                    {
                        const auto out_value{ConsumeMoney(fuzzed_data_provider, in)};
                        in -= out_value;
                        auto& vault{fuzzed_data_provider.ConsumeBool() ? a : b};
                        tx.vout.emplace_back(out_value, vault.GetScriptPubKey(fuzzed_data_provider));
                    }
                    // Spend the remaining input value, if any
                    auto& vault{fuzzed_data_provider.ConsumeBool() ? a : b};
                    tx.vout.emplace_back(in, vault.GetScriptPubKey(fuzzed_data_provider));
                    // Add tx to block
                    block.vtx.emplace_back(MakeTransactionRef(tx));
                    // Check that funding the tx doesn't crash the vault
                    a.FundTx(fuzzed_data_provider, tx);
                    b.FundTx(fuzzed_data_provider, tx);
                }
                // Mine block
                const uint256& hash = block.GetHash();
                interfaces::BlockInfo info{hash};
                info.prev_hash = &block.hashPrevBlock;
                info.height = chain.size();
                info.data = &block;
                // Ensure that no blocks are skipped by the vault by setting the chain's accumulated
                // time to the maximum value. This ensures that the vault's birth time is always
                // earlier than this maximum time.
                info.chain_time_max = std::numeric_limits<unsigned int>::max();
                a.vault->blockConnected(info);
                b.vault->blockConnected(info);
                // Store the coins for the next block
                Coins coins_new;
                for (const auto& tx : block.vtx) {
                    uint32_t i{0};
                    for (const auto& out : tx->vout) {
                        coins_new.emplace(out.nValue, COutPoint{tx->GetHash(), i++});
                    }
                }
                chain.emplace_back(coins_new, CBlock{});
            },
            [&] {
                if (chain.size() <= 1) return; // The first entry can't be removed
                auto& [coins, block]{chain.back()};
                if (block.vtx.empty()) return; // Can only disconnect if the block was submitted first
                // Disconnect block
                const uint256& hash = block.GetHash();
                interfaces::BlockInfo info{hash};
                info.prev_hash = &block.hashPrevBlock;
                info.height = chain.size() - 1;
                info.data = &block;
                a.vault->blockDisconnected(info);
                b.vault->blockDisconnected(info);
                chain.pop_back();
            });
        auto& [coins, first_block]{chain.front()};
        if (!first_block.vtx.empty()) {
            // Only check balance when at least one block was submitted
            const auto bal_a{GetBalance(*a.vault).m_mine_trusted};
            const auto bal_b{GetBalance(*b.vault).m_mine_trusted};
            assert(total_amount == bal_a + bal_b);
        }
    }
}
} // namespace
} // namespace vault
