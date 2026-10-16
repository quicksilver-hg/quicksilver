// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <util/process/contained_child.h>
#include <boost/test/unit_test.hpp>
#include <string>
#include <vector>

namespace {
struct Run { int exit_code; std::string out; };
Run RunHelper(const std::vector<std::string>& argv)
{
    auto child = util::ContainedChild::Spawn(argv, util::ChildStdout::kPipe);
    BOOST_REQUIRE(child);
    std::string out; char buf[4096];
    for (;;) {
        size_t got{0};
        const auto st = child->ReadStdout(buf, sizeof(buf), 30000, got);
        if (st == util::ContainedChild::ReadStatus::kClosed) break;
        if (st == util::ContainedChild::ReadStatus::kTimeout) { child->KillGroup(); break; }
        out.append(buf, got);
    }
    return {child->Wait(), out};
}
const std::string kPre84(168, '0');
} // namespace

BOOST_AUTO_TEST_SUITE(opencl_helper_tests)
BOOST_AUTO_TEST_CASE(version_names_the_helper)
{
    for (const char* flag : {"--version", "-version"}) {
        const Run r = RunHelper({QSGPUSOLVE_OPENCL_PATH, flag});
        BOOST_CHECK_EQUAL(r.exit_code, 0);
        BOOST_CHECK(r.out.find("graphics helper version") != std::string::npos);
    }
}
BOOST_AUTO_TEST_CASE(edgebits_mismatch_exits_3)
{
    BOOST_CHECK_EQUAL(RunHelper({QSGPUSOLVE_OPENCL_E19_PATH, "28", kPre84, "0", "1"}).exit_code, 3);
    BOOST_CHECK_EQUAL(RunHelper({QSGPUSOLVE_OPENCL_E19_PATH, "--device", "pci:0000:04:00.0", "28", kPre84, "0", "1"}).exit_code, 3);
}
BOOST_AUTO_TEST_CASE(absent_device_exits_4)
{
    const Run r = RunHelper({QSGPUSOLVE_OPENCL_E19_PATH, "--device", "pci:ffff:ff:1f.7", "19", kPre84, "0", "1"});
    BOOST_CHECK_EQUAL(r.exit_code, 4);
    BOOST_CHECK(r.out.find("nonce=") == std::string::npos);
}
BOOST_AUTO_TEST_CASE(bad_hex_and_bad_arity_exit_2)
{
    BOOST_CHECK_EQUAL(RunHelper({QSGPUSOLVE_OPENCL_E19_PATH, "19", "zz", "0", "1"}).exit_code, 2);
    BOOST_CHECK_EQUAL(RunHelper({QSGPUSOLVE_OPENCL_E19_PATH, "19", kPre84, "0"}).exit_code, 2);
    BOOST_CHECK_EQUAL(RunHelper({QSGPUSOLVE_OPENCL_E19_PATH, "--device"}).exit_code, 2);
}
BOOST_AUTO_TEST_SUITE_END()
