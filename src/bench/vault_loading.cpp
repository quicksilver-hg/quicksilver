// Copyright (c) 2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <addresstype.h>
#include <bench/bench.h>
#include <quicksilver-build-config.h> // IWYU pragma: keep
#include <consensus/amount.h>
#include <outputtype.h>
#include <primitives/transaction.h>
#include <test/util/setup_common.h>
#include <util/check.h>
#include <vault/context.h>
#include <vault/db.h>
#include <vault/test/util.h>
#include <vault/transaction.h>
#include <vault/vault.h>
#include <vault/vaultutil.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace vault{
static void AddTx(CVault& vault)
{
    CMutableTransaction mtx;
    mtx.vout.emplace_back(COIN, GetScriptForDestination(*Assert(vault.GetNewDestination(OutputType::BECH32, ""))));
    mtx.vin.emplace_back();

    vault.AddToVault(MakeTransactionRef(mtx), TxStateInactive{});
}

static void VaultLoading(benchmark::Bench& bench)
{
    const auto test_setup = MakeNoLogFileContext<TestingSetup>();

    VaultContext context;
    context.args = &test_setup->m_args;
    context.chain = test_setup->m_node.chain.get();

    // Setup the vault
    // Loading the vault will also create it
    const uint64_t create_flags = VAULT_FLAG_DESCRIPTORS;
    auto database = CreateMockableVaultDatabase();
    auto vault = TestLoadVault(std::move(database), context, create_flags);

    // Generate a bunch of transactions and addresses to put into the vault
    for (int i = 0; i < 1000; ++i) {
        AddTx(*vault);
    }

    database = DuplicateMockDatabase(vault->GetDatabase());

    // reload the vault for the actual benchmark
    TestUnloadVault(std::move(vault));

    bench.epochs(5).run([&] {
        vault = TestLoadVault(std::move(database), context, create_flags);

        // Cleanup
        database = DuplicateMockDatabase(vault->GetDatabase());
        TestUnloadVault(std::move(vault));
    });
}

#ifdef USE_SQLITE
static void VaultLoadingDescriptors(benchmark::Bench& bench) { VaultLoading(bench); }
BENCHMARK(VaultLoadingDescriptors, benchmark::PriorityLevel::HIGH);
#endif
} // namespace vault
