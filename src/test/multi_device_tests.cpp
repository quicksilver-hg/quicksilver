// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/cuckatoo/cuckatoo.h>
#include <crypto/cuckatoo/gpu_devices.h>
#include <crypto/cuckatoo/multi_device.h>
#include <chainparams.h>
#include <consensus/merkle.h>
#include <node/block_solve.h>
#include <pow.h>
#include <primitives/block.h>
#include <test/util/setup_common.h>
#include <validation.h>
#include <vault/spend.h>
#include <vault/test/util.h>

#include <boost/test/unit_test.hpp>

#ifdef WIN32
#include <windows.h>
#include <process.h>
#define getpid _getpid
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

BOOST_AUTO_TEST_SUITE(multi_device_tests)

namespace {
constexpr const char* DEVICE_A{"pci:0000:01:00.0"};
constexpr const char* DEVICE_B{"pci:0000:04:00.0"};

void SetEnv(const char* name, const std::string& value)
{
#ifdef WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void UnsetEnv(const char* name)
{
#ifdef WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

std::string TempPath(const std::string& stem)
{
    char pid[32];
    const auto [end, error] = std::to_chars(std::begin(pid), std::end(pid), static_cast<int>(getpid()));
    BOOST_REQUIRE(error == std::errc{});
#ifdef WIN32
    return stem + "-" + std::string{pid, end};
#else
    return "/tmp/" + stem + "-" + std::string{pid, end};
#endif
}

void WriteFile(const std::string& path, const std::string& body)
{
    std::ofstream out{path};
    out << body;
}

std::string Number(uint32_t value, int base = 10)
{
    char out[32];
    const auto [end, error] = std::to_chars(std::begin(out), std::end(out), value, base);
    BOOST_REQUIRE(error == std::errc{});
    return {out, end};
}

std::string CycleLine(const cuckatoo::Cycle& cycle)
{
    std::string out{"cycle="};
    for (size_t i{0}; i < cycle.size(); ++i) {
        if (i != 0) out += " ";
        out += Number(cycle[i], 16);
    }
    return out;
}

std::vector<std::string> ReadLines(const std::string& path)
{
    std::vector<std::string> out;
    std::ifstream in{path};
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
    }
    return out;
}

std::vector<std::string> Fields(const std::string& line)
{
    std::vector<std::string> out;
    size_t start{0};
    for (;;) {
        const size_t end = line.find('\t', start);
        out.push_back(line.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}

uint32_t ParseNumber(const std::string& text)
{
    uint32_t out{0};
    for (const char c : text) {
        BOOST_REQUIRE(c >= '0' && c <= '9');
        out = out * 10 + static_cast<uint32_t>(c - '0');
    }
    return out;
}

struct Launch {
    std::string device;
    uint32_t start;
    uint32_t attempts;
};

std::vector<Launch> Launches(const std::string& path)
{
    std::vector<Launch> out;
    for (const auto& line : ReadLines(path)) {
        const auto fields = Fields(line);
        if (fields.size() != 6 || fields[0] != "--device") continue;
        out.push_back({fields[1], ParseNumber(fields[4]), ParseNumber(fields[5])});
    }
    return out;
}

struct Proof {
    cuckatoo::Cycle cycle;
    uint32_t nonce;
};

std::vector<Proof> CpuProofs(const std::vector<unsigned char>& pre, size_t count)
{
    UnsetEnv("CUCKATOO_GPU_SOLVER");
    UnsetEnv("CUCKATOO_GPU_BUNDLED");
    cuckatoo::ResetGpuDiscoveryForTesting();
    std::vector<Proof> out;
    uint32_t start{0};
    while (out.size() < count) {
        Proof proof{};
        BOOST_REQUIRE(cuckatoo::CuckatooSolveBytes(pre.data(), pre.size(), 19, start,
                                                   1U << 20, proof.cycle, proof.nonce,
                                                   /*cpu_fallback=*/true));
        out.push_back(proof);
        BOOST_REQUIRE(proof.nonce != std::numeric_limits<uint32_t>::max());
        start = proof.nonce + 1;
    }
    return out;
}

struct Fixture {
    std::vector<std::string> files;
    Fixture()
    {
        cuckatoo::ResetGpuDiscoveryForTesting();
        cuckatoo::ResetGpuDeviceStatsForTesting();
    }
    ~Fixture()
    {
        for (const char* name : {"CUCKATOO_GPU_SOLVER", "CUCKATOO_GPU_BUNDLED",
                                 "CUCKATOO_GPU_DEVICES", "CUCKATOO_GPU_TIMEOUT",
                                 "QS_SOLVER_STUB_SCRIPT", "QS_SOLVER_STUB_LISTING",
                                 "QS_SOLVER_STUB_DEVICE_SCRIPTS",
                                 "QS_SOLVER_STUB_ARGV_APPEND"}) UnsetEnv(name);
        cuckatoo::ResetGpuDiscoveryForTesting();
        cuckatoo::ResetGpuDeviceStatsForTesting();
        for (const auto& file : files) std::remove(file.c_str());
    }

    std::string File(const std::string& stem, const std::string& body)
    {
        const std::string path = TempPath(stem);
        std::remove(path.c_str());
        WriteFile(path, body);
        files.push_back(path);
        return path;
    }

    std::string Configure(const std::string& script_a, const std::string& script_b,
                          bool reverse_listing = false)
    {
        const std::string a = File("qs-multi-a", script_a);
        const std::string b = File("qs-multi-b", script_b);
        const std::string listing = File(
            "qs-multi-listing",
            std::string{"echo device backend=stub id="} + (reverse_listing ? DEVICE_B : DEVICE_A) +
                " type=gpu integrated=0 mem=0 status=ok name=First\n" +
            "echo device backend=stub id=" + (reverse_listing ? std::string{DEVICE_A} : std::string{DEVICE_B}) +
                " type=gpu integrated=0 mem=0 status=ok name=Second\nexit 0\n");
        const std::string map = File("qs-multi-map", std::string{DEVICE_A} + "\t" + a + "\n" + DEVICE_B + "\t" + b + "\n");
        const std::string argv = TempPath("qs-multi-argv");
        std::remove(argv.c_str());
        files.push_back(argv);
        SetEnv("CUCKATOO_GPU_SOLVER", SOLVER_STUB_PATH);
        SetEnv("QS_SOLVER_STUB_SCRIPT", a);
        SetEnv("QS_SOLVER_STUB_LISTING", listing);
        SetEnv("QS_SOLVER_STUB_DEVICE_SCRIPTS", map);
        SetEnv("QS_SOLVER_STUB_ARGV_APPEND", argv);
        SetEnv("CUCKATOO_GPU_TIMEOUT", "5");
        cuckatoo::ResetGpuDiscoveryForTesting();
        return argv;
    }
};

// The coordinator's block size (multi_device.cpp). Two workers race for the
// first claim, so a case can't know which card runs which block.
constexpr uint32_t BLOCK{64};

// A helper that reports each proof only when its nonce lies in the range the
// launch was given, as a real one does; "found" in the stub. A case that needs a
// card to answer whichever block it claims gives it a proof in each block.
std::string FoundScript(const std::vector<Proof>& proofs, uint32_t delay_ms = 0)
{
    std::string out;
    if (delay_ms != 0) out += "sleep " + Number(delay_ms) + "\n";
    for (const auto& proof : proofs) out += "found " + Number(proof.nonce) + " " + CycleLine(proof.cycle) + "\n";
    out += "exit 0\n";
    return out;
}

// `per_block` proofs inside each of the first `blocks` blocks from nonce 0.
std::vector<Proof> BlockProofs(const std::vector<unsigned char>& pre, uint32_t blocks, size_t per_block = 1)
{
    std::vector<Proof> out;
    for (uint32_t block{0}; block < blocks; ++block) {
        uint32_t start{block * BLOCK};
        for (size_t i{0}; i < per_block; ++i) {
            UnsetEnv("CUCKATOO_GPU_SOLVER");
            UnsetEnv("CUCKATOO_GPU_BUNDLED");
            cuckatoo::ResetGpuDiscoveryForTesting();
            Proof proof{};
            BOOST_REQUIRE(cuckatoo::CuckatooSolveBytes(pre.data(), pre.size(), 19, start, BLOCK,
                                                       proof.cycle, proof.nonce, /*cpu_fallback=*/true));
            BOOST_REQUIRE(proof.nonce >= block * BLOCK && proof.nonce < (block + 1) * BLOCK);
            out.push_back(proof);
            start = proof.nonce + 1;
        }
    }
    return out;
}

// True while some other process holds the stub's `hold` lock on `path`. The
// kernel drops that lock when the holder dies, so this going false is the one
// statement that the stub process has EXITED. A marker the stub appends and
// then exits cannot say that: the marker is visible for as long as the stub
// takes to leave main(), which is exactly the window F-531 lives in.
//
// A twin of this pair sits in gpu_solver_tests.cpp, which tests the same order
// one layer down. They are copied rather than shared because neither file has a
// header the other can reach without adding a shared test utility for one probe.
bool StubHoldsLock(const std::string& path)
{
#ifdef WIN32
    const HANDLE probe{CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (probe == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION;
    CloseHandle(probe);
    return false;
#else
    const int fd{open(path.c_str(), O_RDWR)};
    if (fd == -1) return false;
    const bool held{flock(fd, LOCK_EX | LOCK_NB) != 0};
    close(fd);
    return held;
#endif
}

// Block until the stub holding `path` is gone; false if it was still there when
// the bound ran out, so a stub that never took the lock fails the case instead
// of hanging the suite. The bound is never reached in the order this is used
// for. It returns rather than asserting because its caller is a coordinator
// callback running on a worker thread: a Boost assertion throws, and an
// exception escaping that lambda would terminate the process instead of failing
// the case.
[[nodiscard]] bool WaitForStubExit(const std::string& path)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
    while (StubHoldsLock(path)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return true;
}

// Creates `path` once `device` has been launched `count` times, read from the
// launch record. Stands in for "the other card is slower": a sleep in one
// card's script only makes the other faster on average, so the block counts it
// produces are a property of the machine, not of the coordinator (F-532).
struct ReleaseAfterLaunches {
    std::atomic<bool> stop{false};
    std::thread thread;
    ReleaseAfterLaunches(const std::string& argv, const std::string& device, size_t count, const std::string& path)
        : thread([this, argv, device, count, path] {
              while (!stop.load(std::memory_order_relaxed)) {
                  size_t seen{0};
                  for (const auto& launch : Launches(argv)) {
                      if (launch.device == device) ++seen;
                  }
                  if (seen >= count) {
                      std::ofstream out{path};
                      out << "x";
                      return;
                  }
                  std::this_thread::sleep_for(std::chrono::milliseconds{1});
              }
          }) {}
    ~ReleaseAfterLaunches()
    {
        stop.store(true, std::memory_order_relaxed);
        if (thread.joinable()) thread.join();
    }
};

std::string FreshPath(Fixture& fixture, const std::string& stem)
{
    const std::string path = TempPath(stem);
    std::remove(path.c_str());
    fixture.files.push_back(path);
    return path;
}

// Creates `path` once `device` is faulted. B must not answer before that: the
// coordinator records the fault only after A's process has exited, and an
// accepted proof from B before then cancels A and drops the unread fault.
struct ReleaseOnFault {
    std::atomic<bool> stop{false};
    std::thread thread;
    ReleaseOnFault(const std::string& device, const std::string& path)
        : thread([this, device, path] {
              while (!stop.load(std::memory_order_relaxed)) {
                  const auto stats = cuckatoo::GetGpuDeviceStats();
                  const auto row = std::find_if(stats.begin(), stats.end(), [&](const auto& value) { return value.device == device; });
                  if (row != stats.end() && row->state == cuckatoo::GpuDeviceState::kFaulted) {
                      std::ofstream out{path};
                      out << "x";
                      return;
                  }
                  std::this_thread::sleep_for(std::chrono::milliseconds{5});
              }
          }) {}
    ~ReleaseOnFault()
    {
        stop.store(true, std::memory_order_relaxed);
        if (thread.joinable()) thread.join();
    }
};

std::string AfterLaunch(const std::string& launched, const std::string& released, const std::string& rest)
{
    return "await " + launched + "\nawait " + released + "\n" + rest;
}

// The proof among `proofs` that lies in `device`'s first launch.
Proof FirstLaunchProof(const std::string& argv, const std::string& device, const std::vector<Proof>& proofs)
{
    for (const auto& launch : Launches(argv)) {
        if (launch.device != device) continue;
        for (const auto& proof : proofs) {
            if (proof.nonce >= launch.start && uint64_t{proof.nonce} - launch.start < launch.attempts) return proof;
        }
        break;
    }
    BOOST_FAIL("no proof lies in " << device << "'s first launch");
    return {};
}

bool Solve(const std::vector<unsigned char>& pre, uint32_t start, uint32_t attempts,
           cuckatoo::Cycle& cycle, uint32_t& nonce,
           const cuckatoo::SolverAcceptPredicate& accept = {},
           const cuckatoo::SolverProgressCallback& progress = {},
           const cuckatoo::SolverCancelCallback& cancel = {}, bool cpu = false,
           cuckatoo::GpuSolveStatus* status = nullptr)
{
    return cuckatoo::CuckatooSolveBytes(pre.data(), pre.size(), 19, start, attempts,
                                        cycle, nonce, cpu, progress, cancel, status,
                                        /*discarded_cycles=*/nullptr, accept);
}
} // namespace

BOOST_AUTO_TEST_CASE(blocks_never_overlap_and_cover_the_budget)
{
    Fixture f;
    const std::vector<unsigned char> pre(64, 0xCD);
    for (const auto& [start, budget] : {std::pair<uint32_t, uint32_t>{7, 197},
                                       {std::numeric_limits<uint32_t>::max() - 69, 70}}) {
        const std::string argv = f.Configure("exit 0\n", "exit 0\n");
        cuckatoo::Cycle cycle{}; uint32_t nonce{0};
        BOOST_CHECK(!Solve(pre, start, budget, cycle, nonce));
        auto launches = Launches(argv);
        std::sort(launches.begin(), launches.end(), [](const auto& x, const auto& y) { return x.start < y.start; });
        BOOST_REQUIRE(!launches.empty());
        uint64_t expected{start};
        uint64_t covered{0};
        for (size_t i{0}; i < launches.size(); ++i) {
            const auto& launch = launches[i];
            BOOST_CHECK_EQUAL(launch.start, expected);
            const uint32_t expected_attempts = i + 1 == launches.size()
                ? (budget % 64 == 0 ? 64 : budget % 64)
                : 64;
            BOOST_CHECK_EQUAL(launch.attempts, expected_attempts);
            expected += launch.attempts;
            covered += launch.attempts;
        }
        BOOST_CHECK_EQUAL(covered, budget);
        BOOST_CHECK_EQUAL(expected, uint64_t{start} + budget);
    }
}

BOOST_AUTO_TEST_CASE(faster_card_takes_more_blocks)
{
    Fixture f;
    // Second Card used to be "slower" by sleeping 100 ms a block, which only
    // made First Card faster on average: the counts the case then asserted were
    // a property of how the two raced on this machine. Here Second Card is held
    // in its first block until First Card has taken every other one, so "faster
    // card takes more blocks" has one answer and the case can assert it exactly
    // (F-532).
    const std::string released = FreshPath(f, "qs-multi-faster-released");
    const std::string argv = f.Configure("exit 0\n", "await " + released + "\nexit 0\n");
    constexpr uint32_t BLOCKS{10};
    ReleaseAfterLaunches gate{argv, DEVICE_A, BLOCKS - 1, released};
    const std::vector<unsigned char> pre(64, 0xCD);
    cuckatoo::Cycle cycle{}; uint32_t nonce{0};
    BOOST_CHECK(!Solve(pre, 0, BLOCKS * BLOCK, cycle, nonce));
    size_t a{0}, b{0};
    for (const auto& launch : Launches(argv)) (launch.device == DEVICE_A ? a : b)++;
    BOOST_CHECK_GT(a, b);
    BOOST_CHECK_EQUAL(b, 1U);
    BOOST_CHECK_EQUAL(a, BLOCKS - 1);
}

BOOST_AUTO_TEST_CASE(accepted_proof_cancels_the_other_card)
{
    Fixture f;
    const std::vector<unsigned char> pre(64, 0xCD);
    const auto proofs = BlockProofs(pre, 2);
    // The three times this case used to run on are all gone (F-532). First
    // Card's 300 ms head start, which stood for "Second Card has started", is
    // now Second Card's own marker. `elapsed < 1.0` and the 400 ms re-read of
    // the marker were each approximating "Second Card's process was already
    // dead when the solve returned"; the lock says exactly that, since the
    // kernel frees it only when the holder exits, and it says it with no clock
    // in the assertion at all.
    const std::string marker = f.File("qs-multi-accepted-marker", "");
    const std::string started = FreshPath(f, "qs-multi-accepted-started");
    const std::string lock = FreshPath(f, "qs-multi-accepted-lock");
    f.Configure("await " + started + "\n" + FoundScript(proofs),
                "hold " + lock + "\nappend " + started + "\nappend " + marker + "\nloop\n");
    cuckatoo::Cycle cycle{}; uint32_t nonce{0};
    BOOST_REQUIRE(Solve(pre, 0, 1U << 20, cycle, nonce));
    BOOST_CHECK_MESSAGE(!StubHoldsLock(lock),
                        "the losing card's helper must already be gone when the solve returns");
    BOOST_CHECK_GT(std::ifstream(marker, std::ios::ate).tellg(), 0);
}

BOOST_AUTO_TEST_CASE(rejected_cycle_does_not_cancel_the_other_card)
{
    Fixture f;
    const std::vector<unsigned char> pre(64, 0xCD);
    const auto proofs = BlockProofs(pre, 2, 2);
    const std::vector<Proof> rejected{proofs[0], proofs[2]};
    const std::vector<Proof> accepted{proofs[1], proofs[3]};
    // "First Card rejects first" was Second Card sleeping 200 ms. The accept
    // predicate already knows when the first rejection happened, so it says so
    // and Second Card waits for that instead (F-532).
    const std::string refused = FreshPath(f, "qs-multi-rejected-refused");
    const std::string argv = f.Configure(FoundScript(rejected),
                                         "await " + refused + "\n" + FoundScript(accepted));
    size_t rejections{0};
    cuckatoo::Cycle cycle{}; uint32_t nonce{0};
    BOOST_REQUIRE(Solve(pre, 0, 1U << 20, cycle, nonce,
                        [&](const cuckatoo::Cycle&, uint32_t candidate) {
                            if (candidate == accepted[0].nonce || candidate == accepted[1].nonce) return true;
                            ++rejections;
                            std::ofstream out{refused};
                            out << "x";
                            return false;
                        }));
    // Without this the case passes whether or not a rejection ever happened:
    // if Second Card simply answered first, nothing was rejected and nothing
    // about "a rejected cycle does not cancel" was exercised. The old 200 ms
    // was what made that unlikely; the gate above makes it impossible, and this
    // is the assertion that says so.
    BOOST_CHECK_GT(rejections, 0U);
    BOOST_CHECK_EQUAL(nonce, FirstLaunchProof(argv, DEVICE_B, accepted).nonce);
}

BOOST_AUTO_TEST_CASE(bad_proof_benches_that_card_and_the_other_continues)
{
    Fixture f;
    const std::vector<unsigned char> pre(64, 0xCD);
    const auto proofs = BlockProofs(pre, 2);
    cuckatoo::Cycle bad{};
    for (size_t i{0}; i < bad.size(); ++i) bad[i] = 1000 + i;
    // A appends `launched` as it starts. B waits for that, then for `released`,
    // which appears only once A is faulted. A leading sleep used to let B solve
    // first and cancel A (F-527).
    const std::string launched = FreshPath(f, "qs-multi-bad-launched");
    const std::string released = FreshPath(f, "qs-multi-bad-released");
    const std::string argv = f.Configure(
        "append " + launched + "\necho nonce=7\necho " + CycleLine(bad) + "\nexit 0\n",
        AfterLaunch(launched, released, FoundScript(proofs)));
    ReleaseOnFault gate{DEVICE_A, released};
    cuckatoo::Cycle cycle{}; uint32_t nonce{0}; cuckatoo::GpuSolveStatus status{};
    BOOST_REQUIRE(Solve(pre, 0, 1U << 20, cycle, nonce, {}, {}, {}, false, &status));
    BOOST_CHECK_EQUAL(nonce, FirstLaunchProof(argv, DEVICE_B, proofs).nonce);
    BOOST_CHECK(status == cuckatoo::GpuSolveStatus::kSolved);
    const auto stats = cuckatoo::GetGpuDeviceStats();
    const auto a = std::find_if(stats.begin(), stats.end(), [](const auto& row) { return row.device == DEVICE_A; });
    BOOST_REQUIRE(a != stats.end());
    BOOST_CHECK(a->state == cuckatoo::GpuDeviceState::kFaulted);
    BOOST_CHECK(a->last_status == cuckatoo::GpuSolveStatus::kSolverError);
}

// F-531: when one card's proof is accepted every other card is cancelled in the
// same slice, and a card that faulted in that slice has ALREADY exited. The
// bridge used to kill the group and report kCancelled without reading the dead
// child's exit status, so the coordinator booked a faulted card as idle: nothing
// in `faults`, nothing in the stats table, and (after F-534) solver_ok stayed
// true while a card was down. It would fault again on the next call, and be lost
// again the same way.
//
// The order is exact and has no sleep in it. Second Card takes an OS lock it
// never releases, prints a progress line and then waits; First Card waits for
// that line before offering its proof. The accept predicate -- which the
// coordinator calls while holding the callback mutex, so Second Card's bridge
// loop is parked behind it -- releases Second Card and then blocks until the
// lock is free, which only Second Card's EXIT can make true. The coordinator's
// cancel is gated on the same two facts, so whichever way the park resolves,
// Second Card's bridge sees cancel with an already-exited child.
BOOST_AUTO_TEST_CASE(fault_in_the_winning_slice_is_still_reported)
{
    Fixture f;
    const std::vector<unsigned char> pre(64, 0xCD);
    const auto proofs = BlockProofs(pre, 2);
    const std::string progressed = FreshPath(f, "qs-multi-f531-progressed");
    const std::string holding = FreshPath(f, "qs-multi-f531-holding");
    const std::string lock = FreshPath(f, "qs-multi-f531-lock");
    f.Configure("await " + progressed + "\n" + FoundScript(proofs),
                "hold " + lock + "\necho progress=1\nawait " + holding +
                    "\necho progress=2\nexit 5\n");

    const auto progress = [&](uint32_t) {
        std::ofstream out{progressed};
        out << "x";
    };
    // Runs on First Card's worker thread, so it records what it saw and the
    // checks are made on the test's own thread once the solve is over.
    std::atomic<bool> rival_was_running{false};
    std::atomic<bool> rival_exited{false};
    const auto accept = [&](const cuckatoo::Cycle&, uint32_t) {
        rival_was_running.store(StubHoldsLock(lock));
        std::ofstream out{holding};
        out << "x";
        out.close();
        rival_exited.store(WaitForStubExit(lock));
        return true;
    };
    const auto cancel = [&] {
        std::ifstream released{holding};
        return released.good() && !StubHoldsLock(lock);
    };

    cuckatoo::Cycle cycle{}; uint32_t nonce{0};
    const auto result = cuckatoo::MultiDeviceSolveBytes(pre.data(), pre.size(), 19, 0, 1U << 20,
                                                        cycle, nonce, accept, progress, cancel);
    BOOST_CHECK_MESSAGE(rival_was_running.load(),
                        "Second Card must still be running when its rival's proof is offered");
    BOOST_CHECK_MESSAGE(rival_exited.load(),
                        "Second Card did not exit while its rival's proof was being accepted");
    BOOST_CHECK(result.solved);
    BOOST_REQUIRE_EQUAL(result.faults.size(), 1U);
    BOOST_CHECK_EQUAL(result.faults.front().device, DEVICE_B);
    BOOST_CHECK(result.faults.front().status == cuckatoo::GpuSolveStatus::kDeviceFault);
    const auto stats = cuckatoo::GetGpuDeviceStats();
    const auto row = std::find_if(stats.begin(), stats.end(), [](const auto& value) { return value.device == DEVICE_B; });
    BOOST_REQUIRE(row != stats.end());
    BOOST_CHECK(row->state == cuckatoo::GpuDeviceState::kFaulted);
    BOOST_CHECK(row->last_status == cuckatoo::GpuSolveStatus::kDeviceFault);
}

// F-526: a valid proof for a nonce outside the block the card was given is not
// one the coordinator handed out. It is never offered to accept, and the card is
// benched with a solver error.
BOOST_AUTO_TEST_CASE(proof_outside_the_given_block_is_never_accepted)
{
    Fixture f;
    const std::vector<unsigned char> pre(64, 0xCD);
    const Proof proof = CpuProofs(pre, 1).front();
    SetEnv("CUCKATOO_GPU_DEVICES", DEVICE_A);
    f.Configure("echo nonce=" + Number(proof.nonce) + "\necho " + CycleLine(proof.cycle) + "\nexit 0\n", "exit 0\n");
    bool offered{false};
    cuckatoo::Cycle cycle{}; uint32_t nonce{0}; cuckatoo::GpuSolveStatus status{};
    BOOST_CHECK(!Solve(pre, proof.nonce + 1, BLOCK, cycle, nonce,
                       [&](const cuckatoo::Cycle&, uint32_t) { offered = true; return true; },
                       {}, {}, false, &status));
    BOOST_CHECK(!offered);
    const auto stats = cuckatoo::GetGpuDeviceStats();
    const auto row = std::find_if(stats.begin(), stats.end(), [](const auto& value) { return value.device == DEVICE_A; });
    BOOST_REQUIRE(row != stats.end());
    BOOST_CHECK(row->state == cuckatoo::GpuDeviceState::kFaulted);
    BOOST_CHECK(row->last_status == cuckatoo::GpuSolveStatus::kSolverError);
}

BOOST_AUTO_TEST_CASE(faulted_card_is_benched_and_the_other_continues)
{
    Fixture f;
    const std::vector<unsigned char> pre(64, 0xCD);
    const auto proofs = BlockProofs(pre, 2);
    const std::string launched = FreshPath(f, "qs-multi-fault-launched");
    const std::string released = FreshPath(f, "qs-multi-fault-released");
    const std::string argv = f.Configure(
        "append " + launched + "\nexit 5\n",
        AfterLaunch(launched, released, FoundScript(proofs)));
    ReleaseOnFault gate{DEVICE_A, released};
    cuckatoo::Cycle cycle{}; uint32_t nonce{0};
    BOOST_REQUIRE(Solve(pre, 0, 1U << 20, cycle, nonce));
    size_t a{0};
    for (const auto& launch : Launches(argv)) if (launch.device == DEVICE_A) ++a;
    BOOST_CHECK_EQUAL(a, 1U);
    const auto stats = cuckatoo::GetGpuDeviceStats();
    const auto row = std::find_if(stats.begin(), stats.end(), [](const auto& value) { return value.device == DEVICE_A; });
    BOOST_REQUIRE(row != stats.end());
    BOOST_CHECK(row->state == cuckatoo::GpuDeviceState::kFaulted);
    BOOST_CHECK(row->last_status == cuckatoo::GpuSolveStatus::kDeviceFault);
}

BOOST_AUTO_TEST_CASE(cpu_fallback_only_when_every_card_faulted)
{
    Fixture f;
    const std::vector<unsigned char> pre(64, 0xCD);
    cuckatoo::Cycle cycle{}; uint32_t nonce{0};
    f.Configure("exit 5\n", "exit 0\n");
    BOOST_CHECK(!Solve(pre, 0, 128, cycle, nonce, {}, {}, {}, true));
    f.Configure("exit 5\n", "exit 5\n");
    BOOST_CHECK(Solve(pre, 0, 1U << 20, cycle, nonce, {}, {}, {}, true));
    BOOST_CHECK(!Solve(pre, 0, 128, cycle, nonce, {}, {}, {}, false));
}

BOOST_AUTO_TEST_CASE(cancel_stops_every_worker)
{
    Fixture f;
    const std::vector<unsigned char> pre(64, 0xCD);
    const std::string argv = f.Configure("sleep 5000\nexit 0\n", "sleep 5000\nexit 0\n");
    cuckatoo::Cycle cycle{}; uint32_t nonce{0};
    const auto started = std::chrono::steady_clock::now();
    std::atomic<std::chrono::steady_clock::rep> cancelled{0};
    BOOST_CHECK(!Solve(pre, 0, 1000, cycle, nonce, {}, {}, [&] {
        const auto now = std::chrono::steady_clock::now();
        if (Launches(argv).size() == 2U || now - started > std::chrono::seconds{30}) {
            auto unset = std::chrono::steady_clock::rep{0};
            cancelled.compare_exchange_strong(unset, now.time_since_epoch().count());
            return true;
        }
        return false;
    }));
    BOOST_REQUIRE_NE(cancelled.load(), 0);
    const auto cancelled_at = std::chrono::steady_clock::time_point{std::chrono::steady_clock::duration{cancelled.load()}};
    BOOST_CHECK_LT(std::chrono::duration<double>(std::chrono::steady_clock::now() - cancelled_at).count(), 1.0);
    BOOST_CHECK_EQUAL(Launches(argv).size(), 2U);
}

BOOST_AUTO_TEST_CASE(callbacks_are_never_called_concurrently)
{
    Fixture f;
    const std::vector<unsigned char> pre(64, 0xCD);
    const auto proofs = BlockProofs(pre, 2);
    f.Configure("echo progress=1\n" + FoundScript(proofs, 50),
                "echo progress=2\n" + FoundScript(proofs, 50));
    std::atomic<int> active{0};
    std::atomic<bool> overlap{false};
    std::atomic<bool> saw_a{false};
    std::atomic<bool> saw_b{false};
    const auto enter = [&] {
        if (active.fetch_add(1) != 0) overlap = true;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        active.fetch_sub(1);
    };
    cuckatoo::Cycle cycle{}; uint32_t nonce{0};
    BOOST_CHECK(Solve(pre, 0, 128, cycle, nonce,
                      [&](const cuckatoo::Cycle&, uint32_t candidate) {
                          enter();
                          // Each block's proof is reported once; the second one seen wins.
                          if (candidate == proofs[0].nonce) { saw_a = true; return saw_b.load(); }
                          if (candidate == proofs[1].nonce) { saw_b = true; return saw_a.load(); }
                          return false;
                      },
                      [&](uint32_t) { enter(); }, [&] { enter(); return false; }));
    BOOST_CHECK(saw_a.load());
    BOOST_CHECK(saw_b.load());
    BOOST_CHECK(!overlap.load());
}

BOOST_AUTO_TEST_CASE(selection_order_does_not_change_the_split)
{
    Fixture f;
    const std::vector<unsigned char> pre(64, 0xCD);
    const auto run = [&](bool reverse) {
        const std::string argv = f.Configure("sleep 10\nexit 0\n", "sleep 10\nexit 0\n", reverse);
        cuckatoo::Cycle cycle{}; uint32_t nonce{0};
        BOOST_CHECK(!Solve(pre, 9, 197, cycle, nonce));
        auto launches = Launches(argv);
        std::vector<std::pair<uint32_t, uint32_t>> ranges;
        for (const auto& launch : launches) ranges.emplace_back(launch.start, launch.attempts);
        std::sort(ranges.begin(), ranges.end());
        return ranges;
    };
    BOOST_CHECK(run(false) == run(true));
}

BOOST_AUTO_TEST_CASE(stats_table_tracks_progress_window)
{
    Fixture f;
    const std::vector<unsigned char> pre(64, 0xCD);
    SetEnv("CUCKATOO_GPU_DEVICES", DEVICE_A);
    // The mean used to be two stub sleeps measured on the real clock, bounded
    // 0.005 < mean < 0.2 because that is all a measurement allows. With the
    // clock injected the gaps are the ones this case chose, so the mean is
    // exact (F-532, R3). One card, so the ticks are consumed in order: the
    // worker's kSolving read, then one read per progress heartbeat.
    struct ClockRestore {
        ~ClockRestore() { cuckatoo::SetGpuStatsClockForTesting({}); }
    } restore;
    constexpr std::chrono::milliseconds GAP{100};
    std::atomic<int> tick{0};
    cuckatoo::SetGpuStatsClockForTesting([&] {
        return std::chrono::steady_clock::time_point{} + GAP * tick.fetch_add(1);
    });
    f.Configure("echo progress=1\necho progress=2\nexit 0\n", "exit 5\n");
    cuckatoo::Cycle cycle{}; uint32_t nonce{0};
    BOOST_CHECK(!Solve(pre, 0, 64, cycle, nonce));
    const auto stats = cuckatoo::GetGpuDeviceStats();
    const auto row = std::find_if(stats.begin(), stats.end(), [](const auto& value) { return value.device == DEVICE_A; });
    BOOST_REQUIRE(row != stats.end());
    BOOST_CHECK_EQUAL(tick.load(), 3);
    BOOST_CHECK_EQUAL(row->attempts, 2U);
    BOOST_CHECK_EQUAL(row->mean_seconds_per_graph, std::chrono::duration<double>(GAP).count());
}

BOOST_FIXTURE_TEST_CASE(block_mining_passes_the_target_as_the_accept_predicate, TestChain100Setup)
{
    Consensus::Params& params = const_cast<Consensus::Params&>(Params().GetConsensus());
    struct ParamsRestorer {
        Consensus::Params& params;
        bool no_cycle;
        uint8_t edge_bits;
        ~ParamsRestorer() { params.fBlockPowNoCycle = no_cycle; params.nEdgeBits = edge_bits; }
    } restore{params, params.fBlockPowNoCycle, params.nEdgeBits};
    params.fBlockPowNoCycle = false;
    params.nEdgeBits = 19;

    CBlock block;
    const CBlockIndex* tip = WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip());
    BOOST_REQUIRE(tip);
    block.nVersion = 1;
    block.hashPrevBlock = tip->GetBlockHash();
    block.hashMerkleRoot = BlockMerkleRoot(block);
    block.nTime = tip->GetMedianTimePast() + 1;
    block.nBits = GetNextWorkRequired(tip, &block, params);
    block.nCongestion = CONGESTION_ONE;

    UnsetEnv("CUCKATOO_GPU_SOLVER");
    cuckatoo::ResetGpuDiscoveryForTesting();
    const auto pre = block.PrePowBytes();
    std::optional<Proof> rejected;
    std::optional<Proof> accepted;
    uint32_t start{0};
    while (!rejected || !accepted) {
        Proof proof{};
        BOOST_REQUIRE(cuckatoo::CuckatooSolve(pre, 19, start, 1U << 20,
                                              proof.cycle, proof.nonce,
                                              /*cpu_fallback=*/true));
        CBlock candidate{block};
        candidate.nNonce = proof.nonce;
        candidate.nCycle = proof.cycle;
        if (CheckProofOfWork(candidate, params)) accepted = proof;
        else rejected = proof;
        BOOST_REQUIRE(proof.nonce != std::numeric_limits<uint32_t>::max());
        start = proof.nonce + 1;
    }

    Fixture f;
    // Both cards know both proofs and report each only inside their own range,
    // so the result doesn't depend on which card claims which block.
    BOOST_TEST_MESSAGE("rejected " << rejected->nonce << " accepted " << accepted->nonce);
    const std::string script = FoundScript({*rejected, *accepted}, 20);
    const std::string argv = f.Configure(script, script);
    uint64_t max_tries{4096};
    BOOST_REQUIRE(node::SolveBlockPoW(*m_node.chainman, block, max_tries,
                                      /*cpu_fallback=*/false));
    BOOST_CHECK_EQUAL(block.nNonce, accepted->nonce);
    BOOST_CHECK(block.nCycle == accepted->cycle);
    const auto launches = Launches(argv);
    BOOST_CHECK(std::any_of(launches.begin(), launches.end(), [](const auto& launch) {
        return launch.device == DEVICE_B;
    }));
}

BOOST_FIXTURE_TEST_CASE(vault_transfer_passes_the_target_as_the_accept_predicate, TestChain100Setup)
{
    Consensus::Params& params = const_cast<Consensus::Params&>(Params().GetConsensus());
    struct ParamsRestorer {
        Consensus::Params& params;
        bool no_cycle;
        uint8_t edge_bits;
        ~ParamsRestorer() { params.fTxPowNoCycle = no_cycle; params.nTxEdgeBits = edge_bits; }
    } restore{params, params.fTxPowNoCycle, params.nTxEdgeBits};
    params.fTxPowNoCycle = false;
    params.nTxEdgeBits = 19;

    CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    auto vault = vault::CreateSyncedVault(*m_node.chain,
        WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain()), coinbaseKey);
    CMutableTransaction transaction;
    transaction.vin.emplace_back(COutPoint{Txid::FromUint256(ArithToUint256(1)), 0});
    transaction.vout.emplace_back(COIN, CScript{});
    const auto anchor_height = vault::TxPowAnchorHeight(*vault);
    BOOST_REQUIRE(anchor_height);
    transaction.nAnchorHeight = static_cast<uint32_t>(*anchor_height);
    const uint64_t charged_bytes{uint64_t{params.nTxWorkRefBytes} * 2};
    const uint256 target = vault->chain().txPowTarget(*anchor_height, charged_bytes,
                                                       transaction.vout.size(), transaction.vin.size());
    const auto pre = CTransaction(transaction).PowPreimage(vault->chain().getBlockHash(*anchor_height));

    UnsetEnv("CUCKATOO_GPU_SOLVER");
    cuckatoo::ResetGpuDiscoveryForTesting();
    std::optional<Proof> rejected;
    std::optional<Proof> accepted;
    uint32_t start{0};
    while (!rejected || !accepted) {
        Proof proof{};
        BOOST_REQUIRE(cuckatoo::CuckatooSolveBytes(pre.data(), pre.size(), 19, start,
                                                   1U << 20, proof.cycle, proof.nonce,
                                                   /*cpu_fallback=*/true));
        if (UintToArith256(cuckatoo::CuckatooProofHash(proof.cycle)) <= UintToArith256(target)) accepted = proof;
        else rejected = proof;
        BOOST_REQUIRE(proof.nonce != std::numeric_limits<uint32_t>::max());
        start = proof.nonce + 1;
    }

    Fixture f;
    // Both cards know both proofs and report each only inside their own range,
    // so the result doesn't depend on which card claims which block.
    BOOST_TEST_MESSAGE("rejected " << rejected->nonce << " accepted " << accepted->nonce);
    const std::string script = FoundScript({*rejected, *accepted}, 20);
    const std::string argv = f.Configure(script, script);
    const auto error = vault::GrindTransactionPow(*vault, transaction, charged_bytes, *anchor_height);
    BOOST_REQUIRE(!error);
    BOOST_CHECK_EQUAL(transaction.nPowNonce, accepted->nonce);
    BOOST_CHECK(transaction.nCycle == accepted->cycle);
    const auto launches = Launches(argv);
    BOOST_CHECK(std::any_of(launches.begin(), launches.end(), [](const auto& launch) {
        return launch.device == DEVICE_B;
    }));
}

BOOST_AUTO_TEST_SUITE_END()
