// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Unit tests for the subprocess GPU-solver bridge (parser + dispatch fallback).
// Uses a scripted stub executable as the "solver" — NO GPU required, so these
// run in CI. The stub is one compiled program driven by a directive file, so the
// same case drives a real child process, identically, on both platforms.
#include <crypto/cuckatoo/cuckatoo.h>
#include <crypto/cuckatoo/gpu_solver.h>
#include <primitives/block.h>

#include <boost/test/unit_test.hpp>

#ifdef WIN32
#include <process.h>   // _getpid
#define getpid _getpid
#else
#include <sys/stat.h>  // chmod
#include <unistd.h>    // getpid
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

BOOST_AUTO_TEST_SUITE(gpu_solver_tests)

namespace {
// Portable overwrite-clear of an env var: POSIX unsetenv() is unavailable under
// MSVC, where assigning an empty value removes the variable.
void qs_test_unsetenv(const char* name) {
#ifdef WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

// --- Platform-neutral solver stubs ------------------------------------------
// The bridge's guarantees (including liveness and empty solver output) are the
// same promise on both platforms and are now tested on both. They were not:
// every case in this file used to be #ifndef WIN32, on the stated grounds that
// porting them was "Layer-2 work". The Windows bridge then deadlocked a live
// soak node on the first cancelled solve. Untested is untested.
void write_file(const std::string& path, const std::string& body) {
    std::ofstream f(path);
    f << body;
    f.close();
}

// "cycle=" in HEX — the real solver prints them with %llx, so a decimal stub
// would pass a parser that was wrong about the base.
std::string cycle_line_from(const cuckatoo::Cycle& cyc) {
    std::string line = "cycle=";
    for (int i = 0; i < cuckatoo::PROOFSIZE; ++i) {
        char hexbuf[16];
        std::snprintf(hexbuf, sizeof(hexbuf), "%x", cyc[i]);
        if (i) line += " ";
        line += hexbuf;
    }
    return line;
}

// PROOFSIZE consecutive indices starting at `first`.
std::string cycle_line(uint32_t first) {
    cuckatoo::Cycle cyc{};
    for (int i = 0; i < cuckatoo::PROOFSIZE; ++i) {
        cyc[i] = first + static_cast<unsigned>(i);
    }
    return cycle_line_from(cyc);
}

std::string nonce_line(uint32_t nonce) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "nonce=%u", nonce);
    return buf;
}

// Locale-independent int-to-string: std::to_string is locale-dependent and
// banned tree-wide (lint-locale-dependence.py), and these numbers end up in a
// script the host shell has to parse.
std::string num(int value) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", value);
    return buf;
}

// Set (or replace) an environment variable. POSIX setenv() is unavailable under
// MSVC and _putenv_s() is unavailable elsewhere, so every case in this file that
// touches the environment goes through here.
void set_env(const char* name, const std::string& value) {
#ifdef WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

// Absolute-or-cwd path for a fixture file. The stub inherits the test process's
// working directory, so a bare name on Windows resolves the same for both.
std::string tmp_path(const std::string& name) {
#ifdef WIN32
    return name;
#else
    return "/tmp/" + name;
#endif
}

// --- The two primitives every stub is built from ----------------------------
// Stub behaviour is a directive script interpreted by test_solver_stub (see
// src/test/solverstub.cpp), not a shell script. It used to be one per platform:
// a `.sh` for /bin/sh and a `.cmd` for cmd.exe, with `echo`/`sleep` spelled
// twice and a comment explaining that cmd.exe terminates echoed lines with CRLF.
// The bridge no longer runs the solver through a shell -- it spawns it with a
// real argv -- so a script fixture has no interpreter to reach it, and the
// dialect split has nothing left to express. One spelling now drives both
// platforms, which is what the cases always claimed to be testing.
std::string echo_stmt(const std::string& line) {
    return "echo " + line + "\n";
}

std::string sleep_stmt(int secs) {
    return "sleep " + num(secs * 1000) + "\n";
}

// Write `body` as the stub's directive script, arrange for the stub to read it,
// and return the stub executable's path -- which is what the bridge is pointed
// at. Every stub in this file goes through here, so the terminating `exit` and
// the QS_SOLVER_STUB_SCRIPT handoff live in exactly one place.
//
// One script is live at a time. No case here needs two, and a single variable
// keeps the fixture honest: the bridge picks the solver out of the environment,
// so the stub does too.
std::string write_script(const std::string& name, const std::string& body, int exit_code) {
    const std::string path = tmp_path(name + ".stub");
    write_file(path, body + "exit " + num(exit_code) + "\n");
    set_env("QS_SOLVER_STUB_SCRIPT", path);
    return SOLVER_STUB_PATH;
}

// A solver that prints `lines` in order, then exits with `exit_code`. That
// covers every contract the solver expresses through its stdout and its exit
// status, and those contracts are the same promise on both platforms while
// Windows does not share the code that keeps them. Its exit-code ladder (`exit_code == 4/5` in gpu_solver.cpp) and
// its PeekNamedPipe/ReadFile drain loop are a separate implementation from the
// POSIX waitpid/poll pair, and until these cases were ported no Windows case in
// this file ever asserted a *successful* solve -- they all asserted a failure,
// so the parse path had never run there at all.
std::string write_scripted_stub(const std::string& name,
                                const std::vector<std::string>& lines,
                                int exit_code)
{
    std::string body;
    for (const std::string& line : lines) body += echo_stmt(line);
    return write_script(name, body, exit_code);
}

// A solver that drips `heartbeats` progress lines a second apart -- each gap
// shorter than the no-progress window -- and then prints `tail` and exits 0.
// A stub that prints everything at once cannot exercise the watchdog's reset:
// only a gap the parent has to sit through proves that arriving output restarts
// the window rather than merely being read.
std::string write_dripping_stub(const std::string& name, int heartbeats,
                                const std::vector<std::string>& tail)
{
    std::string body;
    for (int i = 0; i < heartbeats; ++i) {
        body += echo_stmt("progress=" + num(i));
        body += sleep_stmt(1);
    }
    for (const std::string& line : tail) body += echo_stmt(line);
    return write_script(name, body, 0);
}

// A successful solver process that found no cycle: exit 0, no stdout.
std::string stub_no_solution() {
    return write_scripted_stub("qs_stub_no_solution", {}, 0);
}

// A solver that never prints and never exits. Exercises the path where the
// parent is waiting with nothing to read.
std::string stub_silent_forever() {
    return write_script("qs_stub_silent", sleep_stmt(3600), 0);
}

// A solver that prints `progress=` as fast as it can and never exits. THIS is
// the shape that deadlocked the Windows bridge, and the reason the pre-existing
// cancel test would not have caught it even if it had been ported: that stub was
// silent, so the pipe never filled. Here the child keeps writing, so a parent
// that stops reading and then waits for the child to exit will wait forever —
// the child is blocked writing into the pipe the parent abandoned.
std::string stub_chatty_forever() {
    return write_script("qs_stub_chatty", echo_stmt("progress=1") + "loop\n", 0);
}

void set_solver(const std::string& path) {
    set_env("CUCKATOO_GPU_SOLVER", path);
}

// Force one of the bridge's resource-failure paths. These fire only under real
// resource exhaustion, so a test cannot otherwise reach them -- see the seam's
// comment in gpu_solver.cpp for why they must not ship unexecuted.
void set_fault(const char* which) {
    set_env("CUCKATOO_GPU_FAULT", which);
}

// Bytes currently in `path`, or -1 if it does not exist. The marker stub below
// appends forever, so a size that stops advancing is the observable proof that
// the solver process is dead.
long marker_size(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return -1;
    return static_cast<long>(f.tellg());
}

// "Executable" is spelled with an extension on Windows and with a mode bit
// everywhere else, and a staged copy of the stub has to satisfy whichever the
// host uses before CreateProcess/execvp will start it.
#ifdef WIN32
constexpr const char* kExeSuffix = ".exe";
#else
constexpr const char* kExeSuffix = "";
#endif

// Stage a copy of the stub executable at `dest`. Returns false if it could not
// be made runnable there. chmod() rather than `chmod +x <path>`: the paths this
// is used with are chosen precisely because a shell would mangle them.
bool copy_stub_to(const std::string& dest) {
    {
        std::ifstream in(SOLVER_STUB_PATH, std::ios::binary);
        std::ofstream out(dest, std::ios::binary);
        if (!in || !out) return false;
        out << in.rdbuf();
    }
#ifndef WIN32
    if (chmod(dest.c_str(), 0755) != 0) return false;
#endif
    return true;
}

std::vector<std::string> read_lines(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

// A solver that never exits and never writes to STDOUT, but appends to `marker`
// about once a second. Silence on stdout keeps the watchdog and the parser out
// of the picture, so the only thing under test is whether cancel actually
// reaches the process. A stub that merely blocked would prove nothing: the
// parent returning early looks identical whether the child died or was orphaned.
std::string stub_marker_forever(const std::string& name, const std::string& marker) {
    return write_script(name, "append " + marker + "\n" + sleep_stmt(1) + "loop\n", 0);
}

void set_timeout_env(const char* secs) {
    set_env("CUCKATOO_GPU_TIMEOUT", secs);
}
struct EnvGuard {
    ~EnvGuard() {
        qs_test_unsetenv("CUCKATOO_GPU_SOLVER");
        qs_test_unsetenv("CUCKATOO_GPU_TIMEOUT");
        qs_test_unsetenv("CUCKATOO_GPU_FAULT");
        qs_test_unsetenv("QS_SOLVER_STUB_SCRIPT");
        qs_test_unsetenv("QS_SOLVER_STUB_ARGV");
    }
};
} // namespace

BOOST_AUTO_TEST_CASE(returns_false_when_unset)
{
    EnvGuard g;
    qs_test_unsetenv("CUCKATOO_GPU_SOLVER");
    unsigned char pre[96] = {0};
    cuckatoo::Cycle out{}; uint32_t nonce = 0;
    BOOST_CHECK(!cuckatoo::GpuSolveBytes(pre, sizeof(pre), 29, 0, 10, out, nonce));
}

BOOST_AUTO_TEST_CASE(returns_false_on_no_solution)
{
    EnvGuard g;
    set_solver(stub_no_solution());
    unsigned char pre[96] = {0};
    cuckatoo::Cycle out{}; uint32_t nonce = 0;
    BOOST_CHECK(!cuckatoo::GpuSolveBytes(pre, sizeof(pre), 29, 0, 10, out, nonce));
}

BOOST_AUTO_TEST_CASE(parses_well_formed_output)
{
    EnvGuard g;
    set_solver(write_scripted_stub("qs_stub_parses", {"nonce=42", cycle_line(1)}, 0));

    unsigned char pre[96] = {0};
    cuckatoo::Cycle out{}; uint32_t nonce = 0;
    BOOST_REQUIRE(cuckatoo::GpuSolveBytes(pre, sizeof(pre), 29, 0, 10, out, nonce));
    BOOST_CHECK_EQUAL(nonce, 42u);
    BOOST_CHECK_EQUAL(out[0], 1u);
    BOOST_CHECK_EQUAL(out[cuckatoo::PROOFSIZE - 1], (uint32_t)cuckatoo::PROOFSIZE);
}

BOOST_AUTO_TEST_CASE(streams_progress_heartbeats)
{
    EnvGuard g;
    set_solver(write_scripted_stub("qs_stub_heartbeats",
                                   {"progress=5", "progress=8", "nonce=42", cycle_line(1)}, 0));

    std::vector<uint32_t> progress;
    unsigned char pre[96] = {0};
    cuckatoo::Cycle out{}; uint32_t nonce = 0;
    BOOST_REQUIRE(cuckatoo::GpuSolveBytes(pre, sizeof(pre), 29, 0, 10, out, nonce, [&](uint32_t seen_nonce) {
        progress.push_back(seen_nonce);
    }));
    BOOST_REQUIRE_EQUAL(progress.size(), 2U);
    BOOST_CHECK_EQUAL(progress[0], 5U);
    BOOST_CHECK_EQUAL(progress[1], 8U);
    BOOST_CHECK_EQUAL(nonce, 42u);
}

// qsgpusolve exits 0 with no output both when it finds no cycle in the window and
// -- before this -- when cuInit failed. On a CUDA test node that meant a dead card looked like
// an unlucky grind for hours. Exit 4 now says "no CUDA device", and the bridge has
// to report it as something other than an exhausted window.
//
// Windows reads the exit status through its own ladder (GetExitCodeProcess, then
// `exit_code == 4/5`), and the boxes that actually take display-driver watchdog
// resets are the Windows ones -- kDeviceFault is what produces the TdrDelay
// remedy text. Testing this ladder on POSIX alone left the diagnosis untested on
// exactly the platform whose hardware fault it exists to name.
BOOST_AUTO_TEST_CASE(missing_cuda_device_is_distinguishable)
{
    EnvGuard g;
    unsigned char pre[96] = {0};
    cuckatoo::Cycle out{}; uint32_t nonce = 0;

    // Exit 4, nothing on stdout: what qsgpusolve does with no usable device.
    set_solver(write_scripted_stub("qs_stub_exit4", {}, 4));
    auto status{cuckatoo::GpuSolveStatus::kSolved};
    BOOST_CHECK(!cuckatoo::GpuSolveBytes(pre, sizeof(pre), 28, 0, 10, out, nonce, {}, {}, &status));
    BOOST_CHECK_MESSAGE(status == cuckatoo::GpuSolveStatus::kNoCudaDevice,
                        "a missing GPU must not be reported as an exhausted search window");

    // Exit 0, nothing on stdout: a genuinely empty window. Must NOT be kNoCudaDevice.
    set_solver(write_scripted_stub("qs_stub_exit0", {}, 0));
    status = cuckatoo::GpuSolveStatus::kSolved;
    BOOST_CHECK(!cuckatoo::GpuSolveBytes(pre, sizeof(pre), 28, 0, 10, out, nonce, {}, {}, &status));
    BOOST_CHECK_MESSAGE(status == cuckatoo::GpuSolveStatus::kNoCycle,
                        "an exhausted window must not be reported as a missing GPU");

    // Exit 5: the device faulted mid-solve. Distinct from 4 -- 4 is a card that
    // was never usable, 5 is one that died partway through a run, which is what
    // a display-driver watchdog reset looks like. Collapsing them would send the
    // operator to "install the driver" for a card whose driver is fine.
    set_solver(write_scripted_stub("qs_stub_exit5", {}, 5));
    status = cuckatoo::GpuSolveStatus::kSolved;
    BOOST_CHECK(!cuckatoo::GpuSolveBytes(pre, sizeof(pre), 28, 0, 10, out, nonce, {}, {}, &status));
    BOOST_CHECK_MESSAGE(status == cuckatoo::GpuSolveStatus::kDeviceFault,
                        "a mid-solve GPU fault must not be reported as an exhausted search window");

    // Any other non-zero exit is a solver error, distinct from both.
    set_solver(write_scripted_stub("qs_stub_exit1", {}, 1));
    status = cuckatoo::GpuSolveStatus::kSolved;
    BOOST_CHECK(!cuckatoo::GpuSolveBytes(pre, sizeof(pre), 28, 0, 10, out, nonce, {}, {}, &status));
    BOOST_CHECK_EQUAL(static_cast<int>(status), static_cast<int>(cuckatoo::GpuSolveStatus::kSolverError));
    BOOST_CHECK_MESSAGE(status != cuckatoo::GpuSolveStatus::kDeviceFault,
                        "a generic non-zero exit must not be mistaken for a device fault");

    // No solver configured at all is its own case, not a dead card.
    qs_test_unsetenv("CUCKATOO_GPU_SOLVER");
    status = cuckatoo::GpuSolveStatus::kSolved;
    BOOST_CHECK(!cuckatoo::GpuSolveBytes(pre, sizeof(pre), 28, 0, 10, out, nonce, {}, {}, &status));
    BOOST_CHECK_EQUAL(static_cast<int>(status), static_cast<int>(cuckatoo::GpuSolveStatus::kNoSolver));
}

// --- UNTRUSTED-GPU CONTRACT: runs on BOTH platforms ------------------------
// A solver that reports a proof the node cannot verify must not be believed --
// on either platform. The stub here only prints; what has to *compute* is the
// CPU fallback inside the test process, which is why these cases were never
// blocked on a script dialect the way the file's comment claimed.
BOOST_AUTO_TEST_CASE(dispatch_falls_back_on_bad_gpu_proof)
{
    EnvGuard g;
    // Bogus cycle: indices 1000..1041 — well-formed shape, not a real 19-cycle.
    set_solver(write_scripted_stub("qs_stub_bogus_cycle", {"nonce=7", cycle_line(1000)}, 0));

    // A small deterministic preimage; CPU edgebits-19 solver will find a real cycle.
    std::vector<unsigned char> pre(64, 0xCD);
    cuckatoo::Cycle out{}; uint32_t nonce = 0;
    auto status{cuckatoo::GpuSolveStatus::kSolved};
    bool ok = cuckatoo::CuckatooSolveBytes(pre.data(), pre.size(), 19, 0, 1u << 20, out, nonce,
                                          /*cpu_fallback=*/true, {}, {}, &status);
    BOOST_REQUIRE(ok);  // CPU fallback produced a proof

    // It must be the CPU's valid proof, not the stub's bogus one.
    pre[pre.size()-4] = (unsigned char)(nonce & 0xff);
    pre[pre.size()-3] = (unsigned char)((nonce >> 8) & 0xff);
    pre[pre.size()-2] = (unsigned char)((nonce >> 16) & 0xff);
    pre[pre.size()-1] = (unsigned char)((nonce >> 24) & 0xff);
    auto keys = cuckatoo::CuckatooSetHeader(pre.data(), (uint32_t)pre.size());
    BOOST_CHECK(cuckatoo::CuckatooVerify(out, keys, 19));
    BOOST_CHECK(out[0] != 1000u);  // not the stub's bogus cycle
    // CPU saved the solve; the GPU still produced garbage. kSolved is a healthy
    // outcome, so leaving it here is how getminingstatus.solver_ok stayed true.
    BOOST_CHECK_MESSAGE(status == cuckatoo::GpuSolveStatus::kSolverError,
                        "a rejected GPU cycle must not remain kSolved after CPU fallback, got "
                        << static_cast<int>(status));
    BOOST_CHECK(cuckatoo::SolverFault(status, /*cpu_fallback=*/true).has_value());
}

// The BLOCK-PoW route is the fixed-width std::array overload CuckatooSolve(prepow, ...) (see
// rpc/mining.cpp + test/util/mining.cpp). It carries the SAME untrusted-GPU contract as the
// per-tx CuckatooSolveBytes route tested above: a stub solver emitting a bogus proof must be
// rejected by self-verify and the CPU fallback must produce a real cycle. This mirrors the
// node's own block verify in CheckProofOfWorkImpl (PrePowBytes -> CuckatooSetHeader -> Verify).
BOOST_AUTO_TEST_CASE(block_header_dispatch_falls_back_on_bad_gpu_proof)
{
    EnvGuard g;
    // Bogus cycle: well-formed shape (1000..1041), not a real 19-cycle.
    set_solver(write_scripted_stub("qs_stub_bogus_cycle_header", {"nonce=7", cycle_line(1000)}, 0));

    // A deterministic block header; nNonce is swept by the solver (mutate_nonce).
    CBlockHeader header;
    header.nVersion = 1;
    header.nTime = 1700000000;
    header.nBits = 0x207fffff; // sandbox powLimit-ish; only the cycle matters here
    header.nNonce = 0;
    const std::array<unsigned char, cuckatoo::PREPOW_BYTES> prepow = header.PrePowBytes();

    cuckatoo::Cycle out{}; uint32_t won = 0;
    auto status{cuckatoo::GpuSolveStatus::kSolved};
    BOOST_REQUIRE(cuckatoo::CuckatooSolve(prepow, /*edgebits=*/19, /*start_nonce=*/0, 1u << 20, out, won,
                                         /*cpu_fallback=*/true, {}, {}, &status));

    // The returned proof must be the CPU's valid cycle, not the stub's bogus one, and it must
    // verify exactly as the node would (PrePowBytes with the winning nonce -> keys -> verify).
    BOOST_CHECK(out[0] != 1000u);
    header.nNonce = won;
    const std::array<unsigned char, cuckatoo::PREPOW_BYTES> verify_pre = header.PrePowBytes();
    const cuckatoo::Keys keys = cuckatoo::CuckatooSetHeader(verify_pre.data(), verify_pre.size());
    BOOST_CHECK(cuckatoo::CuckatooVerify(out, keys, 19));
    BOOST_CHECK_MESSAGE(status == cuckatoo::GpuSolveStatus::kSolverError,
                        "a rejected GPU cycle must not remain kSolved after CPU fallback, got "
                        << static_cast<int>(status));
    BOOST_CHECK(cuckatoo::SolverFault(status, /*cpu_fallback=*/true).has_value());
}

// GpuSolveBytes reports kSolved as soon as it parses a cycle. Dispatch then
// self-verifies. If that fails and cpu_fallback is false (the miner's E28
// policy), the call returns false. Leaving gpu_status as kSolved is a lie:
// SolverFault treats kSolved as healthy, so getminingstatus.solver_ok stays
// true while the card emits cycles that do not verify.
BOOST_AUTO_TEST_CASE(rejected_gpu_proof_is_not_reported_solved)
{
    EnvGuard g;
    set_solver(write_scripted_stub("qs_stub_bogus_status", {"nonce=7", cycle_line(1000)}, 0));

    {
        std::vector<unsigned char> pre(64, 0xCD);
        cuckatoo::Cycle out{}; uint32_t nonce = 0;
        auto status{cuckatoo::GpuSolveStatus::kSolved};
        BOOST_CHECK(!cuckatoo::CuckatooSolveBytes(pre.data(), pre.size(), 19, 0, 1u << 20, out, nonce,
                                                 /*cpu_fallback=*/false, {}, {}, &status));
        BOOST_CHECK_MESSAGE(status != cuckatoo::GpuSolveStatus::kSolved,
                            "a rejected GPU cycle must not leave gpu_status as kSolved");
        BOOST_CHECK_EQUAL(static_cast<int>(status), static_cast<int>(cuckatoo::GpuSolveStatus::kSolverError));
        BOOST_CHECK(cuckatoo::SolverFault(status, /*cpu_fallback=*/false).has_value());
    }
    {
        std::array<unsigned char, cuckatoo::PREPOW_BYTES> pre{};
        pre.fill(0xCD);
        cuckatoo::Cycle out{}; uint32_t won = 0;
        auto status{cuckatoo::GpuSolveStatus::kSolved};
        BOOST_CHECK(!cuckatoo::CuckatooSolve(pre, 19, 0, 1u << 20, out, won,
                                            /*cpu_fallback=*/false, {}, {}, &status));
        BOOST_CHECK_MESSAGE(status != cuckatoo::GpuSolveStatus::kSolved,
                            "a rejected GPU cycle must not leave gpu_status as kSolved");
        BOOST_CHECK_EQUAL(static_cast<int>(status), static_cast<int>(cuckatoo::GpuSolveStatus::kSolverError));
        BOOST_CHECK(cuckatoo::SolverFault(status, /*cpu_fallback=*/false).has_value());
    }
}

// The overwrite must not fire on a cycle that does verify. A stub that emits
// a real E19 proof has to leave kSolved, or the health bit would flip on every
// successful GPU solve.
BOOST_AUTO_TEST_CASE(verified_gpu_proof_stays_solved)
{
    EnvGuard g;
    std::vector<unsigned char> pre(64, 0xCD);
    cuckatoo::Cycle real{};
    uint32_t real_nonce = 0;
    qs_test_unsetenv("CUCKATOO_GPU_SOLVER");
    BOOST_REQUIRE(cuckatoo::CuckatooSolveBytes(pre.data(), pre.size(), 19, 0, 1u << 20, real, real_nonce,
                                              /*cpu_fallback=*/true));

    set_solver(write_scripted_stub("qs_stub_real_cycle", {nonce_line(real_nonce), cycle_line_from(real)}, 0));

    cuckatoo::Cycle out{};
    uint32_t nonce = 0;
    auto status{cuckatoo::GpuSolveStatus::kNoCycle};
    BOOST_REQUIRE(cuckatoo::CuckatooSolveBytes(pre.data(), pre.size(), 19, 0, 1u << 20, out, nonce,
                                              /*cpu_fallback=*/false, {}, {}, &status));
    BOOST_CHECK_EQUAL(nonce, real_nonce);
    BOOST_CHECK(out == real);
    BOOST_CHECK_MESSAGE(status == cuckatoo::GpuSolveStatus::kSolved,
                        "a verified GPU cycle must remain kSolved, got "
                        << static_cast<int>(status));
    BOOST_CHECK(!cuckatoo::SolverFault(status, /*cpu_fallback=*/false).has_value());
}

BOOST_AUTO_TEST_CASE(watchdog_survives_slow_but_progressing_solver)
{
    EnvGuard g;
    // progress= every 1s for 3s (each gap < the 2s window), then a well-formed result.
    set_solver(write_dripping_stub("qs_stub_drip", /*heartbeats=*/3, {"nonce=42", cycle_line(1)}));
    set_timeout_env("2");                            // 2s window, progress arrives each 1s

    unsigned char pre[96] = {0};
    cuckatoo::Cycle out{}; uint32_t nonce = 0;
    BOOST_REQUIRE(cuckatoo::GpuSolveBytes(pre, sizeof(pre), 29, 0, 10, out, nonce));
    BOOST_CHECK_EQUAL(nonce, 42u);    // NOT killed; parsed the eventual result
    BOOST_CHECK_EQUAL(out[0], 1u);
}


// The window is not the only place a grind can block: with no GPU configured the
// CPU sweep owns the thread for its whole budget. Cancel has to reach in there too.
BOOST_AUTO_TEST_CASE(cancel_interrupts_the_cpu_sweep)
{
    EnvGuard g;
    qs_test_unsetenv("CUCKATOO_GPU_SOLVER");
    std::vector<unsigned char> pre(64, 0xCD);
    cuckatoo::Cycle out{}; uint32_t nonce = 0;

    // Cancelled from the first poll: the sweep must abandon before any graph.
    std::vector<uint32_t> attempted;
    bool ok = cuckatoo::CuckatooSolveBytes(pre.data(), pre.size(), 19, 0, 1u << 20, out, nonce,
                                           /*cpu_fallback=*/true,
                                           [&](uint32_t n) { attempted.push_back(n); },
                                           [] { return true; });
    BOOST_CHECK(!ok);
    BOOST_CHECK_MESSAGE(attempted.empty(),
                        "cancelled sweep still ground " << attempted.size() << " graph(s)");
}

// --- LIVENESS CONTRACT: runs on BOTH platforms -----------------------------

// THE REGRESSION TEST for the Windows deadlock found on the 2026-08-29 publictest
// bring-up. A cancelled solve must return promptly even when the child is still
// producing output.
//
// The old Windows bridge checked `cancel` only after a blocking read returned,
// then stopped reading and called _pclose -- which WAITS for the child to exit.
// The child had up to 4096 graphs left (1<<24 on the per-tx grind) and kept
// writing `progress=` heartbeats into a pipe nobody drained; once the ~4KB pipe
// buffer filled it blocked in write() and could never exit, so the parent waited
// for an exit that could not happen. The node wedged with the GPU pinned at 100%
// and went on reporting solver_ok=true.
//
// A chatty stub is essential here. cancel_interrupts_a_silent_solver above uses a
// stub that prints nothing, so the pipe never fills and the child can always
// exit -- it would have passed on the broken bridge. Two tests, one invariant,
// and only this one has teeth.
//
// The timeout decorator matters as much as the assertion: on the broken bridge
// this case does not fail, it HANGS, and a hung case is indistinguishable from a
// slow suite. The decorator turns that into a loud failure.
BOOST_AUTO_TEST_CASE(cancel_does_not_wait_for_a_chatty_solver,
                     *boost::unit_test::timeout(30))
{
    EnvGuard g;
    set_solver(stub_chatty_forever());
    set_timeout_env("3600");  // only cancel can end this call, never the watchdog

    std::atomic<bool> cancelled{false};
    unsigned char pre[96] = {0};
    cuckatoo::Cycle out{}; uint32_t nonce = 0;

    // Cancel only after the solver has actually produced output, so the child is
    // mid-flight and its pipe is filling when the cancel lands.
    std::atomic<int> seen{0};
    const auto t0 = std::chrono::steady_clock::now();
    auto status{cuckatoo::GpuSolveStatus::kSolved};
    const bool ok = cuckatoo::GpuSolveBytes(
        pre, sizeof(pre), 28, 0, 1u << 24, out, nonce,
        [&](uint32_t) { if (++seen >= 2) cancelled = true; },
        [&] { return cancelled.load(); },
        &status);
    const auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    BOOST_CHECK(!ok);
    BOOST_CHECK_MESSAGE(seen.load() >= 2, "stub produced no progress; the test proved nothing");
    BOOST_CHECK_MESSAGE(secs < 15.0,
                        "GpuSolveBytes took " << secs << "s to return from a cancel while the "
                        "solver was still writing; the parent must kill the child, not wait for it");
    BOOST_CHECK_MESSAGE(status == cuckatoo::GpuSolveStatus::kCancelled,
                        "a cancelled solve must report kCancelled, not a hardware fault");
}

// A silent solver must also be interruptible: the parent cannot be parked in a
// read that only returns when data arrives, because data never arrives.
//
// This case ABSORBED the POSIX-only cancel_interrupts_a_silent_solver, which
// tested the same invariant with the same stub. Deleting that one would have
// been a quiet weakening on Linux -- it bounded the return at 5s where this case
// allowed 15 -- so the 5s bound moved here rather than being dropped. Windows
// can hold it: the bridge polls `cancel` on a 250ms slice
// (kCancelPollSliceMs in gpu_solver.cpp), so neither platform's return time is
// governed by anything but that slice plus process teardown.
BOOST_AUTO_TEST_CASE(cancel_interrupts_a_silent_solver_both_platforms,
                     *boost::unit_test::timeout(30))
{
    EnvGuard g;
    set_solver(stub_silent_forever());
    set_timeout_env("3600");  // the watchdog must not be what saves this

    std::atomic<bool> cancelled{false};
    unsigned char pre[96] = {0};
    cuckatoo::Cycle out{}; uint32_t nonce = 0;
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        cancelled = true;
    });
    const auto t0 = std::chrono::steady_clock::now();
    auto status{cuckatoo::GpuSolveStatus::kSolved};
    const bool ok = cuckatoo::GpuSolveBytes(pre, sizeof(pre), 28, 0, 10, out, nonce, {},
                                            [&] { return cancelled.load(); }, &status);
    const auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    canceller.join();

    BOOST_CHECK(!ok);
    BOOST_CHECK_MESSAGE(secs < 5.0,
                        "GpuSolveBytes waited " << secs << "s after cancel; shutdown must not "
                        "have to sit out the no-progress window");
    BOOST_CHECK_MESSAGE(status == cuckatoo::GpuSolveStatus::kCancelled,
                        "expected kCancelled, got status " << static_cast<int>(status));
}

// The no-progress watchdog must exist on both platforms. Windows had none at all:
// -cuckatoosolvertimeout was accepted, documented, and silently did nothing there.
//
// This case ABSORBED the POSIX-only watchdog_kills_hung_solver: same silent
// stub, same invariant, and this one additionally asserts kTimedOut. Its 10s
// bound is the deleted case's, kept so the merge costs nothing -- a 2s window
// polled on a 250ms slice has no business taking three times that long.
BOOST_AUTO_TEST_CASE(watchdog_kills_hung_solver_both_platforms,
                     *boost::unit_test::timeout(60))
{
    EnvGuard g;
    set_solver(stub_silent_forever());
    set_timeout_env("2");  // 2s no-progress window, no cancel supplied

    unsigned char pre[96] = {0};
    cuckatoo::Cycle out{}; uint32_t nonce = 0;
    const auto t0 = std::chrono::steady_clock::now();
    auto status{cuckatoo::GpuSolveStatus::kSolved};
    const bool ok = cuckatoo::GpuSolveBytes(pre, sizeof(pre), 28, 0, 10, out, nonce, {}, {}, &status);
    const auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    BOOST_CHECK(!ok);
    BOOST_CHECK_MESSAGE(secs < 10.0,
                        "a silent solver ran " << secs << "s against a 2s window; the watchdog "
                        "is not running on this platform");
    BOOST_CHECK_MESSAGE(status == cuckatoo::GpuSolveStatus::kTimedOut,
                        "a solver killed by the watchdog must report kTimedOut, got "
                        << static_cast<int>(status));
}


// --- F-144: A RESOURCE FAILURE IS NOT AN EMPTY SEARCH WINDOW ---------------
// GpuSolveBytes reports kNoCycle from its first line and every later path
// overwrites it -- except the resource-failure returns, which bail out with the
// default still set. An exhausted fd table, or a fork that could not allocate,
// therefore reads to every caller as "the solver ran fine and this nonce window
// held no cycle": the node grinds on, the operator sees nothing, and
// SolverFault() returns nullopt so not even the fault string fires. Same class
// as a card that dies mid-solve reading as bad luck.
BOOST_AUTO_TEST_CASE(resource_failure_is_not_reported_as_an_empty_search_window)
{
    EnvGuard g;
    // A stub that would succeed if it ever ran, so a kNoCycle here can only have
    // come from the failure path and not from the solver itself.
    set_solver(write_scripted_stub("qs_stub_would_solve", {"nonce=7", cycle_line(1)}, 0));
    unsigned char pre[96] = {0};
    cuckatoo::Cycle out{}; uint32_t nonce = 0;

    set_fault("pipe");
    auto status{cuckatoo::GpuSolveStatus::kSolved};
    BOOST_CHECK(!cuckatoo::GpuSolveBytes(pre, sizeof(pre), 28, 0, 10, out, nonce, {}, {}, &status));
    BOOST_CHECK_MESSAGE(status == cuckatoo::GpuSolveStatus::kSolverError,
                        "a pipe that could not be created must not be reported as an exhausted "
                        "search window, got status " << static_cast<int>(status));

    set_fault("spawn");
    status = cuckatoo::GpuSolveStatus::kSolved;
    BOOST_CHECK(!cuckatoo::GpuSolveBytes(pre, sizeof(pre), 28, 0, 10, out, nonce, {}, {}, &status));
    BOOST_CHECK_MESSAGE(status == cuckatoo::GpuSolveStatus::kSolverError,
                        "a process that could not be spawned must not be reported as an exhausted "
                        "search window, got status " << static_cast<int>(status));
}

// --- F-144: AN UNCONTAINABLE SOLVER IS NEVER STARTED -----------------------
// The bridge kills through a kill-as-a-unit group -- a job object on Windows, a
// process group on POSIX -- because the solver is a GRANDCHILD of the shell.
// Every call that establishes that unit used to have its return discarded
// (CreateJobObjectW may return null; SetInformationJobObject,
// AssignProcessToJobObject and setpgid may fail), and the kill was guarded on
// `if (job)` alone -- so on failure the bridge reported kCancelled having reaped
// nobody, while qsgpusolve kept the card pinned on abandoned work.
//
// Killing harder is NOT the fix and this case deliberately does not ask for it.
// When the bridge went through /bin/sh the solver was a grandchild and no signal
// the parent could send reached it without the group; that measurement is what
// produced this case. The shell is gone now, so the immediate child is reachable
// again -- but -cuckatoosolver is operator-supplied and routinely a wrapper (our
// own shipped Linux dispatcher is one), and a wrapper that does not exec puts the
// solver right back out of reach. The guarantee that is achievable either way is
// refusal: hold the child before it can exec (CREATE_SUSPENDED on Windows, a sync
// pipe on POSIX), confirm containment, and abandon the attempt if it cannot be
// had. Then no orphan can exist to be killed.
BOOST_AUTO_TEST_CASE(solver_is_never_started_when_containment_cannot_be_established,
                     *boost::unit_test::timeout(120))
{
    EnvGuard g;
    // Unique per run: a stub orphaned by an EARLIER, unfixed binary keeps
    // appending to a fixed path even after the test deletes it, which reads as
    // this run having started a solver. That false RED cost real time once.
    const std::string marker = tmp_path("qs_marker_containment_" + num((int)getpid()) + ".txt");
    std::remove(marker.c_str());
    set_solver(stub_marker_forever("qs_stub_marker", marker));
    set_timeout_env("3600");     // the watchdog must not be what ends this
    set_fault("containment");    // no job object / no process group

    unsigned char pre[96] = {0};
    cuckatoo::Cycle out{}; uint32_t nonce = 0;
    const auto t0 = std::chrono::steady_clock::now();
    auto status{cuckatoo::GpuSolveStatus::kSolved};
    const bool ok = cuckatoo::GpuSolveBytes(pre, sizeof(pre), 28, 0, 10, out, nonce, {}, {}, &status);
    const auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    BOOST_CHECK(!ok);
    BOOST_CHECK_MESSAGE(secs < 30.0,
                        "GpuSolveBytes took " << secs << "s to give up on an uncontainable "
                        "solver; it must refuse promptly, not wait out a window");
    BOOST_CHECK_MESSAGE(status == cuckatoo::GpuSolveStatus::kSolverError,
                        "refusing to start must be reported as kSolverError so SolverFault() "
                        "can tell the operator; got status " << static_cast<int>(status));

    // The load-bearing assertion. The stub appends to the marker on its very
    // first loop iteration, so the file existing at all proves the solver ran --
    // which, with no group to kill it through, means an orphan holding the GPU.
    // Waiting first rules out a race where it simply had not got there yet.
    std::this_thread::sleep_for(std::chrono::seconds(3));
    BOOST_CHECK_MESSAGE(marker_size(marker) < 0,
                        "the solver was started despite containment failing: marker is "
                        << marker_size(marker) << " bytes. Without the process group, anything "
                        "the solver forked is an orphan holding the card.");
    std::remove(marker.c_str());
}

// F-149. The solver path used to be interpolated into a shell command string --
// `/bin/sh -c "<path> <args> 2>/dev/null"` on POSIX, `cmd.exe /S /C "..."` on
// Windows -- so every metacharacter in an operator's -cuckatoosolver was live:
// `&` `;` `|` and `$( )` split or substituted, and a path containing a space was
// simply two words. This stages the solver at exactly such a path.
//
// Under a shell the `&` ends the command, the staged stub never runs at all, and
// the text after the `&` is executed in its place -- so the `nonce=99` baked
// into the filename is what the bridge parses. Asserting the nonce is 42 is
// therefore the same assertion as "nothing in the path was interpreted".
BOOST_AUTO_TEST_CASE(the_solver_path_is_never_interpreted_by_a_shell)
{
    EnvGuard g;
    const std::string nasty = tmp_path(std::string("qs stub & echo nonce=99") + kExeSuffix);
    std::remove(nasty.c_str());
    BOOST_REQUIRE_MESSAGE(copy_stub_to(nasty), "could not stage a solver at " << nasty);

    const std::string argv_out = tmp_path("qs_stub_argv.txt");
    std::remove(argv_out.c_str());
    set_env("QS_SOLVER_STUB_ARGV", argv_out);
    // Sets the stub's directive script; the solver path is overridden below to
    // point at the staged copy rather than at the stub's build location.
    write_scripted_stub("qs_stub_nasty_path", {"nonce=42", cycle_line(1)}, 0);
    set_solver(nasty);

    unsigned char pre[96] = {0};
    cuckatoo::Cycle out{}; uint32_t nonce = 0;
    auto status{cuckatoo::GpuSolveStatus::kNoCycle};
    const bool ok = cuckatoo::GpuSolveBytes(pre, sizeof(pre), 28, 0, 10, out, nonce, {}, {}, &status);

    BOOST_CHECK_MESSAGE(ok, "the solver staged at a path containing a space and an `&` never "
                            "produced a cycle; status " << static_cast<int>(status));
    BOOST_CHECK_MESSAGE(nonce == 42u,
                        "expected nonce=42 from the staged stub, got " << nonce <<
                        ": 99 is the value baked into the path, which only a shell could reach");

    // The argv the solver actually received. gpu_solver.h states the contract as
    // `<path> <edgebits> <hex(prepow)> <start_nonce> <max_attempts>`, and a shell
    // is not the only thing that can break it -- a stray redirection token or a
    // path split into two words would both show up here as the wrong count.
    const std::vector<std::string> got = read_lines(argv_out);
    BOOST_REQUIRE_MESSAGE(got.size() == 4,
                          "the solver received " << got.size() << " arguments, expected 4");
    BOOST_CHECK_EQUAL(got[0], "28");
    BOOST_CHECK_EQUAL(got[1], std::string(2 * sizeof(pre), '0'));
    BOOST_CHECK_EQUAL(got[2], "0");
    BOOST_CHECK_EQUAL(got[3], "10");

    std::remove(argv_out.c_str());
    std::remove(nasty.c_str());
}

BOOST_AUTO_TEST_CASE(cpu_fallback_false_skips_cpu_bytes)
{
    EnvGuard g;
    qs_test_unsetenv("CUCKATOO_GPU_SOLVER");        // no GPU: only the CPU path could solve
    std::vector<unsigned char> pre(64, 0xCD);
    cuckatoo::Cycle out{}; uint32_t nonce = 0;

    // cpu_fallback=false: must NOT grind the CPU -> false.
    BOOST_CHECK(!cuckatoo::CuckatooSolveBytes(pre.data(), pre.size(), 19, 0, 1u << 20, out, nonce, /*cpu_fallback=*/false));
    // cpu_fallback=true (default behavior): E19 CPU solves -> true.
    BOOST_CHECK(cuckatoo::CuckatooSolveBytes(pre.data(), pre.size(), 19, 0, 1u << 20, out, nonce, /*cpu_fallback=*/true));
}

BOOST_AUTO_TEST_CASE(cpu_fallback_false_skips_cpu_fixed)
{
    EnvGuard g;
    qs_test_unsetenv("CUCKATOO_GPU_SOLVER");
    std::array<unsigned char, cuckatoo::PREPOW_BYTES> pre{}; pre.fill(0xCD);
    cuckatoo::Cycle out{}; uint32_t won = 0;
    BOOST_CHECK(!cuckatoo::CuckatooSolve(pre, 19, 0, 1u << 20, out, won, /*cpu_fallback=*/false));
    BOOST_CHECK(cuckatoo::CuckatooSolve(pre, 19, 0, 1u << 20, out, won, /*cpu_fallback=*/true));
}

BOOST_AUTO_TEST_CASE(cpu_fallback_streams_progress)
{
    EnvGuard g;
    qs_test_unsetenv("CUCKATOO_GPU_SOLVER");
    std::vector<unsigned char> pre(64, 0xCD);
    cuckatoo::Cycle out{}; uint32_t nonce = 0;

    std::vector<uint32_t> byte_progress;
    (void)cuckatoo::CuckatooSolveBytes(pre.data(), pre.size(), 19, 11, 1, out, nonce, /*cpu_fallback=*/true, [&](uint32_t seen_nonce) {
        byte_progress.push_back(seen_nonce);
    });
    BOOST_REQUIRE_EQUAL(byte_progress.size(), 1U);
    BOOST_CHECK_EQUAL(byte_progress[0], 11U);

    std::array<unsigned char, cuckatoo::PREPOW_BYTES> fixed_pre{}; fixed_pre.fill(0xCD);
    std::vector<uint32_t> fixed_progress;
    (void)cuckatoo::CuckatooSolve(fixed_pre, 19, 17, 1, out, nonce, /*cpu_fallback=*/true, [&](uint32_t seen_nonce) {
        fixed_progress.push_back(seen_nonce);
    });
    BOOST_REQUIRE_EQUAL(fixed_progress.size(), 1U);
    BOOST_CHECK_EQUAL(fixed_progress[0], 17U);
}

using cuckatoo::GpuSolveStatus;
using cuckatoo::SolverFault;

BOOST_AUTO_TEST_CASE(solver_fault_healthy_outcomes)
{
    // A solved graph and an exhausted window are both healthy: the solver ran.
    BOOST_CHECK(!SolverFault(GpuSolveStatus::kSolved, /*cpu_fallback=*/false).has_value());
    BOOST_CHECK(!SolverFault(GpuSolveStatus::kNoCycle, /*cpu_fallback=*/false).has_value());
    // Cancellation says nothing about health.
    BOOST_CHECK(!SolverFault(GpuSolveStatus::kCancelled, /*cpu_fallback=*/false).has_value());
    // On sandbox the solver is never reached and the status stays kNoCycle.
    BOOST_CHECK(!SolverFault(GpuSolveStatus::kNoCycle, /*cpu_fallback=*/true).has_value());
}

BOOST_AUTO_TEST_CASE(solver_fault_missing_solver_depends_on_fallback)
{
    // With CPU mining permitted, no GPU solver is the intended configuration.
    BOOST_CHECK(!SolverFault(GpuSolveStatus::kNoSolver, /*cpu_fallback=*/true).has_value());
    // Without it, an unset -cuckatoosolver means nothing is mining at all.
    const auto fault = SolverFault(GpuSolveStatus::kNoSolver, /*cpu_fallback=*/false);
    BOOST_REQUIRE(fault.has_value());
    BOOST_CHECK(fault->find("-cuckatoosolver") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(solver_missing_names_only_the_unconfigured_case)
{
    // The GUI substitutes its own wording on this one outcome, so it is the one
    // outcome that must be identifiable without reading the sentence. Every other
    // fault carries advice the GUI cannot improve on -- the card, the driver, the
    // log -- and has to pass through unaltered.
    BOOST_CHECK(cuckatoo::SolverMissing(GpuSolveStatus::kNoSolver, /*cpu_fallback=*/false));
    // With a CPU fallback an unconfigured GPU solver is not a fault at all, so
    // there is nothing for the GUI to reword and nothing to report.
    BOOST_CHECK(!cuckatoo::SolverMissing(GpuSolveStatus::kNoSolver, /*cpu_fallback=*/true));
    for (const bool fallback : {false, true}) {
        BOOST_CHECK(!cuckatoo::SolverMissing(GpuSolveStatus::kSolved, fallback));
        BOOST_CHECK(!cuckatoo::SolverMissing(GpuSolveStatus::kNoCycle, fallback));
        BOOST_CHECK(!cuckatoo::SolverMissing(GpuSolveStatus::kCancelled, fallback));
        BOOST_CHECK(!cuckatoo::SolverMissing(GpuSolveStatus::kNoCudaDevice, fallback));
        BOOST_CHECK(!cuckatoo::SolverMissing(GpuSolveStatus::kDeviceFault, fallback));
        BOOST_CHECK(!cuckatoo::SolverMissing(GpuSolveStatus::kSolverError, fallback));
        BOOST_CHECK(!cuckatoo::SolverMissing(GpuSolveStatus::kTimedOut, fallback));
        // The GUI reads this flag only inside the "a fault was reported" branch, so
        // it must never be true where SolverFault stayed silent.
        for (const GpuSolveStatus status : {GpuSolveStatus::kSolved, GpuSolveStatus::kNoCycle,
                                            GpuSolveStatus::kCancelled, GpuSolveStatus::kNoSolver,
                                            GpuSolveStatus::kNoCudaDevice, GpuSolveStatus::kDeviceFault,
                                            GpuSolveStatus::kSolverError, GpuSolveStatus::kTimedOut}) {
            if (cuckatoo::SolverMissing(status, fallback)) {
                BOOST_CHECK(SolverFault(status, fallback).has_value());
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(solver_fault_hardware_and_process_failures)
{
    // These are faults whether or not a CPU fallback exists: the card or the
    // child process is broken, and that is what the 402k-attempts-per-second
    // incident on a CUDA test node failed to say for seven hours.
    for (const bool fallback : {false, true}) {
        BOOST_CHECK(SolverFault(GpuSolveStatus::kNoCudaDevice, fallback).has_value());
        BOOST_CHECK(SolverFault(GpuSolveStatus::kDeviceFault, fallback).has_value());
        BOOST_CHECK(SolverFault(GpuSolveStatus::kSolverError, fallback).has_value());
        BOOST_CHECK(SolverFault(GpuSolveStatus::kTimedOut, fallback).has_value());
    }
    BOOST_CHECK(SolverFault(GpuSolveStatus::kNoCudaDevice, false)->find("CUDA") != std::string::npos);
    // The whole point of the status is that the message tells the operator what
    // to go and look at, so assert the text does that rather than merely exists.
    const auto device_fault = SolverFault(GpuSolveStatus::kDeviceFault, false);
    BOOST_REQUIRE(device_fault.has_value());
#ifdef WIN32
    BOOST_CHECK(device_fault->find("TdrDelay") != std::string::npos);
#else
    BOOST_CHECK(device_fault->find("dmesg") != std::string::npos);
#endif
    BOOST_CHECK(*device_fault != *SolverFault(GpuSolveStatus::kNoCudaDevice, false));
}

BOOST_AUTO_TEST_SUITE_END()
