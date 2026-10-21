// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Tests for the contained-child unit: the Windows command-line quoting (which
// is pure string manipulation and therefore runs on every platform) and the
// containment contract itself, driven against the compiled solver stub.
#include <util/process/contained_child.h>

#include <util/fs.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifndef WIN32
#include <stdlib.h>   // setenv
#endif

BOOST_AUTO_TEST_SUITE(contained_child_tests)

// The Windows branch cannot pass an argv vector: CreateProcess takes one string
// and the child's runtime splits it again with the CommandLineToArgvW rules. So
// the quoting IS the argv on that platform, and getting it wrong reintroduces
// exactly what gpu_solver_tests' the_solver_path_is_never_interpreted_by_a_shell
// forbids.
//
// Defined and tested on every platform on purpose. Left inside the WIN32 branch,
// the only gate that ever ran these rules would be the one platform this project
// cannot run in CI -- which is how the Windows bridge came to ship a deadlock.
BOOST_AUTO_TEST_CASE(windows_command_line_quotes_what_the_child_must_unquote)
{
    using util::WindowsCommandLine;

    // Nothing to quote: no quotes added, because a quoted path is not what an
    // operator sees in their own logs.
    BOOST_CHECK_EQUAL(WindowsCommandLine({"qsgpusolve-opencl.exe", "28"}), "qsgpusolve-opencl.exe 28");

    // The ordinary Windows case, and the reason `cmd.exe /S /C` needed its own
    // comment before: a program path with a space in it.
    BOOST_CHECK_EQUAL(WindowsCommandLine({"C:\\Program Files\\qs\\qsgpusolve-opencl.exe", "28"}),
                      "\"C:\\Program Files\\qs\\qsgpusolve-opencl.exe\" 28");

    // A trailing backslash inside a quoted argument must be doubled, or it
    // escapes the closing quote and swallows the argument that follows.
    BOOST_CHECK_EQUAL(WindowsCommandLine({"C:\\qs dir\\", "28"}), "\"C:\\qs dir\\\\\" 28");

    // An embedded quote forces quoting even with no space present.
    BOOST_CHECK_EQUAL(WindowsCommandLine({"a\"b"}), "\"a\\\"b\"");

    // An empty argument would vanish entirely if it were not quoted.
    BOOST_CHECK_EQUAL(WindowsCommandLine({"solver", "", "28"}), "solver \"\" 28");

    // The path from that sibling case. cmd would cut it at the `&`; the runtime
    // splitter has no opinion about `&` at all, so quoting the space is enough.
    BOOST_CHECK_EQUAL(WindowsCommandLine({"qs stub & echo nonce=99", "28"}),
                      "\"qs stub & echo nonce=99\" 28");
}

namespace {
// Text-mode stdout expands the stub's newline on Windows.
#ifdef WIN32
constexpr const char* EXPECTED_READY_LINE{"ready\r\n"};
#else
constexpr const char* EXPECTED_READY_LINE{"ready\n"};
#endif

// The stub reads its directives from a file named by QS_SOLVER_STUB_SCRIPT --
// see src/test/solverstub.cpp. It is a general-purpose child here: this unit
// does not know what a solver is, and the stub is simply the compiled program
// the tree already has for driving a real child process on both platforms.
void set_env(const char* name, const std::string& value)
{
#ifdef WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void unset_env(const char* name)
{
#ifdef WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

struct EnvUnsetGuard {
    const char* name;
    ~EnvUnsetGuard() { unset_env(name); }
};

std::string tmp_path(const std::string& name)
{
#ifdef WIN32
    return name;
#else
    return "/tmp/" + name;
#endif
}

std::string write_script(const std::string& name, const std::string& body)
{
    const std::string path = tmp_path(name + ".stub");
    std::ofstream f(path);
    f << body;
    f.close();
    set_env("QS_SOLVER_STUB_SCRIPT", path);
    return SOLVER_STUB_PATH;
}

std::string write_directive_file(const std::string& name, const std::string& body)
{
    const std::string path = tmp_path(name + ".stub");
    std::ofstream f(path);
    f << body;
    return path;
}

//! Size of the marker file the stub appends to, or 0 if it does not exist. A
//! size that stops advancing is the observable proof that a process is gone;
//! its ABSENCE proves nothing, because a parent returning early looks identical
//! whether the child died or was orphaned.
uintmax_t marker_size(const std::string& path)
{
    std::error_code ec;
    const auto n = fs::file_size(fs::PathFromString(path), ec);
    return ec ? 0 : n;
}

#ifdef __linux__
std::string read_all(util::ContainedChild& child, int timeout_ms)
{
    std::string out;
    char buf[256];
    for (;;) {
        size_t got = 0;
        const auto status = child.ReadStdout(buf, sizeof(buf), timeout_ms, got);
        if (status == util::ContainedChild::ReadStatus::kClosed) return out;
        BOOST_REQUIRE(status == util::ContainedChild::ReadStatus::kData);
        out.append(buf, got);
    }
}

std::set<std::string> reported_nonstandard_pipes()
{
    // Do not use a shell pipeline here: it would add pipes of its own. Fds 0,
    // 1, and 2 belong to this child; everything above them was inherited.
    const char* command =
        "for fd in /proc/self/fd/*; do "
        "n=${fd##*/}; [ \"$n\" -le 2 ] && continue; "
        "target=$(readlink \"$fd\") || continue; "
        "case \"$target\" in pipe:\\[*\\]) printf '%s\\n' \"$target\";; esac; "
        "done";
    auto child = util::ContainedChild::Spawn({"/bin/sh", "-c", command}, util::ChildStdout::kPipe);
    BOOST_REQUIRE(child);
    const std::string output = read_all(*child, 5000);
    BOOST_REQUIRE_EQUAL(child->Wait(), 0);

    std::set<std::string> pipes;
    std::istringstream lines(output);
    for (std::string line; std::getline(lines, line);) pipes.insert(std::move(line));
    return pipes;
}


void check_concurrent_child_does_not_inherit_an_existing_childs_pipe()
{
    const auto baseline = reported_nonstandard_pipes();
    auto first = util::ContainedChild::Spawn({"/bin/sh", "-c", "sleep 3"}, util::ChildStdout::kPipe);
    BOOST_REQUIRE(first);
    const auto alongside = reported_nonstandard_pipes();
    BOOST_CHECK_EQUAL_COLLECTIONS(alongside.begin(), alongside.end(), baseline.begin(), baseline.end());
}
#else
void check_concurrent_child_does_not_inherit_an_existing_childs_pipe()
{
    BOOST_TEST_MESSAGE("skipped: descriptor inventory requires Linux /proc/self/fd");
}
#endif
} // namespace

BOOST_AUTO_TEST_CASE(spawn_reads_stdout_and_reports_the_exit_code)
{
    const std::string program = write_script("cc_echo", "echo hello\nexit 7\n");
    auto child = util::ContainedChild::Spawn({program}, util::ChildStdout::kPipe);
    BOOST_REQUIRE(child);

    std::string acc;
    char buf[256];
    for (;;) {
        size_t got = 0;
        const auto st = child->ReadStdout(buf, sizeof(buf), 5000, got);
        if (st == util::ContainedChild::ReadStatus::kClosed) break;
        BOOST_REQUIRE(st == util::ContainedChild::ReadStatus::kData);
        acc.append(buf, got);
    }
    BOOST_CHECK(acc.find("hello") != std::string::npos);
    BOOST_CHECK_EQUAL(child->Wait(), 7);
}

BOOST_AUTO_TEST_CASE(a_discarded_stdout_is_never_a_pipe_the_parent_must_drain)
{
    // The failure this rules out: a long-lived child whose output nobody reads
    // fills the pipe buffer, blocks in write(), and can never exit -- the exact
    // deadlock the solver bridge shipped, with the GPU pinned on abandoned work.
    // A kDiscard child writes to the null device, so it runs to completion with
    // the parent reading nothing at all. 200 lines is well past any pipe buffer.
    std::string body;
    for (int i = 0; i < 200; ++i) body += "echo aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n";
    body += "exit 0\n";
    const std::string program = write_script("cc_discard", body);

    auto child = util::ContainedChild::Spawn({program}, util::ChildStdout::kDiscard);
    BOOST_REQUIRE(child);
    size_t got = 0;
    char buf[16];
    BOOST_CHECK(child->ReadStdout(buf, sizeof(buf), 0, got) == util::ContainedChild::ReadStatus::kClosed);
    BOOST_CHECK_EQUAL(child->Wait(), 0);
}

BOOST_AUTO_TEST_CASE(exited_is_false_while_running_and_true_once_gone)
{
    const std::string program = write_script("cc_exited", "sleep 2000\nexit 3\n");
    auto child = util::ContainedChild::Spawn({program}, util::ChildStdout::kDiscard);
    BOOST_REQUIRE(child);
    BOOST_CHECK(!child->Exited());
    BOOST_CHECK_EQUAL(child->Wait(), 3);
    BOOST_CHECK(child->Exited());
    BOOST_CHECK_EQUAL(child->Wait(), 3);   // idempotent, and still the same answer
}

BOOST_AUTO_TEST_CASE(destroying_the_handle_kills_a_child_that_never_exits)
{
    // The supervisor's promise: nothing survives the handle. Asserted by RATE,
    // not by absence -- the marker file must stop growing.
    const std::string marker = tmp_path("cc_orphan.marker");
    std::remove(marker.c_str());
    const std::string program = write_script(
        "cc_orphan", "append " + marker + "\nsleep 50\nloop\n");
    {
        auto child = util::ContainedChild::Spawn({program}, util::ChildStdout::kDiscard);
        BOOST_REQUIRE(child);
        // Let it prove it is alive before the handle is destroyed. A case that
        // killed a child which had not started yet would pass for the wrong
        // reason, every time.
        for (int i = 0; i < 100 && marker_size(marker) == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        BOOST_REQUIRE_MESSAGE(marker_size(marker) > 0, "the stub never started");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const uintmax_t settled = marker_size(marker);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    BOOST_CHECK_MESSAGE(marker_size(marker) == settled,
                        "the marker is still growing: the child outlived its handle");
    std::remove(marker.c_str());
}

BOOST_AUTO_TEST_CASE(a_concurrent_child_does_not_inherit_an_existing_childs_pipe)
{
    check_concurrent_child_does_not_inherit_an_existing_childs_pipe();
}

BOOST_AUTO_TEST_CASE(a_short_child_reports_eof_while_a_concurrent_child_sleeps)
{
    const std::string short_script = write_directive_file("cc_concurrent_short", "echo ready\nexit 0\n");
    const std::string sleep_script = write_directive_file("cc_concurrent_sleep", "sleep 3000\nexit 0\n");
    const std::string map = write_directive_file(
        "cc_concurrent_map", "short\t" + short_script + "\nsleep\t" + sleep_script + "\n");
    set_env("QS_SOLVER_STUB_DEVICE_SCRIPTS", map);
    const EnvUnsetGuard map_guard{"QS_SOLVER_STUB_DEVICE_SCRIPTS"};

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::unique_ptr<util::ContainedChild> short_child;
    std::unique_ptr<util::ContainedChild> sleeping_child;

    auto spawn_when_released = [&](std::unique_ptr<util::ContainedChild>& destination,
                                   std::vector<std::string> argv) {
        ready.fetch_add(1);
        while (!go.load()) std::this_thread::yield();
        destination = util::ContainedChild::Spawn(argv, util::ChildStdout::kPipe);
    };
    std::thread short_thread(spawn_when_released, std::ref(short_child),
                             std::vector<std::string>{SOLVER_STUB_PATH, "--device", "short"});
    std::thread sleeping_thread(spawn_when_released, std::ref(sleeping_child),
                                std::vector<std::string>{SOLVER_STUB_PATH, "--device", "sleep"});
    while (ready.load() != 2) std::this_thread::yield();
    go.store(true);
    short_thread.join();
    sleeping_thread.join();
    BOOST_REQUIRE(short_child);
    BOOST_REQUIRE(sleeping_child);

    char buf[32];
    size_t got = 0;
    BOOST_REQUIRE(short_child->ReadStdout(buf, sizeof(buf), 1000, got) ==
                  util::ContainedChild::ReadStatus::kData);
    BOOST_CHECK_EQUAL(std::string(buf, got), EXPECTED_READY_LINE);
    const auto started = std::chrono::steady_clock::now();
    BOOST_CHECK(short_child->ReadStdout(buf, sizeof(buf), 750, got) ==
                util::ContainedChild::ReadStatus::kClosed);
    BOOST_CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
}

BOOST_AUTO_TEST_SUITE_END()
