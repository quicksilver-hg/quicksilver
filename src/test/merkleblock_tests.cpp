// Copyright (c) 2012-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <merkleblock.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/check.h>

#include <boost/test/unit_test.hpp>

#include <set>
#include <vector>

BOOST_AUTO_TEST_SUITE(merkleblock_tests)

/**
 * Create a CMerkleBlock using a list of txids which will be found in the
 * given block.
 */
BOOST_AUTO_TEST_CASE(merkleblock_construct_from_txids_found)
{
    CBlock block = getTestBlock9Tx();

    std::set<Txid> txids;

    // Last txn in block. (Derived from the block: Quicksilver's per-tx PoW tail is bound into
    // the txid.)
    Txid txhash1{block.vtx.back()->GetHash()};

    // Second txn in block.
    Txid txhash2{block.vtx[1]->GetHash()};

    txids.insert(txhash1);
    txids.insert(txhash2);

    CMerkleBlock merkleBlock(block, txids);

    BOOST_CHECK_EQUAL(merkleBlock.header.GetHash().GetHex(), block.GetHash().GetHex());

    std::vector<uint256> vMatched;
    std::vector<unsigned int> vIndex;

    BOOST_CHECK_EQUAL(merkleBlock.txn.ExtractMatches(vMatched, vIndex).GetHex(), block.hashMerkleRoot.GetHex());
    BOOST_REQUIRE_EQUAL(vMatched.size(), 2U);
    BOOST_REQUIRE_EQUAL(vIndex.size(), 2U);

    // Ordered by occurrence in depth-first tree traversal.
    BOOST_CHECK_EQUAL(vMatched[0].ToString(), txhash2.ToString());
    BOOST_CHECK_EQUAL(vIndex[0], 1U);

    BOOST_CHECK_EQUAL(vMatched[1].ToString(), txhash1.ToString());
    BOOST_CHECK_EQUAL(vIndex[1], 8U);
}


/**
 * Create a CMerkleBlock using a list of txids which will not be found in the
 * given block.
 */
BOOST_AUTO_TEST_CASE(merkleblock_construct_from_txids_not_found)
{
    CBlock block = getTestBlock9Tx();

    std::set<Txid> txids2;
    txids2.insert(Txid::FromHex("c0ffee00003bafa802c8aa084379aa98d9fcd632ddc2ed9782b586ec87451f20").value());
    CMerkleBlock merkleBlock(block, txids2);

    BOOST_CHECK_EQUAL(merkleBlock.header.GetHash().GetHex(), block.GetHash().GetHex());

    std::vector<uint256> vMatched;
    std::vector<unsigned int> vIndex;

    BOOST_CHECK_EQUAL(merkleBlock.txn.ExtractMatches(vMatched, vIndex).GetHex(), block.hashMerkleRoot.GetHex());
    BOOST_CHECK_EQUAL(vMatched.size(), 0U);
    BOOST_CHECK_EQUAL(vIndex.size(), 0U);
}

BOOST_AUTO_TEST_CASE(merkleblock_builder_preconditions)
{
    BOOST_CHECK_THROW(CPartialMerkleTree({}, {}), NonFatalCheckError);
    BOOST_CHECK_THROW(CPartialMerkleTree({uint256{1}}, {}), NonFatalCheckError);
}

BOOST_AUTO_TEST_CASE(merkleblock_replaces_outputs)
{
    const uint256 txid{1};
    CPartialMerkleTree valid({txid}, {true});
    std::vector<uint256> matches{uint256{2}};
    std::vector<unsigned int> indices{99};
    BOOST_CHECK_EQUAL(valid.ExtractMatches(matches, indices), txid);
    BOOST_REQUIRE_EQUAL(matches.size(), 1U);
    BOOST_CHECK_EQUAL(matches.front(), txid);
    BOOST_REQUIRE_EQUAL(indices.size(), 1U);
    BOOST_CHECK_EQUAL(indices.front(), 0U);

    // An invalid proof must discard the matches found before validation failed.
    CPartialMerkleTree invalid({txid, txid}, {true, true});
    BOOST_CHECK(invalid.ExtractMatches(matches, indices).IsNull());
    BOOST_CHECK(matches.empty());
    BOOST_CHECK(indices.empty());
}

BOOST_AUTO_TEST_SUITE_END()
