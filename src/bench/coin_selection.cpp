// Copyright (c) 2012-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <consensus/amount.h>
#include <interfaces/chain.h>
#include <node/context.h>
#include <outputtype.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <random.h>
#include <sync.h>
#include <util/result.h>
#include <vault/coinselection.h>
#include <vault/spend.h>
#include <vault/test/util.h>
#include <vault/transaction.h>
#include <vault/vault.h>

#include <cassert>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

using node::NodeContext;
using vault::AttemptSelection;
using vault::COutput;
using vault::CVault;
using vault::CVaultTx;
using vault::CoinEligibilityFilter;
using vault::CoinSelectionParams;
using vault::CreateMockableVaultDatabase;
using vault::OutputGroup;
using vault::SelectCoinsExactOrLargest;
using vault::TxStateInactive;

static void addCoin(const CAmount& nValue, const CVault& vault, std::vector<std::unique_ptr<CVaultTx>>& wtxs)
{
    static int nextLockTime = 0;
    CMutableTransaction tx;
    tx.nLockTime = nextLockTime++; // so all transactions get different hashes
    tx.vout.resize(1);
    tx.vout[0].nValue = nValue;
    wtxs.push_back(std::make_unique<CVaultTx>(MakeTransactionRef(std::move(tx)), TxStateInactive{}));
}

// Simple benchmark for vault coin selection. Note that it maybe be necessary
// to build up more complicated scenarios in order to get meaningful
// measurements of performance. From laanwj, "Vault coin selection is probably
// the hardest, as you need a wider selection of scenarios, just testing the
// same one over and over isn't too useful. Generating random isn't useful
// either for measurements."
static void CoinSelection(benchmark::Bench& bench)
{
    NodeContext node;
    auto chain = interfaces::MakeChain(node);
    CVault vault(chain.get(), "", CreateMockableVaultDatabase());
    std::vector<std::unique_ptr<CVaultTx>> wtxs;
    LOCK(vault.cs_vault);

    // Add coins.
    for (int i = 0; i < 1000; ++i) {
        addCoin(1000 * COIN, vault, wtxs);
    }
    addCoin(3 * COIN, vault, wtxs);

    // Create coins
    vault::CoinsResult available_coins;
    for (const auto& wtx : wtxs) {
        const auto txout = wtx->tx->vout.at(0);
        available_coins.Add(OutputType::BECH32, COutput{COutPoint(wtx->GetHash(), 0), txout, /*depth=*/6 * 24, CalculateMaximumSignedInputSize(txout, &vault, /*coin_control=*/nullptr), /*solvable=*/true, /*safe=*/true, wtx->GetTxTime(), /*from_me=*/true});
    }

    const CoinEligibilityFilter filter_standard(1, 6, 0);
    FastRandomContext rand{};
    const CoinSelectionParams coin_selection_params{rand};
    auto group = vault::GroupOutputs(vault, available_coins, coin_selection_params, {{filter_standard}})[filter_standard];
    bench.run([&] {
        auto result = AttemptSelection(1002.99 * COIN, group, coin_selection_params, /*allow_mixed_output_types=*/true);
        assert(result);
        // No subset of {1000 x 1000 COIN, 1 x 3 COIN} sums to 1002.99 COIN, so the
        // exact search finds nothing and SelectCoinsExactOrLargest falls back to
        // largest-first: two 1000 COIN coins, 2000 total. Upstream's Branch-and-Bound
        // picked {1000, 3} = 1003 here, and this asserted that until the fee purge
        // replaced the selector -- with no fee to pay, "overshoot" costs the sender
        // nothing but a change output, so there is nothing left for BnB to minimise.
        assert(result->GetAlgo() == vault::SelectionAlgorithm::LARGEST_FIRST);
        assert(result->GetSelectedValue() == 2000 * COIN);
        assert(result->GetInputSet().size() == 2);
    });
}

// Copied from src/vault/test/coinselector_tests.cpp
static void add_coin(const CAmount& nValue, int nInput, std::vector<OutputGroup>& set)
{
    CMutableTransaction tx;
    tx.vout.resize(nInput + 1);
    tx.vout[nInput].nValue = nValue;
    COutput output(COutPoint(tx.GetHash(), nInput), tx.vout.at(nInput), /*depth=*/ 0, /*input_bytes=*/ -1, /*solvable=*/ true, /*safe=*/ true, /*time=*/ 0, /*from_me=*/ true);
    set.emplace_back();
    set.back().Insert(std::make_shared<COutput>(output), /*ancestors=*/ 0, /*descendants=*/ 0);
}
// Copied from src/vault/test/coinselector_tests.cpp
static CAmount make_hard_case(int utxos, std::vector<OutputGroup>& utxo_pool)
{
    utxo_pool.clear();
    CAmount target = 0;
    for (int i = 0; i < utxos; ++i) {
        target += CAmount{1} << (utxos+i);
        add_coin(CAmount{1} << (utxos+i), 2*i, utxo_pool);
        add_coin((CAmount{1} << (utxos+i)) + (CAmount{1} << (utxos-1-i)), 2*i + 1, utxo_pool);
    }
    return target;
}

static void ExactSearchExhaustion(benchmark::Bench& bench)
{
    // Setup
    std::vector<OutputGroup> utxo_pool;

    bench.run([&] {
        // Benchmark
        CAmount target = make_hard_case(17, utxo_pool);
        SelectCoinsExactOrLargest(utxo_pool, target, MAX_STANDARD_TX_WEIGHT);

        // Cleanup
        utxo_pool.clear();
    });
}

BENCHMARK(CoinSelection, benchmark::PriorityLevel::HIGH);
BENCHMARK(ExactSearchExhaustion, benchmark::PriorityLevel::HIGH);
