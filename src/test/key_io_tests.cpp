// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/data/key_io_invalid.json.h>
#include <test/data/key_io_valid.json.h>

#include <base58.h>
#include <chainparams.h>
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

// Goal: check that decoded keys and destinations match the test payload
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
        std::string encoded = test[0].get_str();
        const std::vector<std::byte> exp_payload{ParseHex<std::byte>(test[1].get_str())};
        const UniValue &metadata = test[2].get_obj();
        bool isPrivkey = metadata.find_value("isPrivkey").get_bool();
        SelectParams(ChainTypeFromString(metadata.find_value("chain").get_str()).value());
        bool try_case_flip = metadata.find_value("tryCaseFlip").isNull() ? false : metadata.find_value("tryCaseFlip").get_bool();
        if (isPrivkey) {
            bool isCompressed = metadata.find_value("isCompressed").get_bool();
            // Must be valid private key
            privkey = DecodeSecret(encoded);
            BOOST_CHECK_MESSAGE(privkey.IsValid(), "!IsValid:" + strTest);
            BOOST_CHECK_MESSAGE(privkey.IsCompressed() == isCompressed, "compressed mismatch:" + strTest);
            BOOST_CHECK_MESSAGE(std::ranges::equal(privkey, exp_payload), "key mismatch:" + strTest);

            // Secret must not decode as a destination
            destination = DecodeDestination(encoded);
            BOOST_CHECK_MESSAGE(!IsValidDestination(destination), "Secret decoded as destination:" + strTest);
        } else {
            // Must be a valid destination
            destination = DecodeDestination(encoded);
            CScript script = GetScriptForDestination(destination);
            BOOST_CHECK_MESSAGE(IsValidDestination(destination), "!IsValid:" + strTest);
            BOOST_CHECK_EQUAL(HexStr(script), HexStr(exp_payload));

            // Try flipped case version
            for (char& c : encoded) {
                if (c >= 'a' && c <= 'z') {
                    c = (c - 'a') + 'A';
                } else if (c >= 'A' && c <= 'Z') {
                    c = (c - 'A') + 'a';
                }
            }
            destination = DecodeDestination(encoded);
            BOOST_CHECK_MESSAGE(IsValidDestination(destination) == try_case_flip, "!IsValid case flipped:" + strTest);
            if (IsValidDestination(destination)) {
                script = GetScriptForDestination(destination);
                BOOST_CHECK_EQUAL(HexStr(script), HexStr(exp_payload));
            }

            // Destination must not decode as a secret
            privkey = DecodeSecret(encoded);
            BOOST_CHECK_MESSAGE(!privkey.IsValid(), "Destination decoded as secret:" + strTest);
        }
    }
}

// Goal: check that encoded keys and destinations match test vectors
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
        std::string encoded = test[0].get_str();
        std::vector<unsigned char> exp_payload = ParseHex(test[1].get_str());
        const UniValue &metadata = test[2].get_obj();
        bool isPrivkey = metadata.find_value("isPrivkey").get_bool();
        SelectParams(ChainTypeFromString(metadata.find_value("chain").get_str()).value());
        if (isPrivkey) {
            bool isCompressed = metadata.find_value("isCompressed").get_bool();
            CKey key;
            key.Set(exp_payload.begin(), exp_payload.end(), isCompressed);
            assert(key.IsValid());
            BOOST_CHECK_MESSAGE(EncodeSecret(key) == encoded, "result mismatch: " + strTest);
        } else {
            CTxDestination dest;
            CScript exp_script(exp_payload.begin(), exp_payload.end());
            BOOST_CHECK(ExtractDestination(exp_script, dest));
            std::string address = EncodeDestination(dest);

            BOOST_CHECK_EQUAL(address, encoded);
        }
    }

    SelectParams(ChainType::MAIN);
}


// Goal: check that address and secret parsing reject corrupted data
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
        std::string encoded = test[0].get_str();

        // must be invalid as a destination and as a secret
        for (const auto& chain : {ChainType::MAIN, ChainType::PUBLIC_TEST, ChainType::SANDBOX}) {
            SelectParams(chain);
            destination = DecodeDestination(encoded);
            BOOST_CHECK_MESSAGE(!IsValidDestination(destination), "Valid destination on " + ChainTypeToString(chain) + ":" + strTest);
            privkey = DecodeSecret(encoded);
            BOOST_CHECK_MESSAGE(!privkey.IsValid(), "Valid secret on " + ChainTypeToString(chain) + ":" + strTest);
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

// G-07: the signing errors name the key-hash lead from the chain's prefix, so it
// must hold for every key hash, not only the all-zero one it is derived from.
BOOST_AUTO_TEST_CASE(base58_key_hash_lead)
{
    const std::vector<std::pair<ChainType, std::string>> chains{
        {ChainType::MAIN, "Q"}, {ChainType::PUBLIC_TEST, "P"}, {ChainType::SANDBOX, "S"}};
    for (const auto& [chain, lead] : chains) {
        const auto params{CreateChainParams(*m_node.args, chain)};
        BOOST_CHECK_EQUAL(Base58KeyHashLead(*params), lead);
        for (const unsigned char fill : {0x00, 0xff}) {
            std::vector<unsigned char> payload{params->Base58Prefix(CChainParams::PUBKEY_ADDRESS)};
            payload.insert(payload.end(), 20, fill);
            BOOST_CHECK_EQUAL(EncodeBase58Check(payload).substr(0, 1), lead);
            // The script-hash kind never shares it.
            std::vector<unsigned char> script_payload{params->Base58Prefix(CChainParams::SCRIPT_ADDRESS)};
            script_payload.insert(script_payload.end(), 20, fill);
            BOOST_CHECK(EncodeBase58Check(script_payload).substr(0, 1) != lead);
        }
    }
    BOOST_CHECK_EQUAL(Base58KeyHashLead(), Base58KeyHashLead(Params()));
}

BOOST_AUTO_TEST_SUITE_END()
