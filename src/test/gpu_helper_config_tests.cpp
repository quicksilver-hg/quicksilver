// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <common/args.h>
#include <crypto/cuckatoo/gpu_solver.h>
#include <gpu/helper_config.h>
#include <tinyformat.h>
#include <util/fs.h>

#include <boost/test/unit_test.hpp>

#include <cstdlib>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace {
std::string Env(const char* n)
{
    const char* v = std::getenv(n);
    return v ? v : "";
}
void Unset(const char* n)
{
#ifdef WIN32
    _putenv_s(n, "");
#else
    unsetenv(n);
#endif
}
void SetEnv(const char* n, const std::string& value)
{
#ifdef WIN32
    _putenv_s(n, value.c_str());
#else
    setenv(n, value.c_str(), 1);
#endif
}
struct EnvGuard {
    std::vector<std::pair<const char*, std::optional<std::string>>> saved;
    EnvGuard()
    {
        for (const char* n : {"CUCKATOO_GPU_SOLVER", "CUCKATOO_GPU_BUNDLED", "CUCKATOO_GPU_DEVICES", "CUCKATOO_GPU_TIMEOUT"}) {
            const char* value = std::getenv(n);
            saved.emplace_back(n, value ? std::optional<std::string>{value} : std::nullopt);
            Unset(n);
        }
    }
    ~EnvGuard()
    {
        for (const auto& [name, value] : saved) {
            if (value) SetEnv(name, *value);
            else Unset(name);
        }
    }
};
//! An ArgsManager holding exactly `argv` (argv[0] is a dummy program name).
void Parse(ArgsManager& args, std::vector<const char*> argv)
{
    gpu::AddGpuHelperArgs(args);
    std::string error;
    argv.insert(argv.begin(), "quicksilver-test");
    BOOST_REQUIRE_MESSAGE(args.ParseParameters(static_cast<int>(argv.size()), argv.data(), error), error);
}
} // namespace

BOOST_AUTO_TEST_SUITE(gpu_helper_config_tests)

BOOST_AUTO_TEST_CASE(environment_guard_restores_value_and_absence)
{
    constexpr const char* name{"CUCKATOO_GPU_TIMEOUT"};
    const char* original = std::getenv(name);
    const std::optional<std::string> saved = original ? std::optional<std::string>{original} : std::nullopt;
    SetEnv(name, "inherited-timeout");
    {
        EnvGuard guard;
        BOOST_CHECK(std::getenv(name) == nullptr);
        SetEnv(name, "changed-timeout");
    }
    BOOST_CHECK_EQUAL(Env(name), "inherited-timeout");
    Unset(name);
    {
        EnvGuard guard;
        SetEnv(name, "changed-timeout");
    }
    BOOST_CHECK(std::getenv(name) == nullptr);
    if (saved) SetEnv(name, *saved);
    else Unset(name);
}

BOOST_AUTO_TEST_CASE(bundled_helper_search_order)
{
    const fs::path bin = fs::u8path("/opt/qs/bin");
    const fs::path libexec = fs::u8path("/opt/qs/bin/../libexec/quicksilver/qsgpusolve-opencl");
    const fs::path beside = fs::u8path("/opt/qs/bin/qsgpusolve-opencl");
    std::set<fs::path> present{libexec, beside};
    const auto usable = [&](const fs::path& p) { return present.count(p) > 0; };
    BOOST_CHECK(*gpu::FindBundledHelper(bin, "qsgpusolve-opencl", usable) == libexec);
    present.erase(libexec);
    BOOST_CHECK(*gpu::FindBundledHelper(bin, "qsgpusolve-opencl", usable) == beside);
    present.clear();
    BOOST_CHECK(!gpu::FindBundledHelper(bin, "qsgpusolve-opencl", usable));
    BOOST_CHECK(!gpu::FindBundledHelper(fs::path{}, "qsgpusolve-opencl", [](const fs::path&) { return true; }));
}

BOOST_AUTO_TEST_CASE(automatic_with_bundled_helper)
{
    EnvGuard g;
    ArgsManager args;
    Parse(args, {});
    BOOST_REQUIRE(gpu::ApplyGpuHelperArgs(args, fs::u8path("/usr/libexec/quicksilver/qsgpusolve-opencl")));
    BOOST_CHECK_EQUAL(Env("CUCKATOO_GPU_SOLVER"), "");
    BOOST_CHECK_EQUAL(Env("CUCKATOO_GPU_BUNDLED"), "/usr/libexec/quicksilver/qsgpusolve-opencl");
    BOOST_CHECK_EQUAL(Env("CUCKATOO_GPU_DEVICES"), "");
}

BOOST_AUTO_TEST_CASE(override_devices_and_timeout_reach_the_environment)
{
    EnvGuard g;
    ArgsManager args;
    Parse(args, {"-cuckatoosolver= /opt/mine/my-helper ", "-cuckatoodevice=pci:0000:04:00.0",
                 "-cuckatoodevice=pci:0000:01:00.0", "-cuckatoosolvertimeout=45"});
    BOOST_REQUIRE(gpu::ApplyGpuHelperArgs(args, std::nullopt));
    BOOST_CHECK_EQUAL(Env("CUCKATOO_GPU_SOLVER"), "/opt/mine/my-helper");
    BOOST_CHECK_EQUAL(Env("CUCKATOO_GPU_DEVICES"), "pci:0000:04:00.0,pci:0000:01:00.0");
    BOOST_CHECK_EQUAL(Env("CUCKATOO_GPU_TIMEOUT"), "45");
}

BOOST_AUTO_TEST_CASE(cpu_cannot_combine_with_a_card_at_startup)
{
    EnvGuard g;
    ArgsManager args;
    Parse(args, {"-cuckatoodevice=cpu", "-cuckatoodevice=pci:0000:04:00.0"});
    const auto r = gpu::ApplyGpuHelperArgs(args, std::nullopt);
    BOOST_REQUIRE(!r);
    BOOST_CHECK_EQUAL(util::ErrorString(r).original, "-cuckatoodevice=cpu cannot be combined with a graphics card.");
    BOOST_CHECK_EQUAL(Env("CUCKATOO_GPU_DEVICES"), "");
}

BOOST_AUTO_TEST_CASE(cpu_only_reaches_the_environment)
{
    EnvGuard g;
    ArgsManager args;
    Parse(args, {"-cuckatoodevice=cpu"});
    BOOST_REQUIRE(gpu::ApplyGpuHelperArgs(args, std::nullopt));
    BOOST_CHECK_EQUAL(Env("CUCKATOO_GPU_DEVICES"), "cpu");
}

BOOST_AUTO_TEST_CASE(solver_timeout_is_validated)
{
    EnvGuard guard;
    for (const char* value : {"1", "30", "86400", " 45 "}) {
        ArgsManager args;
        const std::string option = std::string{"-cuckatoosolvertimeout="} + value;
        Parse(args, {option.c_str()});
        BOOST_REQUIRE(gpu::ApplyGpuHelperArgs(args, std::nullopt));
    }
    for (const char* value : {"0", "-5", "abc", "45s", "86401", "99999999999", "\t45\t"}) {
        ArgsManager args;
        const std::string option = std::string{"-cuckatoosolvertimeout="} + value;
        Parse(args, {option.c_str()});
        const std::string before = Env("CUCKATOO_GPU_TIMEOUT");
        const auto result = gpu::ApplyGpuHelperArgs(args, std::nullopt);
        BOOST_CHECK(!result);
        BOOST_CHECK_EQUAL(Env("CUCKATOO_GPU_TIMEOUT"), before);
    }
}

// F-695: the help states the default and the range, and they are the numbers the
// solver applies: the default the watchdog falls back to unset, and the ends of
// the range the option accepts.
BOOST_AUTO_TEST_CASE(solver_timeout_help_states_what_the_solver_applies)
{
    EnvGuard guard;
    ArgsManager args;
    gpu::AddGpuHelperArgs(args);
    // GetHelpMessage wraps at the terminal width; read it as one line.
    std::string help;
    for (const char c : args.GetHelpMessage()) {
        const bool space{c == ' ' || c == '\n'};
        if (!space || (!help.empty() && help.back() != ' ')) help += space ? ' ' : c;
    }
    const std::string stated{strprintf("in seconds, from %d to %d (default: %d).", cuckatoo::kSolverTimeoutMinSec,
                                       cuckatoo::kSolverTimeoutMaxSec, cuckatoo::kSolverTimeoutDefaultSec)};
    BOOST_CHECK_MESSAGE(help.find(stated) != std::string::npos, "the -cuckatoosolvertimeout help does not say: " << stated);
    BOOST_CHECK(help.find("from 1 to 86400 (default: 30)") != std::string::npos);

    // Unset, the solver uses the stated default.
    BOOST_CHECK(Env("CUCKATOO_GPU_TIMEOUT").empty());
    BOOST_CHECK_EQUAL(cuckatoo::NoProgressTimeoutMs(), cuckatoo::kSolverTimeoutDefaultSec * 1000);
    // The stated ends are accepted and reach the solver; one past either end is refused.
    for (const int seconds : {cuckatoo::kSolverTimeoutMinSec, cuckatoo::kSolverTimeoutMaxSec}) {
        ArgsManager accepted;
        const std::string option{strprintf("-cuckatoosolvertimeout=%d", seconds)};
        Parse(accepted, {option.c_str()});
        BOOST_REQUIRE(gpu::ApplyGpuHelperArgs(accepted, std::nullopt));
        BOOST_CHECK_EQUAL(cuckatoo::NoProgressTimeoutMs(), seconds * 1000);
    }
    for (const int seconds : {cuckatoo::kSolverTimeoutMinSec - 1, cuckatoo::kSolverTimeoutMaxSec + 1}) {
        ArgsManager refused;
        const std::string option{strprintf("-cuckatoosolvertimeout=%d", seconds)};
        Parse(refused, {option.c_str()});
        BOOST_CHECK(!gpu::ApplyGpuHelperArgs(refused, std::nullopt));
    }
}

BOOST_AUTO_TEST_SUITE_END()
