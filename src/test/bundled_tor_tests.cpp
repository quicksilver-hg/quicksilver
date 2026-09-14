// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Tests for the bundled-Tor subsystem. The pure half -- torrc generation,
// control-port parsing, binary search -- takes its platform facts as arguments,
// so the Windows shapes are exercised by the Linux gate. That is the rule the
// solver-bridge fix established: Windows-only string and path logic goes
// OUTSIDE the #ifdef, or it never runs anywhere it is tested.
#include <tor/bundled_tor.h>

#include <test/util/setup_common.h>
#include <util/fs_helpers.h>
#include <util/readwritefile.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdlib>
#include <set>
#include <string>
#include <system_error>
#include <thread>

#ifndef WIN32
#include <stdlib.h>   // setenv
#endif

BOOST_FIXTURE_TEST_SUITE(bundled_tor_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(torrc_names_every_directive_the_design_requires)
{
    const tor::TorrcPaths paths{"/home/u/.quicksilver/tor/data",
                                "/home/u/.quicksilver/tor/control_port",
                                "/home/u/.quicksilver/tor/tor.log"};
    const std::string rc = tor::BuildTorrc(paths, 4242);

    BOOST_CHECK(rc.find("DataDirectory /home/u/.quicksilver/tor/data\n") != std::string::npos);
    BOOST_CHECK(rc.find("ControlPort auto\n") != std::string::npos);
    BOOST_CHECK(rc.find("ControlPortWriteToFile /home/u/.quicksilver/tor/control_port\n") != std::string::npos);
    BOOST_CHECK(rc.find("CookieAuthentication 1\n") != std::string::npos);
    BOOST_CHECK(rc.find("SocksPort auto\n") != std::string::npos);
    BOOST_CHECK(rc.find("Log notice file /home/u/.quicksilver/tor/tor.log\n") != std::string::npos);
    // Tor exits when this process does. On POSIX a process group does NOT die
    // with the process that created it, so without this an abruptly killed node
    // leaves a Tor running with our onion service still published.
    BOOST_CHECK(rc.find("__OwningControllerProcess 4242\n") != std::string::npos);
    // No fixed ports anywhere: a machine already running Tor must see no
    // conflict and need no diagnosis.
    BOOST_CHECK(rc.find("9050") == std::string::npos);
    BOOST_CHECK(rc.find("9051") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(torrc_values_reject_what_tor_would_misread)
{
    // Tor takes an unquoted value literally to end of line, so a backslash in a
    // Windows path is fine -- but '#' starts a comment, '"' starts a quoted
    // value, and a line break ends the directive early and silently.
    BOOST_CHECK(tor::IsTorrcSafeValue("C:\\Users\\me\\AppData\\Roaming\\Quicksilver\\tor"));
    BOOST_CHECK(tor::IsTorrcSafeValue("/home/user with space/.quicksilver/tor"));
    BOOST_CHECK(!tor::IsTorrcSafeValue("/home/u/tor#1"));
    BOOST_CHECK(!tor::IsTorrcSafeValue("/home/u/\"tor\""));
    BOOST_CHECK(!tor::IsTorrcSafeValue("/home/u/tor\nDataDirectory /tmp"));
    BOOST_CHECK(!tor::IsTorrcSafeValue("/home/u/tor\r"));
    BOOST_CHECK(!tor::IsTorrcSafeValue("/home/u/tor "));   // Tor strips it; we would name a different directory
}

BOOST_AUTO_TEST_CASE(control_port_file_parsing_covers_absent_truncated_and_malformed)
{
    std::string endpoint;

    // Absent or empty: keep waiting. Tor has not written it yet.
    BOOST_CHECK(tor::ParseControlPortFile("", endpoint) == tor::ControlPortParse::kIncomplete);

    // Truncated: no terminating newline means we are reading a partial write.
    // Accepting this is how a supervisor connects to port 4 instead of 41234.
    BOOST_CHECK(tor::ParseControlPortFile("PORT=127.0.0.1:41234", endpoint) == tor::ControlPortParse::kIncomplete);

    // Complete and well formed.
    BOOST_CHECK(tor::ParseControlPortFile("PORT=127.0.0.1:41234\n", endpoint) == tor::ControlPortParse::kOk);
    BOOST_CHECK_EQUAL(endpoint, "127.0.0.1:41234");

    // Tor on Windows terminates with CRLF.
    endpoint.clear();
    BOOST_CHECK(tor::ParseControlPortFile("PORT=127.0.0.1:41234\r\n", endpoint) == tor::ControlPortParse::kOk);
    BOOST_CHECK_EQUAL(endpoint, "127.0.0.1:41234");

    // A complete line that is not a usable PORT= is a hard failure, not a wait:
    // Tor has written its answer and the answer is unusable.
    BOOST_CHECK(tor::ParseControlPortFile("PORT=127.0.0.1:0\n", endpoint) == tor::ControlPortParse::kMalformed);
    BOOST_CHECK(tor::ParseControlPortFile("PORT=nonsense\n", endpoint) == tor::ControlPortParse::kMalformed);
    BOOST_CHECK(tor::ParseControlPortFile("UNIX_PORT=/run/tor/control\n", endpoint) == tor::ControlPortParse::kMalformed);
}

BOOST_AUTO_TEST_CASE(binary_search_order_is_override_then_beside_us_then_path)
{
    // F-200: fs::path composes with the PLATFORM's separator, so a fixture that
    // matched the JOINED string asserts which separator the standard library
    // picked rather than which directory the search chose -- and passes here
    // while failing on Windows, where "/usr/local/lib/quicksilver" + "tor"
    // composes "/usr/local/lib/quicksilver\tor" and misses the set. It went
    // undetected until windowsqs2's first gate on this suite. Match
    // (parent, filename), as the Windows-shapes case below already does.
    const std::set<std::string> present_dirs{
        "/opt/custom",
        "/usr/local/lib/quicksilver",
        "/usr/bin",
    };
    const auto usable = [&present_dirs](const fs::path& p) {
        return fs::PathToString(p.filename()) == "tor" &&
               present_dirs.count(fs::PathToString(p.parent_path())) > 0;
    };

    // 1. The override wins over both.
    auto found = tor::FindTorBinary("/opt/custom/tor", "/usr/local/lib/quicksilver",
                                    "/usr/bin:/bin", ':', "tor", usable);
    BOOST_REQUIRE(found.has_value());
    BOOST_CHECK_EQUAL(fs::PathToString(found->parent_path()), "/opt/custom");
    BOOST_CHECK_EQUAL(fs::PathToString(found->filename()), "tor");

    // 2. An override that is not there is an error, NOT a silent fall-through to
    //    something else: the operator named a file and we did not use it.
    BOOST_CHECK(!tor::FindTorBinary("/opt/missing/tor", "/usr/local/lib/quicksilver",
                                    "/usr/bin:/bin", ':', "tor", usable).has_value());

    // 3. No override: beside our own binary beats PATH.
    found = tor::FindTorBinary("", "/usr/local/lib/quicksilver", "/usr/bin:/bin", ':', "tor", usable);
    BOOST_REQUIRE(found.has_value());
    BOOST_CHECK_EQUAL(fs::PathToString(found->parent_path()), "/usr/local/lib/quicksilver");
    BOOST_CHECK_EQUAL(fs::PathToString(found->filename()), "tor");

    // 4. Nothing beside us: the first PATH entry that has one.
    found = tor::FindTorBinary("", "/elsewhere", "/nowhere:/usr/bin:/bin", ':', "tor", usable);
    BOOST_REQUIRE(found.has_value());
    BOOST_CHECK_EQUAL(fs::PathToString(found->parent_path()), "/usr/bin");
    BOOST_CHECK_EQUAL(fs::PathToString(found->filename()), "tor");

    // 5. Nothing anywhere.
    BOOST_CHECK(!tor::FindTorBinary("", "/elsewhere", "/nowhere", ':', "tor", usable).has_value());
}

BOOST_AUTO_TEST_CASE(binary_search_handles_the_windows_shapes_on_every_platform)
{
    // fs::path composes with the PLATFORM's own separator -- '\\' on Windows,
    // '/' here -- so a fixture that matched the joined string would
    // be asserting which separator the standard library picked, not which
    // directory the search chose. The decision under test is the directory, so
    // the predicate and the assertions look at (parent, filename) and the
    // Windows shapes then run identically on both platforms.
    const std::set<std::string> present_dirs{
        "C:\\Program Files\\Quicksilver",
        "C:\\tools\\tor",
    };
    const auto usable = [&present_dirs](const fs::path& p) {
        return fs::PathToString(p.filename()) == "tor.exe" &&
               present_dirs.count(fs::PathToString(p.parent_path())) > 0;
    };

    // Beside our own executable wins over PATH, with a space in the directory.
    auto found = tor::FindTorBinary("", "C:\\Program Files\\Quicksilver",
                                    "C:\\Windows;C:\\tools\\tor", ';', "tor.exe", usable);
    BOOST_REQUIRE(found.has_value());
    BOOST_CHECK_EQUAL(fs::PathToString(found->parent_path()), "C:\\Program Files\\Quicksilver");
    BOOST_CHECK_EQUAL(fs::PathToString(found->filename()), "tor.exe");

    // Nothing beside us: ';' splits PATH and the second entry has one.
    found = tor::FindTorBinary("", "C:\\Elsewhere", "C:\\Windows;C:\\tools\\tor", ';', "tor.exe", usable);
    BOOST_REQUIRE(found.has_value());
    BOOST_CHECK_EQUAL(fs::PathToString(found->parent_path()), "C:\\tools\\tor");

    // An empty PATH element must not produce a bare "tor.exe" candidate that
    // resolves against the working directory.
    BOOST_CHECK(!tor::FindTorBinary("", "C:\\Elsewhere", ";;", ';', "tor.exe", usable).has_value());
}

namespace {
//! Point the supervisor at the stub and give the stub its directives. Returns
//! the path the stub will be invoked as, which is what -bundledtorpath receives.
fs::path arrange_stub(const fs::path& dir, const std::string& directives)
{
    // TryCreateDirectories returns false for a directory that already exists,
    // and the fixture's datadir always does.
    BOOST_REQUIRE(TryCreateDirectories(dir) || fs::is_directory(dir));
    const fs::path script = dir / "tor_stub_script";
    BOOST_REQUIRE(WriteBinaryFile(script, directives));
#ifdef WIN32
    _putenv_s("QS_TOR_STUB_SCRIPT", fs::PathToString(script).c_str());
#else
    setenv("QS_TOR_STUB_SCRIPT", fs::PathToString(script).c_str(), 1);
#endif
    return fs::PathFromString(TOR_STUB_PATH);
}

uintmax_t file_size_or_zero(const fs::path& p)
{
    std::error_code ec;
    const auto n = fs::file_size(p, ec);
    return ec ? 0 : n;
}
} // namespace

BOOST_AUTO_TEST_CASE(a_reported_control_port_is_returned_and_the_child_is_supervised)
{
    const fs::path stub = arrange_stub(m_args.GetDataDirNet(),
                                       "port PORT=127.0.0.1:41234\\n\nsleep 60000\n");
    const auto result = tor::StartBundledTor(m_args.GetDataDirNet(), stub);
    BOOST_REQUIRE_MESSAGE(result, util::ErrorString(result).original);
    BOOST_CHECK_EQUAL(*result, "127.0.0.1:41234");

    // The torrc we generated is on disk and is what the stub read back.
    const auto [ok, torrc] = ReadBinaryFile(m_args.GetDataDirNet() / "tor" / "torrc");
    BOOST_REQUIRE(ok);
    BOOST_CHECK(torrc.find("ControlPort auto\n") != std::string::npos);

    tor::StopBundledTor();
}

BOOST_AUTO_TEST_CASE(a_stale_control_port_file_is_never_believed)
{
    // The failure this rules out: a previous run's file names a port nothing is
    // listening on any more, and the supervisor reports success instantly while
    // the new Tor is still starting -- or never starts at all.
    const fs::path tor_dir = m_args.GetDataDirNet() / "tor";
    BOOST_REQUIRE(TryCreateDirectories(tor_dir) || fs::is_directory(tor_dir));
    BOOST_REQUIRE(WriteBinaryFile(tor_dir / "control_port", "PORT=127.0.0.1:9051\n"));

    const fs::path stub = arrange_stub(m_args.GetDataDirNet(), "sleep 2000\nexit 0\n");
    const auto result = tor::StartBundledTor(m_args.GetDataDirNet(), stub);
    BOOST_CHECK_MESSAGE(!result, "the stale port was accepted as this run's answer");
    tor::StopBundledTor();
}

BOOST_AUTO_TEST_CASE(a_tor_that_exits_before_reporting_fails_at_once)
{
    const fs::path stub = arrange_stub(m_args.GetDataDirNet(), "exit 1\n");
    const auto start = std::chrono::steady_clock::now();
    const auto result = tor::StartBundledTor(m_args.GetDataDirNet(), stub);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    BOOST_CHECK(!result);
    // Fails on the child's exit, not on the 60s startup timeout: a user staring
    // at a stopped desktop for a minute is a defect even when the message is
    // eventually right.
    BOOST_CHECK(elapsed < std::chrono::seconds(10));
    BOOST_CHECK(util::ErrorString(result).original.find("exited") != std::string::npos);
    tor::StopBundledTor();
}

BOOST_AUTO_TEST_CASE(a_malformed_control_port_file_is_an_error_not_a_wait)
{
    const fs::path stub = arrange_stub(m_args.GetDataDirNet(),
                                       "port PORT=nonsense\\n\nsleep 60000\n");
    const auto start = std::chrono::steady_clock::now();
    const auto result = tor::StartBundledTor(m_args.GetDataDirNet(), stub);
    BOOST_CHECK(!result);
    BOOST_CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));
    tor::StopBundledTor();
}

BOOST_AUTO_TEST_CASE(a_missing_tor_names_the_remedy)
{
    const auto result = tor::StartBundledTor(m_args.GetDataDirNet(),
                                             m_args.GetDataDirNet() / "no-such-tor");
    BOOST_REQUIRE(!result);
    const std::string message = util::ErrorString(result).original;
    BOOST_CHECK(message.find("-bundledtorpath") != std::string::npos);
    BOOST_CHECK(message.find("-bundledtor=0") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(stopping_leaves_no_orphan)
{
    // Asserted by RATE. The stub appends to a marker on every pass, so a size
    // that stops advancing is proof the process is gone; its ABSENCE would prove
    // nothing, because a parent returning cleanly looks identical whether the
    // child died or was orphaned holding an onion service open.
    const fs::path marker = m_args.GetDataDirNet() / "tor-alive.marker";
    const fs::path stub = arrange_stub(
        m_args.GetDataDirNet(),
        "port PORT=127.0.0.1:41234\\n\nappend " + fs::PathToString(marker) + "\nsleep 50\nloop\n");
    const auto result = tor::StartBundledTor(m_args.GetDataDirNet(), stub);
    BOOST_REQUIRE_MESSAGE(result, util::ErrorString(result).original);

    for (int i = 0; i < 100 && file_size_or_zero(marker) == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    BOOST_REQUIRE_MESSAGE(file_size_or_zero(marker) > 0, "the stub never started");

    tor::StopBundledTor();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const uintmax_t settled = file_size_or_zero(marker);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    BOOST_CHECK_MESSAGE(file_size_or_zero(marker) == settled,
                        "the marker is still growing: a Tor outlived StopBundledTor");
}

BOOST_AUTO_TEST_CASE(stopping_without_starting_is_safe)
{
    tor::StopBundledTor();
    tor::StopBundledTor();
}

BOOST_AUTO_TEST_SUITE_END()
