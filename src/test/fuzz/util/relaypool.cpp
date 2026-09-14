// Copyright (c) 2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <kernel/relaypool_entry.h>
#include <primitives/transaction.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/util.h>
#include <test/fuzz/util/relaypool.h>

#include <cassert>
#include <cstdint>
#include <limits>

CTxRelayPoolEntry ConsumeTxRelayPoolEntry(FuzzedDataProvider& fuzzed_data_provider, const CTransaction& tx) noexcept
{
    const int64_t time = fuzzed_data_provider.ConsumeIntegral<int64_t>();
    const uint64_t entry_sequence{fuzzed_data_provider.ConsumeIntegral<uint64_t>()};
    const unsigned int entry_height = fuzzed_data_provider.ConsumeIntegral<unsigned int>();
    const bool spends_coinbase = fuzzed_data_provider.ConsumeBool();
    const unsigned int sig_op_cost = fuzzed_data_provider.ConsumeIntegralInRange<unsigned int>(0, MAX_BLOCK_SIGOPS_COST);
    return CTxRelayPoolEntry{MakeTransactionRef(tx), time, entry_height, entry_sequence, spends_coinbase, sig_op_cost, {}};
}
