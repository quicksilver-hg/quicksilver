// Copyright (c) 2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <primitives/transaction.h>
#include <policy/policy.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <vault/coinselection.h>

#include <limits>
#include <numeric>
#include <vector>

namespace vault {

static void AddCoin(CAmount value, int n_input, int n_input_bytes, int locktime, std::vector<COutput>& coins)
{
    CMutableTransaction tx;
    tx.vout.resize(n_input + 1);
    tx.vout[n_input].nValue = value;
    tx.nLockTime = locktime;
    coins.emplace_back(COutPoint(tx.GetHash(), n_input), tx.vout.at(n_input), /*depth=*/0, n_input_bytes, /*solvable=*/true, /*safe=*/true, /*time=*/0, /*from_me=*/true);
}

static CAmount CreateCoins(FuzzedDataProvider& fuzzed_data_provider, std::vector<COutput>& utxo_pool, int& next_locktime)
{
    CAmount total_balance{0};
    LIMITED_WHILE(fuzzed_data_provider.ConsumeBool(), 1000)
    {
        const int n_input{fuzzed_data_provider.ConsumeIntegralInRange<int>(0, 10)};
        const int n_input_bytes{fuzzed_data_provider.ConsumeIntegralInRange<int>(1, 10000)};
        const CAmount amount{fuzzed_data_provider.ConsumeIntegralInRange<CAmount>(1, MAX_MONEY)};
        if (total_balance + amount >= MAX_MONEY) break;
        AddCoin(amount, n_input, n_input_bytes, ++next_locktime, utxo_pool);
        total_balance += amount;
    }
    return total_balance;
}

static void GroupCoins(FuzzedDataProvider& fuzzed_data_provider, const std::vector<COutput>& coins, std::vector<OutputGroup>& output_groups)
{
    OutputGroup output_group;
    for (const COutput& coin : coins) {
        output_group.Insert(std::make_shared<COutput>(coin), /*ancestors=*/0, /*descendants=*/0);
        if (!output_group.m_outputs.empty() && fuzzed_data_provider.ConsumeBool()) {
            output_groups.push_back(output_group);
            output_group = OutputGroup();
        }
    }
    if (!output_group.m_outputs.empty()) output_groups.push_back(output_group);
}

static SelectionResult ManualSelection(const std::vector<COutput>& utxos, CAmount total_amount)
{
    SelectionResult result(total_amount, SelectionAlgorithm::MANUAL);
    std::set<std::shared_ptr<COutput>> utxo_pool;
    for (const auto& utxo : utxos) {
        utxo_pool.insert(std::make_shared<COutput>(utxo));
    }
    result.AddInputs(utxo_pool);
    return result;
}

FUZZ_TARGET(coin_grinder)
{
    FuzzedDataProvider fuzzed_data_provider{buffer.data(), buffer.size()};
    std::vector<COutput> utxo_pool;
    int next_locktime{0};
    const CAmount total_balance{CreateCoins(fuzzed_data_provider, utxo_pool, next_locktime)};
    const CAmount target{fuzzed_data_provider.ConsumeIntegralInRange<CAmount>(1, MAX_MONEY)};

    std::vector<OutputGroup> groups;
    GroupCoins(fuzzed_data_provider, utxo_pool, groups);
    const int max_selection_weight{fuzzed_data_provider.ConsumeIntegralInRange<int>(0, std::numeric_limits<int>::max())};

    auto result{SelectCoinsExactOrLargest(groups, target, max_selection_weight)};
    if (!result) return;

    assert(total_balance >= target);
    assert(result->GetSelectedValue() >= target);
    assert(result->GetChange() == result->GetSelectedValue() - target);
    assert(result->GetWeight() <= max_selection_weight);
    (void)result->GetShuffledInputVector();
    (void)result->GetInputSet();
}

FUZZ_TARGET(coin_grinder_is_optimal)
{
    FuzzedDataProvider fuzzed_data_provider{buffer.data(), buffer.size()};
    std::vector<COutput> utxo_pool;
    int next_locktime{0};
    LIMITED_WHILE(fuzzed_data_provider.ConsumeBool(), 16)
    {
        const int n_input_bytes{fuzzed_data_provider.ConsumeIntegralInRange<int>(1, 1000)};
        const CAmount amount{fuzzed_data_provider.ConsumeIntegralInRange<CAmount>(1, COIN)};
        AddCoin(amount, /*n_input=*/0, n_input_bytes, ++next_locktime, utxo_pool);
    }

    std::vector<OutputGroup> groups;
    GroupCoins(fuzzed_data_provider, utxo_pool, groups);
    if (groups.empty()) return;

    const CAmount target{fuzzed_data_provider.ConsumeIntegralInRange<CAmount>(1, COIN)};
    const int max_selection_weight{fuzzed_data_provider.ConsumeIntegralInRange<int>(0, MAX_STANDARD_TX_WEIGHT)};
    auto result{SelectCoinsExactOrLargest(groups, target, max_selection_weight)};
    if (!result) return;

    bool exact_exists{false};
    for (uint32_t pattern = 1; (pattern >> groups.size()) == 0; ++pattern) {
        CAmount subset_amount{0};
        int subset_weight{0};
        for (unsigned i = 0; i < groups.size(); ++i) {
            if ((pattern >> i) & 1) {
                subset_amount += groups.at(i).m_value;
                subset_weight += groups.at(i).m_weight;
            }
        }
        exact_exists |= subset_amount == target && subset_weight <= max_selection_weight;
    }
    if (exact_exists) {
        assert(result->GetAlgo() == SelectionAlgorithm::EXACT);
        assert(result->GetSelectedValue() == target);
        assert(result->GetChange() == 0);
    }
}

FUZZ_TARGET(coinselection)
{
    FuzzedDataProvider fuzzed_data_provider{buffer.data(), buffer.size()};
    std::vector<COutput> utxo_pool;
    int next_locktime{0};
    (void)CreateCoins(fuzzed_data_provider, utxo_pool, next_locktime);

    std::vector<OutputGroup> groups;
    GroupCoins(fuzzed_data_provider, utxo_pool, groups);

    const CAmount target{fuzzed_data_provider.ConsumeIntegralInRange<CAmount>(1, MAX_MONEY)};
    const int max_selection_weight{fuzzed_data_provider.ConsumeIntegralInRange<int>(0, std::numeric_limits<int>::max())};
    auto result{SelectCoinsExactOrLargest(groups, target, max_selection_weight)};
    if (!result) return;

    std::vector<COutput> manual_inputs;
    const CAmount manual_balance{CreateCoins(fuzzed_data_provider, manual_inputs, next_locktime)};
    if (manual_balance == 0) return;

    const size_t input_count{result->GetInputSet().size()};
    const int old_weight{result->GetWeight()};
    auto manual_selection{ManualSelection(manual_inputs, manual_balance)};
    result->Merge(manual_selection);
    assert(result->GetInputSet().size() == input_count + manual_inputs.size());
    assert(result->GetWeight() == old_weight + manual_selection.GetWeight());
}

} // namespace vault
