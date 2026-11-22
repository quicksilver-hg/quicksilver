// Copyright (c) 2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <vault/coinselection.h>
#include <vault/spend.h>
#include <vault/test/util.h>
#include <vault/vault.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <vector>

namespace vault {
BOOST_FIXTURE_TEST_SUITE(group_outputs_tests, TestingSetup)

static int nextLockTime = 0;

static std::shared_ptr<CVault> NewVault(const node::NodeContext& m_node)
{
    std::unique_ptr<CVault> vault = std::make_unique<CVault>(m_node.chain.get(), "", CreateMockableVaultDatabase());
    BOOST_REQUIRE(vault->LoadVault() == DBErrors::LOAD_OK);
    LOCK(vault->cs_vault);
    vault->SetVaultFlag(VAULT_FLAG_DESCRIPTORS);
    vault->SetupDescriptorScriptPubKeyMans();
    return vault;
}

static void addCoin(CoinsResult& coins,
                     CVault& vault,
                     const CTxDestination& dest,
                     const CAmount& nValue,
                     bool is_from_me,
                     int depth = 6)
{
    CMutableTransaction tx;
    tx.nLockTime = nextLockTime++;        // so all transactions get different hashes
    tx.vout.resize(1);
    tx.vout[0].nValue = nValue;
    tx.vout[0].scriptPubKey = GetScriptForDestination(dest);

    const auto txid{tx.GetHash().ToUint256()};
    LOCK(vault.cs_vault);
    auto ret = vault.mapVault.emplace(std::piecewise_construct, std::forward_as_tuple(txid), std::forward_as_tuple(MakeTransactionRef(std::move(tx)), TxStateInactive{}));
    assert(ret.second);
    CVaultTx& wtx = (*ret.first).second;
    const auto& txout = wtx.tx->vout.at(0);
    coins.Add(*Assert(OutputTypeFromDestination(dest)),
              {COutPoint(wtx.GetHash(), 0),
                   txout,
                   depth,
                   CalculateMaximumSignedInputSize(txout, &vault, /*coin_control=*/nullptr),
                   /*solvable=*/ true,
                   /*safe=*/ true,
                   wtx.GetTxTime(),
                   is_from_me});
}

 CoinSelectionParams makeSelectionParams(FastRandomContext& rand, bool avoid_partial_spends)
{
    CoinSelectionParams params{rand};
    params.m_avoid_partial_spends = avoid_partial_spends;
    return params;
}

class GroupVerifier
{
public:
    std::shared_ptr<CVault> vault{nullptr};
    CoinsResult coins_pool;
    FastRandomContext rand;

    void GroupVerify(const OutputType type,
                     const CoinEligibilityFilter& filter,
                     bool avoid_partial_spends,
                     bool positive_only,
                     int expected_size)
    {
        OutputGroupTypeMap groups = GroupOutputs(*vault, coins_pool, makeSelectionParams(rand, avoid_partial_spends), {{filter}})[filter];
        std::vector<OutputGroup>& groups_out = positive_only ? groups.groups_by_type[type].positive_group :
                                               groups.groups_by_type[type].mixed_group;
        BOOST_CHECK_EQUAL(groups_out.size(), expected_size);
    }

    void GroupAndVerify(const OutputType type,
                        const CoinEligibilityFilter& filter,
                        int expected_with_partial_spends_size,
                        int expected_without_partial_spends_size,
                        bool positive_only)
    {
        // First avoid partial spends
        GroupVerify(type, filter, /*avoid_partial_spends=*/false, positive_only,  expected_with_partial_spends_size);
        // Second don't avoid partial spends
        GroupVerify(type, filter, /*avoid_partial_spends=*/true, positive_only, expected_without_partial_spends_size);
    }
};

BOOST_AUTO_TEST_CASE(outputs_grouping_tests)
{
    const auto& vault = NewVault(m_node);
    GroupVerifier group_verifier;
    group_verifier.vault = vault;

    const CoinEligibilityFilter& BASIC_FILTER{1, 6, 0};

    // #################################################################################
    // 10 outputs from different txs going to the same script
    // 1) if partial spends is enabled --> must not be grouped
    // 2) if partial spends is not enabled --> must be grouped into a single OutputGroup
    // #################################################################################

    unsigned long GROUP_SIZE = 10;
    const CTxDestination dest = *Assert(vault->GetNewDestination(OutputType::BECH32, ""));
    for (unsigned long i = 0; i < GROUP_SIZE; i++) {
        addCoin(group_verifier.coins_pool, *vault, dest, 10 * COIN, /*is_from_me=*/true);
    }

    group_verifier.GroupAndVerify(OutputType::BECH32,
                                  BASIC_FILTER,
                                  /*expected_with_partial_spends_size=*/ GROUP_SIZE,
                                  /*expected_without_partial_spends_size=*/ 1,
                                  /*positive_only=*/ true);

    // ####################################################################################
    // 3) 10 more UTXO are added with a different script --> must be grouped into a single
    //    group for avoid partial spends and 10 different output groups for partial spends
    // ####################################################################################

    const CTxDestination dest2 = *Assert(vault->GetNewDestination(OutputType::BECH32, ""));
    for (unsigned long i = 0; i < GROUP_SIZE; i++) {
        addCoin(group_verifier.coins_pool, *vault, dest2, 5 * COIN, /*is_from_me=*/true);
    }

    group_verifier.GroupAndVerify(OutputType::BECH32,
            BASIC_FILTER,
            /*expected_with_partial_spends_size=*/ GROUP_SIZE * 2,
            /*expected_without_partial_spends_size=*/ 2,
            /*positive_only=*/ true);

    // ################################################################################
    // 4) Now add a tiny positive output --> the nValue > 0 filter does not skip it.
    // ################################################################################

    const CTxDestination dest3 = *Assert(vault->GetNewDestination(OutputType::BECH32, ""));
    addCoin(group_verifier.coins_pool, *vault, dest3, 1, true);
    BOOST_CHECK(group_verifier.coins_pool.coins[OutputType::BECH32].back().txout.nValue > 0);

    // First expect the tiny positive output to be included with "positive_only" enabled.
    group_verifier.GroupAndVerify(OutputType::BECH32,
            BASIC_FILTER,
            /*expected_with_partial_spends_size=*/ GROUP_SIZE * 2 + 1,
            /*expected_without_partial_spends_size=*/ 3,
            /*positive_only=*/ true);

    // Then the same counts with "positive_only" disabled: every coin so far is
    // positive, so the two pools hold the same coins. The split itself is pinned by
    // outputs_grouping_zero_value_coins (F-668, S0206-A-02).
    group_verifier.GroupAndVerify(OutputType::BECH32,
            BASIC_FILTER,
            /*expected_with_partial_spends_size=*/ GROUP_SIZE * 2 + 1,
            /*expected_without_partial_spends_size=*/ 3,
            /*positive_only=*/ false);


    // ##############################################################################
    // 5) Try to add a non-eligible UTXO (due not fulfilling the min depth target for
    //    "not mine" UTXOs) --> it must not be added to any group
    // ##############################################################################

    const CTxDestination dest4 = *Assert(vault->GetNewDestination(OutputType::BECH32, ""));
    addCoin(group_verifier.coins_pool, *vault, dest4, 6 * COIN,
            /*is_from_me=*/false, /*depth=*/5);

    // Expect no changes from this round and the previous one (point 4)
    group_verifier.GroupAndVerify(OutputType::BECH32,
            BASIC_FILTER,
            /*expected_with_partial_spends_size=*/ GROUP_SIZE * 2 + 1,
            /*expected_without_partial_spends_size=*/ 3,
            /*positive_only=*/ false);


    // ##############################################################################
    // 6) Try to add a non-eligible UTXO (due not fulfilling the min depth target for
    //    "mine" UTXOs) --> it must not be added to any group
    // ##############################################################################

    const CTxDestination dest5 = *Assert(vault->GetNewDestination(OutputType::BECH32, ""));
    addCoin(group_verifier.coins_pool, *vault, dest5, 6 * COIN,
            /*is_from_me=*/true, /*depth=*/0);

    // Expect no changes from this round and the previous one (point 5)
    group_verifier.GroupAndVerify(OutputType::BECH32,
            BASIC_FILTER,
            /*expected_with_partial_spends_size=*/ GROUP_SIZE * 2 + 1,
            /*expected_without_partial_spends_size=*/ 3,
            /*positive_only=*/ false);

    // ###########################################################################################
    // 7) Surpass the OUTPUT_GROUP_MAX_ENTRIES and verify that a second partial group gets created
    // ###########################################################################################

    const CTxDestination dest7 = *Assert(vault->GetNewDestination(OutputType::BECH32, ""));
    uint16_t NUM_SINGLE_ENTRIES = 101;
    for (unsigned long i = 0; i < NUM_SINGLE_ENTRIES; i++) { // OUTPUT_GROUP_MAX_ENTRIES{100}
        addCoin(group_verifier.coins_pool, *vault, dest7, 9 * COIN, /*is_from_me=*/true);
    }

    // Exclude partial groups only adds one more group to the previous test case (point 6)
    int PREVIOUS_ROUND_COUNT = GROUP_SIZE * 2 + 1;
    group_verifier.GroupAndVerify(OutputType::BECH32,
            BASIC_FILTER,
            /*expected_with_partial_spends_size=*/ PREVIOUS_ROUND_COUNT + NUM_SINGLE_ENTRIES,
            /*expected_without_partial_spends_size=*/ 4,
            /*positive_only=*/ false);

    // Include partial groups should add one more group inside the "avoid partial spends" count
    const CoinEligibilityFilter& avoid_partial_groups_filter{1, 6, 0, 0, /*include_partial=*/ true};
    group_verifier.GroupAndVerify(OutputType::BECH32,
            avoid_partial_groups_filter,
            /*expected_with_partial_spends_size=*/ PREVIOUS_ROUND_COUNT + NUM_SINGLE_ENTRIES,
            /*expected_without_partial_spends_size=*/ 5,
            /*positive_only=*/ false);
}

// F-668 (S0206-A-02): a zero-value coin is the one coin a feeless chain can put in the
// mixed pool but not the positive-only one. Without it, removing the nValue > 0 filter
// in GroupOutputs, or swapping which map it pushes as positive-only, leaves
// outputs_grouping_tests green.
BOOST_AUTO_TEST_CASE(outputs_grouping_zero_value_coins)
{
    const auto& vault = NewVault(m_node);
    GroupVerifier group_verifier;
    group_verifier.vault = vault;
    const CoinEligibilityFilter& BASIC_FILTER{1, 6, 0};

    // Two positive coins and one zero-value coin on one address, and a zero-value coin
    // alone on a second address.
    const CTxDestination shared = *Assert(vault->GetNewDestination(OutputType::BECH32, ""));
    const CTxDestination zero_only = *Assert(vault->GetNewDestination(OutputType::BECH32, ""));
    addCoin(group_verifier.coins_pool, *vault, shared, 10 * COIN, /*is_from_me=*/true);
    addCoin(group_verifier.coins_pool, *vault, shared, 10 * COIN, /*is_from_me=*/true);
    addCoin(group_verifier.coins_pool, *vault, shared, 0, /*is_from_me=*/true);
    addCoin(group_verifier.coins_pool, *vault, zero_only, 0, /*is_from_me=*/true);

    // Positive-only: the two 10 Hg coins, as two groups or as one address group.
    group_verifier.GroupAndVerify(OutputType::BECH32, BASIC_FILTER,
                                  /*expected_with_partial_spends_size=*/2,
                                  /*expected_without_partial_spends_size=*/1,
                                  /*positive_only=*/true);
    // Mixed: all four coins, as four groups or as two address groups.
    group_verifier.GroupAndVerify(OutputType::BECH32, BASIC_FILTER,
                                  /*expected_with_partial_spends_size=*/4,
                                  /*expected_without_partial_spends_size=*/2,
                                  /*positive_only=*/false);

    // The shared address's group holds its zero-value coin only in the mixed pool.
    OutputGroupTypeMap groups = GroupOutputs(*vault, group_verifier.coins_pool,
                                             makeSelectionParams(group_verifier.rand, /*avoid_partial_spends=*/true),
                                             {{BASIC_FILTER}})[BASIC_FILTER];
    const auto& by_type = groups.groups_by_type[OutputType::BECH32];
    BOOST_REQUIRE_EQUAL(by_type.positive_group.size(), 1U);
    BOOST_CHECK_EQUAL(by_type.positive_group[0].m_outputs.size(), 2U);
    BOOST_CHECK_EQUAL(by_type.positive_group[0].GetSelectionAmount(), 20 * COIN);
    std::vector<size_t> mixed_sizes;
    for (const OutputGroup& group : by_type.mixed_group) mixed_sizes.push_back(group.m_outputs.size());
    std::sort(mixed_sizes.begin(), mixed_sizes.end());
    BOOST_CHECK(mixed_sizes == (std::vector<size_t>{1, 3}));
}

BOOST_AUTO_TEST_SUITE_END()
} // end namespace vault
