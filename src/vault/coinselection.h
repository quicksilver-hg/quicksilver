// Copyright (c) 2017-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_VAULT_COINSELECTION_H
#define QUICKSILVER_VAULT_COINSELECTION_H

#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <outputtype.h>
#include <primitives/transaction.h>
#include <random.h>
#include <util/insert.h>
#include <util/result.h>

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace vault {

/** A UTXO under consideration for use in funding a new transaction. */
struct COutput {
    COutPoint outpoint;
    CTxOut txout;
    int depth;
    int input_bytes;
    bool solvable;
    bool safe;
    int64_t time;
    bool from_me;

    COutput(const COutPoint& outpoint, const CTxOut& txout, int depth, int input_bytes, bool solvable, bool safe, int64_t time, bool from_me)
        : outpoint{outpoint},
          txout{txout},
          depth{depth},
          input_bytes{input_bytes},
          solvable{solvable},
          safe{safe},
          time{time},
          from_me{from_me}
    {
    }

    std::string ToString() const;

    bool operator<(const COutput& rhs) const { return outpoint < rhs.outpoint; }
};

/** Parameters for one iteration of coin selection. */
struct CoinSelectionParams {
    FastRandomContext& rng_fast;
    int change_output_size = 0;
    int tx_noinputs_size = 0;
    bool m_avoid_partial_spends = false;
    bool m_include_unsafe_inputs = false;
    std::optional<int> m_max_tx_weight{std::nullopt};

    explicit CoinSelectionParams(FastRandomContext& rng_fast) : rng_fast{rng_fast} {}
};

/** Parameters for filtering which OutputGroups we may use in coin selection. */
struct CoinEligibilityFilter
{
    const int conf_mine;
    const int conf_theirs;
    const uint64_t max_ancestors;
    const uint64_t max_descendants;
    const bool m_include_partial_groups{false};

    CoinEligibilityFilter() = delete;
    CoinEligibilityFilter(int conf_mine, int conf_theirs, uint64_t max_ancestors) : conf_mine(conf_mine), conf_theirs(conf_theirs), max_ancestors(max_ancestors), max_descendants(max_ancestors) {}
    CoinEligibilityFilter(int conf_mine, int conf_theirs, uint64_t max_ancestors, uint64_t max_descendants) : conf_mine(conf_mine), conf_theirs(conf_theirs), max_ancestors(max_ancestors), max_descendants(max_descendants) {}
    CoinEligibilityFilter(int conf_mine, int conf_theirs, uint64_t max_ancestors, uint64_t max_descendants, bool include_partial) : conf_mine(conf_mine), conf_theirs(conf_theirs), max_ancestors(max_ancestors), max_descendants(max_descendants), m_include_partial_groups(include_partial) {}

    bool operator<(const CoinEligibilityFilter& other) const
    {
        return std::tie(conf_mine, conf_theirs, max_ancestors, max_descendants, m_include_partial_groups)
               < std::tie(other.conf_mine, other.conf_theirs, other.max_ancestors, other.max_descendants, other.m_include_partial_groups);
    }
};

/** A group of UTXOs paid to the same output script. */
struct OutputGroup
{
    std::vector<std::shared_ptr<COutput>> m_outputs;
    bool m_from_me{true};
    CAmount m_value{0};
    int m_depth{999};
    size_t m_ancestors{0};
    size_t m_descendants{0};
    int m_weight{0};

    OutputGroup() = default;

    void Insert(const std::shared_ptr<COutput>& output, size_t ancestors, size_t descendants);
    bool EligibleForSpending(const CoinEligibilityFilter& eligibility_filter) const;
    CAmount GetSelectionAmount() const { return m_value; }
};

struct Groups {
    std::vector<OutputGroup> positive_group;
    std::vector<OutputGroup> mixed_group;
};

struct OutputGroupTypeMap
{
    std::map<OutputType, Groups> groups_by_type;
    Groups all_groups;

    void Push(const OutputGroup& group, OutputType type, bool insert_positive, bool insert_mixed);
    size_t TypesCount() { return groups_by_type.size(); }
};

using FilteredOutputGroups = std::map<CoinEligibilityFilter, OutputGroupTypeMap>;

enum class SelectionAlgorithm : uint8_t
{
    EXACT = 0,
    LARGEST_FIRST = 1,
    MANUAL = 2,
};

std::string GetAlgorithmName(SelectionAlgorithm algo);

struct SelectionResult
{
private:
    std::set<std::shared_ptr<COutput>> m_selected_inputs;
    CAmount m_target;
    SelectionAlgorithm m_algo;
    bool m_algo_completed{true};
    size_t m_selections_evaluated{0};
    int m_weight{0};

    template<typename T>
    void InsertInputs(const T& inputs)
    {
        const size_t expected_count = m_selected_inputs.size() + inputs.size();
        util::insert(m_selected_inputs, inputs);
        if (m_selected_inputs.size() != expected_count) {
            throw std::runtime_error(STR_INTERNAL_BUG("Shared UTXOs among selection results"));
        }
    }

public:
    explicit SelectionResult(CAmount target, SelectionAlgorithm algo) : m_target(target), m_algo(algo) {}
    SelectionResult() = delete;

    [[nodiscard]] CAmount GetSelectedValue() const;
    [[nodiscard]] CAmount GetChange() const;

    void Clear();
    void AddInput(const OutputGroup& group);
    void AddInputs(const std::set<std::shared_ptr<COutput>>& inputs);
    void SetAlgoCompleted(bool algo_completed) { m_algo_completed = algo_completed; }
    bool GetAlgoCompleted() const { return m_algo_completed; }
    void SetSelectionsEvaluated(size_t attempts) { m_selections_evaluated = attempts; }
    size_t GetSelectionsEvaluated() const { return m_selections_evaluated; }
    void Merge(const SelectionResult& other);

    const std::set<std::shared_ptr<COutput>>& GetInputSet() const { return m_selected_inputs; }
    std::vector<std::shared_ptr<COutput>> GetShuffledInputVector() const;
    CAmount GetTarget() const { return m_target; }
    SelectionAlgorithm GetAlgo() const { return m_algo; }
    int GetWeight() const { return m_weight; }
};

util::Result<SelectionResult> SelectCoinsExactOrLargest(std::vector<OutputGroup>& utxo_pool, CAmount target_value, int max_selection_weight, std::optional<int> fallback_max_selection_weight = std::nullopt);

} // namespace vault

#endif // QUICKSILVER_VAULT_COINSELECTION_H
