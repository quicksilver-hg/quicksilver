// Copyright (c) 2023-present The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <quicksilver-build-config.h> // IWYU pragma: keep
#include <random.h>
#include <support/allocators/secure.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/fs.h>
#include <util/translation.h>
#include <vault/context.h>
#include <vault/db.h>
#include <vault/vault.h>
#include <vault/vaultutil.h>

#include <cassert>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vault {
static void VaultCreate(benchmark::Bench& bench, bool encrypted)
{
    auto test_setup = MakeNoLogFileContext<TestingSetup>();
    FastRandomContext random;

    VaultContext context;
    context.args = &test_setup->m_args;
    context.chain = test_setup->m_node.chain.get();

    DatabaseOptions options;
    options.require_format = DatabaseFormat::SQLITE;
    options.require_create = true;
    options.create_flags = VAULT_FLAG_DESCRIPTORS;

    if (encrypted) {
        options.create_passphrase = random.rand256().ToString();
    }

    DatabaseStatus status;
    bilingual_str error_string;
    std::vector<bilingual_str> warnings;

    auto vault_path = fs::PathToString(test_setup->m_path_root / "test_vault");
    bench.run([&] {
        auto vault = CreateVault(context, vault_path, /*load_on_start=*/std::nullopt, options, status, error_string, warnings);
        assert(status == DatabaseStatus::SUCCESS);
        assert(vault != nullptr);

        // Release vault
        RemoveVault(context, vault, /*load_on_start=*/ std::nullopt);
        WaitForDeleteVault(std::move(vault));
        fs::remove_all(vault_path);
    });
}

static void VaultCreatePlain(benchmark::Bench& bench) { VaultCreate(bench, /*encrypted=*/false); }
static void VaultCreateEncrypted(benchmark::Bench& bench) { VaultCreate(bench, /*encrypted=*/true); }

#ifdef USE_SQLITE
BENCHMARK(VaultCreatePlain, benchmark::PriorityLevel::LOW);
BENCHMARK(VaultCreateEncrypted, benchmark::PriorityLevel::LOW);
#endif

} // namespace vault
