// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <crypto/cuckatoo/gpu_devices.h>
#include <test/util/gpucardtables.h>
#include <util/fs.h>
#include <util/string.h>

#include <boost/test/unit_test.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <optional>
#include <thread>
#include <utility>
#include <vector>
#ifdef WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

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
struct Guard {
    std::vector<std::pair<const char*, std::optional<std::string>>> saved;
    Guard() {
        for (const char* n : {"CUCKATOO_GPU_SOLVER","CUCKATOO_GPU_BUNDLED","CUCKATOO_GPU_DEVICES","QS_SOLVER_STUB_LISTING","QS_SOLVER_STUB_SCRIPT"}) {
            const char* value = std::getenv(n);
            saved.emplace_back(n, value ? std::optional<std::string>{value} : std::nullopt);
            Unset(n);
        }
        ResetGpuDiscoveryForTesting();
    }
    ~Guard() {
        for (const auto& [name, value] : saved) {
            if (value) SetEnv(name, *value);
            else Unset(name);
        }
        ResetGpuDiscoveryForTesting();
    }
};
std::vector<GpuDevice> Parse(std::initializer_list<std::string> lines) { std::string s; for (const auto& l : lines) s += l + "\n"; return ParseDeviceListing(s); }
DeviceChoice Chosen(std::vector<std::string> ids) { return {DeviceChoice::Mode::kChosen, std::move(ids)}; }
std::vector<std::string> Ids(const std::vector<GpuDevice>& ds) { std::vector<std::string> r; for (const auto& d : ds) r.push_back(d.id); return r; }
std::string Tmp(const char* name) {
#ifdef WIN32
    const auto pid = util::ToString(GetCurrentProcessId());
#else
    const auto pid = util::ToString(getpid());
#endif
    return fs::PathToString(fs::temp_directory_path() / (std::string{"qs-gpu-devices-"} + pid + "-" + name));
}
std::string Script(const char* name, const std::string& content) {
    const std::string p = Tmp(name);
    std::ofstream{p} << content; return p;
}
//! How many times the stub's `append` ran on this mark: 0 before the first.
long Appended(const std::string& mark) { std::ifstream f(mark, std::ios::binary | std::ios::ate); return f ? static_cast<long>(f.tellg()) : 0L; }
} // namespace

BOOST_FIXTURE_TEST_SUITE(gpu_devices_tests, GpuCardTablesTestSetup)
BOOST_AUTO_TEST_CASE(environment_guard_restores_value_and_absence)
{
    constexpr const char* name{"CUCKATOO_GPU_DEVICES"};
    const char* original = std::getenv(name);
    const std::optional<std::string> saved = original ? std::optional<std::string>{original} : std::nullopt;
    SetEnv(name, "inherited-device");
    {
        Guard guard;
        BOOST_CHECK(std::getenv(name) == nullptr);
        SetEnv(name, "changed-device");
    }
    const char* restored = std::getenv(name);
    BOOST_CHECK(restored != nullptr);
    if (restored) BOOST_CHECK_EQUAL(std::string{restored}, "inherited-device");
    Unset(name);
    {
        Guard guard;
        SetEnv(name, "changed-device");
    }
    BOOST_CHECK(std::getenv(name) == nullptr);
    if (saved) SetEnv(name, *saved);
    else Unset(name);
}
BOOST_AUTO_TEST_CASE(parse_name_is_rest_of_line)
{
    auto d = ParseDeviceLine(RX); BOOST_REQUIRE(d); BOOST_CHECK_EQUAL(d->mem, 8589934592ULL); BOOST_CHECK_EQUAL(d->name, "AMD Radeon RX 480 Graphics (radeonsi, polaris10)");
    auto t = ParseDeviceLine("device backend=opencl id=pci:0000:04:00.0 type=gpu status=ok name=X id=wrong type=cpu"); BOOST_REQUIRE(t); BOOST_CHECK_EQUAL(t->id, "pci:0000:04:00.0"); BOOST_CHECK_EQUAL(t->name, "X id=wrong type=cpu");
}
BOOST_AUTO_TEST_CASE(parse_device_line_accepts_empty_name)
{
    const auto d = ParseDeviceLine("device backend=opencl id=pci:0000:04:00.0 type=gpu integrated=0 mem=6 status=ok name=");
    BOOST_REQUIRE(d);
    BOOST_CHECK_EQUAL(d->name, d->id);
    BOOST_CHECK_EQUAL(d->mem, 6U);
    BOOST_CHECK(d->status == DeviceStatus::kOk);
    BOOST_CHECK(!ParseDeviceLine("device backend=opencl type=gpu status=ok name="));
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
BOOST_AUTO_TEST_CASE(select_devices_accepts_saved_enumeration_id)
{
    const auto listed = Parse({"device backend=opencl id=pci:0000:04:00.0 type=gpu status=ok alias=opencl:0:1 name=Card"});
    BOOST_REQUIRE_EQUAL(listed.size(), 1U);
    BOOST_CHECK_EQUAL(listed[0].alias, "opencl:0:1");
    const auto selected = SelectDevices(listed, Chosen({"opencl:0:1"}));
    BOOST_REQUIRE_EQUAL(selected.use.size(), 1U);
    BOOST_CHECK_EQUAL(selected.use[0].id, "pci:0000:04:00.0");
    BOOST_CHECK_EQUAL(*SolveDevice(selected), "pci:0000:04:00.0");
    const auto without_alias = Parse({RX});
    BOOST_REQUIRE_EQUAL(without_alias.size(), 1U);
    BOOST_CHECK(without_alias[0].alias.empty());
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
static bool WaitFor(bool want_held, const std::string& path, std::chrono::milliseconds bound);
BOOST_AUTO_TEST_CASE(listing_timeout_kills_the_helper)
{
    Guard g;
    const std::string lock{Tmp("qs-list-timeout-lock")};
    std::remove(lock.c_str());
    SetEnv("QS_SOLVER_STUB_LISTING", Script("qs-list-timeout", "hold " + lock + "\nsleep 60000\n"));
    const auto start = std::chrono::steady_clock::now();
    auto listing = std::async(std::launch::async, [] {
        return ListDevices(SOLVER_STUB_PATH, std::chrono::milliseconds{3000});
    });
    BOOST_REQUIRE_MESSAGE(WaitFor(true, lock, std::chrono::milliseconds{2000}), "listing helper did not reach its lock");
    BOOST_CHECK(listing.get().outcome == ListOutcome::kTimedOut);
    BOOST_CHECK_MESSAGE(WaitFor(false, lock, std::chrono::milliseconds{2000}), "timed-out listing helper remains alive");
    BOOST_CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{20});
    std::remove(lock.c_str());
}
BOOST_AUTO_TEST_CASE(helper_graph_size)
{
    Guard g; SetEnv("CUCKATOO_GPU_BUNDLED", "/helper"); BOOST_CHECK_EQUAL(*GpuHelperFor(28), "/helper"); BOOST_CHECK(!GpuHelperFor(19)); SetEnv("CUCKATOO_GPU_SOLVER", "/custom"); BOOST_CHECK_EQUAL(*GpuHelperFor(19), "/custom");
}
//! True while some process holds the stub's `hold` lock on `path`. The OS drops the lock when the holder
//! exits, so this is the OS's statement about the stub, not a marker the stub wrote. Same primitive the stub
//! takes: flock on POSIX, an exclusive (share 0) open on Windows.
static bool LockHeld(const std::string& path)
{
#ifdef WIN32
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION;
    CloseHandle(h);
    return false;
#else
    const int fd = open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) return false;
    const bool free = flock(fd, LOCK_EX | LOCK_NB) == 0;
    close(fd);
    return !free;
#endif
}
static bool WaitFor(bool want_held, const std::string& path, std::chrono::milliseconds bound)
{
    const auto end = std::chrono::steady_clock::now() + bound;
    while (std::chrono::steady_clock::now() < end) { if (LockHeld(path) == want_held) return true; std::this_thread::sleep_for(std::chrono::milliseconds{10}); }
    return LockHeld(path) == want_held;
}
//! The test's own hold on the stub's lock, by the same primitive the stub takes. Not inherited by the stub
//! (O_CLOEXEC; a Windows handle is non-inheritable by default), so Release() really frees it.
class TestLock
{
public:
    explicit TestLock(const std::string& path)
    {
#ifdef WIN32
        m_file = CreateFileW(fs::PathFromString(path).wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                             0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        m_held = m_file != INVALID_HANDLE_VALUE;
#else
        m_fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        m_held = m_fd >= 0 && flock(m_fd, LOCK_EX | LOCK_NB) == 0;
#endif
    }
    ~TestLock() { Release(); }
    TestLock(const TestLock&) = delete;
    TestLock& operator=(const TestLock&) = delete;
    bool Held() const { return m_held; }
    void Release()
    {
#ifdef WIN32
        if (m_file != INVALID_HANDLE_VALUE) CloseHandle(m_file);
        m_file = INVALID_HANDLE_VALUE;
#else
        if (m_fd >= 0) close(m_fd);
        m_fd = -1;
#endif
        m_held = false;
    }

private:
#ifdef WIN32
    HANDLE m_file{INVALID_HANDLE_VALUE};
#else
    int m_fd{-1};
#endif
    bool m_held{false};
};
BOOST_AUTO_TEST_CASE(listing_stub_waits_for_contended_lock)
{
    Guard g;
    const std::string lock{Tmp("qs-list-contended-lock")};
    const std::string started{Tmp("qs-list-contended-started")};
    std::remove(started.c_str());
    const std::string script{Script("qs-list-contended", "append " + started + "\nhold " + lock + "\nsleep 60000\n")};
    SetEnv("QS_SOLVER_STUB_LISTING", script);

    TestLock test_lock{lock};
    BOOST_REQUIRE_MESSAGE(test_lock.Held(), "the test could not take the lock itself");
    auto listing = std::async(std::launch::async, [] {
        return ListDevices(SOLVER_STUB_PATH, std::chrono::milliseconds{5000});
    });
    // Start the contention window once the stub is about to execute `hold`.
    const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{2}};
    while (Appended(started) == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    const bool reached_hold{Appended(started) > 0};
    const bool waited{listing.wait_for(std::chrono::milliseconds{100}) == std::future_status::timeout};
    test_lock.Release();
    const bool acquired{reached_hold && waited && WaitFor(true, lock, std::chrono::seconds{2})};
    if (acquired) KillOutstandingListings();
    listing.wait();
    std::remove(script.c_str());
    std::remove(started.c_str());
    std::remove(lock.c_str());

    BOOST_REQUIRE_MESSAGE(reached_hold, "the stub did not reach the contended lock");
    BOOST_REQUIRE_MESSAGE(waited, "the stub exited while the test still held the lock");
    BOOST_CHECK_MESSAGE(acquired, "the stub did not hold the lock after the test released it");
}
//! F-606: a hung listing child must not outlive the app. The exit-time kill ends it and the listing returns.
BOOST_AUTO_TEST_CASE(exit_kill_ends_outstanding_listing_child)
{
    Guard g;
    const std::string lock{Tmp("qs-list-exitkill-lock")};
    std::remove(lock.c_str());
    SetEnv("QS_SOLVER_STUB_LISTING", Script("qs-list-exitkill", "hold " + lock + "\nsleep 90000\n"));
    auto listing = std::async(std::launch::async, [] { return ListDevices(SOLVER_STUB_PATH, std::chrono::milliseconds{60000}); });
    BOOST_REQUIRE_MESSAGE(WaitFor(true, lock, std::chrono::milliseconds{5000}), "the stub never started holding its lock");
    KillOutstandingListings();
    BOOST_CHECK_MESSAGE(WaitFor(false, lock, std::chrono::milliseconds{2000}), "the listing child is still running after the exit-time kill");
    const bool returned = listing.wait_for(std::chrono::seconds{5}) == std::future_status::ready;
    BOOST_REQUIRE_MESSAGE(returned, "the listing did not return after the exit-time kill");
    BOOST_CHECK(listing.get().outcome == ListOutcome::kFailed);
    std::remove(lock.c_str());
}
// S0118-B-01: a listing helper that closes its stdout and keeps running used to hold
// ListDevices in Wait(), under the discovery lock, for as long as it lived. The reap
// is bounded and the helper killed; a killed listing is a failed one.
BOOST_AUTO_TEST_CASE(listing_helper_that_outlives_its_stdout_is_reaped, *boost::unit_test::timeout(60))
{
    Guard g;
    SetEnv("QS_SOLVER_STUB_LISTING", Script("qs-list-closeout", "echo " + RX + "\ncloseout\nsleep 20000\n"));
    SetReapLimitForTesting(std::chrono::milliseconds{300});
    const auto start = std::chrono::steady_clock::now();
    const DeviceListing listing{ListDevices(SOLVER_STUB_PATH)};
    const auto took = std::chrono::steady_clock::now() - start;
    BOOST_CHECK_MESSAGE(took < std::chrono::seconds{10}, "the listing waited "
                        << std::chrono::duration_cast<std::chrono::milliseconds>(took).count() << " ms for the helper");
    BOOST_CHECK(listing.outcome == ListOutcome::kFailed);
}

// S0228-B-02: LastGpuDiscovery is read by the desktop's once-a-second mining status
// poll on the GUI thread. It must not wait while a listing holds the discovery lock.
BOOST_AUTO_TEST_CASE(last_discovery_does_not_wait_for_a_listing, *boost::unit_test::timeout(60))
{
    Guard g;
    const std::string mark{Tmp("qs-list-last")};
    std::remove(mark.c_str());
    SetEnv("CUCKATOO_GPU_SOLVER", SOLVER_STUB_PATH);
    SetEnv("QS_SOLVER_STUB_LISTING", Script("qs-list-last-slow", "append " + mark + "\nsleep 3000\necho " + RX + "\nexit 0\n"));
    auto listing = std::async(std::launch::async, [] { return DiscoverGpus(28); });
    const auto started_by = std::chrono::steady_clock::now() + std::chrono::seconds{20};
    while (Appended(mark) == 0 && std::chrono::steady_clock::now() < started_by) std::this_thread::sleep_for(std::chrono::milliseconds{10});
    BOOST_REQUIRE_EQUAL(Appended(mark), 1L);
    const auto start = std::chrono::steady_clock::now();
    BOOST_CHECK(!LastGpuDiscovery(28).has_value());
    const auto took = std::chrono::steady_clock::now() - start;
    BOOST_CHECK_MESSAGE(took < std::chrono::milliseconds{1000}, "LastGpuDiscovery waited "
                        << std::chrono::duration_cast<std::chrono::milliseconds>(took).count() << " ms for the listing");
    BOOST_CHECK(listing.get().situation == GpuSituation::kReady);
    BOOST_REQUIRE(LastGpuDiscovery(28).has_value());
    BOOST_CHECK(LastGpuDiscovery(28)->situation == GpuSituation::kReady);
    std::remove(mark.c_str());
}

// S0118-B-02a: a hand-set CUCKATOO_GPU_DEVICES the parser refuses used to read as
// automatic, so every card mined past a value such as "CPU" (the CPU-only setting is
// "cpu"). It now reads as a chosen card that no listing has, so no card is used.
BOOST_AUTO_TEST_CASE(refused_device_variable_uses_no_card)
{
    Guard g;
    SetEnv("CUCKATOO_GPU_SOLVER", SOLVER_STUB_PATH);
    SetEnv("QS_SOLVER_STUB_LISTING", Script("qs-list-refused-env", "echo " + RX + "\nexit 0\n"));
    for (const std::string value : {"CPU", "cpu,pci:0000:04:00.0", "card0"}) {
        ResetGpuDiscoveryForTesting();
        SetEnv("CUCKATOO_GPU_DEVICES", value);
        const DeviceChoice choice{DeviceChoiceFromEnv()};
        BOOST_CHECK(choice.mode == DeviceChoice::Mode::kChosen);
        BOOST_CHECK(choice.ids == std::vector<std::string>{value});
        const GpuDiscovery discovery{DiscoverGpus(28)};
        BOOST_CHECK_MESSAGE(discovery.selection.use.empty(), value << " put a card to use");
        BOOST_CHECK(discovery.situation == GpuSituation::kChosenMissing);
    }
    // The values the node itself installs are read as before.
    SetEnv("CUCKATOO_GPU_DEVICES", "cpu");
    BOOST_CHECK(DeviceChoiceFromEnv().mode == DeviceChoice::Mode::kCpuOnly);
    SetEnv("CUCKATOO_GPU_DEVICES", "pci:0000:04:00.0");
    BOOST_CHECK(DeviceChoiceFromEnv().mode == DeviceChoice::Mode::kChosen);
    Unset("CUCKATOO_GPU_DEVICES");
    BOOST_CHECK(DeviceChoiceFromEnv().mode == DeviceChoice::Mode::kAutomatic);
}
BOOST_AUTO_TEST_SUITE_END()
