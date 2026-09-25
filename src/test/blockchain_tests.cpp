// Copyright (c) 2017-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <chainparamsbase.h>
#include <node/blockstorage.h>
#include <rpc/blockchain.h>
#include <sync.h>
#include <test/util/setup_common.h>

/* Given a BlockIndex with the provided nbits,
 * verify that the expected difficulty results.
 */
static void TestDifficulty(uint32_t nbits, const uint256& pow_limit, double expected_difficulty)
{
    CBlockIndex block_index;
    block_index.nBits = nbits;
    BOOST_CHECK_EQUAL(GetDifficulty(block_index, pow_limit), expected_difficulty);
}

BOOST_FIXTURE_TEST_SUITE(blockchain_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(get_difficulty_relative_to_each_chain_pow_limit)
{
    for (const ChainType chain_type : {ChainType::MAIN, ChainType::PUBLIC_TEST, ChainType::SANDBOX}) {
        const auto params{CreateChainParams(*m_node.args, chain_type)};
        const uint256& pow_limit{params->GetConsensus().powLimit};
        const uint32_t minimum_bits{UintToArith256(pow_limit).GetCompact()};
        TestDifficulty(minimum_bits, pow_limit, 1.0);

        arith_uint256 harder_target;
        harder_target.SetCompact(minimum_bits);
        harder_target >>= 8;
        TestDifficulty(harder_target.GetCompact(), pow_limit, 256.0);
    }
}

//! Prune chain from height down to genesis block and check that
//! GetPruneHeight returns the correct value
static void CheckGetPruneHeight(node::BlockManager& blockman, CChain& chain, int height) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    AssertLockHeld(::cs_main);

    // Emulate pruning all blocks from `height` down to the genesis block
    // by unsetting the `BLOCK_HAVE_DATA` flag from `nStatus`
    for (CBlockIndex* it{chain[height]}; it != nullptr && it->nHeight > 0; it = it->pprev) {
        it->nStatus &= ~BLOCK_HAVE_DATA;
    }

    const auto prune_height{GetPruneHeight(blockman, chain)};
    BOOST_REQUIRE(prune_height.has_value());
    BOOST_CHECK_EQUAL(*prune_height, height);
}

BOOST_FIXTURE_TEST_CASE(get_prune_height, TestChain100Setup)
{
    LOCK(::cs_main);
    auto& chain = m_node.chainman->ActiveChain();
    auto& blockman = m_node.chainman->m_blockman;

    // Fresh chain of 100 blocks without any pruned blocks, so std::nullopt should be returned
    BOOST_CHECK(!GetPruneHeight(blockman, chain).has_value());

    // Start pruning
    CheckGetPruneHeight(blockman, chain, 1);
    CheckGetPruneHeight(blockman, chain, 99);
    CheckGetPruneHeight(blockman, chain, 100);
}

BOOST_AUTO_TEST_CASE(num_chain_tx_max)
{
    CBlockIndex block_index{};
    block_index.m_chain_tx_count = std::numeric_limits<uint64_t>::max();
    BOOST_CHECK_EQUAL(block_index.m_chain_tx_count, std::numeric_limits<uint64_t>::max());
}

BOOST_AUTO_TEST_SUITE_END()
