// Equihash timing driver for the Tier 0 PoW Harness.
//
// Measures per-tx PoW create (solve) time vs verify time and emits the shared
// 9-column CSV schema (see drivers/common/timing.h).
//
// ODR constraint: equi.h defines verify(), setheader(), verifyrec(), genhash(),
// duped(), compu32(), and errstr[] as plain (non-inline, non-static) C functions
// directly in the header. equi_miner.h defines struct equi, thread_ctx, worker(),
// barrier() similarly. Both headers are included in THIS ONE translation unit only.
// blake/blake2b.cpp is compiled separately and does NOT include equi.h, so there
// is no ODR conflict.
//
// Unlike the cuckoo driver, solve is fully in-process (no popen). This lets us
// measure create_peak_kb via RUSAGE_SELF (captures the real ~145MB allocation).
//
// Solver stdout: worker() prints progress lines to stdout. We temporarily redirect
// stdout to /dev/null during each solve so CSV rows land cleanly on real stdout.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <cstdint>
#include <unistd.h>       // dup, dup2, open, close
#include <fcntl.h>        // O_WRONLY, O_RDWR
#include <pthread.h>
#include <sys/resource.h>

// Single-TU include of equi_miner.h (which pulls in equi.h transitively).
// DO NOT include these headers in any other TU.
#include "equi_miner.h"

#include "../common/timing.h"

// personal string must match equi_miner.cpp's default
static const char *PERSONAL = "ZcashPoW";

// Temporarily silence stdout by redirecting it to /dev/null.
// Returns the saved fd for real stdout (must pass to restore_stdout).
static int silence_stdout() {
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    if (saved < 0) { perror("dup"); return -1; }
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull < 0) { perror("open /dev/null"); close(saved); return -1; }
    dup2(devnull, STDOUT_FILENO);
    close(devnull);
    return saved;
}

static void restore_stdout(int saved) {
    if (saved < 0) return;
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
}

// Build the headernonce buffer: empty header (all zeros), nonce at word 27.
// Mirrors equi_miner.cpp main() with header="".
static void make_headernonce(char headernonce[HEADERNONCELEN], u32 nonce) {
    memset(headernonce, 0, HEADERNONCELEN);
    ((u32 *)headernonce)[27] = htole32(nonce);
}

// Solve for one nonce using the equi solver. Runs 1 thread for determinism.
// Returns true if at least one solution was found; copies sol[0] into proof_out.
// Timing (wall clock from setheadernonce through join) written to *create_ms_out.
static bool solve_nonce(u32 nonce, u32 proof_out[PROOFSIZE], double *create_ms_out) {
    char headernonce[HEADERNONCELEN];
    make_headernonce(headernonce, nonce);

    // Suppress solver's progress printf output during solve.
    int saved_stdout = silence_stdout();

    equi eq(1);
    eq.setheadernonce(headernonce, sizeof(headernonce), PERSONAL);

    thread_ctx tc;
    tc.id = 0;
    tc.eq = &eq;

    auto t0 = bench::clk::now();
    int err = pthread_create(&tc.thread, NULL, worker, (void *)&tc);
    assert(err == 0);
    err = pthread_join(tc.thread, NULL);
    assert(err == 0);
    *create_ms_out = bench::ms_since(t0);

    restore_stdout(saved_stdout);

    u32 nsols = eq.nsols < MAXSOLS ? (u32)eq.nsols : MAXSOLS;
    if (nsols == 0) return false;

    // Copy first solution out (eq.sols[0] is a proof = u32[PROOFSIZE]).
    memcpy(proof_out, eq.sols[0], PROOFSIZE * sizeof(u32));
    return true;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr,
                "usage: %s <param_label> <hardware_label> <runs> <verify_loops>\n",
                argv[0]);
        return 2;
    }
    const char *param_label    = argv[1];
    const char *hardware_label = argv[2];
    int  runs          = atoi(argv[3]);
    long verify_loops  = atol(argv[4]);
    if (runs <= 0 || verify_loops <= 0) {
        fprintf(stderr, "FATAL: runs and verify_loops must be positive\n");
        return 2;
    }

    const long proof_bytes = (long)PROOFSIZE * sizeof(u32);  // 512*4 = 2048

    for (int run = 0; run < runs; run++) {
        // Use a different nonce per run for variety.
        u32 nonce = (u32)run;

        // --- CREATE phase: find a nonce that yields at least one solution. ---
        u32 proof[PROOFSIZE];
        double create_ms = 0.0;
        char headernonce[HEADERNONCELEN];

        bool found = false;
        for (u32 candidate = nonce; candidate < nonce + 100000; candidate++) {
            double ms = 0.0;
            if (solve_nonce(candidate, proof, &ms)) {
                create_ms += ms;
                nonce = candidate;
                make_headernonce(headernonce, nonce);
                found = true;
                break;
            }
            create_ms += ms;
        }
        if (!found) {
            fprintf(stderr, "FATAL: no solution found in 100000 nonces from run %d\n", run);
            return 2;
        }

        // Capture peak RSS now (includes the ~145MB equi allocation + solver overhead).
        // equi object is destroyed when solve_nonce() returns (stack-allocated inside).
        // The allocation may already be freed, but peak_rss_kb() reads the high-water
        // mark which persists until process exit.
        long create_peak_kb = bench::peak_rss_kb();

        // --- Self-validation BEFORE timing ---
        // 1. Valid proof must verify as POW_OK.
        int rc = verify(proof, headernonce, HEADERNONCELEN, PERSONAL);
        if (rc != POW_OK) {
            fprintf(stderr,
                    "FATAL: self-validation failed: genuine proof at nonce %u did NOT "
                    "verify (rc=%d: %s). Key/header mismatch — do not fudge.\n",
                    nonce, rc, errstr[rc]);
            return 1;
        }

        // 2. Tampered proof must be rejected.
        u32 tampered[PROOFSIZE];
        memcpy(tampered, proof, sizeof(tampered));
        // Corrupt index 0 by setting it to a clearly bogus value; verifyrec will
        // fail XOR or ordering checks.
        tampered[0] = tampered[0] ^ 0xDEADBEEFu;
        int rc_bad = verify(tampered, headernonce, HEADERNONCELEN, PERSONAL);
        if (rc_bad == POW_OK) {
            fprintf(stderr,
                    "FATAL: self-validation failed: tampered proof at nonce %u still "
                    "verified as OK. verify() is not actually checking.\n",
                    nonce);
            return 1;
        }

        // --- VERIFY timing ---
        // volatile accumulator forces each verify() call to execute.
        volatile long acc = 0;
        auto t0 = bench::clk::now();
        for (long i = 0; i < verify_loops; i++) {
            acc += verify(proof, headernonce, HEADERNONCELEN, PERSONAL);
        }
        double total_us = bench::us_since(t0);
        (void)acc;
        double verify_us = total_us / (double)verify_loops;

        long verify_peak_kb = bench::peak_rss_kb();

        bench::emit_row("equihash", param_label, hardware_label, run,
                        create_ms, verify_us, proof_bytes,
                        verify_peak_kb, create_peak_kb);
        fflush(stdout);
    }

    return 0;
}
