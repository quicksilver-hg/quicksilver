// Copyright (c) 2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <addresstype.h>
#include <bench/bench.h>
#include <quicksilver-build-config.h> // IWYU pragma: keep
#include <key.h>
#include <key_io.h>
#include <script/descriptor.h>
#include <script/script.h>
#include <script/signingprovider.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <vault/context.h>
#include <vault/db.h>
#include <vault/test/util.h>
#include <vault/types.h>
#include <vault/vault.h>
#include <vault/vaultutil.h>

#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace vault {
static void VaultIsMine(benchmark::Bench& bench, int num_combo = 0)
{
    const auto test_setup = MakeNoLogFileContext<TestingSetup>();

    VaultContext context;
    context.args = &test_setup->m_args;
    context.chain = test_setup->m_node.chain.get();

    // Setup the vault
    // Loading the vault will also create it
    auto database = CreateMockableVaultDatabase();
    auto vault = TestLoadVault(std::move(database), context, VAULT_FLAG_DESCRIPTORS);

    // Fill with num_combo combo descriptors with random keys.
    // This benchmarks a non-HD vault migrated to descriptors.
    if (num_combo > 0) {
        LOCK(vault->cs_vault);
        for (int i = 0; i < num_combo; ++i) {
            CKey key;
            key.MakeNewKey(/*fCompressed=*/true);
            FlatSigningProvider keys;
            std::string error;
            std::vector<std::unique_ptr<Descriptor>> desc = Parse("combo(" + EncodeSecret(key) + ")", keys, error, /*require_checksum=*/false);
            VaultDescriptor w_desc(std::move(desc.at(0)), /*creation_time=*/0, /*range_start=*/0, /*range_end=*/0, /*next_index=*/0);
            auto spkm = vault->AddVaultDescriptor(w_desc, keys, /*label=*/"", /*internal=*/false);
            assert(spkm);
        }
    }

    const CScript script = GetScriptForDestination(DecodeDestination(ADDRESS_SANDBOX_UNSPENDABLE));

    bench.run([&] {
        LOCK(vault->cs_vault);
        isminetype mine = vault->IsMine(script);
        assert(mine == ISMINE_NO);
    });

    TestUnloadVault(std::move(vault));
}

#ifdef USE_SQLITE
static void VaultIsMineDescriptors(benchmark::Bench& bench) { VaultIsMine(bench); }
static void VaultIsMineMigratedDescriptors(benchmark::Bench& bench) { VaultIsMine(bench, /*num_combo=*/2000); }
BENCHMARK(VaultIsMineDescriptors, benchmark::PriorityLevel::LOW);
BENCHMARK(VaultIsMineMigratedDescriptors, benchmark::PriorityLevel::LOW);
#endif
} // namespace vault
