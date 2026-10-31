// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <crypto/cuckatoo/gpu_devices.h>
#include <test/util/gpucardtables.h>

#include <boost/test/unit_test.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>

using namespace cuckatoo;
namespace {
const std::string RX{"device backend=opencl id=pci:0000:04:00.0 type=gpu integrated=0 mem=8589934592 status=ok name=AMD Radeon RX 480 Graphics (radeonsi, polaris10)"};
const std::string NV{"device backend=opencl id=pci:0000:01:00.0 type=gpu integrated=0 mem=6 status=ok name=NVIDIA P106-100"};
const std::string IGPU{"device backend=opencl id=pci:0000:00:02.0 type=gpu integrated=1 mem=1 status=ok name=Intel HD"};
const std::string BAD{"device backend=opencl id=pci:0000:05:00.0 type=gpu integrated=0 mem=1 status=compile-failed reason=bad_driver name=Old Card"};
void SetEnv(const char* n, const std::string& v) {
#ifdef WIN32
    _putenv_s(n, v.c_str());
#else
    setenv(n, v.c_str(), 1);
#endif
}
void Unset(const char* n) {
#ifdef WIN32
    _putenv_s(n, "");
#else
    unsetenv(n);
#endif
}
struct Guard { Guard(){ ResetGpuDiscoveryForTesting(); } ~Guard(){ for (const char* n : {"CUCKATOO_GPU_SOLVER","CUCKATOO_GPU_BUNDLED","CUCKATOO_GPU_DEVICES","QS_SOLVER_STUB_LISTING","QS_SOLVER_STUB_SCRIPT"}) Unset(n); ResetGpuDiscoveryForTesting(); } };
std::vector<GpuDevice> Parse(std::initializer_list<std::string> lines) { std::string s; for (const auto& l : lines) s += l + "\n"; return ParseDeviceListing(s); }
DeviceChoice Chosen(std::vector<std::string> ids) { return {DeviceChoice::Mode::kChosen, std::move(ids)}; }
std::vector<std::string> Ids(const std::vector<GpuDevice>& ds) { std::vector<std::string> r; for (const auto& d : ds) r.push_back(d.id); return r; }
std::string Tmp(const char* name) {
#ifdef WIN32
    return name;
#else
    return std::string{"/tmp/"} + name;
#endif
}
std::string Script(const char* name, const std::string& content) {
    const std::string p = Tmp(name);
    std::ofstream{p} << content; return p;
}
//! How many times the stub's `append` ran on this mark: 0 before the first.
long Appended(const std::string& mark) { std::ifstream f(mark, std::ios::binary | std::ios::ate); return f ? static_cast<long>(f.tellg()) : 0L; }
} // namespace

BOOST_FIXTURE_TEST_SUITE(gpu_devices_tests, GpuCardTablesTestSetup)
BOOST_AUTO_TEST_CASE(parse_name_is_rest_of_line)
{
    auto d = ParseDeviceLine(RX); BOOST_REQUIRE(d); BOOST_CHECK_EQUAL(d->mem, 8589934592ULL); BOOST_CHECK_EQUAL(d->name, "AMD Radeon RX 480 Graphics (radeonsi, polaris10)");
    auto t = ParseDeviceLine("device backend=opencl id=pci:0000:04:00.0 type=gpu status=ok name=X id=wrong type=cpu"); BOOST_REQUIRE(t); BOOST_CHECK_EQUAL(t->id, "pci:0000:04:00.0"); BOOST_CHECK_EQUAL(t->name, "X id=wrong type=cpu");
}
BOOST_AUTO_TEST_CASE(malformed_and_status)
{
    BOOST_CHECK(!ParseDeviceLine("progress=1")); BOOST_CHECK(!ParseDeviceLine("device backend=opencl type=gpu status=ok name=X"));
    auto d = ParseDeviceLine(BAD); BOOST_REQUIRE(d); BOOST_CHECK(d->status == DeviceStatus::kCompileFailed); BOOST_CHECK_EQUAL(d->reason, "bad driver");
}
BOOST_AUTO_TEST_CASE(cpu_cannot_combine_with_a_card)
{
    BOOST_CHECK(IsWellFormedDeviceId("pci:0000:04:00.0")); BOOST_CHECK(IsWellFormedDeviceId("opencl:1:0")); BOOST_CHECK(IsWellFormedDeviceId("rocm:gfx1100-0"));
    BOOST_CHECK(!IsWellFormedDeviceId("cpu")); BOOST_CHECK(!IsWellFormedDeviceId("pci:0000:4:00.0")); BOOST_CHECK(!IsWellFormedDeviceId("04:00.0"));
    BOOST_CHECK(!IsWellFormedDeviceId("pci:0000:04:00.0,pci:0000:05:00.0")); BOOST_CHECK(!IsWellFormedDeviceId("pci:0000:04:00.0 "));
    std::string e; BOOST_CHECK(!ParseDeviceChoice({"cpu", "pci:0000:04:00.0"}, e)); BOOST_CHECK_EQUAL(e, "-cuckatoodevice=cpu cannot be combined with a graphics card.");
    BOOST_CHECK(!ParseDeviceChoice({"pci:0000:4:00.0"}, e)); BOOST_CHECK_EQUAL(e, "-cuckatoodevice=pci:0000:4:00.0 is not a device id. Use an id from the helper's --list-devices output, such as pci:0000:04:00.0, or cpu.");
}
BOOST_AUTO_TEST_CASE(selection_ignores_listing_order)
{
    DeviceChoice a; auto x = SelectDevices(Parse({IGPU, RX, NV}), a); auto y = SelectDevices(Parse({RX, NV, IGPU}), a);
    BOOST_CHECK(Ids(x.use) == Ids(y.use)); BOOST_CHECK((Ids(x.use) == std::vector<std::string>{"pci:0000:01:00.0", "pci:0000:04:00.0"})); BOOST_CHECK_EQUAL(*SolveDevice(x), "pci:0000:01:00.0");
}
BOOST_AUTO_TEST_CASE(chosen_never_substitutes_and_classifies)
{
    auto c = Chosen({"pci:0000:09:00.0"}); auto l = Parse({RX}); auto s = SelectDevices(l, c); BOOST_CHECK(s.use.empty()); BOOST_CHECK_EQUAL(s.missing.size(), 1U);
    DeviceListing dl{ListOutcome::kListed, l, {}}; BOOST_CHECK(ClassifySituation(dl, c, s) == GpuSituation::kChosenMissing);
    DeviceChoice automatic; auto partial = SelectDevices(Parse({RX, BAD}), automatic); DeviceListing pl{ListOutcome::kListed, Parse({RX, BAD}), {}}; BOOST_CHECK(ClassifySituation(pl, automatic, partial) == GpuSituation::kPartial);
}
BOOST_AUTO_TEST_CASE(listing_exit_timeout_and_cache)
{
    Guard g;
    SetEnv("QS_SOLVER_STUB_LISTING", Script("qs-list-ok", "echo " + RX + "\nexit 0\n")); auto l = ListDevices(SOLVER_STUB_PATH); BOOST_CHECK(l.outcome == ListOutcome::kListed); BOOST_CHECK_EQUAL(l.devices.size(), 1U);
    SetEnv("QS_SOLVER_STUB_LISTING", Script("qs-list-four", "exit 4\n")); BOOST_CHECK(ListDevices(SOLVER_STUB_PATH).outcome == ListOutcome::kNoLoader);
    SetEnv("QS_SOLVER_STUB_LISTING", Script("qs-list-slow", "sleep 60000\n")); BOOST_CHECK(ListDevices(SOLVER_STUB_PATH, std::chrono::milliseconds{100}).outcome == ListOutcome::kTimedOut);
    const std::string mark{Tmp("qs-list-count")}; std::remove(mark.c_str()); SetEnv("QS_SOLVER_STUB_LISTING", Script("qs-list-counted", "append " + mark + "\necho " + RX + "\nexit 0\n")); SetEnv("CUCKATOO_GPU_SOLVER", SOLVER_STUB_PATH);
    BOOST_CHECK(DiscoverGpus(28).situation == GpuSituation::kReady); BOOST_CHECK(DiscoverGpus(28).situation == GpuSituation::kReady); std::ifstream m(mark, std::ios::binary|std::ios::ate); BOOST_CHECK_EQUAL(static_cast<long>(m.tellg()), 1L);
}
BOOST_AUTO_TEST_CASE(failed_listing_retries_and_keeps_stable_outcomes_cached)
{
    Guard g;
    const std::string mark{Tmp("qs-list-retry-failed")};
    const std::string script{Tmp("qs-list-retry-failed-script")};
    std::remove(mark.c_str());
    SetEnv("CUCKATOO_GPU_SOLVER", SOLVER_STUB_PATH);
    SetEnv("QS_SOLVER_STUB_LISTING", script);
    std::ofstream{script} << "append " << mark << "\nexit 5\n";
    BOOST_CHECK(DiscoverGpus(28).listing.outcome == ListOutcome::kFailed);
    std::ofstream{script} << "append " << mark << "\necho " << RX << "\nexit 0\n";
    BOOST_CHECK(DiscoverGpus(28).situation == GpuSituation::kReady);
    std::ifstream count(mark, std::ios::binary | std::ios::ate);
    BOOST_CHECK_EQUAL(static_cast<long>(count.tellg()), 2L);
    std::remove(script.c_str());
    std::remove(mark.c_str());

    ResetGpuDiscoveryForTesting();
    const std::string stable_mark{Tmp("qs-list-retry-stable")};
    const std::string stable_script{Tmp("qs-list-retry-stable-script")};
    std::remove(stable_mark.c_str());
    std::ofstream{stable_script} << "append " << stable_mark << "\nexit 4\n";
    SetEnv("QS_SOLVER_STUB_LISTING", stable_script);
    BOOST_CHECK(DiscoverGpus(28).listing.outcome == ListOutcome::kNoLoader);
    BOOST_CHECK(DiscoverGpus(28).listing.outcome == ListOutcome::kNoLoader);
    std::ifstream stable_count(stable_mark, std::ios::binary | std::ios::ate);
    BOOST_CHECK_EQUAL(static_cast<long>(stable_count.tellg()), 1L);
    std::remove(stable_script.c_str());
    std::remove(stable_mark.c_str());

    ResetGpuDiscoveryForTesting();
    Unset("CUCKATOO_GPU_SOLVER");
    Unset("CUCKATOO_GPU_BUNDLED");
    BOOST_CHECK(DiscoverGpus(28).listing.outcome == ListOutcome::kNoHelper);
    BOOST_CHECK(DiscoverGpus(28).listing.outcome == ListOutcome::kNoHelper);
}
BOOST_AUTO_TEST_CASE(timed_out_listing_retries)
{
    Guard g;
    const std::string mark{Tmp("qs-list-retry-timeout")};
    const std::string script{Tmp("qs-list-retry-timeout-script")};
    std::remove(mark.c_str());
    SetEnv("CUCKATOO_GPU_SOLVER", SOLVER_STUB_PATH);
    SetEnv("QS_SOLVER_STUB_LISTING", script);
    // The limit is injected so this case proves the retry without sleeping out
    // the real kListingLimit; Guard's ResetGpuDiscoveryForTesting restores it.
    SetListingLimitForTesting(std::chrono::milliseconds{150});
    std::ofstream{script} << "append " << mark << "\nsleep 60000\n";
    BOOST_CHECK(DiscoverGpus(28).listing.outcome == ListOutcome::kTimedOut);
    // F-583: the timed-out helper can be killed before it reaches `append`. A loaded
    // Windows runner took longer than the limit to start it, and this read 1, not 2.
    // So the retry is counted from whatever the first call left.
    const long after_timeout{Appended(mark)};
    BOOST_CHECK_LE(after_timeout, 1L);
    // The retry runs under the production limit: it proves the timeout was not
    // cached, and the 150ms limit would let a slow start time it out again.
    SetListingLimitForTesting(kListingLimit);
    std::ofstream{script} << "append " << mark << "\necho " << RX << "\nexit 0\n";
    BOOST_CHECK(DiscoverGpus(28).situation == GpuSituation::kReady);
    BOOST_CHECK_EQUAL(Appended(mark), after_timeout + 1);
    std::remove(script.c_str());
    std::remove(mark.c_str());
}
BOOST_AUTO_TEST_CASE(listing_timeout_kills_the_helper)
{
    Guard g;
    SetEnv("QS_SOLVER_STUB_LISTING", Script("qs-list-timeout", "sleep 60000\n"));
    const auto start = std::chrono::steady_clock::now();
    BOOST_CHECK(ListDevices(SOLVER_STUB_PATH, std::chrono::milliseconds{100}).outcome == ListOutcome::kTimedOut);
    BOOST_CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{20});
}
BOOST_AUTO_TEST_CASE(helper_graph_size)
{
    Guard g; SetEnv("CUCKATOO_GPU_BUNDLED", "/helper"); BOOST_CHECK_EQUAL(*GpuHelperFor(28), "/helper"); BOOST_CHECK(!GpuHelperFor(19)); SetEnv("CUCKATOO_GPU_SOLVER", "/custom"); BOOST_CHECK_EQUAL(*GpuHelperFor(19), "/custom");
}
BOOST_AUTO_TEST_SUITE_END()
