// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/mining_service.h>

#include <addresstype.h>
#include <chain.h>
#include <chainparams.h>
#include <common/args.h>
#include <consensus/amount.h>
#include <crypto/cuckatoo/gpu_devices.h>
#include <crypto/cuckatoo/gpu_solver.h>
#include <crypto/cuckatoo/multi_device.h>
#include <interfaces/mining.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <node/block_solve.h>
#include <node/types.h>
#include <primitives/block.h>
#include <rpc/request.h>
#include <rpc/server.h>
#include <script/script.h>
#include <test/util/logging.h>
#include <test/util/setup_common.h>
#include <test/util/txrelaypool.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/time.h>
#include <util/utf16.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef WIN32
#include <dirent.h>
#include <signal.h>
#include <unistd.h>
#else
#include <windows.h>
#include <tlhelp32.h>
#endif

using node::MiningService;
using node::MiningStatus;

namespace {
void SetSolverEnv(const std::string& value)
{
#ifdef WIN32
    _putenv_s("CUCKATOO_GPU_SOLVER", value.c_str());
#else
    setenv("CUCKATOO_GPU_SOLVER", value.c_str(), 1);
#endif
}

void UnsetSolverEnv()
{
#ifdef WIN32
    _putenv_s("CUCKATOO_GPU_SOLVER", "");
#else
    unsetenv("CUCKATOO_GPU_SOLVER");
#endif
}

//! A silent command that keeps the external-solver bridge occupied until the
//! mining service cancels it. The comment token consumes the solver arguments
//! appended by the bridge on each platform.
std::string BlockingSolverCommand()
{
#ifdef WIN32
    return "ping -n 3600 127.0.0.1 >NUL & rem";
#else
    return "sleep 3600 #";
#endif
}

void SetEnvVar(const char* name, const std::string& value)
{
#ifdef WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void UnsetEnvVar(const char* name)
{
#ifdef WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

std::string EnvOrEmpty(const char* name)
{
    const char* value{std::getenv(name)};
    return value ? value : "";
}

std::string_view TrimAscii(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    return text;
}

bool IsSolverStubPath(std::string_view path)
{
    if (path == SOLVER_STUB_PATH) return true;
    const auto slash = path.find_last_of("/\\");
    const std::string_view base = slash == std::string_view::npos ? path : path.substr(slash + 1);
    return base == "test_solver_stub" || base == "test_solver_stub.exe";
}

//! End the held solver window. The child is already inside SolveBlockPoW, so
//! killing it returns "no cycle" without the abandon path, which would rebuild
//! the template and stamp a fresh time on its own.
void KillSolverStubChildren()
{
#ifdef WIN32
    HANDLE snap{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    const DWORD self{GetCurrentProcessId()};
    if (Process32FirstW(snap, &entry)) {
        do {
            if (entry.th32ParentProcessID != self) continue;
            if (!IsSolverStubPath(util::Utf16ToUtf8(entry.szExeFile))) continue;
            HANDLE child{OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID)};
            if (!child) continue;
            static_cast<void>(TerminateProcess(child, 1));
            CloseHandle(child);
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
#elif defined(__linux__)
    DIR* dir{opendir("/proc")};
    if (!dir) return;
    const pid_t self{getpid()};
    while (const dirent* ent = readdir(dir)) {
        if (ent->d_name[0] < '1' || ent->d_name[0] > '9') continue;
        std::ifstream status{std::string{"/proc/"} + ent->d_name + "/status"};
        int32_t ppid{-1};
        std::string line;
        while (std::getline(status, line)) {
            constexpr std::string_view key{"PPid:"};
            if (line.compare(0, key.size(), key) != 0) continue;
            int32_t parsed{0};
            if (ParseInt32(TrimAscii(std::string_view{line}.substr(key.size())), &parsed)) ppid = parsed;
            break;
        }
        if (ppid != self) continue;
        char buf[4096];
        const ssize_t n{::readlink((std::string{"/proc/"} + ent->d_name + "/exe").c_str(), buf, sizeof(buf) - 1)};
        if (n < 0) continue;
        buf[n] = '\0';
        if (!IsSolverStubPath(buf)) continue;
        int32_t pid{0};
        if (!ParseInt32(ent->d_name, &pid)) continue;
        static_cast<void>(::kill(pid, SIGKILL));
    }
    closedir(dir);
#endif
}

std::string TempPath(const std::string& name)
{
#ifdef WIN32
    return name;
#else
    return "/tmp/" + name;
#endif
}

std::string ProcessIdTag()
{
#ifdef WIN32
    return util::ToString(static_cast<uint64_t>(GetCurrentProcessId()));
#else
    return util::ToString(static_cast<int64_t>(::getpid()));
#endif
}

bool WaitUntil(const std::function<bool()>& done, std::chrono::milliseconds budget)
{
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(mining_service_tests, TestChain100Setup)

BOOST_AUTO_TEST_CASE(mines_and_reports_session_subsidy)
{
    auto mining = interfaces::MakeMining(m_node);
    BOOST_REQUIRE(mining);

    const CScript payout = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    MiningService svc(*m_node.chainman, *mining);

    const int start_height = WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height());

    BOOST_CHECK(svc.Start(payout, "test-payout"));
    BOOST_CHECK(!svc.Start(payout, "test-payout"));  // idempotent: already active

    // Sandbox fBlockPowNoCycle makes block PoW trivial; wait for a few blocks.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (svc.GetStatus().blocks_found < 3 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    svc.Stop();

    const MiningStatus st = svc.GetStatus();
    BOOST_CHECK(!st.active);
    BOOST_CHECK_GE(st.blocks_found, 3);
    BOOST_CHECK_EQUAL(st.address, "test-payout");

    // Derived expectation (never hardcoded): empty blocks pay subsidy only.
    const Consensus::Params& params = m_node.chainman->GetConsensus();
    CAmount expected = 0;
    for (int h = start_height + 1; h <= start_height + st.blocks_found; ++h) {
        expected += GetBlockSubsidy(h, params);
    }
    BOOST_CHECK_EQUAL(st.coins_minted_session, expected);

    // The chain actually advanced by blocks_found.
    const int end_height = WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height());
    BOOST_CHECK_EQUAL(end_height, start_height + st.blocks_found);
}

BOOST_AUTO_TEST_CASE(inactive_status_is_empty)
{
    auto mining = interfaces::MakeMining(m_node);
    MiningService svc(*m_node.chainman, *mining);
    const MiningStatus st = svc.GetStatus();
    BOOST_CHECK(!st.active);
    BOOST_CHECK_EQUAL(st.blocks_found, 0);
    BOOST_CHECK_EQUAL(st.coins_minted_session, 0);
    BOOST_CHECK(st.address.empty());
}

BOOST_AUTO_TEST_CASE(build_mining_status_maps_fields)
{
    // Construct the service the same way the other cases in this file do; the
    // TestChain100Setup fixture does not populate m_node.mining_service.
    auto mining = interfaces::MakeMining(m_node);
    MiningService svc(*m_node.chainman, *mining);
    ChainstateManager& chainman = *m_node.chainman;

    // Idle service: inactive, zero telemetry, congestion at the hardware floor.
    interfaces::MiningStatus st = node::BuildMiningStatus(svc, chainman);
    BOOST_CHECK(!st.active);
    BOOST_CHECK_EQUAL(st.blocks_found, 0);
    BOOST_CHECK_EQUAL(st.coins_minted_session, 0);
    BOOST_CHECK_EQUAL(st.elapsed_seconds, 0);
    // Never armed, so no solver attempt has ever been observed. That is unknown,
    // not a rate of zero -- a zero would be indistinguishable from a running
    // miner whose card has died, which is the reading this field exists to give.
    BOOST_CHECK(!st.attempts_per_second.has_value());
    BOOST_CHECK_CLOSE(st.congestion_multiplier, 1.0, 0.001);
}

BOOST_AUTO_TEST_CASE(mining_status_reports_seeded_card_rates_and_faults)
{
    auto mining = interfaces::MakeMining(m_node);
    MiningService svc(*m_node.chainman, *mining);
    cuckatoo::SetGpuDeviceStatsForTesting({
        {"pci:0000:04:00.0", cuckatoo::GpuDeviceState::kSolving, cuckatoo::GpuSolveStatus::kNoCycle, 5, 2.0},
        {"pci:0000:05:00.0", cuckatoo::GpuDeviceState::kSolving, cuckatoo::GpuSolveStatus::kNoCycle, 5, 4.0},
    });
    const auto running = node::BuildMiningStatus(svc, *m_node.chainman);
    BOOST_REQUIRE_EQUAL(running.devices.size(), 2U);
    BOOST_CHECK_EQUAL(running.devices[0].id, "pci:0000:04:00.0");
    BOOST_CHECK_EQUAL(running.devices[0].state, "solving");
    BOOST_REQUIRE(running.devices[0].attempts_per_second);
    BOOST_CHECK_CLOSE(*running.devices[0].attempts_per_second, 0.5, 0.001);
    BOOST_REQUIRE(running.devices[1].attempts_per_second);
    BOOST_CHECK_CLOSE(*running.devices[1].attempts_per_second, 0.25, 0.001);
    BOOST_REQUIRE(running.attempts_per_second);
    BOOST_CHECK_CLOSE(*running.attempts_per_second, 0.75, 0.001);

    cuckatoo::SetGpuDeviceStatsForTesting({
        {"pci:0000:04:00.0", cuckatoo::GpuDeviceState::kFaulted, cuckatoo::GpuSolveStatus::kDeviceFault, 5, 2.0},
        {"pci:0000:05:00.0", cuckatoo::GpuDeviceState::kIdle, cuckatoo::GpuSolveStatus::kNoCycle, 5, 4.0},
    });
    const auto stopped = node::BuildMiningStatus(svc, *m_node.chainman);
    BOOST_CHECK_EQUAL(stopped.devices[0].state, "faulted");
    BOOST_CHECK(!stopped.devices[0].attempts_per_second);
    BOOST_CHECK(!stopped.devices[0].last_error.empty());
    BOOST_CHECK_EQUAL(stopped.devices[1].state, "idle");
    BOOST_CHECK(stopped.devices[1].last_error.empty());
    BOOST_CHECK(!stopped.attempts_per_second); // no active card rate or CPU attempt
    cuckatoo::ResetGpuDeviceStatsForTesting();
    BOOST_CHECK(node::BuildMiningStatus(svc, *m_node.chainman).devices.empty());
}

BOOST_AUTO_TEST_CASE(solver_health_follows_the_card_table_between_windows)
{
    // F-534: a card faults mid-window, long before the window returns and is
    // recorded; and a reported card fault clears once the next window has the
    // card solving again. Faults that aren't a card's stay with the window.
    auto mining = interfaces::MakeMining(m_node);
    MiningService svc(*m_node.chainman, *mining);
    const std::string card{"pci:0000:04:00.0"};
    const auto sentence = *cuckatoo::SolverFault(cuckatoo::GpuSolveStatus::kDeviceFault, false);

    cuckatoo::SetGpuDeviceStatsForTesting({
        {card, cuckatoo::GpuDeviceState::kFaulted, cuckatoo::GpuSolveStatus::kDeviceFault, 5, 2.0},
        {"pci:0000:05:00.0", cuckatoo::GpuDeviceState::kSolving, cuckatoo::GpuSolveStatus::kNoCycle, 5, 4.0},
    });
    BOOST_REQUIRE(svc.GetStatus().solver_ok); // nothing reported yet
    const auto mid_window = node::BuildMiningStatus(svc, *m_node.chainman);
    BOOST_CHECK(!mid_window.solver_ok);
    BOOST_CHECK_EQUAL(mid_window.last_solver_error, card + " (" + card + "): " + sentence);

    svc.RecordSolverOutcome(cuckatoo::GpuSolveStatus::kNoCycle, false, {{card, cuckatoo::GpuSolveStatus::kDeviceFault}});
    cuckatoo::SetGpuDeviceStatsForTesting({
        {card, cuckatoo::GpuDeviceState::kSolving, cuckatoo::GpuSolveStatus::kNoCycle, 5, 2.0},
        {"pci:0000:05:00.0", cuckatoo::GpuDeviceState::kSolving, cuckatoo::GpuSolveStatus::kNoCycle, 5, 4.0},
    });
    BOOST_REQUIRE(!svc.GetStatus().solver_ok); // the window that faulted is still the last reported
    const auto next_window = node::BuildMiningStatus(svc, *m_node.chainman);
    BOOST_CHECK(next_window.solver_ok);
    BOOST_CHECK(next_window.last_solver_error.empty());

    svc.RecordSolverOutcome(cuckatoo::GpuSolveStatus::kNoSolver, false, {});
    const auto no_solver = node::BuildMiningStatus(svc, *m_node.chainman);
    BOOST_CHECK(!no_solver.solver_ok);
    BOOST_CHECK_EQUAL(no_solver.last_solver_error, svc.GetStatus().last_solver_error);
    cuckatoo::ResetGpuDeviceStatsForTesting();
}

BOOST_AUTO_TEST_CASE(one_faulted_stub_card_makes_solver_unhealthy_until_recovery)
{
    auto mining = interfaces::MakeMining(m_node);
    MiningService svc(*m_node.chainman, *mining);
    const std::string tag = ProcessIdTag();
    const std::string listing = TempPath("ey-listing-" + tag);
    const std::string first = TempPath("ey-first-" + tag);
    const std::string second = TempPath("ey-second-" + tag);
    const std::string mapping = TempPath("ey-map-" + tag);
    const std::string launched = TempPath("ey-launched-" + tag);
    struct Restore {
        std::string solver, script, listed, mapped, devices;
        std::array<std::string, 5> files;
        ~Restore()
        {
            if (solver.empty()) UnsetSolverEnv(); else SetSolverEnv(solver);
            for (const auto& [name, value] : std::array<std::pair<const char*, std::string>, 4>{{
                {"QS_SOLVER_STUB_SCRIPT", script}, {"QS_SOLVER_STUB_LISTING", listed},
                {"QS_SOLVER_STUB_DEVICE_SCRIPTS", mapped}, {"CUCKATOO_GPU_DEVICES", devices}}}) {
                if (value.empty()) UnsetEnvVar(name); else SetEnvVar(name, value);
            }
            cuckatoo::ResetGpuDiscoveryForTesting();
            cuckatoo::ResetGpuDeviceStatsForTesting();
            for (const auto& file : files) static_cast<void>(std::remove(file.c_str()));
        }
    } restore{EnvOrEmpty("CUCKATOO_GPU_SOLVER"), EnvOrEmpty("QS_SOLVER_STUB_SCRIPT"),
              EnvOrEmpty("QS_SOLVER_STUB_LISTING"), EnvOrEmpty("QS_SOLVER_STUB_DEVICE_SCRIPTS"),
              EnvOrEmpty("CUCKATOO_GPU_DEVICES"), {listing, first, second, mapping, launched}};
    const auto write = [](const std::string& path, const std::string& body) {
        std::ofstream out{path};
        out << body;
        BOOST_REQUIRE(out.good());
    };
    write(listing,
          "echo device backend=stub id=pci:0000:04:00.0 type=gpu integrated=0 mem=0 status=ok name=First Card\n"
          "echo device backend=stub id=pci:0000:05:00.0 type=gpu integrated=0 mem=0 status=ok name=Second Card\nexit 0\n");
    // Two blocks, and Second Card holds its block until First Card has launched: First Card always gets the other
    // block. With one block, or no wait, Second Card could take all the work and First Card never fault.
    static_cast<void>(std::remove(launched.c_str()));
    write(first, "append " + launched + "\nexit 5\n");
    write(second, "await " + launched + "\necho progress=1\nexit 0\n");
    write(mapping, "pci:0000:04:00.0\t" + first + "\npci:0000:05:00.0\t" + second + "\n");
    SetSolverEnv(SOLVER_STUB_PATH);
    SetEnvVar("QS_SOLVER_STUB_SCRIPT", first);
    SetEnvVar("QS_SOLVER_STUB_LISTING", listing);
    SetEnvVar("QS_SOLVER_STUB_DEVICE_SCRIPTS", mapping);
    SetEnvVar("CUCKATOO_GPU_DEVICES", "pci:0000:04:00.0,pci:0000:05:00.0");
    cuckatoo::ResetGpuDiscoveryForTesting();

    const std::array<unsigned char, 80> pre{};
    cuckatoo::Cycle cycle{};
    uint32_t nonce{0};
    const auto run = [&] {
        return cuckatoo::MultiDeviceSolveBytes(pre.data(), pre.size(), 19, 0, 128,
                                               cycle, nonce, {}, {}, {});
    };
    const auto faulted = run();
    BOOST_REQUIRE_EQUAL(faulted.faults.size(), 1U);
    // F-534: the card is faulted in the stats table before any window is reported.
    const auto unreported = node::BuildMiningStatus(svc, *m_node.chainman);
    BOOST_CHECK(!unreported.solver_ok);
    std::string rows;
    for (const auto& device : unreported.devices) rows += " [" + device.id + " '" + device.name + "' " + device.state + " '" + device.last_error + "']";
    BOOST_CHECK_MESSAGE(unreported.last_solver_error.starts_with("First Card (pci:0000:04:00.0): "),
                        "last_solver_error '" << unreported.last_solver_error << "', fault " << faulted.faults.front().device << ", rows" << rows);
    svc.RecordSolverOutcome(faulted.status, false, faulted.faults);
    BOOST_CHECK(!svc.GetStatus().solver_ok);
    BOOST_CHECK_MESSAGE(svc.GetStatus().last_solver_error.starts_with("First Card (pci:0000:04:00.0): "),
                        "last_solver_error '" << svc.GetStatus().last_solver_error << "'");

    write(first, "echo progress=1\nexit 0\n");
    const auto recovered = run();
    BOOST_CHECK(recovered.faults.empty());
    svc.RecordSolverOutcome(recovered.status, false, recovered.faults);
    BOOST_CHECK(svc.GetStatus().solver_ok);
    BOOST_CHECK(svc.GetStatus().last_solver_error.empty());
}

BOOST_AUTO_TEST_CASE(mining_status_reports_runtime_solver_after_setting_change)
{
    auto mining = interfaces::MakeMining(m_node);
    MiningService svc(*m_node.chainman, *mining);

    std::optional<common::SettingsValue> original_setting;
    gArgs.LockSettings([&](common::Settings& settings) {
        const auto it = settings.rw_settings.find("cuckatoosolver");
        if (it != settings.rw_settings.end()) original_setting = it->second;
        settings.rw_settings["cuckatoosolver"] = "/next-start/qsgpusolve-opencl";
    });
    const char* original_solver = std::getenv("CUCKATOO_GPU_SOLVER");
    const std::optional<std::string> original_env = original_solver
        ? std::optional<std::string>{original_solver}
        : std::nullopt;
    struct RestoreSolverState {
        std::optional<common::SettingsValue> setting;
        std::optional<std::string> environment;
        ~RestoreSolverState()
        {
            gArgs.LockSettings([&](common::Settings& settings) {
                if (setting) {
                    settings.rw_settings["cuckatoosolver"] = *setting;
                } else {
                    settings.rw_settings.erase("cuckatoosolver");
                }
            });
            if (environment) {
                SetSolverEnv(*environment);
            } else {
                UnsetSolverEnv();
            }
        }
    } restore{original_setting, original_env};

    // OptionsModel writes the new path immediately but marks it restart-only.
    // Until that restart, the execution bridge remains unset. The old status
    // path read gArgs here and claimed "GPU bridge", disagreeing with the next
    // solve attempt's kNoSolver health result.
    SetSolverEnv("");
    BOOST_REQUIRE(gArgs.IsArgSet("-cuckatoosolver"));
    BOOST_CHECK(!cuckatoo::GpuHelperFor(m_node.chainman->GetConsensus().nEdgeBits).has_value());
    BOOST_CHECK(!node::BuildMiningStatus(svc, *m_node.chainman).gpu_solver);

    // The converse pins the source of truth too: status follows the bridge that
    // can run now, regardless of what is merely queued for the next startup.
    SetSolverEnv("/this-process/qsgpusolve-opencl");
    BOOST_REQUIRE(cuckatoo::GpuHelperFor(m_node.chainman->GetConsensus().nEdgeBits).has_value());
    BOOST_CHECK(node::BuildMiningStatus(svc, *m_node.chainman).gpu_solver);
}

BOOST_AUTO_TEST_CASE(block_solving_possible_rereads_allowcpumining_without_restart)
{
    // Run() calls CpuBlockFallbackEnabled once per arming. This is that read,
    // against the same gArgs a running process already holds. The fixture chain
    // is sandbox (edgebits 19), where CPU block mining is always permitted, so
    // arming it cannot show the E28 refusal. The edgebits argument is what the
    // call passes through.
    std::optional<common::SettingsValue> original_rw;
    std::optional<common::SettingsValue> original_forced;
    std::optional<std::vector<common::SettingsValue>> original_cli;
    gArgs.LockSettings([&](common::Settings& settings) {
        const auto rw = settings.rw_settings.find("allowcpumining");
        if (rw != settings.rw_settings.end()) original_rw = rw->second;
        const auto forced = settings.forced_settings.find("allowcpumining");
        if (forced != settings.forced_settings.end()) original_forced = forced->second;
        const auto cli = settings.command_line_options.find("allowcpumining");
        if (cli != settings.command_line_options.end()) original_cli = cli->second;
        settings.rw_settings["allowcpumining"] = false;
        settings.forced_settings.erase("allowcpumining");
        settings.command_line_options.erase("allowcpumining");
    });
    const char* original_solver = std::getenv("CUCKATOO_GPU_SOLVER");
    const std::optional<std::string> original_env = original_solver
        ? std::optional<std::string>{original_solver}
        : std::nullopt;
    struct Restore {
        std::optional<common::SettingsValue> rw;
        std::optional<common::SettingsValue> forced;
        std::optional<std::vector<common::SettingsValue>> cli;
        std::optional<std::string> environment;
        ~Restore()
        {
            gArgs.LockSettings([&](common::Settings& settings) {
                if (rw) settings.rw_settings["allowcpumining"] = *rw;
                else settings.rw_settings.erase("allowcpumining");
                if (forced) settings.forced_settings["allowcpumining"] = *forced;
                else settings.forced_settings.erase("allowcpumining");
                if (cli) settings.command_line_options["allowcpumining"] = *cli;
                else settings.command_line_options.erase("allowcpumining");
            });
            if (environment) SetSolverEnv(*environment);
            else UnsetSolverEnv();
        }
    } restore{original_rw, original_forced, original_cli, original_env};
    UnsetSolverEnv();

    BOOST_CHECK(!node::CpuBlockFallbackEnabled(28));
    BOOST_CHECK(!node::BlockSolvingPossible(28));
    BOOST_CHECK(node::CpuBlockFallbackEnabled(19));
    BOOST_CHECK(node::BlockSolvingPossible(19));

    gArgs.LockSettings([&](common::Settings& settings) {
        settings.rw_settings["allowcpumining"] = true;
    });
    BOOST_CHECK(node::CpuBlockFallbackEnabled(28));
    BOOST_CHECK(node::BlockSolvingPossible(28));

    // Command line beats the settings file. The desktop must not be able to
    // write a persistent true over an explicit -allowcpumining=0.
    gArgs.LockSettings([&](common::Settings& settings) {
        settings.command_line_options["allowcpumining"] = {common::SettingsValue{"0"}};
    });
    BOOST_CHECK(!node::CpuBlockFallbackEnabled(28));
    BOOST_CHECK(!node::BlockSolvingPossible(28));

    // Sandbox arming publishes the same read before the thread has solved
    // anything. edgebits 19 stays permitted with the opt-in off.
    BOOST_CHECK_EQUAL(static_cast<int>(m_node.chainman->GetConsensus().nEdgeBits), 19);
    auto mining = interfaces::MakeMining(m_node);
    MiningService svc(*m_node.chainman, *mining);
    const CScript payout = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    BOOST_REQUIRE(svc.Start(payout, "permit-read"));
    BOOST_CHECK(svc.GetStatus().block_solving_possible);
    BOOST_CHECK(node::BuildMiningStatus(svc, *m_node.chainman).block_solving_possible);
    svc.Stop();
    BOOST_CHECK(!svc.GetStatus().active);
}

//! Sandbox is edgebits 19, and CPU block mining is always permitted there, so
//! an idle BuildMiningStatus on the suite fixture cannot report false. Mainnet
//! is the chain the desktop runs, and its edgebits is 28.
struct MainnetMiningSetup : public TestingSetup {
    MainnetMiningSetup() : TestingSetup{ChainType::MAIN, {.setup_net = false}} {}
};

BOOST_FIXTURE_TEST_CASE(idle_build_mining_status_reads_block_solving_possible, MainnetMiningSetup)
{
    BOOST_CHECK_EQUAL(static_cast<int>(m_node.chainman->GetConsensus().nEdgeBits), 28);

    std::optional<common::SettingsValue> original_rw;
    std::optional<common::SettingsValue> original_forced;
    std::optional<std::vector<common::SettingsValue>> original_cli;
    gArgs.LockSettings([&](common::Settings& settings) {
        const auto rw = settings.rw_settings.find("allowcpumining");
        if (rw != settings.rw_settings.end()) original_rw = rw->second;
        const auto forced = settings.forced_settings.find("allowcpumining");
        if (forced != settings.forced_settings.end()) original_forced = forced->second;
        const auto cli = settings.command_line_options.find("allowcpumining");
        if (cli != settings.command_line_options.end()) original_cli = cli->second;
        settings.rw_settings["allowcpumining"] = false;
        settings.forced_settings.erase("allowcpumining");
        settings.command_line_options.erase("allowcpumining");
    });
    const char* original_solver = std::getenv("CUCKATOO_GPU_SOLVER");
    const std::optional<std::string> original_env = original_solver
        ? std::optional<std::string>{original_solver}
        : std::nullopt;
    struct Restore {
        std::optional<common::SettingsValue> rw;
        std::optional<common::SettingsValue> forced;
        std::optional<std::vector<common::SettingsValue>> cli;
        std::optional<std::string> environment;
        ~Restore()
        {
            gArgs.LockSettings([&](common::Settings& settings) {
                if (rw) settings.rw_settings["allowcpumining"] = *rw;
                else settings.rw_settings.erase("allowcpumining");
                if (forced) settings.forced_settings["allowcpumining"] = *forced;
                else settings.forced_settings.erase("allowcpumining");
                if (cli) settings.command_line_options["allowcpumining"] = *cli;
                else settings.command_line_options.erase("allowcpumining");
            });
            if (environment) SetSolverEnv(*environment);
            else UnsetSolverEnv();
        }
    } restore{original_rw, original_forced, original_cli, original_env};
    // The bridge this process can launch, not the path queued for next start.
    UnsetSolverEnv();
    BOOST_REQUIRE(!cuckatoo::GpuHelperFor(m_node.chainman->GetConsensus().nEdgeBits).has_value());

    auto mining = interfaces::MakeMining(m_node);
    BOOST_REQUIRE(mining);
    MiningService svc(*m_node.chainman, *mining);
    BOOST_CHECK(!svc.IsActive());

    const interfaces::MiningStatus denied = node::BuildMiningStatus(svc, *m_node.chainman);
    BOOST_CHECK(!denied.active);
    BOOST_CHECK(!denied.gpu_solver);
    BOOST_CHECK(!denied.block_solving_possible);

    // The desktop writes the opt-in into rw settings and does not restart.
    gArgs.LockSettings([&](common::Settings& settings) {
        settings.rw_settings["allowcpumining"] = true;
    });
    const interfaces::MiningStatus allowed = node::BuildMiningStatus(svc, *m_node.chainman);
    BOOST_CHECK(!allowed.active);
    BOOST_CHECK(!allowed.gpu_solver);
    BOOST_CHECK(allowed.block_solving_possible);
}

//! The window is fed explicit instants rather than a clock so every case below is
//! an exact expected value, not a tolerance around whatever the machine did.
static SteadySeconds At(int64_t s) { return SteadySeconds{std::chrono::seconds{s}}; }

BOOST_AUTO_TEST_CASE(attempt_rate_window_reports_unknown_before_any_attempt)
{
    node::AttemptRateWindow w;
    BOOST_CHECK(!w.Rate(At(0)).has_value());
    BOOST_CHECK(!w.Rate(At(1'000'000)).has_value());
}

BOOST_AUTO_TEST_CASE(attempt_rate_window_measures_the_trailing_window)
{
    node::AttemptRateWindow w;
    // One attempt per second for the full window: the rate is exactly 1/s, and
    // stays 1/s as the window slides, because every second inside it holds one.
    for (int64_t t = 0; t < node::AttemptRateWindow::WINDOW_SECONDS; ++t) w.Record(At(t));
    const int64_t last{node::AttemptRateWindow::WINDOW_SECONDS - 1};
    BOOST_CHECK_EQUAL(*w.Rate(At(last)), 1.0);

    // Two per second for the next full window: the old seconds fall out and the
    // rate is exactly 2/s, not the 1.5 a session average would still be reporting.
    for (int64_t t = node::AttemptRateWindow::WINDOW_SECONDS;
         t < 2 * node::AttemptRateWindow::WINDOW_SECONDS; ++t) {
        w.Record(At(t));
        w.Record(At(t));
    }
    BOOST_CHECK_EQUAL(*w.Rate(At(2 * node::AttemptRateWindow::WINDOW_SECONDS - 1)), 2.0);
}

BOOST_AUTO_TEST_CASE(attempt_rate_window_does_not_ramp_from_zero)
{
    // The defect being fixed: dividing by seconds-since-arming made a healthy
    // miner read far below its true rate while the arming and solver-spawn dead
    // time dominated. Here the first attempt lands at t=10 after a 10 s spawn,
    // then one every second; the reported rate must be the miner's, not the
    // session's, from the very first reading.
    node::AttemptRateWindow w;
    for (int64_t t = 10; t <= 14; ++t) w.Record(At(t));
    // Five attempts spanning seconds 10..14 inclusive: five seconds, 1.0/s. The
    // session average this replaces would have read 5/15 = 0.333 at the same
    // instant, and would still be climbing towards 1.0 minutes later.
    BOOST_CHECK_EQUAL(*w.Rate(At(14)), 1.0);
}

BOOST_AUTO_TEST_CASE(attempt_rate_window_decays_when_the_solver_goes_quiet)
{
    node::AttemptRateWindow w;
    for (int64_t t = 0; t < 60; ++t) w.Record(At(t));
    BOOST_CHECK_EQUAL(*w.Rate(At(59)), 1.0);

    // Half the window has no attempts: 60 of them now span 120 seconds.
    BOOST_CHECK_EQUAL(*w.Rate(At(119)), 0.5);

    // Every attempt has fallen out of the window. Not zero: one attempt is known
    // to have happened at t=59 and none since, so the rate is at most
    // 1/(elapsed since it) -- a bound that keeps a genuinely slow solver
    // distinguishable from a stopped one, and trends to zero either way.
    BOOST_CHECK_EQUAL(*w.Rate(At(219)), 1.0 / 161.0);
    BOOST_CHECK_EQUAL(*w.Rate(At(3659)), 1.0 / 3601.0);
    BOOST_CHECK(*w.Rate(At(3659)) > 0.0);
}

BOOST_AUTO_TEST_CASE(attempt_rate_window_survives_a_gap_longer_than_the_ring)
{
    // A solver that stops for longer than the whole window and restarts must not
    // have its old counts read as current: the buckets it lands in are the same
    // ones it used before.
    node::AttemptRateWindow w;
    for (int64_t t = 0; t < node::AttemptRateWindow::WINDOW_SECONDS; ++t) w.Record(At(t));
    const int64_t restart{10 * node::AttemptRateWindow::WINDOW_SECONDS};
    w.Record(At(restart));
    // Exactly one attempt in the trailing window. Counting the stale buckets the
    // restart landed on top of would give 121/120 instead.
    BOOST_CHECK_EQUAL(*w.Rate(At(restart)),
                      1.0 / double(node::AttemptRateWindow::WINDOW_SECONDS));
}

BOOST_AUTO_TEST_CASE(attempt_rate_window_reset_forgets_the_previous_session)
{
    node::AttemptRateWindow w;
    for (int64_t t = 0; t < 10; ++t) w.Record(At(t));
    BOOST_CHECK(w.Rate(At(9)).has_value());
    w.Reset();
    BOOST_CHECK(!w.Rate(At(9)).has_value());
    BOOST_CHECK(!w.Rate(At(1000)).has_value());
}

BOOST_AUTO_TEST_CASE(start_mining_helper_rejects_bad_address)
{
    auto mining = interfaces::MakeMining(m_node);
    MiningService svc(*m_node.chainman, *mining);
    auto res = node::StartMining(svc, "not-a-valid-address");
    BOOST_CHECK(!res);   // util::Result in error state
    BOOST_CHECK(!svc.IsActive());
}

BOOST_AUTO_TEST_CASE(start_mining_helper_starts_on_valid_address)
{
    auto mining = interfaces::MakeMining(m_node);
    MiningService svc(*m_node.chainman, *mining);
    const std::string addr = EncodeDestination(PKHash(coinbaseKey.GetPubKey()));
    auto res = node::StartMining(svc, addr);
    BOOST_CHECK(res);   // success
    BOOST_CHECK(svc.IsActive());
    svc.Stop();
}

BOOST_AUTO_TEST_CASE(wait_for_sync_predicate)
{
    ChainstateManager& chainman = *m_node.chainman;

    // At the frontier (best header == active tip): do NOT wait — this is the
    // genesis-bootstrap case (best == tip == genesis) generalized.
    BOOST_CHECK(!node::MiningShouldWaitForSync(chainman));

    // Knowing of a more-work header chain we have not connected: WAIT.
    CBlockIndex fake_best;
    CBlockIndex* saved_best;
    {
        LOCK(cs_main);
        saved_best = chainman.m_best_header;
        fake_best.nChainWork = chainman.ActiveChain().Tip()->nChainWork + 1;
        chainman.m_best_header = &fake_best;
    }
    BOOST_CHECK(node::MiningShouldWaitForSync(chainman));
    {
        LOCK(cs_main);
        chainman.m_best_header = saved_best;  // restore before fixture teardown
    }
}

BOOST_AUTO_TEST_CASE(mining_status_reports_base_tx_work_at_the_floor)
{
    // BaseTxWork = (mean block work over the MA window) / K, times the
    // congestion multiplier, floored at 1 cycle. On a fresh chain the mean
    // block work is far below K=106, so the quotient truncates to zero and the
    // floor is what survives. The harness checks the measured grind against
    // this number, so it must be exposed exactly, not approximated.
    auto mining = interfaces::MakeMining(m_node);
    BOOST_REQUIRE(mining);
    MiningService svc(*m_node.chainman, *mining);

    const interfaces::MiningStatus st =
        node::BuildMiningStatus(svc, *m_node.chainman);
    // TestChain100Setup's tip sits at the sandbox floor, so mean block work is
    // 2 cycles; 2/106 truncates to 0 and the floor is what survives. This holds
    // whatever the congestion multiplier is, since it scales a zero base.
    // The published number is the CHAIN-dependent scalar. After the Stage 2 flag day
    // there is no single "required work" at a tip -- a 400-byte payment and a 100 kB
    // bloat transaction owe different amounts -- so publishing one under that name
    // would be publishing a fiction, and it is the one number every sender reads.
    BOOST_CHECK_EQUAL(st.base_tx_work, 1.0);
}

BOOST_AUTO_TEST_CASE(solve_returns_false_when_abandoned)
{
    // A template supersedes the moment a block lands on its parent. The abandon
    // predicate is checked before any grinding, including the sandbox trivial-PoW
    // path -- otherwise a sandbox solve would succeed anyway and hide the wiring.
    auto mining = interfaces::MakeMining(m_node);
    BOOST_REQUIRE(mining);
    const CScript payout = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    auto tmpl = mining->createNewBlock(node::BlockCreateOptions{.coinbase_output_script = payout});
    BOOST_REQUIRE(tmpl);
    CBlock block = tmpl->getBlock();

    uint64_t budget = 4096;
    const bool solved = node::SolveBlockPoW(*m_node.chainman, block, budget, /*cpu_fallback=*/true,
                                            /*abandon=*/[] { return true; });
    BOOST_CHECK(!solved);
    BOOST_CHECK_EQUAL(budget, 4096u);  // returned before consuming any window
}

BOOST_AUTO_TEST_CASE(solve_proceeds_when_not_abandoned)
{
    // Guards the inverted predicate, which would silently stop all mining.
    auto mining = interfaces::MakeMining(m_node);
    BOOST_REQUIRE(mining);
    const CScript payout = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    auto tmpl = mining->createNewBlock(node::BlockCreateOptions{.coinbase_output_script = payout});
    BOOST_REQUIRE(tmpl);
    CBlock block = tmpl->getBlock();

    uint64_t budget = 4096;
    const bool solved = node::SolveBlockPoW(*m_node.chainman, block, budget, /*cpu_fallback=*/true,
                                            /*abandon=*/[] { return false; });
    BOOST_CHECK(solved);  // sandbox fBlockPowNoCycle: the trivial grind succeeds
}

BOOST_AUTO_TEST_CASE(relaypool_update_rebuilds_same_height_template)
{
    auto mining = interfaces::MakeMining(m_node);
    BOOST_REQUIRE(mining);

    const CScript payout = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    const CScript destination = GetScriptForDestination(PKHash(coinbaseKey.GetPubKey()));
    CMutableTransaction transfer = CreateValidRelayPoolTransaction(
        m_coinbase_txns[0], 0, 0, coinbaseKey, destination,
        m_coinbase_txns[0]->vout[0].nValue, /*submit=*/false);
    {
        LOCK(cs_main);
        ProveTxPowForTest(transfer, *Assert(m_node.chainman->ActiveChain().Tip()), Params().GetConsensus());
    }

    // Keep the worker on one height so the test can distinguish a relay-pool
    // rebuild from the ordinary next-block rebuild. Disable sandbox's trivial
    // block solve and hold the external solver in a cancellable command. Preserve
    // E19 so the transfer proof built above remains valid.
    Consensus::Params& params = const_cast<Consensus::Params&>(Params().GetConsensus());
    struct TestStateRestorer {
        Consensus::Params& params;
        const bool block_pow_no_cycle;
        const bool tx_pow_no_cycle;
        const std::string solver;
        ~TestStateRestorer()
        {
            params.fBlockPowNoCycle = block_pow_no_cycle;
            params.fTxPowNoCycle = tx_pow_no_cycle;
            SetSolverEnv(solver);
        }
    } restore{params, params.fBlockPowNoCycle, params.fTxPowNoCycle,
              std::getenv("CUCKATOO_GPU_SOLVER") ? std::getenv("CUCKATOO_GPU_SOLVER") : ""};
    params.fBlockPowNoCycle = false;
    params.fTxPowNoCycle = true;
    SetSolverEnv(BlockingSolverCommand());

    MiningService svc(*m_node.chainman, *mining);
    BOOST_REQUIRE(svc.Start(payout, "test-payout"));

    const auto initial_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (svc.GetStatus().template_height == 0 && std::chrono::steady_clock::now() < initial_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const MiningStatus before = svc.GetStatus();
    BOOST_REQUIRE_GT(before.template_height, 0);
    BOOST_CHECK_EQUAL(before.template_transactions, 0);
    const int chain_height = WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height());

    TestRelayPoolEntryHelper entry;
    entry.Height(chain_height).Time(Now<NodeSeconds>()).SpendsCoinbase(true);
    AddToRelayPool(*m_node.relaypool, entry.FromTx(transfer));

    const auto refresh_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (svc.GetStatus().template_transactions != 1 && std::chrono::steady_clock::now() < refresh_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const MiningStatus after = svc.GetStatus();
    svc.Stop();

    BOOST_CHECK_EQUAL(after.template_height, before.template_height);
    BOOST_CHECK_EQUAL(after.template_transactions, 1);
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height()), chain_height);
}

BOOST_AUTO_TEST_CASE(solved_block_time_follows_the_clock_not_the_template)
{
    // The template is stamped once. A window that ends with no cycle must pick
    // up a later mock time before the next solve, and the block that lands must
    // carry that later time. The waits below only watch the worker reach a
    // marker and then a block; the clock under test moves with SetMockTime.
    auto mining = interfaces::MakeMining(m_node);
    BOOST_REQUIRE(mining);

    Consensus::Params& params = const_cast<Consensus::Params&>(Params().GetConsensus());
    const std::string tag{ProcessIdTag()};
    const std::string marker{TempPath("f479-window-" + tag)};
    const std::string script{TempPath("f479-solver-" + tag + ".stub")};
    static_cast<void>(std::remove(marker.c_str()));

    const bool had_devices{std::getenv("CUCKATOO_GPU_DEVICES") != nullptr};
    const bool had_timeout{std::getenv("CUCKATOO_GPU_TIMEOUT") != nullptr};
    struct TestStateRestorer {
        Consensus::Params& params;
        const bool block_pow_no_cycle;
        const uint8_t edge_bits;
        const std::string solver;
        const std::string devices;
        const std::string timeout;
        const bool had_devices;
        const bool had_timeout;
        const std::string marker;
        const std::string script;
        ~TestStateRestorer()
        {
            params.fBlockPowNoCycle = block_pow_no_cycle;
            params.nEdgeBits = edge_bits;
            SetSolverEnv(solver);
            if (had_devices) SetEnvVar("CUCKATOO_GPU_DEVICES", devices);
            else UnsetEnvVar("CUCKATOO_GPU_DEVICES");
            if (had_timeout) SetEnvVar("CUCKATOO_GPU_TIMEOUT", timeout);
            else UnsetEnvVar("CUCKATOO_GPU_TIMEOUT");
            UnsetEnvVar("QS_SOLVER_STUB_SCRIPT");
            cuckatoo::ResetGpuDiscoveryForTesting();
            KillSolverStubChildren();
            static_cast<void>(std::remove(marker.c_str()));
            static_cast<void>(std::remove(script.c_str()));
        }
    } restore{params, params.fBlockPowNoCycle, params.nEdgeBits, EnvOrEmpty("CUCKATOO_GPU_SOLVER"),
              EnvOrEmpty("CUCKATOO_GPU_DEVICES"), EnvOrEmpty("CUCKATOO_GPU_TIMEOUT"), had_devices, had_timeout,
              marker, script};

    // Edge bits 19 always falls back to the CPU solver, which would solve the
    // header the held window already captured. 28 leaves that fallback off, so
    // the held window can end with no cycle. Restored by the destructor above.
    params.fBlockPowNoCycle = false;
    params.nEdgeBits = 28;
    {
        std::ofstream out{script};
        BOOST_REQUIRE(out);
        out << "append " << marker << "\n";
        out << "sleep 300000\n";
        out << "exit 1\n";
        BOOST_REQUIRE(out);
    }
    SetSolverEnv(SOLVER_STUB_PATH);
    SetEnvVar("QS_SOLVER_STUB_SCRIPT", script);
    UnsetEnvVar("CUCKATOO_GPU_DEVICES");
    SetEnvVar("CUCKATOO_GPU_TIMEOUT", "600");
    cuckatoo::ResetGpuDiscoveryForTesting();

    const int64_t time_built{GetTime()};
    constexpr int64_t delta{1000};
    const int start_height{WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height())};
    const uint256 parent{WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash())};

    const CScript payout = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    MiningService svc(*m_node.chainman, *mining);
    BOOST_REQUIRE(svc.Start(payout, "test-payout"));

    BOOST_REQUIRE(WaitUntil([&] { return static_cast<bool>(std::ifstream{marker}); }, std::chrono::seconds(5)));

    // The worker is inside the window that captured the template header. Move
    // the clock, then let that window end with no cycle. The next window is the
    // one that must stamp the new time and solve.
    SetMockTime(time_built + delta);
    params.fBlockPowNoCycle = true;
    KillSolverStubChildren();

    BOOST_REQUIRE(WaitUntil([&] { return svc.GetStatus().blocks_found >= 1; }, std::chrono::seconds(5)));
    svc.Stop();

    const CBlockIndex* mined{WITH_LOCK(cs_main, return m_node.chainman->ActiveChain()[start_height + 1])};
    BOOST_REQUIRE(mined);
    BOOST_REQUIRE(mined->pprev);
    BOOST_CHECK(mined->pprev->GetBlockHash() == parent);
    BOOST_CHECK_GE(mined->GetBlockTime(), time_built + delta);
}

BOOST_AUTO_TEST_CASE(rpc_generated_block_time_follows_the_clock_not_the_template)
{
    // F-483: the RPC generate path has the same shape as F-479. One template,
    // one solve call; a block found after the clock moved must not keep the
    // template's time. Hold the first window, move mock time, and let a later
    // window solve; the block the RPC returns must carry the later time.
    Consensus::Params& params = const_cast<Consensus::Params&>(Params().GetConsensus());
    const std::string tag{ProcessIdTag()};
    const std::string marker{TempPath("f483-window-" + tag)};
    const std::string script{TempPath("f483-solver-" + tag + ".stub")};
    static_cast<void>(std::remove(marker.c_str()));

    // The RPC finds the miner on the node context; this fixture leaves it unset.
    // Declared before the joiner, so it outlives the RPC thread.
    BOOST_REQUIRE(!m_node.mining);
    m_node.mining = interfaces::MakeMining(m_node);
    BOOST_REQUIRE(m_node.mining);
    struct MiningResetter {
        node::NodeContext& node;
        ~MiningResetter() { node.mining.reset(); }
    } mining_resetter{m_node};

    // Joined after the restorer below has run: once the stub script is gone and
    // the sandbox params are back, any window still running ends on its own.
    std::thread rpc_thread;
    struct ThreadJoiner {
        std::thread& thread;
        ~ThreadJoiner()
        {
            if (thread.joinable()) thread.join();
        }
    } joiner{rpc_thread};

    const bool had_devices{std::getenv("CUCKATOO_GPU_DEVICES") != nullptr};
    const bool had_timeout{std::getenv("CUCKATOO_GPU_TIMEOUT") != nullptr};
    struct TestStateRestorer {
        Consensus::Params& params;
        const bool block_pow_no_cycle;
        const uint8_t edge_bits;
        const std::string solver;
        const std::string devices;
        const std::string timeout;
        const bool had_devices;
        const bool had_timeout;
        const std::string marker;
        const std::string script;
        ~TestStateRestorer()
        {
            params.fBlockPowNoCycle = block_pow_no_cycle;
            params.nEdgeBits = edge_bits;
            SetSolverEnv(solver);
            if (had_devices) SetEnvVar("CUCKATOO_GPU_DEVICES", devices);
            else UnsetEnvVar("CUCKATOO_GPU_DEVICES");
            if (had_timeout) SetEnvVar("CUCKATOO_GPU_TIMEOUT", timeout);
            else UnsetEnvVar("CUCKATOO_GPU_TIMEOUT");
            UnsetEnvVar("QS_SOLVER_STUB_SCRIPT");
            cuckatoo::ResetGpuDiscoveryForTesting();
            KillSolverStubChildren();
            static_cast<void>(std::remove(marker.c_str()));
            static_cast<void>(std::remove(script.c_str()));
        }
    } restore{params, params.fBlockPowNoCycle, params.nEdgeBits, EnvOrEmpty("CUCKATOO_GPU_SOLVER"),
              EnvOrEmpty("CUCKATOO_GPU_DEVICES"), EnvOrEmpty("CUCKATOO_GPU_TIMEOUT"), had_devices, had_timeout,
              marker, script};

    // The RPC solves with the CPU fallback on. Edge bits 20 has no CPU solver,
    // so the held window ends with no cycle when the stub dies. Edge bits 19,
    // set before that, makes every later window fall back to the CPU solver.
    // CuckatooSolve takes edge bits by value, so the held window keeps 20.
    params.fBlockPowNoCycle = false;
    params.nEdgeBits = 20;
    {
        std::ofstream out{script};
        BOOST_REQUIRE(out);
        out << "append " << marker << "\n";
        out << "sleep 300000\n";
        out << "exit 1\n";
        BOOST_REQUIRE(out);
    }
    SetSolverEnv(SOLVER_STUB_PATH);
    SetEnvVar("QS_SOLVER_STUB_SCRIPT", script);
    UnsetEnvVar("CUCKATOO_GPU_DEVICES");
    SetEnvVar("CUCKATOO_GPU_TIMEOUT", "600");
    cuckatoo::ResetGpuDiscoveryForTesting();

    const auto marker_bytes = [&]() -> std::streamoff {
        std::ifstream in{marker, std::ios::binary | std::ios::ate};
        return in ? static_cast<std::streamoff>(in.tellg()) : 0;
    };

    const int64_t time_built{GetTime()};
    constexpr int64_t delta{1000};
    const uint256 parent{WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash())};

    const CScript payout = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    JSONRPCRequest request;
    request.context = &m_node;
    request.strMethod = "generatetodescriptor";
    request.params = UniValue{UniValue::VARR};
    request.params.push_back(1);
    request.params.push_back("raw(" + HexStr(payout) + ")");
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();

    UniValue hashes;
    std::string rpc_error;
    std::atomic<bool> rpc_done{false};
    rpc_thread = std::thread{[&] {
        try {
            hashes = tableRPC.execute(request);
        } catch (const UniValue& err) {
            rpc_error = err.write();
        } catch (const std::exception& e) {
            rpc_error = e.what();
        }
        rpc_done = true;
    }};

    // An RPC that fails before it reaches the solver must say why, not time out.
    BOOST_REQUIRE(WaitUntil([&] { return rpc_done.load() || marker_bytes() >= 1; }, std::chrono::seconds(5)));
    BOOST_REQUIRE_MESSAGE(!rpc_done.load(), "generatetodescriptor returned before the first window: " + rpc_error);

    // The RPC is inside the window that captured the template header. Move the
    // clock, arm the CPU solver for later windows, then end this one with no
    // cycle. Later stubs exit at once: a cycle the CPU finds can still miss the
    // target, so the block may land a window or two after the next one, and
    // each of those windows must stamp the new time too.
    SetMockTime(time_built + delta);
    params.nEdgeBits = 19;
    {
        std::ofstream out{script, std::ios::trunc};
        BOOST_REQUIRE(out);
        out << "append " << marker << "\n";
        out << "exit 1\n";
        BOOST_REQUIRE(out);
    }
    KillSolverStubChildren();

    BOOST_REQUIRE(WaitUntil([&] { return rpc_done.load(); }, std::chrono::seconds(60)));
    rpc_thread.join();
    BOOST_REQUIRE_GE(marker_bytes(), 2);

    BOOST_REQUIRE_MESSAGE(rpc_error.empty(), rpc_error);
    BOOST_REQUIRE(hashes.isArray());
    BOOST_REQUIRE_EQUAL(hashes.size(), 1U);
    const uint256 hash{*uint256::FromHex(hashes[0].get_str())};
    const CBlockIndex* mined{WITH_LOCK(cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(hash))};
    BOOST_REQUIRE(mined);
    BOOST_REQUIRE(mined->pprev);
    BOOST_CHECK(mined->pprev->GetBlockHash() == parent);
    BOOST_CHECK_GE(mined->GetBlockTime(), time_built + delta);
}

BOOST_AUTO_TEST_CASE(worker_thread_is_wrapped_so_an_escaping_exception_is_logged)
{
    auto mining = interfaces::MakeMining(m_node);
    BOOST_REQUIRE(mining);
    const CScript payout = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;

    // Run() executes on its own thread and calls createNewBlock(), getBlock(),
    // getTip() and SolveBlockPoW(), any of which can throw. An exception that
    // escapes a thread entry point calls std::terminate() -- on Windows that is
    // a __fastfail, which exits 0xC0000409 with no log line, no exception type
    // and no message. util::TraceThread is the project's wrapper for exactly
    // this: it logs the exception via PrintExceptionContinue before rethrowing,
    // and every other long-lived thread in the tree is spawned through it.
    //
    // Assert the wrapper is in place by the start line it emits. Reverting
    // Start() to a bare `std::thread(&MiningService::Run, ...)` removes that
    // line and fails this case. Stop() joins, so the entry point has certainly
    // run by the time it returns.
    {
        ASSERT_DEBUG_LOG("miner thread start");
        MiningService svc(*m_node.chainman, *mining);
        BOOST_REQUIRE(svc.Start(payout, "test-payout"));
        svc.Stop();
    }
}

BOOST_AUTO_TEST_CASE(concurrent_start_stop_does_not_terminate)
{
    auto mining = interfaces::MakeMining(m_node);
    BOOST_REQUIRE(mining);
    const CScript payout = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    MiningService svc(*m_node.chainman, *mining);

    // startmining and stopmining arrive on separate HTTP worker threads and
    // used to share m_thread with no mutex. Stop() sets m_active=false and
    // blocks in join(); a concurrent Start() then sees "not active" and
    // assigns to the still-joinable std::thread, which calls std::terminate.
    // The same hole: Start() used to set m_active before constructing the
    // thread, so a throwing constructor left the service permanently "active"
    // with no worker.
    //
    // This case cannot assert terminate() itself (that ends the process). It
    // asserts the lifecycle that the mutex makes possible: mixed Start/Stop
    // from several threads must complete, leave the service idle, and leave
    // it able to Start again. Reverting Start/Stop to unsynchronized
    // exchange+assign is the defect; under load that abort is how it fails.
    constexpr int kThreads = 4;
    constexpr int kIters = 25;
    std::atomic<int> starts{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kIters; ++i) {
                if (svc.Start(payout, "test-payout")) {
                    starts.fetch_add(1, std::memory_order_relaxed);
                }
                svc.Stop();
            }
        });
    }
    for (std::thread& t : threads) t.join();

    BOOST_CHECK(!svc.IsActive());
    BOOST_CHECK_GE(starts.load(), 1);
    BOOST_CHECK(svc.Start(payout, "test-payout"));
    BOOST_CHECK(svc.IsActive());
    svc.Stop();
    BOOST_CHECK(!svc.IsActive());
}

BOOST_AUTO_TEST_SUITE_END()
