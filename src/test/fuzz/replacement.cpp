// Copyright (c) 2020-present The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/relaypool_args.h>
#include <policy/replacement.h>
#include <primitives/transaction.h>
#include <sync.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <test/fuzz/util/relaypool.h>
#include <test/util/setup_common.h>
#include <test/util/txrelaypool.h>
#include <txrelaypool.h>
#include <util/check.h>
#include <util/translation.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {
const BasicTestingSetup* g_setup;
} // namespace

const int NUM_ITERS = 10000;

void initialize_replacement()
{
    static const auto testing_setup = MakeNoLogFileContext<>();
    g_setup = testing_setup.get();
}

FUZZ_TARGET(replacement, .init = initialize_replacement)
{
    SeedRandomStateForTest(SeedRand::ZEROS);
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());
    SetMockTime(ConsumeTime(fuzzed_data_provider));
    std::optional<CMutableTransaction> mtx = ConsumeDeserializable<CMutableTransaction>(fuzzed_data_provider, TX_WITH_WITNESS);
    if (!mtx) {
        return;
    }

    bilingual_str error;
    CTxRelayPool pool{RelayPoolOptionsForTest(g_setup->m_node), error};
    Assert(error.empty());

    LIMITED_WHILE(fuzzed_data_provider.ConsumeBool(), NUM_ITERS)
    {
        const std::optional<CMutableTransaction> another_mtx = ConsumeDeserializable<CMutableTransaction>(fuzzed_data_provider, TX_WITH_WITNESS);
        if (!another_mtx) {
            break;
        }
        const CTransaction another_tx{*another_mtx};
        if (fuzzed_data_provider.ConsumeBool() && !mtx->vin.empty()) {
            mtx->vin[0].prevout = COutPoint{another_tx.GetHash(), 0};
        }
        LOCK2(cs_main, pool.cs);
        if (!pool.GetIter(another_tx.GetHash())) {
            AddToRelayPool(pool, ConsumeTxRelayPoolEntry(fuzzed_data_provider, another_tx));
        }
    }
    const CTransaction tx{*mtx};
    if (fuzzed_data_provider.ConsumeBool()) {
        LOCK2(cs_main, pool.cs);
        if (!pool.GetIter(tx.GetHash())) {
            AddToRelayPool(pool, ConsumeTxRelayPoolEntry(fuzzed_data_provider, tx));
        }
    }
    {
        LOCK(pool.cs);
    }
}

