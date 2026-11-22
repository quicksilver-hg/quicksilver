// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <outputtype.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>

BOOST_AUTO_TEST_SUITE(outputtype_tests)

/**
 * These numbers live in every vault file on disk.
 *
 * `VaultBatch::WriteActiveScriptPubKeyMan` stores the address type as a raw
 * `uint8_t` in the record key. The loader validates the stored value with
 * `OutputTypeFromStored`; renumbering a recognized type could still associate
 * an existing descriptor with a different address type.
 *
 * This test establishes the compact pre-launch mapping so future edits fail
 * here, loudly, instead of silently changing a vault database format.
 */
BOOST_AUTO_TEST_CASE(output_type_values_are_a_stored_format)
{
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(OutputType::BASE58), 0);
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(OutputType::BECH32), 1);
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(OutputType::BECH32M), 2);
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(OutputType::UNKNOWN), 3);
}

BOOST_AUTO_TEST_SUITE_END()
