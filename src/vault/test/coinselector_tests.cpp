// Copyright (c) 2017-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <node/context.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <vault/coinselection.h>
#include <vault/spend.h>
#include <vault/test/util.h>
#include <vault/test/vault_test_fixture.h>
#include <vault/vault.h>

#include <boost/test/unit_test.hpp>

namespace vault {
BOOST_FIXTURE_TEST_SUITE(coinselector_tests, VaultTestingSetup)

static int next_lock_time{0};

static std::unique_ptr<CVault> NewVault(const node::NodeContext& node)
{
    auto vault{std::make_unique<CVault>(node.chain.get(), "", CreateMockableVaultDatabase())};
    BOOST_REQUIRE(vault->LoadVault() == DBErrors::LOAD_OK);
    LOCK(vault->cs_vault);
    vault->SetVaultFlag(VAULT_FLAG_DESCRIPTORS);
    vault->SetupDescriptorScriptPubKeyMans();
    return vault;
}

static COutput MakeOutput(CVault& vault, CAmount value, int n_input, int input_bytes)
{
    CMutableTransaction tx;
    tx.nLockTime = next_lock_time++;
    tx.vout.resize(n_input + 1);
    tx.vout[n_input].nValue = value;
    tx.vout[n_input].scriptPubKey = CScript() << OP_TRUE;

    const uint256 txid{tx.GetHash()};
    LOCK(vault.cs_vault);
    auto ret{vault.mapVault.emplace(std::piecewise_construct, std::forward_as_tuple(txid), std::forward_as_tuple(MakeTransactionRef(std::move(tx)), TxStateInactive{}))};
    BOOST_REQUIRE(ret.second);
    const CVaultTx& wtx{ret.first->second};
    const CTxOut& txout{wtx.tx->vout.at(n_input)};
    return COutput{COutPoint(wtx.GetHash(), n_input), txout, /*depth=*/6, input_bytes, /*solvable=*/true, /*safe=*/true, wtx.GetTxTime(), /*from_me=*/true};
}

static void AddCoin(CoinsResult& available_coins, CVault& vault, CAmount value, int n_input, int input_bytes)
{
    available_coins.Add(OutputType::BECH32, MakeOutput(vault, value, n_input, input_bytes));
}

static OutputGroup MakeGroup(CVault& vault, CAmount value, int n_input, int input_bytes)
{
    OutputGroup group;
    group.Insert(std::make_shared<COutput>(MakeOutput(vault, value, n_input, input_bytes)), /*ancestors=*/0, /*descendants=*/0);
    return group;
}

static std::vector<OutputGroup> GroupCoins(const std::vector<COutput>& coins)
{
    std::vector<OutputGroup> groups;
    for (const COutput& coin : coins) {
        OutputGroup group;
        group.Insert(std::make_shared<COutput>(coin), /*ancestors=*/0, /*descendants=*/0);
        groups.push_back(group);
    }
    return groups;
}

BOOST_AUTO_TEST_CASE(exact_match_wins_before_largest_first)
{
    CoinsResult available_coins;
    auto vault{NewVault(m_node)};

    AddCoin(available_coins, *vault, 101 * CENT, /*n_input=*/0, /*input_bytes=*/68);
    AddCoin(available_coins, *vault, 60 * CENT, /*n_input=*/1, /*input_bytes=*/68);
    AddCoin(available_coins, *vault, 40 * CENT, /*n_input=*/2, /*input_bytes=*/68);

    auto groups{GroupCoins(available_coins.All())};
    auto result{SelectCoinsExactOrLargest(groups, 100 * CENT, MAX_STANDARD_TX_WEIGHT)};

    BOOST_REQUIRE(result);
    BOOST_CHECK_EQUAL(result->GetAlgo(), SelectionAlgorithm::EXACT);
    BOOST_CHECK_EQUAL(result->GetSelectedValue(), 100 * CENT);
    BOOST_CHECK_EQUAL(result->GetChange(), 0);
    BOOST_CHECK_EQUAL(result->GetInputSet().size(), 2);
}

BOOST_AUTO_TEST_CASE(largest_first_fallback_minimizes_input_count)
{
    CoinsResult available_coins;
    auto vault{NewVault(m_node)};

    AddCoin(available_coins, *vault, 70 * CENT, /*n_input=*/0, /*input_bytes=*/68);
    AddCoin(available_coins, *vault, 40 * CENT, /*n_input=*/1, /*input_bytes=*/68);
    AddCoin(available_coins, *vault, 30 * CENT, /*n_input=*/2, /*input_bytes=*/68);

    auto groups{GroupCoins(available_coins.All())};
    auto result{SelectCoinsExactOrLargest(groups, 75 * CENT, MAX_STANDARD_TX_WEIGHT)};

    BOOST_REQUIRE(result);
    BOOST_CHECK_EQUAL(result->GetAlgo(), SelectionAlgorithm::LARGEST_FIRST);
    BOOST_CHECK_EQUAL(result->GetSelectedValue(), 110 * CENT);
    BOOST_CHECK_EQUAL(result->GetChange(), 35 * CENT);
    BOOST_CHECK_EQUAL(result->GetInputSet().size(), 2);
}

BOOST_AUTO_TEST_CASE(exact_match_respects_max_selection_weight)
{
    CoinsResult available_coins;
    auto vault{NewVault(m_node)};

    AddCoin(available_coins, *vault, 5 * COIN, /*n_input=*/0, /*input_bytes=*/MAX_STANDARD_TX_WEIGHT);
    AddCoin(available_coins, *vault, 3 * COIN, /*n_input=*/1, /*input_bytes=*/68);
    AddCoin(available_coins, *vault, 2 * COIN, /*n_input=*/2, /*input_bytes=*/68);

    auto groups{GroupCoins(available_coins.All())};
    auto result{SelectCoinsExactOrLargest(groups, 5 * COIN, MAX_STANDARD_TX_WEIGHT - WITNESS_SCALE_FACTOR)};

    BOOST_REQUIRE(result);
    BOOST_CHECK_EQUAL(result->GetAlgo(), SelectionAlgorithm::EXACT);
    BOOST_CHECK_EQUAL(result->GetSelectedValue(), 5 * COIN);
    BOOST_CHECK_EQUAL(result->GetInputSet().size(), 2);
    BOOST_CHECK_LE(result->GetWeight(), MAX_STANDARD_TX_WEIGHT - WITNESS_SCALE_FACTOR);
}

BOOST_AUTO_TEST_CASE(selection_fails_when_total_value_is_insufficient)
{
    CoinsResult available_coins;
    auto vault{NewVault(m_node)};

    AddCoin(available_coins, *vault, 2 * CENT, /*n_input=*/0, /*input_bytes=*/68);
    AddCoin(available_coins, *vault, 3 * CENT, /*n_input=*/1, /*input_bytes=*/68);

    auto groups{GroupCoins(available_coins.All())};
    auto result{SelectCoinsExactOrLargest(groups, 6 * CENT, MAX_STANDARD_TX_WEIGHT)};

    BOOST_CHECK(!result);
}

BOOST_AUTO_TEST_CASE(exact_search_is_bounded)
{
    CoinsResult available_coins;
    auto vault{NewVault(m_node)};

    CAmount available{0};
    for (int i{0}; i < 17; ++i) {
        const CAmount value{CAmount{2} << i};
        AddCoin(available_coins, *vault, value, /*n_input=*/i, /*input_bytes=*/68);
        available += value;
    }

    auto groups{GroupCoins(available_coins.All())};
    auto result{SelectCoinsExactOrLargest(groups, available - 1, MAX_STANDARD_TX_WEIGHT)};

    BOOST_REQUIRE(result);
    BOOST_CHECK_EQUAL(result->GetAlgo(), SelectionAlgorithm::LARGEST_FIRST);
    BOOST_CHECK_EQUAL(result->GetSelectedValue(), available);
    BOOST_CHECK_EQUAL(result->GetChange(), 1);
    BOOST_CHECK_EQUAL(result->GetSelectionsEvaluated(), 100'000);
    BOOST_CHECK(!result->GetAlgoCompleted());
}

BOOST_AUTO_TEST_CASE(attempt_selection_prefers_exact_match_across_output_types)
{
    auto vault{NewVault(m_node)};

    OutputGroupTypeMap groups;
    groups.groups_by_type[OutputType::BECH32].positive_group.push_back(MakeGroup(*vault, 101 * CENT, /*n_input=*/0, /*input_bytes=*/68));
    groups.groups_by_type[OutputType::BASE58].positive_group.push_back(MakeGroup(*vault, 60 * CENT, /*n_input=*/1, /*input_bytes=*/68));
    groups.groups_by_type[OutputType::BASE58].positive_group.push_back(MakeGroup(*vault, 40 * CENT, /*n_input=*/2, /*input_bytes=*/68));
    groups.all_groups.positive_group.insert(groups.all_groups.positive_group.end(),
                                            groups.groups_by_type[OutputType::BECH32].positive_group.begin(),
                                            groups.groups_by_type[OutputType::BECH32].positive_group.end());
    groups.all_groups.positive_group.insert(groups.all_groups.positive_group.end(),
                                            groups.groups_by_type[OutputType::BASE58].positive_group.begin(),
                                            groups.groups_by_type[OutputType::BASE58].positive_group.end());

    FastRandomContext rng{/*fDeterministic=*/true};
    CoinSelectionParams params{rng};
    auto result{AttemptSelection(100 * CENT, groups, params, /*allow_mixed_output_types=*/true)};

    BOOST_REQUIRE(result);
    BOOST_CHECK_EQUAL(result->GetAlgo(), SelectionAlgorithm::EXACT);
    BOOST_CHECK_EQUAL(result->GetSelectedValue(), 100 * CENT);
    BOOST_CHECK_EQUAL(result->GetChange(), 0);
    BOOST_CHECK_EQUAL(result->GetInputSet().size(), 2);
}

BOOST_AUTO_TEST_CASE(largest_first_reserves_change_output_weight)
{
    auto vault{NewVault(m_node)};

    Groups groups;
    groups.positive_group.push_back(MakeGroup(*vault, 70 * CENT, /*n_input=*/0, /*input_bytes=*/68));
    groups.positive_group.push_back(MakeGroup(*vault, 40 * CENT, /*n_input=*/1, /*input_bytes=*/68));
    groups.positive_group.push_back(MakeGroup(*vault, 30 * CENT, /*n_input=*/2, /*input_bytes=*/68));

    FastRandomContext rng{/*fDeterministic=*/true};
    CoinSelectionParams params{rng};
    params.tx_noinputs_size = 10;
    params.change_output_size = 31;
    const int no_input_weight{params.tx_noinputs_size * WITNESS_SCALE_FACTOR};
    const int two_input_weight{2 * 68 * WITNESS_SCALE_FACTOR};
    params.m_max_tx_weight = no_input_weight + two_input_weight;

    auto result{ChooseSelectionResult(75 * CENT, groups, params)};

    BOOST_CHECK(!result);
    BOOST_CHECK(!util::ErrorString(result).empty());
}

BOOST_AUTO_TEST_CASE(exact_match_does_not_reserve_change_output_weight)
{
    auto vault{NewVault(m_node)};

    Groups groups;
    groups.positive_group.push_back(MakeGroup(*vault, 60 * CENT, /*n_input=*/0, /*input_bytes=*/68));
    groups.positive_group.push_back(MakeGroup(*vault, 40 * CENT, /*n_input=*/1, /*input_bytes=*/68));

    FastRandomContext rng{/*fDeterministic=*/true};
    CoinSelectionParams params{rng};
    params.tx_noinputs_size = 10;
    params.change_output_size = 31;
    const int no_input_weight{params.tx_noinputs_size * WITNESS_SCALE_FACTOR};
    const int two_input_weight{2 * 68 * WITNESS_SCALE_FACTOR};
    params.m_max_tx_weight = no_input_weight + two_input_weight;

    auto result{ChooseSelectionResult(100 * CENT, groups, params)};

    BOOST_REQUIRE(result);
    BOOST_CHECK_EQUAL(result->GetAlgo(), SelectionAlgorithm::EXACT);
    BOOST_CHECK_EQUAL(result->GetChange(), 0);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace vault
