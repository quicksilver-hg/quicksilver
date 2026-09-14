// Copyright (c) 2019-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//

#include <test/util/setup_common.h>
#include <common/run_command.h>
#include <tinyformat.h>
#include <univalue.h>
#include <util/subprocess.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(system_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(run_command)
{
    {
        const UniValue result = RunCommandParseJSON("");
        BOOST_CHECK(result.isNull());
    }
    {
#ifdef WIN32
        // cpp-subprocess splits the command on whitespace and re-quotes every
        // token containing a double quote, so a JSON object literal cannot
        // survive the CreateProcess() command line. An array literal needs no
        // quotes; the object form is exercised over stdin at the end of this
        // test.
        const UniValue result = RunCommandParseJSON("cmd.exe /c echo [1,2,3]");
        BOOST_CHECK(result.isArray());
        BOOST_CHECK_EQUAL(result.size(), 3U);
        BOOST_CHECK_EQUAL(result[0].getInt<int>(), 1);
        BOOST_CHECK_EQUAL(result[1].getInt<int>(), 2);
        BOOST_CHECK_EQUAL(result[2].getInt<int>(), 3);
#else
        const UniValue result = RunCommandParseJSON("echo {\"success\": true}");
        BOOST_CHECK(result.isObject());
        const UniValue& success = result.find_value("success");
        BOOST_CHECK(!success.isNull());
        BOOST_CHECK_EQUAL(success.get_bool(), true);
#endif
    }
    {
        // An invalid command is handled by cpp-subprocess
#ifdef WIN32
        const std::string expected{"CreateProcess failed: "};
#else
        const std::string expected{"execve failed: "};
#endif
        BOOST_CHECK_EXCEPTION(RunCommandParseJSON("invalid_command"), subprocess::CalledProcessError, HasReason(expected));
    }
    {
        // Return non-zero exit code, no output to stderr
#ifdef WIN32
        const std::string command{"cmd.exe /c exit 1"};
#else
        const std::string command{"false"};
#endif
        BOOST_CHECK_EXCEPTION(RunCommandParseJSON(command), std::runtime_error, [&](const std::runtime_error& e) {
            const std::string what{e.what()};
            BOOST_CHECK(what.find(strprintf("RunCommandParseJSON error: process(%s) returned 1: \n", command)) != std::string::npos);
            return true;
        });
    }
    {
        // Return non-zero exit code, with error message for stderr
#ifdef WIN32
        const std::string command{"cmd.exe /c echo err 1>&2 && exit 1"};
#else
        const std::string command{"sh -c 'echo err 1>&2 && false'"};
#endif
        const std::string expected{"err"};
        BOOST_CHECK_EXCEPTION(RunCommandParseJSON(command), std::runtime_error, [&](const std::runtime_error& e) {
            const std::string what(e.what());
            BOOST_CHECK(what.find(strprintf("RunCommandParseJSON error: process(%s) returned", command)) != std::string::npos);
            BOOST_CHECK(what.find(expected) != std::string::npos);
            return true;
        });
    }
    {
        // Unable to parse JSON
#ifdef WIN32
        const std::string command{"cmd.exe /c echo {"};
#else
        const std::string command{"echo {"};
#endif
        BOOST_CHECK_EXCEPTION(RunCommandParseJSON(command), std::runtime_error, HasReason("Unable to parse JSON: {"));
    }
    // Test std::in
    {
#ifdef WIN32
        // `sort` returns a single line of input unchanged, so it stands in for
        // `cat` here.
        const UniValue result = RunCommandParseJSON("cmd.exe /c sort", "{\"success\": true}");
#else
        const UniValue result = RunCommandParseJSON("cat", "{\"success\": true}");
#endif
        BOOST_CHECK(result.isObject());
        const UniValue& success = result.find_value("success");
        BOOST_CHECK(!success.isNull());
        BOOST_CHECK_EQUAL(success.get_bool(), true);
    }
}

BOOST_AUTO_TEST_SUITE_END()
