// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/data/key_io_invalid.json.h>
#include <test/data/key_io_valid.json.h>

#include <key.h>
#include <key_io.h>
#include <script/script.h>
#include <test/util/json.h>
#include <test/util/setup_common.h>
#include <univalue.h>
#include <util/chaintype.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstring>

BOOST_FIXTURE_TEST_SUITE(key_io_tests, BasicTestingSetup)

// Goal: check that parsed keys match test payload
BOOST_AUTO_TEST_CASE(key_io_valid_parse)
{
    UniValue tests = read_json(json_tests::key_io_valid);
    CKey privkey;
    CTxDestination destination;
    SelectParams(ChainType::MAIN);

    for (unsigned int idx = 0; idx < tests.size(); idx++) {
        const UniValue& test = tests[idx];
        std::string strTest = test.write();
        if (test.size() < 3) { // Allow for extra stuff (useful for comments)
            BOOST_ERROR("Bad test: " << strTest);
            continue;
        }
        std::string exp_base58string = test[0].get_str();
        const std::vector<std::byte> exp_payload{ParseHex<std::byte>(test[1].get_str())};
        const UniValue &metadata = test[2].get_obj();
        bool isPrivkey = metadata.find_value("isPrivkey").get_bool();
        SelectParams(ChainTypeFromString(metadata.find_value("chain").get_str()).value());
        bool try_case_flip = metadata.find_value("tryCaseFlip").isNull() ? false : metadata.find_value("tryCaseFlip").get_bool();
        if (isPrivkey) {
            bool isCompressed = metadata.find_value("isCompressed").get_bool();
            // Must be valid private key
            privkey = DecodeSecret(exp_base58string);
            BOOST_CHECK_MESSAGE(privkey.IsValid(), "!IsValid:" + strTest);
            BOOST_CHECK_MESSAGE(privkey.IsCompressed() == isCompressed, "compressed mismatch:" + strTest);
            BOOST_CHECK_MESSAGE(std::ranges::equal(privkey, exp_payload), "key mismatch:" + strTest);

            // Private key must be invalid public key
            destination = DecodeDestination(exp_base58string);
            BOOST_CHECK_MESSAGE(!IsValidDestination(destination), "IsValid privkey as pubkey:" + strTest);
        } else {
            // Must be valid public key
            destination = DecodeDestination(exp_base58string);
            CScript script = GetScriptForDestination(destination);
            BOOST_CHECK_MESSAGE(IsValidDestination(destination), "!IsValid:" + strTest);
            BOOST_CHECK_EQUAL(HexStr(script), HexStr(exp_payload));

            // Try flipped case version
            for (char& c : exp_base58string) {
                if (c >= 'a' && c <= 'z') {
                    c = (c - 'a') + 'A';
                } else if (c >= 'A' && c <= 'Z') {
                    c = (c - 'A') + 'a';
                }
            }
            destination = DecodeDestination(exp_base58string);
            BOOST_CHECK_MESSAGE(IsValidDestination(destination) == try_case_flip, "!IsValid case flipped:" + strTest);
            if (IsValidDestination(destination)) {
                script = GetScriptForDestination(destination);
                BOOST_CHECK_EQUAL(HexStr(script), HexStr(exp_payload));
            }

            // Public key must be invalid private key
            privkey = DecodeSecret(exp_base58string);
            BOOST_CHECK_MESSAGE(!privkey.IsValid(), "IsValid pubkey as privkey:" + strTest);
        }
    }
}

// Goal: check that generated keys match test vectors
BOOST_AUTO_TEST_CASE(key_io_valid_gen)
{
    UniValue tests = read_json(json_tests::key_io_valid);

    for (unsigned int idx = 0; idx < tests.size(); idx++) {
        const UniValue& test = tests[idx];
        std::string strTest = test.write();
        if (test.size() < 3) // Allow for extra stuff (useful for comments)
        {
            BOOST_ERROR("Bad test: " << strTest);
            continue;
        }
        std::string exp_base58string = test[0].get_str();
        std::vector<unsigned char> exp_payload = ParseHex(test[1].get_str());
        const UniValue &metadata = test[2].get_obj();
        bool isPrivkey = metadata.find_value("isPrivkey").get_bool();
        SelectParams(ChainTypeFromString(metadata.find_value("chain").get_str()).value());
        if (isPrivkey) {
            bool isCompressed = metadata.find_value("isCompressed").get_bool();
            CKey key;
            key.Set(exp_payload.begin(), exp_payload.end(), isCompressed);
            assert(key.IsValid());
            BOOST_CHECK_MESSAGE(EncodeSecret(key) == exp_base58string, "result mismatch: " + strTest);
        } else {
            CTxDestination dest;
            CScript exp_script(exp_payload.begin(), exp_payload.end());
            BOOST_CHECK(ExtractDestination(exp_script, dest));
            std::string address = EncodeDestination(dest);

            BOOST_CHECK_EQUAL(address, exp_base58string);
        }
    }

    SelectParams(ChainType::MAIN);
}


// Goal: check that base58 parsing code is robust against a variety of corrupted data
BOOST_AUTO_TEST_CASE(key_io_invalid)
{
    UniValue tests = read_json(json_tests::key_io_invalid); // Negative testcases
    CKey privkey;
    CTxDestination destination;

    for (unsigned int idx = 0; idx < tests.size(); idx++) {
        const UniValue& test = tests[idx];
        std::string strTest = test.write();
        if (test.size() < 1) // Allow for extra stuff (useful for comments)
        {
            BOOST_ERROR("Bad test: " << strTest);
            continue;
        }
        std::string exp_base58string = test[0].get_str();

        // must be invalid as public and as private key
        for (const auto& chain : {ChainType::MAIN, ChainType::PUBLIC_TEST, ChainType::SANDBOX}) {
            SelectParams(chain);
            destination = DecodeDestination(exp_base58string);
            BOOST_CHECK_MESSAGE(!IsValidDestination(destination), "IsValid pubkey in mainnet:" + strTest);
            privkey = DecodeSecret(exp_base58string);
            BOOST_CHECK_MESSAGE(!privkey.IsValid(), "IsValid privkey in mainnet:" + strTest);
        }
    }
}

// Quicksilver's Bech32 HRPs are prefixes of its own Base58 leading characters --
// sandbox encodes P2SH as 's...' against HRP "shg", publictest as 'p...' against
// "phg" -- which upstream's "bc"/"tb"/"bcrt" never are. Choosing the decoder by
// prefix therefore handed roughly one Base58 address in 841 to the Bech32 decoder,
// which rejected it as invalid. Found via tool_vault.py, which mints 500 addresses
// and so failed about 45% of runs.
BOOST_AUTO_TEST_CASE(base58_address_beginning_with_the_bech32_hrp_still_decodes)
{
    const struct {
        ChainType chain;
        const char* hrp;
        bool collides;  //!< whether a Base58 address on this network can begin with the HRP
    } cases[]{
        // main's Base58 addresses lead with 'Q' (P2PKH, version 58) or 'M'/'N'
        // (P2SH, version 50), so they can never begin "hg". It is safe by the
        // accident of which version bytes the rebrand picked -- not by design,
        // which is why the decoder is fixed for every network rather than two.
        {ChainType::MAIN, "hg", false},
        {ChainType::PUBLIC_TEST, "phg", true},
        {ChainType::SANDBOX, "shg", true},
    };

    for (const auto& c : cases) {
        SelectParams(c.chain);
        const auto& params{Params()};
        BOOST_REQUIRE_EQUAL(params.Bech32HRP(), c.hrp);

        // Search the P2SH and P2PKH keyspaces for an address whose Base58 form
        // begins with this network's HRP. Such addresses genuinely occur; this
        // finds a real one rather than asserting on a hand-made string.
        bool found{false};
        for (uint32_t i = 0; i < 200000 && !found; ++i) {
            uint160 h;
            std::memcpy(h.begin(), &i, sizeof(i));
            for (const CTxDestination& dest : {CTxDestination{ScriptHash(h)}, CTxDestination{PKHash(h)}}) {
                const std::string addr{EncodeDestination(dest)};
                if (ToLower(addr.substr(0, params.Bech32HRP().size())) != params.Bech32HRP()) continue;
                found = true;

                std::string error;
                const CTxDestination decoded{DecodeDestination(addr, error)};
                BOOST_CHECK_MESSAGE(IsValidDestination(decoded),
                                    ChainTypeToString(c.chain) << ": " << addr
                                    << " begins with the Bech32 HRP and was rejected: " << error);
                BOOST_CHECK_MESSAGE(decoded == dest,
                                    ChainTypeToString(c.chain) << ": " << addr
                                    << " did not round-trip to the destination it encodes");
                break;
            }
        }
        BOOST_CHECK_MESSAGE(found == c.collides,
                            ChainTypeToString(c.chain) << ": expected a Base58 address beginning "
                            "with HRP '" << c.hrp << "' to be "
                            << (c.collides ? "reachable" : "unreachable")
                            << ", but it was not. Either the version bytes or the HRP moved, and "
                            "the prefix collision this test covers has changed shape.");
    }
}

BOOST_AUTO_TEST_SUITE_END()
