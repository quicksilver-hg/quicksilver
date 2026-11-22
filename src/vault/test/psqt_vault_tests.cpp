// Copyright (c) 2017-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <primitives/transaction.h>
#include <psqt.h>
#include <script/interpreter.h>
#include <util/bip32.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <vector>

namespace vault {
BOOST_AUTO_TEST_SUITE(psqt_vault_tests)

BOOST_AUTO_TEST_CASE(parse_hd_keypath)
{
    std::vector<uint32_t> keypath;

    BOOST_CHECK(ParseHDKeypath("1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1", keypath));
    BOOST_CHECK(!ParseHDKeypath("///////////////////////////", keypath));

    BOOST_CHECK(ParseHDKeypath("1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1'/1", keypath));
    BOOST_CHECK(!ParseHDKeypath("//////////////////////////'/", keypath));

    BOOST_CHECK(ParseHDKeypath("1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/", keypath));
    BOOST_CHECK(!ParseHDKeypath("1///////////////////////////", keypath));

    BOOST_CHECK(ParseHDKeypath("1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1'/", keypath));
    BOOST_CHECK(!ParseHDKeypath("1/'//////////////////////////", keypath));

    BOOST_CHECK(ParseHDKeypath("", keypath));
    BOOST_CHECK(!ParseHDKeypath(" ", keypath));

    BOOST_CHECK(ParseHDKeypath("0", keypath));
    BOOST_CHECK(!ParseHDKeypath("O", keypath));

    BOOST_CHECK(ParseHDKeypath("0000'/0000'/0000'", keypath));
    BOOST_CHECK(!ParseHDKeypath("0000,/0000,/0000,", keypath));

    BOOST_CHECK(ParseHDKeypath("01234", keypath));
    BOOST_CHECK(!ParseHDKeypath("0x1234", keypath));

    BOOST_CHECK(ParseHDKeypath("1", keypath));
    BOOST_CHECK(!ParseHDKeypath(" 1", keypath));

    BOOST_CHECK(ParseHDKeypath("42", keypath));
    BOOST_CHECK(!ParseHDKeypath("m42", keypath));

    BOOST_CHECK(ParseHDKeypath("4294967295", keypath)); // 4294967295 == 0xFFFFFFFF (uint32_t max)
    BOOST_CHECK(!ParseHDKeypath("4294967296", keypath)); // 4294967296 == 0xFFFFFFFF (uint32_t max) + 1

    BOOST_CHECK(ParseHDKeypath("m", keypath));
    BOOST_CHECK(!ParseHDKeypath("n", keypath));

    BOOST_CHECK(ParseHDKeypath("m/", keypath));
    BOOST_CHECK(!ParseHDKeypath("n/", keypath));

    BOOST_CHECK(ParseHDKeypath("m/0", keypath));
    BOOST_CHECK(!ParseHDKeypath("n/0", keypath));

    BOOST_CHECK(ParseHDKeypath("m/0'", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/0''", keypath));

    BOOST_CHECK(ParseHDKeypath("m/0'/0'", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/'0/0'", keypath));

    BOOST_CHECK(ParseHDKeypath("m/0/0", keypath));
    BOOST_CHECK(!ParseHDKeypath("n/0/0", keypath));

    BOOST_CHECK(ParseHDKeypath("m/0/0/00", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/0/0/f00", keypath));

    BOOST_CHECK(ParseHDKeypath("m/0/0/000000000000000000000000000000000000000000000000000000000000000000000000000000000000", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/1/1/111111111111111111111111111111111111111111111111111111111111111111111111111111111111", keypath));

    BOOST_CHECK(ParseHDKeypath("m/0/00/0", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/0'/00/'0", keypath));

    BOOST_CHECK(ParseHDKeypath("m/1/", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/1//", keypath));

    BOOST_CHECK(ParseHDKeypath("m/0/4294967295", keypath)); // 4294967295 == 0xFFFFFFFF (uint32_t max)
    BOOST_CHECK(!ParseHDKeypath("m/0/4294967296", keypath)); // 4294967296 == 0xFFFFFFFF (uint32_t max) + 1

    BOOST_CHECK(ParseHDKeypath("m/4294967295", keypath)); // 4294967295 == 0xFFFFFFFF (uint32_t max)
    BOOST_CHECK(!ParseHDKeypath("m/4294967296", keypath)); // 4294967296 == 0xFFFFFFFF (uint32_t max) + 1

    // The cases above check only accept or refuse; these pin the parsed path
    // (S0207-A-03). Each starts from an empty vector.
    const auto parsed = [](const std::string& text) {
        std::vector<uint32_t> path;
        BOOST_REQUIRE_MESSAGE(ParseHDKeypath(text, path), text);
        return path;
    };
    constexpr uint32_t H{0x80000000};
    BOOST_CHECK(parsed("m").empty());
    BOOST_CHECK(parsed("").empty());
    BOOST_CHECK((parsed("m/0'/1/2'") == std::vector<uint32_t>{H | 0, 1, H | 2}));
    BOOST_CHECK((parsed("0000'/0000'/0000'") == std::vector<uint32_t>{H, H, H}));
    BOOST_CHECK((parsed("01234") == std::vector<uint32_t>{1234}));
    BOOST_CHECK((parsed("m/1/") == std::vector<uint32_t>{1}));
    BOOST_CHECK((parsed("4294967295") == std::vector<uint32_t>{0xFFFFFFFF}));
    BOOST_CHECK((parsed("m/2147483647'") == std::vector<uint32_t>{0xFFFFFFFF}));
}

// F-638: CombinePSQTs copies the first PSQT and merges the rest into it. Records
// that only a later PSQT carries (proprietary records at every level, an input's
// sighash type) must survive the combine instead of being dropped without error.
BOOST_AUTO_TEST_CASE(combine_keeps_later_proprietary_and_sighash)
{
    CMutableTransaction mtx;
    mtx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 0});
    mtx.vout.emplace_back(1, CScript{} << OP_TRUE);

    const PartiallySignedQuicksilverTransaction first{mtx};
    PartiallySignedQuicksilverTransaction later{mtx};
    const auto prop = [](unsigned char tag) {
        PSQTProprietary p;
        p.subtype = 7;
        p.identifier = {'q', 's'};
        p.key = {0xFC, 0x02, 'q', 's', 0x07, tag};
        p.value = {tag, tag};
        return p;
    };
    later.m_proprietary.insert(prop(1));
    later.inputs[0].m_proprietary.insert(prop(2));
    later.outputs[0].m_proprietary.insert(prop(3));
    later.inputs[0].sighash_type = SIGHASH_ALL;

    PartiallySignedQuicksilverTransaction out;
    BOOST_REQUIRE(CombinePSQTs(out, {first, later}));
    BOOST_CHECK_EQUAL(out.m_proprietary.size(), 1U);
    BOOST_CHECK_EQUAL(out.inputs[0].m_proprietary.size(), 1U);
    BOOST_CHECK_EQUAL(out.outputs[0].m_proprietary.size(), 1U);
    BOOST_CHECK(out.m_proprietary.count(prop(1)) == 1);
    BOOST_CHECK(out.inputs[0].m_proprietary.count(prop(2)) == 1);
    BOOST_CHECK(out.outputs[0].m_proprietary.count(prop(3)) == 1);
    BOOST_CHECK(out.inputs[0].sighash_type == std::optional<int>{SIGHASH_ALL});

    // The first PSQT's own sighash type is kept when a later one differs.
    PartiallySignedQuicksilverTransaction first_typed{mtx};
    first_typed.inputs[0].sighash_type = SIGHASH_SINGLE;
    BOOST_REQUIRE(CombinePSQTs(out, {first_typed, later}));
    BOOST_CHECK(out.inputs[0].sighash_type == std::optional<int>{SIGHASH_SINGLE});

    // An empty list is refused, not read past.
    BOOST_CHECK(!CombinePSQTs(out, {}));
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace vault
