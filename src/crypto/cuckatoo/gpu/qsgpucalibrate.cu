// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// GPU calibration harness for the transaction-cost flag day: the CUDA twin of
// bench/qscalibrate.cpp. Searches N graphs at a given EDGEBITS and prints one
// CSV row per graph. M2 (cycle rate) and M3 (GPU time per graph) both come from
// this output. Mining-class code — never on a validation path.
//
//   qsgpucalibrate --edgebits=25 --graphs=1300 [--device=0] > gpu-e25.csv
//
// WHY THIS EXISTS: qscalibrate resolves its solver through the bench registry,
// which holds only CPU (lean.cpp) instantiations. It does not consult
// CUCKATOO_GPU_SOLVER — that variable is honoured solely by the node's mining
// bridge in ../gpu_solver.cpp. Running qscalibrate with that variable set
// therefore measures the CPU and silently labels it GPU, which is the one
// failure this harness exists to make impossible.
//
// The pre-image, the nonce placement and the CSV columns deliberately match
// qscalibrate byte for byte, so a GPU row and a CPU row bearing the same nonce
// describe the SAME graph. M2b's agreement check depends on that: comparing the
// SET of solved nonces across the two solver families is only meaningful if both
// searched identical graphs.
//
// Exit 5 means the CUDA device faulted mid-run and the CSV is unusable. This
// matters more here than in the miner: a faulted card still lets the event
// timers run, so every row keeps a plausible-looking seconds value that measures
// no work at all. A timing this harness printed under a Windows watchdog reset
// was quoted as a real card speed once already; the run must fail, not round
// down. Rows already flushed stay on stdout -- they were measured before the
// fault -- but the nonzero exit says not to trust the file.

#define CUCKATOO_NO_MAIN
#define SQUASH_OUTPUT 1  // keep stdout to the CSV contract only
#include "../vendor/lean.cu"

#include "device_health.cuh"  // must follow lean.cu

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

namespace {

//! Six fixed decimal places, locale-independently -- the CUDA twin of
//! qscalibrate.cpp's Seconds6(). printf's "%.6f" honours the numeric locale, so
//! under a comma-decimal one it emits "1,234567" and silently corrupts the CSV
//! this tool exists to produce. These binaries never install a locale from the
//! environment, so today they run in the classic "C" locale by construction --
//! lint-cuckatoo-source-policy.py bans the call outright. But the CPU twin does
//! not rely on that and neither should this one, because M2b compares the two
//! files column for column and a divergence would be silent. The measurement is
//! a duration, so integer microseconds are exact and hide no rounding decision.
//! util::ToString is a node header and cannot be pulled into an nvcc TU, so the
//! arithmetic is spelled out rather than shared.
std::string Seconds6(double secs)
{
    if (secs < 0) secs = 0;
    const unsigned long long us = (unsigned long long)(secs * 1e6 + 0.5);
    char frac[8];
    std::snprintf(frac, sizeof(frac), "%06llu", us % 1000000ULL);
    char whole[24];
    std::snprintf(whole, sizeof(whole), "%llu", us / 1000000ULL);
    return std::string(whole) + "." + frac;
}

enum class ParseResult { NotThisFlag, Ok, Malformed };

//! Parse "--name=value" into `out`. A flag that is present but malformed is an
//! error rather than a silent default: a sweep run under the wrong parameters is
//! worse than one that did not run.
ParseResult ParseUInt(const std::string& arg, const char* name, unsigned& out, bool& seen)
{
    const std::string prefix = std::string("--") + name + "=";
    if (arg.compare(0, prefix.size(), prefix) != 0) return ParseResult::NotThisFlag;
    const std::string value = arg.substr(prefix.size());
    if (value.empty()) return ParseResult::Malformed;
    for (char c : value) {
        if (c < '0' || c > '9') return ParseResult::Malformed;
    }
    errno = 0;
    const unsigned long parsed = std::strtoul(value.c_str(), nullptr, 10);
    if (errno != 0 || parsed > 0xffffffffUL) return ParseResult::Malformed;
    out = (unsigned)parsed;
    seen = true;
    return ParseResult::Ok;
}

} // namespace

int main(int argc, char** argv)
{
    unsigned edgebits = 0, graphs = 0, device = 0;
    bool have_edgebits = false, have_graphs = false, have_device = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg{argv[i]};
        bool matched = false;
        const char* names[3] = {"edgebits", "graphs", "device"};
        unsigned* outs[3] = {&edgebits, &graphs, &device};
        bool* seens[3] = {&have_edgebits, &have_graphs, &have_device};
        for (int f = 0; f < 3; ++f) {
            const ParseResult res = ParseUInt(arg, names[f], *outs[f], *seens[f]);
            if (res == ParseResult::Malformed) {
                std::fprintf(stderr, "qsgpucalibrate: malformed argument: %s\n", argv[i]);
                return 1;
            }
            if (res == ParseResult::Ok) { matched = true; break; }
        }
        if (!matched) {
            std::fprintf(stderr, "qsgpucalibrate: unknown argument: %s\n", argv[i]);
            return 1;
        }
    }

    if (!have_edgebits || !have_graphs) {
        std::fprintf(stderr,
                     "usage: qsgpucalibrate --edgebits=N --graphs=N [--device=N]\n");
        return 1;
    }
    // This binary is compiled for one graph size. Accepting a mismatched
    // --edgebits would mislabel every row in the output file.
    if (edgebits != EDGEBITS) {
        std::fprintf(stderr, "qsgpucalibrate: FATAL: --edgebits=%u but this binary is "
                             "compiled for EDGEBITS=%d\n", edgebits, EDGEBITS);
        return 3;
    }
    // Refuse to emit a header-only file: downstream that reads as "this size
    // found no cycles" rather than "this size was never measured".
    if (graphs == 0) {
        std::fprintf(stderr, "qsgpucalibrate: --graphs must be at least 1\n");
        return 1;
    }

    const cudaError_t dev_rc = cudaSetDevice((int)device);
    if (dev_rc != cudaSuccess) {
        std::fprintf(stderr, "qsgpucalibrate: cudaSetDevice(%u) failed: %s\n",
                     device, cudaGetErrorString(dev_rc));
        return 3;
    }

    if (const char* fault = gpu_health_startup_fault()) {
        std::fprintf(stderr, "qsgpucalibrate: CUDA startup probe failed: %s\n", fault);
        return 3;
    }

    gpu_health_warn_if_watchdog_short("qsgpucalibrate");

    // The SAME pre-image qscalibrate uses. Do not change one without the other:
    // M2b compares solved-nonce sets across the two, which is only valid if the
    // graphs are identical.
    std::vector<unsigned char> prepow(84, 0);  // production block pre-pow width
    const char* seed = "quicksilver-calibration-vector";
    std::memcpy(prepow.data(), seed, std::strlen(seed));

    SolverParams params;
    fill_default_params(&params);
    params.mutate_nonce = 0;  // we place the nonce ourselves (alignment-safe)
    SolverCtx* ctx = create_solver_ctx(&params);
    if (ctx == nullptr) {
        std::fprintf(stderr, "qsgpucalibrate: solver context allocation failed\n");
        return 3;
    }

    std::vector<char> buf(prepow.begin(), prepow.end());
    const size_t len = buf.size();

    auto set_nonce = [&buf, len](uint32_t nonce) {
        buf[len - 4] = (char)(nonce & 0xff);
        buf[len - 3] = (char)((nonce >> 8) & 0xff);
        buf[len - 2] = (char)((nonce >> 16) & 0xff);
        buf[len - 1] = (char)((nonce >> 24) & 0xff);
    };

    // One untimed warm-up graph before the first recorded row. The first kernel
    // launch on a fresh context pays module load and allocation costs that are
    // not part of steady-state per-graph time; without this the E22 mean — where
    // a graph is milliseconds — would be dominated by setup. The warm-up uses a
    // nonce OUTSIDE the recorded range so no measured graph is consumed by it.
    {
        SolverSolutions warm;
        std::memset(&warm, 0, sizeof(warm));
        set_nonce(graphs);
        gpu_health_arm();
        run_solver(ctx, buf.data(), (u32)len, graphs, /*range=*/1, &warm, nullptr);
        if (const char* fault = gpu_health_fault()) {
            std::fprintf(stderr, "qsgpucalibrate: CUDA device fault on the warm-up graph: %s\n", fault);
            std::fprintf(stderr, "qsgpucalibrate: %s\n", gpu_health_hint());
            destroy_solver_ctx(ctx);
            return 5;
        }
    }

    std::printf("edgebits,nonce,seconds,found,device,cycle\n");
    for (unsigned n = 0; n < graphs; ++n) {
        SolverSolutions sols;
        std::memset(&sols, 0, sizeof(sols));
        set_nonce(n);

        cudaEvent_t start, stop;
        cudaEventCreate(&start);
        cudaEventCreate(&stop);
        cudaEventRecord(start);
        gpu_health_arm();
        run_solver(ctx, buf.data(), (u32)len, n, /*range=*/1, &sols, nullptr);
        cudaEventRecord(stop);
        cudaEventSynchronize(stop);
        float ms = 0.0f;
        cudaEventElapsedTime(&ms, start, stop);
        cudaEventDestroy(start);
        cudaEventDestroy(stop);

        // Before the row is printed, not after: an unusable timing must never
        // reach stdout, because that is the number someone quotes later.
        if (const char* fault = gpu_health_fault()) {
            std::fprintf(stderr, "qsgpucalibrate: CUDA device fault at graph %u: %s\n", n, fault);
            std::fprintf(stderr, "qsgpucalibrate: %s\n", gpu_health_hint());
            std::fprintf(stderr, "qsgpucalibrate: %u of %u rows were measured before the fault; "
                                 "the run is incomplete\n", n, graphs);
            std::fflush(stdout);
            destroy_solver_ctx(ctx);
            return 5;
        }

        const bool found = sols.num_sols > 0;
        std::printf("%u,%u,%s,%d,%u,", edgebits, n, Seconds6(ms / 1000.0).c_str(),
                    found ? 1 : 0, device);
        // The cycle travels with the row so a CPU-side verifier can re-check it
        // at the same graph size. lean.cu self-verifies before recording a
        // solution, but that is the same code family that found it; the
        // independent check is what M2b actually rests on.
        if (found) {
            for (int j = 0; j < PROOFSIZE; ++j) {
                std::printf("%llx%s", (unsigned long long)sols.sols[0].proof[j],
                            j + 1 < PROOFSIZE ? " " : "");
            }
        }
        std::printf("\n");
        std::fflush(stdout);  // a killed long run keeps the rows it earned
    }

    destroy_solver_ctx(ctx);
    return 0;
}
