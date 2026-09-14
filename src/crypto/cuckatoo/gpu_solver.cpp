// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Mining-only subprocess bridge to an external GPU Cuckatoo solver. NEVER on the
// validation path. The caller MUST verify any returned cycle with CuckatooVerify.
// The solver is spawned with a real argument vector and no shell, so a solver
// path containing a space or a shell metacharacter is passed through untouched.
// The parent runs the child in a kill-as-a-unit group — a process group on POSIX,
// a job object on Windows — and enforces a no-progress watchdog: any line
// (including advisory `progress=` heartbeats) resets the deadline; a silent child
// past the window is killed along with anything it spawned. Both branches also
// poll `cancel` on a 250ms slice so shutdown never waits out the window.
// Correctness is still owned by CuckatooVerify — this file only guards liveness.
//
// The two platform branches used to live here, side by side, and they were not
// feature-matched: the Windows side was a bare _popen loop with no watchdog, no
// process group and a cancel check that could not run while a read was blocked,
// and it deadlocked a live soak node. There is now one branch. The per-platform
// half lives in util/process/contained_child.cpp, where bundled Tor uses the
// same code, so the two callers cannot drift the way the two platforms did.
#include <crypto/cuckatoo/gpu_solver.h>

#include <util/process/contained_child.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace cuckatoo {

namespace {
std::string to_hex(const unsigned char* b, size_t n) {
    static const char* h = "0123456789abcdef";
    std::string s; s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { s += h[b[i] >> 4]; s += h[b[i] & 0xf]; }
    return s;
}

// Parse accumulated solver stdout for the nonce=/cycle= contract. `progress=`
// and anything else are ignored. Returns true iff both fields were parsed.
bool parse_solver_output(const std::string& text, uint32_t& out_nonce, Cycle& out) {
    bool have_nonce = false, have_cycle = false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        std::string line = (eol == std::string::npos) ? text.substr(pos)
                                                       : text.substr(pos, eol - pos);
        pos = (eol == std::string::npos) ? text.size() : eol + 1;
        if (line.rfind("nonce=", 0) == 0) {
            out_nonce = (uint32_t)std::strtoul(line.c_str() + 6, nullptr, 10);
            have_nonce = true;
        } else if (line.rfind("cycle=", 0) == 0) {
            const char* p = line.c_str() + 6; int n = 0;
            while (n < PROOFSIZE) {
                char* end = nullptr;
                unsigned long long v = std::strtoull(p, &end, 16);
                if (end == p) break;
                out[n++] = (uint32_t)v; p = end;
            }
            have_cycle = (n == PROOFSIZE);
        }
    }
    return have_nonce && have_cycle;
}

void dispatch_progress_line(const std::string& line, const SolverProgressCallback& progress)
{
    if (!progress || line.rfind("progress=", 0) != 0) return;
    progress((uint32_t)std::strtoul(line.c_str() + 9, nullptr, 10));
}

void dispatch_progress_lines(std::string& pending, const char* data, size_t len, const SolverProgressCallback& progress)
{
    if (!progress) return;
    pending.append(data, len);
    size_t pos = 0;
    while (pos < pending.size()) {
        size_t eol = pending.find('\n', pos);
        if (eol == std::string::npos) break;
        dispatch_progress_line(pending.substr(pos, eol - pos), progress);
        pos = eol + 1;
    }
    pending.erase(0, pos);
}

// No-progress watchdog window, seconds. Calibrated on the rig (SP1b Task 7):
// a single solve attempt on a P104-100 (sm_61) took ~3.4s at the then-current
// E29, and qsgpusolve emits one progress= heartbeat per attempt, so the max
// no-progress gap under healthy operation was ~3.44s. The shipped size is now
// E28, which is ~2x faster per graph, so the gap shrinks and the margin only
// widens — the 30s window stays valid without recalibration. 30s gave ~8.7x
// margin over the E29 cadence —
// tolerant of thermal throttling, multi-card contention, and slower single
// attempts on weaker cards — while still bounding a genuine hang to 30s
// (blocks are 5 min apart). See research/tier0-pow/tools/calibration/sp1b-gpu-parity/report.md.
// -cuckatoosolvertimeout (init sets CUCKATOO_GPU_TIMEOUT) overrides at runtime.
// The solver's argument vector, exactly as gpu_solver.h states the contract.
// Built once, in the shared part of this file, so the two branches cannot drift
// on what the solver is actually asked to do -- and so that neither of them has
// to reassemble it into a string a shell would then take apart again.
std::vector<std::string> solver_argv(const std::string& solver, uint8_t edgebits,
                                     const unsigned char* prepow, size_t len,
                                     uint32_t start_nonce, uint32_t max_attempts)
{
    return {solver,
            std::to_string((int)edgebits),
            to_hex(prepow, len),
            std::to_string(start_nonce),
            std::to_string(max_attempts)};
}

int no_progress_timeout_sec() {
    const char* t = std::getenv("CUCKATOO_GPU_TIMEOUT");
    if (t && t[0]) { long v = std::strtol(t, nullptr, 10); if (v > 0) return (int)v; }
    return 30;
}
} // namespace

std::optional<std::string> GpuSolverPath()
{
    const char* solver = std::getenv("CUCKATOO_GPU_SOLVER");
    if (!solver || solver[0] == '\0') return std::nullopt;
    return solver;
}

std::optional<std::string> SolverFault(GpuSolveStatus status, bool cpu_fallback)
{
    switch (status) {
    case GpuSolveStatus::kSolved:
    case GpuSolveStatus::kNoCycle:
    case GpuSolveStatus::kCancelled:
        return std::nullopt;
    case GpuSolveStatus::kNoSolver:
        if (cpu_fallback) return std::nullopt;
        return "No GPU solver is configured; set -cuckatoosolver=<path>";
    case GpuSolveStatus::kNoCudaDevice:
        return "The GPU solver reports no usable CUDA device; check the driver with nvidia-smi";
    case GpuSolveStatus::kDeviceFault:
#ifdef WIN32
        return "The GPU faulted while solving; on Windows this is usually the display-driver "
               "watchdog. Set GraphicsDrivers\\TdrDelay to 60 and reboot, then check Event ID 4101";
#else
        return "The GPU faulted while solving; check dmesg for NVRM/Xid messages and nvidia-smi";
#endif
    case GpuSolveStatus::kSolverError:
        return "The GPU solver exited with an error; see the debug log";
    case GpuSolveStatus::kTimedOut:
        return "The GPU solver stopped reporting progress and was killed; see the debug log";
    }
    assert(false);
}

bool SolverMissing(GpuSolveStatus status, bool cpu_fallback)
{
    return status == GpuSolveStatus::kNoSolver && SolverFault(status, cpu_fallback).has_value();
}

bool GpuSolveBytes(const unsigned char* prepow, size_t len, uint8_t edgebits,
                   uint32_t start_nonce, uint32_t max_attempts,
                   Cycle& out, uint32_t& out_nonce,
                   const SolverProgressCallback& progress,
                   const SolverCancelCallback& cancel,
                   GpuSolveStatus* status)
{
    const auto report = [status](GpuSolveStatus s) { if (status) *status = s; };
    report(GpuSolveStatus::kNoCycle);

    const std::optional<std::string> solver = GpuSolverPath();
    if (!solver || len < 4) { report(GpuSolveStatus::kNoSolver); return false; }
    if (cancel && cancel()) { report(GpuSolveStatus::kCancelled); return false; }  // already shutting down: do not spawn

    // NOT kNoCycle on failure: a child we could not start or could not contain is
    // a failure of ours, and the default set at the top of this function would
    // report it to the caller as a solver that ran fine over an empty nonce
    // window. The difference is what SolverFault() needs to tell the operator
    // anything at all.
    auto child = util::ContainedChild::Spawn(
        solver_argv(*solver, edgebits, prepow, len, start_nonce, max_attempts),
        util::ChildStdout::kPipe,
        util::SpawnFaultFromEnv("CUCKATOO_GPU_FAULT"));
    if (!child) { report(GpuSolveStatus::kSolverError); return false; }

    const int timeout_ms = no_progress_timeout_sec() * 1000;
    // Wait in short slices rather than one long poll. The no-progress window is
    // 30s by default, and a healthy solver is silent for most of it, so polling
    // the whole window at once means shutdown has to wait it out. Slicing bounds
    // the shutdown latency at one slice while leaving the watchdog deadline
    // intact -- `waited_ms` accumulates only silence, and any output resets it.
    constexpr int kCancelPollSliceMs = 250;
    std::string acc;
    std::string pending_progress;
    char rbuf[8192];
    bool killed = false;
    int waited_ms = 0;
    for (;;) {
        if (cancel && cancel()) { child->KillGroup(); killed = true; break; }
        const int remaining = timeout_ms - waited_ms;
        const int slice = remaining < kCancelPollSliceMs ? remaining : kCancelPollSliceMs;
        size_t got = 0;
        const auto st = child->ReadStdout(rbuf, sizeof(rbuf), slice, got);
        if (st == util::ContainedChild::ReadStatus::kClosed) break;   // EOF: child finished
        if (st == util::ContainedChild::ReadStatus::kTimeout) {       // silence for this slice
            waited_ms += slice;
            if (waited_ms >= timeout_ms) { child->KillGroup(); killed = true; break; }  // no progress -> hung
            continue;
        }
        acc.append(rbuf, got);
        waited_ms = 0;                 // progress resets the window
        dispatch_progress_lines(pending_progress, rbuf, got, progress);
    }
    if (!pending_progress.empty()) dispatch_progress_line(pending_progress, progress);

    if (killed) {
        report(cancel && cancel() ? GpuSolveStatus::kCancelled : GpuSolveStatus::kTimedOut);
        return false;
    }
    // The solver's own exit code, with no shell in between to reinterpret it. 4 is
    // "no usable CUDA device" and 5 is "the device faulted mid-solve" -- see the
    // exit-code block at the top of gpu/qsgpusolve.cu. Without them a dead card is
    // byte-identical to an exhausted nonce window. Wait() reports -1 for a child
    // that did not exit normally, which lands on kSolverError with the rest.
    const int exit_code = child->Wait();
    if (exit_code == 4) { report(GpuSolveStatus::kNoCudaDevice); return false; }
    if (exit_code == 5) { report(GpuSolveStatus::kDeviceFault); return false; }
    if (exit_code != 0) { report(GpuSolveStatus::kSolverError); return false; }
    if (!parse_solver_output(acc, out_nonce, out)) { report(GpuSolveStatus::kNoCycle); return false; }
    report(GpuSolveStatus::kSolved);
    return true;
}

} // namespace cuckatoo
