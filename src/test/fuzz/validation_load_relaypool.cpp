// Copyright (c) 2020-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/relaypool_persist.h>

#include <node/relaypool_args.h>
#include <node/relaypool_persist_args.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <test/fuzz/util/relaypool.h>
#include <test/util/setup_common.h>
#include <test/util/txrelaypool.h>
#include <txrelaypool.h>
#include <util/check.h>
#include <util/fs.h>
#include <util/time.h>
#include <util/translation.h>
#include <validation.h>

#include <cstdint>
#include <vector>

using node::DumpRelayPool;
using node::LoadRelayPool;

using node::RelayPoolPath;

namespace {
const TestingSetup* g_setup;
} // namespace

void initialize_validation_load_relaypool()
{
    static const auto testing_setup = MakeNoLogFileContext<const TestingSetup>();
    g_setup = testing_setup.get();
}

FUZZ_TARGET(validation_load_relaypool, .init = initialize_validation_load_relaypool)
{
    SeedRandomStateForTest(SeedRand::ZEROS);
    FuzzedDataProvider fuzzed_data_provider{buffer.data(), buffer.size()};
    SetMockTime(ConsumeTime(fuzzed_data_provider));
    FuzzedFileProvider fuzzed_file_provider{fuzzed_data_provider};

    bilingual_str error;
    CTxRelayPool pool{RelayPoolOptionsForTest(g_setup->m_node), error};
    Assert(error.empty());

    auto& chainstate{static_cast<DummyChainState&>(g_setup->m_node.chainman->ActiveChainstate())};
    chainstate.SetRelayPool(&pool);

    auto fuzzed_fopen = [&](const fs::path&, const char*) {
        return fuzzed_file_provider.open();
    };
    (void)LoadRelayPool(pool, RelayPoolPath(g_setup->m_args), chainstate,
                      {
                          .mockable_fopen_function = fuzzed_fopen,
                      });
    pool.SetLoadTried(true);
    const fs::path dump_path{RelayPoolPath(g_setup->m_args)};
    if (fuzzed_data_provider.ConsumeBool()) {
        (void)DumpRelayPool(pool, dump_path, fuzzed_fopen, true);
    } else {
        // S0406-C-01: a real file, so the dumper's .new file exists and the
        // rename over the dump path (relaypool_persist.cpp) actually runs.
        Assert(DumpRelayPool(pool, dump_path, fsbridge::fopen, /*skip_file_commit=*/true));
        Assert(fs::exists(dump_path));
        Assert(!fs::exists(dump_path + ".new"));
        fs::remove(dump_path);
    }
}
