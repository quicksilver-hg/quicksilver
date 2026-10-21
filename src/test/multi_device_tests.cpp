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
#include <process.h>
#define getpid _getpid
#else
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
    const std::string argv = f.Configure("exit 0\n", "sleep 100\nexit 0\n");
    const std::vector<unsigned char> pre(64, 0xCD);
    cuckatoo::Cycle cycle{}; uint32_t nonce{0};
    BOOST_CHECK(!Solve(pre, 0, 640, cycle, nonce));
    size_t a{0}, b{0};
    for (const auto& launch : Launches(argv)) (launch.device == DEVICE_A ? a : b)++;
    BOOST_CHECK_GT(a, b);
}

BOOST_AUTO_TEST_CASE(accepted_proof_cancels_the_other_card)
{
    Fixture f;
    const std::vector<unsigned char> pre(64, 0xCD);
    const auto proofs = BlockProofs(pre, 2);
    const std::string marker = f.File("qs-multi-accepted-marker", "");
    f.Configure(FoundScript(proofs, 300), "append " + marker + "\nsleep 50\nloop\n");
    cuckatoo::Cycle cycle{}; uint32_t nonce{0};
    const auto before = std::chrono::steady_clock::now();
    BOOST_REQUIRE(Solve(pre, 0, 1U << 20, cycle, nonce));
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - before).count();
    BOOST_CHECK_LT(elapsed, 1.0);
    const auto size_before = std::ifstream(marker, std::ios::ate).tellg();
    std::this_thread::sleep_for(std::chrono::milliseconds{400});
    BOOST_CHECK_EQUAL(std::ifstream(marker, std::ios::ate).tellg(), size_before);
}

BOOST_AUTO_TEST_CASE(rejected_cycle_does_not_cancel_the_other_card)
{
    Fixture f;
    const std::vector<unsigned char> pre(64, 0xCD);
    const auto proofs = BlockProofs(pre, 2, 2);
    const std::vector<Proof> rejected{proofs[0], proofs[2]};
    const std::vector<Proof> accepted{proofs[1], proofs[3]};
    const std::string argv = f.Configure(FoundScript(rejected), FoundScript(accepted, 200));
    cuckatoo::Cycle cycle{}; uint32_t nonce{0};
    BOOST_REQUIRE(Solve(pre, 0, 1U << 20, cycle, nonce,
                        [&](const cuckatoo::Cycle&, uint32_t candidate) {
                            return candidate == accepted[0].nonce || candidate == accepted[1].nonce;
                        }));
    BOOST_CHECK_EQUAL(nonce, FirstLaunchProof(argv, DEVICE_B, accepted).nonce);
}

BOOST_AUTO_TEST_CASE(bad_proof_benches_that_card_and_the_other_continues)
{
    Fixture f;
    const std::vector<unsigned char> pre(64, 0xCD);
    const auto proofs = BlockProofs(pre, 2);
    cuckatoo::Cycle bad{};
    for (size_t i{0}; i < bad.size(); ++i) bad[i] = 1000 + i;
    const std::string argv = f.Configure("echo nonce=7\necho " + CycleLine(bad) + "\nexit 0\n", FoundScript(proofs, 200));
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
    const std::string argv = f.Configure("exit 5\n", FoundScript(proofs, 200));
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
    BOOST_CHECK(!Solve(pre, 0, 1000, cycle, nonce, {}, {}, [&] {
        return std::chrono::steady_clock::now() - started > std::chrono::milliseconds{300};
    }));
    BOOST_CHECK_LT(std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(), 1.0);
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
    f.Configure("sleep 20\necho progress=1\nsleep 30\necho progress=2\nexit 0\n", "exit 5\n");
    cuckatoo::Cycle cycle{}; uint32_t nonce{0};
    BOOST_CHECK(!Solve(pre, 0, 64, cycle, nonce));
    const auto stats = cuckatoo::GetGpuDeviceStats();
    const auto row = std::find_if(stats.begin(), stats.end(), [](const auto& value) { return value.device == DEVICE_A; });
    BOOST_REQUIRE(row != stats.end());
    BOOST_CHECK_EQUAL(row->attempts, 2U);
    BOOST_CHECK_GT(row->mean_seconds_per_graph, 0.005);
    BOOST_CHECK_LT(row->mean_seconds_per_graph, 0.2);
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
