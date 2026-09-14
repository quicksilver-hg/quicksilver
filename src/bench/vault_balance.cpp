// Copyright (c) 2012-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <interfaces/chain.h>
#include <kernel/chainparams.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <sync.h>
#include <test/util/mining.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/time.h>
#include <validation.h>
#include <vault/receive.h>
#include <vault/test/util.h>
#include <vault/vault.h>
#include <vault/vaultutil.h>

#include <cassert>
#include <memory>
#include <optional>
#include <string>

namespace vault {
static void VaultBalance(benchmark::Bench& bench, const bool set_dirty, const bool add_mine)
{
    const auto test_setup = MakeNoLogFileContext<const TestingSetup>();

    const auto& ADDRESS_NOT_MINE = ADDRESS_SANDBOX_UNSPENDABLE;

    // Set clock to genesis block, so the descriptors/keys creation time don't interfere with the blocks scanning process.
    // The reason is 'generatetoaddress', which creates a chain with deterministic timestamps in the past.
    SetMockTime(test_setup->m_node.chainman->GetParams().GenesisBlock().nTime);
    CVault vault{test_setup->m_node.chain.get(), "", CreateMockableVaultDatabase()};
    {
        LOCK(vault.cs_vault);
        vault.SetVaultFlag(VAULT_FLAG_DESCRIPTORS);
        vault.SetupDescriptorScriptPubKeyMans();
    }
    auto handler = test_setup->m_node.chain->handleNotifications({&vault, [](CVault*) {}});

    const std::optional<std::string> address_mine{add_mine ? std::optional<std::string>{getnewaddress(vault)} : std::nullopt};

    for (int i = 0; i < 100; ++i) {
        generatetoaddress(test_setup->m_node, address_mine.value_or(ADDRESS_NOT_MINE));
        generatetoaddress(test_setup->m_node, ADDRESS_NOT_MINE);
    }
    // Calls SyncWithValidationInterfaceQueue
    vault.chain().waitForNotificationsIfTipChanged(uint256::ZERO);

    auto bal = GetBalance(vault); // Cache

    bench.run([&] {
        if (set_dirty) vault.MarkDirty();
        bal = GetBalance(vault);
        if (add_mine) assert(bal.m_mine_trusted > 0);
    });
}

static void VaultBalanceDirty(benchmark::Bench& bench) { VaultBalance(bench, /*set_dirty=*/true, /*add_mine=*/true); }
static void VaultBalanceClean(benchmark::Bench& bench) { VaultBalance(bench, /*set_dirty=*/false, /*add_mine=*/true); }
static void VaultBalanceMine(benchmark::Bench& bench) { VaultBalance(bench, /*set_dirty=*/false, /*add_mine=*/true); }
static void VaultBalanceNoMine(benchmark::Bench& bench) { VaultBalance(bench, /*set_dirty=*/false, /*add_mine=*/false); }

BENCHMARK(VaultBalanceDirty, benchmark::PriorityLevel::HIGH);
BENCHMARK(VaultBalanceClean, benchmark::PriorityLevel::HIGH);
BENCHMARK(VaultBalanceMine, benchmark::PriorityLevel::HIGH);
BENCHMARK(VaultBalanceNoMine, benchmark::PriorityLevel::HIGH);
} // namespace vault
