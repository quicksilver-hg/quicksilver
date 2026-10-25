// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Two PoCL CPU devices through the real E28 helper and the multi-card
// coordinator. Skips unless QS_OPENCL_TWO_DEVICES names both ids.

#include <crypto/cuckatoo/gpu_devices.h>
#include <crypto/cuckatoo/multi_device.h>

#include <test/util/gpucardtables.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#ifndef WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
boost::test_tools::assertion_result two_devices_named(boost::unit_test::test_unit_id)
{
    const char* value = std::getenv("QS_OPENCL_TWO_DEVICES");
    if (value && value[0]) return true;
    boost::test_tools::assertion_result ans(false);
    ans.message() << "QS_OPENCL_TWO_DEVICES unset — skipping two-device OpenCL solve";
    return ans;
}

std::vector<std::string> SplitComma(const std::string& text)
{
    std::vector<std::string> out;
    std::string current;
    for (const char c : text) {
        if (c == ',') {
            out.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    out.push_back(current);
    return out;
}

#ifndef WIN32
// SelectDevices keeps only type=gpu. PoCL's stand-ins are type=cpu, so the
// listing the coordinator reads says gpu; every solve execs the real helper.
bool WriteShim(const std::string& path, const std::string& real)
{
    std::ofstream out{path};
    out << "#!/bin/sh\n"
        << "real='" << real << "'\n"
        << "if [ \"$1\" = --list-devices ]; then\n"
        << "  tmp=$(mktemp)\n"
        << "  \"$real\" --list-devices > \"$tmp\"\n"
        << "  rc=$?\n"
        << "  sed 's/ type=cpu / type=gpu /' \"$tmp\"\n"
        << "  rm -f \"$tmp\"\n"
        << "  exit \"$rc\"\n"
        << "fi\n"
        << "exec \"$real\" \"$@\"\n";
    out.close();
    return static_cast<bool>(out) && chmod(path.c_str(), 0755) == 0;
}

struct Env {
    std::string script;
    Env()
    {
        cuckatoo::ResetGpuDiscoveryForTesting();
        cuckatoo::ResetGpuDeviceStatsForTesting();
    }
    ~Env()
    {
        unsetenv("CUCKATOO_GPU_SOLVER");
        unsetenv("CUCKATOO_GPU_DEVICES");
        unsetenv("CUCKATOO_GPU_TIMEOUT");
        unsetenv("CUCKATOO_GPU_BUNDLED");
        cuckatoo::ResetGpuDiscoveryForTesting();
        cuckatoo::ResetGpuDeviceStatsForTesting();
        if (!script.empty()) std::remove(script.c_str());
    }
};

bool BothWorking(const std::vector<std::string>& ids)
{
    const auto stats = cuckatoo::GetGpuDeviceStats();
    for (const auto& id : ids) {
        const auto row = std::find_if(stats.begin(), stats.end(), [&](const auto& value) { return value.device == id; });
        if (row == stats.end() || row->attempts == 0 || !(row->mean_seconds_per_graph > 0.0)) return false;
    }
    return true;
}

// Digits only. std::to_string is locale-dependent and the lint rejects it.
std::string Decimal(unsigned long long value)
{
    if (value == 0) return "0";
    std::string digits;
    while (value > 0) {
        digits.push_back(static_cast<char>('0' + static_cast<int>(value % 10)));
        value /= 10;
    }
    std::reverse(digits.begin(), digits.end());
    return digits;
}

void RunBothDevices(const std::vector<std::string>& ids)
{
    Env env;
    env.script = "/tmp/qs-opencl-two-shim-" + Decimal(static_cast<unsigned long long>(getpid()));
    BOOST_REQUIRE(WriteShim(env.script, QSGPUSOLVE_OPENCL_PATH));
    setenv("CUCKATOO_GPU_SOLVER", env.script.c_str(), 1);
    setenv("CUCKATOO_GPU_DEVICES", (ids[0] + "," + ids[1]).c_str(), 1);
    // One graph on two PoCL CPUs was 39 s here, past the 30 s watchdog.
    setenv("CUCKATOO_GPU_TIMEOUT", "180", 1);
    unsetenv("CUCKATOO_GPU_BUNDLED");
    cuckatoo::ResetGpuDiscoveryForTesting();
    cuckatoo::ResetGpuDeviceStatsForTesting();

    std::vector<unsigned char> pre(84, 0x5A);
    cuckatoo::Cycle cycle{};
    uint32_t nonce{0};
    const auto started = std::chrono::steady_clock::now();
    cuckatoo::MultiDeviceSolveBytes(
        pre.data(), pre.size(), 28, 0, /*two blocks*/ 128, cycle, nonce,
        [](const cuckatoo::Cycle&, uint32_t) { return false; }, {},
        [&] {
            if (BothWorking(ids)) return true;
            return std::chrono::steady_clock::now() - started > std::chrono::seconds{240};
        });

    const auto stats = cuckatoo::GetGpuDeviceStats();
    for (const auto& id : ids) {
        const auto row = std::find_if(stats.begin(), stats.end(), [&](const auto& value) { return value.device == id; });
        BOOST_REQUIRE_MESSAGE(row != stats.end(), id << " did not launch");
        BOOST_CHECK_GT(row->attempts, 0U);
        BOOST_CHECK_GT(row->mean_seconds_per_graph, 0.0);
    }
}
#else
void RunBothDevices(const std::vector<std::string>&)
{
    BOOST_FAIL("QS_OPENCL_TWO_DEVICES is set, but the two-device solve runs on Linux");
}
#endif
} // namespace

BOOST_FIXTURE_TEST_SUITE(opencl_two_device_tests, GpuCardTablesTestSetup)

BOOST_AUTO_TEST_CASE(both_devices_run_a_block, *boost::unit_test::precondition(two_devices_named))
{
    const char* value = std::getenv("QS_OPENCL_TWO_DEVICES");
    BOOST_REQUIRE(value != nullptr);
    const std::vector<std::string> ids = SplitComma(value);
    BOOST_REQUIRE_EQUAL(ids.size(), 2U);
    BOOST_REQUIRE(!ids[0].empty());
    BOOST_REQUIRE(!ids[1].empty());
    BOOST_REQUIRE(ids[0] != ids[1]);
    RunBothDevices(ids);
}

BOOST_AUTO_TEST_SUITE_END()
