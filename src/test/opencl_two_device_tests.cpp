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
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
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
bool WriteShim(int fd, const std::string& path, const std::string& real)
{
    FILE* file = fdopen(fd, "w");
    if (!file) { close(fd); return false; }
    const std::string body = "#!/bin/sh\nreal='" + real + "'\n"
        "if [ \"$1\" = --list-devices ]; then\n"
        "  tmp=$(mktemp)\n"
        "  \"$real\" --list-devices > \"$tmp\"\n"
        "  rc=$?\n"
        "  sed 's/ type=cpu / type=gpu /' \"$tmp\"\n"
        "  rm -f \"$tmp\"\n"
        "  exit \"$rc\"\n"
        "fi\n"
        "exec \"$real\" \"$@\"\n";
    const bool written = std::fwrite(body.data(), 1, body.size(), file) == body.size();
    const bool closed = std::fclose(file) == 0;
    return written && closed && chmod(path.c_str(), 0755) == 0;
}

struct Env {
    static constexpr std::array<const char*, 4> names{"CUCKATOO_GPU_SOLVER", "CUCKATOO_GPU_DEVICES", "CUCKATOO_GPU_TIMEOUT", "CUCKATOO_GPU_BUNDLED"};
    std::string script;
    std::array<std::optional<std::string>, 4> previous;
    Env()
    {
        for (size_t i = 0; i < names.size(); ++i) {
            if (const char* value = std::getenv(names[i])) previous[i] = value;
        }
        cuckatoo::ResetGpuDiscoveryForTesting();
        cuckatoo::ResetGpuDeviceStatsForTesting();
    }
    ~Env()
    {
        for (size_t i = 0; i < names.size(); ++i) {
            if (previous[i]) setenv(names[i], previous[i]->c_str(), 1);
            else unsetenv(names[i]);
        }
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

void RunBothDevices(const std::vector<std::string>& ids)
{
    Env env;
    char path[] = "/tmp/qs-opencl-two-shim-XXXXXX";
    const int fd = mkstemp(path);
    BOOST_REQUIRE(fd >= 0);
    env.script = path;
    BOOST_REQUIRE(WriteShim(fd, env.script, QSGPUSOLVE_OPENCL_PATH));
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
    const auto result = cuckatoo::MultiDeviceSolveBytes(
        pre.data(), pre.size(), 28, 0, /*two blocks*/ 128, cycle, nonce,
        [](const cuckatoo::Cycle&, uint32_t) { return false; }, {},
        [&] {
            if (BothWorking(ids)) return true;
            return std::chrono::steady_clock::now() - started > std::chrono::seconds{240};
        });
    BOOST_CHECK(result.had_devices);
    BOOST_CHECK(result.all_benched);
    BOOST_CHECK(result.faults.empty());

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
