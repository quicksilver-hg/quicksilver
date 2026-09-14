// Copyright (c) 2017-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <vault/coinselection.h>

#include <common/system.h>
#include <consensus/consensus.h>
#include <util/moneystr.h>

#include <algorithm>
#include <functional>
#include <map>
#include <numeric>
#include <utility>

namespace vault {
// Exact subset selection is exponential for varied UTXO values. Bound the work
// and fall back to largest-first when the exact search cannot finish.
static constexpr size_t MAX_EXACT_SELECTION_TRIES{100'000};

static util::Result<SelectionResult> ErrorMaxWeightExceeded()
{
    return util::Error{_("The inputs size exceeds the maximum weight. Please try sending a smaller amount or manually consolidating your vault's UTXOs")};
}

static bool DescendingRawValueThenWeight(const OutputGroup& a, const OutputGroup& b)
{
    if (a.m_value == b.m_value) return a.m_weight < b.m_weight;
    return a.m_value > b.m_value;
}

struct ExactCandidate {
    int weight{0};
    std::vector<size_t> indexes;
};

static bool BetterExactCandidate(const ExactCandidate& candidate, const ExactCandidate& current)
{
    if (candidate.weight != current.weight) return candidate.weight < current.weight;
    return candidate.indexes.size() < current.indexes.size();
}

util::Result<SelectionResult> SelectCoinsExactOrLargest(std::vector<OutputGroup>& utxo_pool, CAmount target_value, int max_selection_weight, std::optional<int> fallback_max_selection_weight)
{
    utxo_pool.erase(std::remove_if(utxo_pool.begin(), utxo_pool.end(), [](const OutputGroup& group) {
        return group.m_value <= 0;
    }), utxo_pool.end());

    CAmount available{0};
    for (const OutputGroup& group : utxo_pool) {
        available += group.m_value;
    }
    if (available < target_value) return util::Error();

    std::sort(utxo_pool.begin(), utxo_pool.end(), DescendingRawValueThenWeight);

    bool max_weight_exceeded{false};
    bool exact_search_completed{true};
    size_t selections_evaluated{0};
    std::map<CAmount, ExactCandidate> best_by_sum;
    best_by_sum.emplace(0, ExactCandidate{});

    for (size_t index{0}; index < utxo_pool.size(); ++index) {
        const OutputGroup& group{utxo_pool.at(index)};
        // Defer map updates so a group cannot be selected more than once. Unlike
        // copying best_by_sum, this frontier contains only newly formed states.
        std::vector<std::pair<CAmount, ExactCandidate>> pending;
        pending.reserve(std::min(best_by_sum.size(), MAX_EXACT_SELECTION_TRIES - selections_evaluated));

        auto it{best_by_sum.cbegin()};
        for (; it != best_by_sum.cend() && selections_evaluated < MAX_EXACT_SELECTION_TRIES; ++it) {
            const auto& [sum, candidate]{*it};
            ++selections_evaluated;
            const CAmount next_sum{sum + group.m_value};
            if (next_sum > target_value) continue;

            ExactCandidate next{candidate};
            next.weight += group.m_weight;
            next.indexes.push_back(index);
            if (next.weight > max_selection_weight) {
                max_weight_exceeded = true;
                continue;
            }

            pending.emplace_back(next_sum, std::move(next));
        }

        for (auto& [sum, candidate] : pending) {
            auto candidate_it{best_by_sum.lower_bound(sum)};
            if (candidate_it == best_by_sum.end() || candidate_it->first != sum) {
                best_by_sum.emplace_hint(candidate_it, sum, std::move(candidate));
            } else if (BetterExactCandidate(candidate, candidate_it->second)) {
                candidate_it->second = std::move(candidate);
            }
        }

        if (it != best_by_sum.cend() || (selections_evaluated == MAX_EXACT_SELECTION_TRIES && index + 1 < utxo_pool.size())) {
            exact_search_completed = false;
            break;
        }
    }

    if (auto it{best_by_sum.find(target_value)}; it != best_by_sum.end() && !it->second.indexes.empty()) {
        SelectionResult result(target_value, SelectionAlgorithm::EXACT);
        for (size_t index : it->second.indexes) {
            result.AddInput(utxo_pool.at(index));
        }
        result.SetSelectionsEvaluated(selections_evaluated);
        result.SetAlgoCompleted(exact_search_completed);
        return result;
    }

    const int fallback_weight{fallback_max_selection_weight.value_or(max_selection_weight)};
    if (fallback_weight <= 0) return ErrorMaxWeightExceeded();

    SelectionResult result(target_value, SelectionAlgorithm::LARGEST_FIRST);
    CAmount selected{0};
    int selected_weight{0};
    for (const OutputGroup& group : utxo_pool) {
        if (selected >= target_value) break;
        if (selected_weight + group.m_weight > fallback_weight) {
            max_weight_exceeded = true;
            continue;
        }
        result.AddInput(group);
        selected += group.m_value;
        selected_weight += group.m_weight;
    }
    if (selected < target_value) {
        return max_weight_exceeded ? ErrorMaxWeightExceeded() : util::Error();
    }
    result.SetSelectionsEvaluated(selections_evaluated);
    result.SetAlgoCompleted(exact_search_completed);
    return result;
}

void OutputGroup::Insert(const std::shared_ptr<COutput>& output, size_t ancestors, size_t descendants)
{
    m_outputs.push_back(output);
    const COutput& coin{*m_outputs.back()};

    m_from_me &= coin.from_me;
    m_value += coin.txout.nValue;
    m_depth = std::min(m_depth, coin.depth);
    m_ancestors += ancestors;
    m_descendants = std::max(m_descendants, descendants);

    if (coin.input_bytes > 0) {
        m_weight += coin.input_bytes * WITNESS_SCALE_FACTOR;
    }
}

bool OutputGroup::EligibleForSpending(const CoinEligibilityFilter& eligibility_filter) const
{
    return m_depth >= (m_from_me ? eligibility_filter.conf_mine : eligibility_filter.conf_theirs)
        && m_ancestors <= eligibility_filter.max_ancestors
        && m_descendants <= eligibility_filter.max_descendants;
}

void OutputGroupTypeMap::Push(const OutputGroup& group, OutputType type, bool insert_positive, bool insert_mixed)
{
    if (group.m_outputs.empty()) return;

    Groups& groups = groups_by_type[type];
    if (insert_positive && group.GetSelectionAmount() > 0) {
        groups.positive_group.emplace_back(group);
        all_groups.positive_group.emplace_back(group);
    }
    if (insert_mixed) {
        groups.mixed_group.emplace_back(group);
        all_groups.mixed_group.emplace_back(group);
    }
}

CAmount SelectionResult::GetSelectedValue() const
{
    return std::accumulate(m_selected_inputs.cbegin(), m_selected_inputs.cend(), CAmount{0}, [](CAmount sum, const auto& coin) {
        return sum + coin->txout.nValue;
    });
}

CAmount SelectionResult::GetChange() const
{
    const CAmount selected_value{GetSelectedValue()};
    assert(selected_value >= m_target);
    return selected_value - m_target;
}

void SelectionResult::Clear()
{
    m_selected_inputs.clear();
    m_weight = 0;
}

void SelectionResult::AddInput(const OutputGroup& group)
{
    InsertInputs(group.m_outputs);
    m_weight += group.m_weight;
}

void SelectionResult::AddInputs(const std::set<std::shared_ptr<COutput>>& inputs)
{
    InsertInputs(inputs);
    m_weight += std::accumulate(inputs.cbegin(), inputs.cend(), 0, [](int sum, const auto& coin) {
        return sum + std::max(coin->input_bytes, 0) * WITNESS_SCALE_FACTOR;
    });
}

void SelectionResult::Merge(const SelectionResult& other)
{
    InsertInputs(other.m_selected_inputs);
    m_target += other.m_target;
    if (m_algo == SelectionAlgorithm::MANUAL) {
        m_algo = other.m_algo;
    }
    m_weight += other.m_weight;
}

std::vector<std::shared_ptr<COutput>> SelectionResult::GetShuffledInputVector() const
{
    std::vector<std::shared_ptr<COutput>> coins(m_selected_inputs.begin(), m_selected_inputs.end());
    std::shuffle(coins.begin(), coins.end(), FastRandomContext());
    return coins;
}

std::string COutput::ToString() const
{
    return strprintf("COutput(%s, %d, %d) [%s]", outpoint.hash.ToString(), outpoint.n, depth, FormatMoney(txout.nValue));
}

std::string GetAlgorithmName(const SelectionAlgorithm algo)
{
    switch (algo) {
    case SelectionAlgorithm::EXACT: return "exact";
    case SelectionAlgorithm::LARGEST_FIRST: return "largest-first";
    case SelectionAlgorithm::MANUAL: return "manual";
    }
    assert(false);
}

} // namespace vault
