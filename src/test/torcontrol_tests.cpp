// Copyright (c) 2017 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//

#include <boost/test/unit_test.hpp>
#include <torcontrol.h>

#include <map>
#include <string>
#include <utility>


std::pair<std::string, std::string> SplitTorReplyLine(const std::string& s);
std::map<std::string, std::string> ParseTorReplyMapping(const std::string& s);

struct TorControlConnectionTest {
    static void CheckReplyBound(const std::string& line, size_t accepted_lines)
    {
        TorControlConnection conn{nullptr};
        int drops{0};
        conn.disconnected = [&](TorControlConnection&) { ++drops; };
        conn.connection_active = true;
        for (size_t i = 0; i < accepted_lines; ++i) {
            BOOST_REQUIRE(conn.AddReplyLine(line));
        }
        BOOST_CHECK(!conn.AddReplyLine(line));
        BOOST_CHECK_EQUAL(drops, 1);
        BOOST_CHECK(conn.message.lines.empty());
        conn.Disconnect();
        BOOST_CHECK_EQUAL(drops, 1);
    }

    static void CheckStaleHandlerCleared()
    {
        TorControlConnection conn{nullptr};
        int old_calls{0};
        int new_calls{0};
        conn.connection_active = true;
        conn.reply_handlers.push_back([&](TorControlConnection&, const TorControlReply&) { ++old_calls; });
        BOOST_REQUIRE(conn.AddReplyLine("250-partial"));
        conn.Disconnect();
        BOOST_CHECK(conn.reply_handlers.empty());
        BOOST_CHECK(conn.message.lines.empty());
        conn.connection_active = true;
        conn.reply_handlers.push_back([&](TorControlConnection&, const TorControlReply&) { ++new_calls; });
        BOOST_REQUIRE(conn.AddReplyLine("250 OK"));
        BOOST_CHECK_EQUAL(old_calls, 0);
        BOOST_CHECK_EQUAL(new_calls, 1);
    }
};


BOOST_AUTO_TEST_SUITE(torcontrol_tests)

BOOST_AUTO_TEST_CASE(reply_bounds_disconnect_and_clear_handlers)
{
    TorControlConnectionTest::CheckReplyBound("250-more", 1000);
    TorControlConnectionTest::CheckReplyBound(std::string(100000, 'x').replace(0, 4, "250-"), 10);
    TorControlConnectionTest::CheckStaleHandlerCleared();
}

BOOST_AUTO_TEST_CASE(reconnect_delay_is_capped)
{
    float timeout{1.0F};
    for (int i = 0; i < 100; ++i) {
        const auto delay = TorController::NextReconnectDelayMillis(timeout);
        BOOST_CHECK(delay <= 600000);
    }
    BOOST_CHECK_EQUAL(timeout, 600.0F);
    BOOST_CHECK_EQUAL(TorController::NextReconnectDelayMillis(timeout), 600000);
}

BOOST_AUTO_TEST_CASE(password_is_escaped)
{
    BOOST_CHECK_EQUAL(TorController::PasswordAuthCommand("a\\b\"c"), "AUTHENTICATE \"a\\\\b\\\"c\"");
}

static void CheckSplitTorReplyLine(std::string input, std::string command, std::string args)
{
    auto ret = SplitTorReplyLine(input);
    BOOST_CHECK_EQUAL(ret.first, command);
    BOOST_CHECK_EQUAL(ret.second, args);
}

BOOST_AUTO_TEST_CASE(util_SplitTorReplyLine)
{
    // Data we should receive during normal usage
    CheckSplitTorReplyLine(
        "PROTOCOLINFO PIVERSION",
        "PROTOCOLINFO", "PIVERSION");
    CheckSplitTorReplyLine(
        "AUTH METHODS=COOKIE,SAFECOOKIE COOKIEFILE=\"/home/x/.tor/control_auth_cookie\"",
        "AUTH", "METHODS=COOKIE,SAFECOOKIE COOKIEFILE=\"/home/x/.tor/control_auth_cookie\"");
    CheckSplitTorReplyLine(
        "AUTH METHODS=NULL",
        "AUTH", "METHODS=NULL");
    CheckSplitTorReplyLine(
        "AUTH METHODS=HASHEDPASSWORD",
        "AUTH", "METHODS=HASHEDPASSWORD");
    CheckSplitTorReplyLine(
        "VERSION Tor=\"0.2.9.8 (git-a0df013ea241b026)\"",
        "VERSION", "Tor=\"0.2.9.8 (git-a0df013ea241b026)\"");
    CheckSplitTorReplyLine(
        "AUTHCHALLENGE SERVERHASH=aaaa SERVERNONCE=bbbb",
        "AUTHCHALLENGE", "SERVERHASH=aaaa SERVERNONCE=bbbb");

    // Other valid inputs
    CheckSplitTorReplyLine("COMMAND", "COMMAND", "");
    CheckSplitTorReplyLine("COMMAND SOME  ARGS", "COMMAND", "SOME  ARGS");

    // These inputs are valid because PROTOCOLINFO accepts an OtherLine that is
    // just an OptArguments, which enables multiple spaces to be present
    // between the command and arguments.
    CheckSplitTorReplyLine("COMMAND  ARGS", "COMMAND", " ARGS");
    CheckSplitTorReplyLine("COMMAND   EVEN+more  ARGS", "COMMAND", "  EVEN+more  ARGS");
}

static void CheckParseTorReplyMapping(std::string input, std::map<std::string,std::string> expected)
{
    auto ret = ParseTorReplyMapping(input);
    BOOST_CHECK_EQUAL(ret.size(), expected.size());
    auto r_it = ret.begin();
    auto e_it = expected.begin();
    while (r_it != ret.end() && e_it != expected.end()) {
        BOOST_CHECK_EQUAL(r_it->first, e_it->first);
        BOOST_CHECK_EQUAL(r_it->second, e_it->second);
        r_it++;
        e_it++;
    }
}

BOOST_AUTO_TEST_CASE(util_ParseTorReplyMapping)
{
    // Data we should receive during normal usage
    CheckParseTorReplyMapping(
        "METHODS=COOKIE,SAFECOOKIE COOKIEFILE=\"/home/x/.tor/control_auth_cookie\"", {
            {"METHODS", "COOKIE,SAFECOOKIE"},
            {"COOKIEFILE", "/home/x/.tor/control_auth_cookie"},
        });
    CheckParseTorReplyMapping(
        "METHODS=NULL", {
            {"METHODS", "NULL"},
        });
    CheckParseTorReplyMapping(
        "METHODS=HASHEDPASSWORD", {
            {"METHODS", "HASHEDPASSWORD"},
        });
    CheckParseTorReplyMapping(
        "Tor=\"0.2.9.8 (git-a0df013ea241b026)\"", {
            {"Tor", "0.2.9.8 (git-a0df013ea241b026)"},
        });
    CheckParseTorReplyMapping(
        "SERVERHASH=aaaa SERVERNONCE=bbbb", {
            {"SERVERHASH", "aaaa"},
            {"SERVERNONCE", "bbbb"},
        });
    CheckParseTorReplyMapping(
        "ServiceID=exampleonion1234", {
            {"ServiceID", "exampleonion1234"},
        });
    CheckParseTorReplyMapping(
        "PrivateKey=ED25519-V3:BLOB", {
            {"PrivateKey", "ED25519-V3:BLOB"},
        });
    CheckParseTorReplyMapping(
        "ClientAuth=bob:BLOB", {
            {"ClientAuth", "bob:BLOB"},
        });

    // Other valid inputs
    CheckParseTorReplyMapping(
        "Foo=Bar=Baz Spam=Eggs", {
            {"Foo", "Bar=Baz"},
            {"Spam", "Eggs"},
        });
    CheckParseTorReplyMapping(
        "Foo=\"Bar=Baz\"", {
            {"Foo", "Bar=Baz"},
        });
    CheckParseTorReplyMapping(
        "Foo=\"Bar Baz\"", {
            {"Foo", "Bar Baz"},
        });

    // Escapes
    CheckParseTorReplyMapping(
        "Foo=\"Bar\\ Baz\"", {
            {"Foo", "Bar Baz"},
        });
    CheckParseTorReplyMapping(
        "Foo=\"Bar\\Baz\"", {
            {"Foo", "BarBaz"},
        });
    CheckParseTorReplyMapping(
        "Foo=\"Bar\\@Baz\"", {
            {"Foo", "Bar@Baz"},
        });
    CheckParseTorReplyMapping(
        "Foo=\"Bar\\\"Baz\" Spam=\"\\\"Eggs\\\"\"", {
            {"Foo", "Bar\"Baz"},
            {"Spam", "\"Eggs\""},
        });
    CheckParseTorReplyMapping(
        "Foo=\"Bar\\\\Baz\"", {
            {"Foo", "Bar\\Baz"},
        });

    // C escapes
    CheckParseTorReplyMapping(
        "Foo=\"Bar\\nBaz\\t\" Spam=\"\\rEggs\" Octals=\"\\1a\\11\\17\\18\\81\\377\\378\\400\\2222\" Final=Check", {
            {"Foo", "Bar\nBaz\t"},
            {"Spam", "\rEggs"},
            {"Octals", "\1a\11\17\1" "881\377\37" "8\40" "0\222" "2"},
            {"Final", "Check"},
        });
    CheckParseTorReplyMapping(
        "Valid=Mapping Escaped=\"Escape\\\\\"", {
            {"Valid", "Mapping"},
            {"Escaped", "Escape\\"},
        });
    CheckParseTorReplyMapping(
        "Valid=Mapping Bare=\"Escape\\\"", {});
    CheckParseTorReplyMapping(
        "OneOctal=\"OneEnd\\1\" TwoOctal=\"TwoEnd\\11\"", {
            {"OneOctal", "OneEnd\1"},
            {"TwoOctal", "TwoEnd\11"},
        });

    // Special handling for null case
    // (needed because string comparison reads the null as end-of-string)
    auto ret = ParseTorReplyMapping("Null=\"\\0\"");
    BOOST_REQUIRE_EQUAL(ret.size(), 1U);
    auto r_it = ret.begin();
    BOOST_CHECK_EQUAL(r_it->first, "Null");
    BOOST_CHECK_EQUAL(r_it->second.size(), 1U);
    BOOST_CHECK_EQUAL(r_it->second[0], '\0');

    // A more complex valid grammar. PROTOCOLINFO accepts a VersionLine that
    // takes a key=value pair followed by an OptArguments, making this valid.
    // Because an OptArguments contains no semantic data, there is no point in
    // parsing it.
    CheckParseTorReplyMapping(
        "SOME=args,here MORE optional=arguments  here", {
            {"SOME", "args,here"},
        });

    // Inputs that are effectively invalid under the target grammar.
    // PROTOCOLINFO accepts an OtherLine that is just an OptArguments, which
    // would make these inputs valid. However,
    // - This parser is never used in that situation, because the
    //   SplitTorReplyLine parser enables OtherLine to be skipped.
    // - Even if these were valid, an OptArguments contains no semantic data,
    //   so there is no point in parsing it.
    CheckParseTorReplyMapping("ARGS", {});
    CheckParseTorReplyMapping("MORE ARGS", {});
    CheckParseTorReplyMapping("MORE  ARGS", {});
    CheckParseTorReplyMapping("EVEN more=ARGS", {});
    CheckParseTorReplyMapping("EVEN+more ARGS", {});
}

BOOST_AUTO_TEST_SUITE_END()
