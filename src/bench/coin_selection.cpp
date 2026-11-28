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
#include <util/check.h>
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

// A P2WPKH input's signed size. The benchmark coins carry no script, so their size
// is set explicitly; without it every group weighs 0 and no weight limit is exercised.
static constexpr int BENCH_INPUT_BYTES{68};

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
// measurements of performance. As laanwj noted for the upstream wallet, coin
// selection is probably the hardest to benchmark: it needs a wider range of
// scenarios, since repeating one is not very useful, and random generation
// is not useful for measurements either.
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
        available_coins.Add(OutputType::BECH32, COutput{COutPoint(wtx->GetHash(), 0), txout, /*depth=*/6 * 24, BENCH_INPUT_BYTES, /*solvable=*/true, /*safe=*/true, wtx->GetTxTime(), /*from_me=*/true});
    }

    const CoinEligibilityFilter filter_standard(1, 6, 0);
    FastRandomContext rand{};
    const CoinSelectionParams coin_selection_params{rand};
    auto group = vault::GroupOutputs(vault, available_coins, coin_selection_params, {{filter_standard}})[filter_standard];
    Assert(!group.all_groups.positive_group.empty());
    for (const OutputGroup& g : group.all_groups.positive_group) Assert(g.m_weight > 0);
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

static void add_coin(const CAmount& nValue, int nInput, std::vector<OutputGroup>& set)
{
    CMutableTransaction tx;
    tx.vout.resize(nInput + 1);
    tx.vout[nInput].nValue = nValue;
    COutput output(COutPoint(tx.GetHash(), nInput), tx.vout.at(nInput), /*depth=*/ 0, /*input_bytes=*/ BENCH_INPUT_BYTES, /*solvable=*/ true, /*safe=*/ true, /*time=*/ 0, /*from_me=*/ true);
    set.emplace_back();
    set.back().Insert(std::make_shared<COutput>(output), /*ancestors=*/ 0, /*descendants=*/ 0);
}
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

    // The hard case must exhaust the exact search: it stops at the try cap without
    // completing, and still returns a selection (the largest-first fallback).
    {
        const CAmount target = make_hard_case(17, utxo_pool);
        const auto result = SelectCoinsExactOrLargest(utxo_pool, target, MAX_STANDARD_TX_WEIGHT);
        Assert(result);
        Assert(!result->GetAlgoCompleted());
        Assert(result->GetSelectionsEvaluated() == 100'000); // MAX_EXACT_SELECTION_TRIES in vault/coinselection.cpp
        Assert(result->GetWeight() > 0);
        utxo_pool.clear();
    }

    bench.run([&] {
        // Benchmark
        CAmount target = make_hard_case(17, utxo_pool);
        auto result = SelectCoinsExactOrLargest(utxo_pool, target, MAX_STANDARD_TX_WEIGHT);
        ankerl::nanobench::doNotOptimizeAway(result);

        // Cleanup
        utxo_pool.clear();
    });
}

// S0304-B-02: a pool no single OutputType can fund but the mixed pool can.
// 600 COIN of BECH32 and 600 COIN of BECH32M against a 1000 COIN target: each
// type alone falls short, so AttemptSelection must take the mixed path, and the
// same pool without mixing must fail.
static void CoinSelectionMixedOutputTypes(benchmark::Bench& bench)
{
    NodeContext node;
    auto chain = interfaces::MakeChain(node);
    CVault vault(chain.get(), "", CreateMockableVaultDatabase());
    std::vector<std::unique_ptr<CVaultTx>> wtxs;
    LOCK(vault.cs_vault);

    addCoin(600 * COIN, vault, wtxs);
    addCoin(600 * COIN, vault, wtxs);
    vault::CoinsResult available_coins;
    std::set<COutPoint> bech32_outpoints;
    std::set<COutPoint> bech32m_outpoints;
    for (size_t i = 0; i < wtxs.size(); ++i) {
        const COutPoint outpoint{wtxs[i]->GetHash(), 0};
        const OutputType type{i == 0 ? OutputType::BECH32 : OutputType::BECH32M};
        (i == 0 ? bech32_outpoints : bech32m_outpoints).insert(outpoint);
        available_coins.Add(type, COutput{outpoint, wtxs[i]->tx->vout.at(0), /*depth=*/6 * 24, BENCH_INPUT_BYTES, /*solvable=*/true, /*safe=*/true, wtxs[i]->GetTxTime(), /*from_me=*/true});
    }

    const CoinEligibilityFilter filter_standard(1, 6, 0);
    FastRandomContext rand{};
    const CoinSelectionParams coin_selection_params{rand};
    auto group = vault::GroupOutputs(vault, available_coins, coin_selection_params, {{filter_standard}})[filter_standard];
    Assert(group.TypesCount() == 2);
    for (const OutputGroup& g : group.all_groups.positive_group) Assert(g.m_weight > 0);

    // The success path, asserted before timing it.
    Assert(!AttemptSelection(1000 * COIN, group, coin_selection_params, /*allow_mixed_output_types=*/false));
    const auto check = [&](const util::Result<vault::SelectionResult>& result) {
        assert(result);
        bool has_bech32{false};
        bool has_bech32m{false};
        for (const auto& input : result->GetInputSet()) {
            has_bech32 |= bech32_outpoints.count(input->outpoint) > 0;
            has_bech32m |= bech32m_outpoints.count(input->outpoint) > 0;
        }
        assert(has_bech32 && has_bech32m);
        assert(result->GetSelectedValue() == 1200 * COIN);
    };
    check(AttemptSelection(1000 * COIN, group, coin_selection_params, /*allow_mixed_output_types=*/true));

    bench.run([&] {
        auto result = AttemptSelection(1000 * COIN, group, coin_selection_params, /*allow_mixed_output_types=*/true);
        ankerl::nanobench::doNotOptimizeAway(result);
    });
}

BENCHMARK(CoinSelection, benchmark::PriorityLevel::HIGH);
BENCHMARK(CoinSelectionMixedOutputTypes, benchmark::PriorityLevel::HIGH);
BENCHMARK(ExactSearchExhaustion, benchmark::PriorityLevel::HIGH);
