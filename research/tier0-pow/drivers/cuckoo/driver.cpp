// Cuckoo (cuckatoo) timing driver for the Tier 0 PoW Harness.
//
// Measures per-tx PoW create (solve) time vs verify time and emits the shared
// 9-column CSV schema (see drivers/common/timing.h).
//
// ODR / single-TU constraint: cuckatoo.h defines verify/setheader/errstr/sipnode
// etc. NON-inline, so it may be #included in exactly ONE translation unit. This
// file is that TU. It links ONLY blake2b-ref.c (for blake2b, called by
// setheader). It does NOT link lean.cpp (which also includes cuckatoo.h) — that
// would cause multiple-definition link errors. Instead the solver is a separate
// binary (lean<EDGEBITS>x1) invoked via popen.
//
// Key-matching crux: both lean.cpp (solver) and this driver must derive the
// siphash keys identically, or verify() will reject a genuine proof. The solver,
// invoked with `-n <nonce>`, uses mutate_nonce=1 and does:
//     char header[246] = {0};
//     ((u32*)header)[246/sizeof(u32)-1] = htole32(nonce);  // nonce at offset 240
//     setheader(header, 246, &keys);
// We replicate that byte-for-byte below in make_keys().

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cassert>
#include <vector>
#include <string>
#include <unistd.h>       // access, X_OK
#include <sys/resource.h> // getrusage, RUSAGE_CHILDREN

#include "cuckatoo.h"               // verify, setheader, errstr, word_t, POW_OK, ...
                                    // (transitively pulls in siphash.hpp ->
                                    //  portable_endian.h, which defines htole32)
#include "../common/timing.h"

#ifndef HEADERLEN
#define HEADERLEN 246
#endif

// Path to the solver binary, relative to this driver's own location. The Makefile
// builds lean<EDGEBITS>x1 into the cuckatoo source dir; the driver resolves it via
// its own argv[0] directory + this relative hop. Overridable via env CUCKOO_SOLVER.
#ifndef SOLVER_REL_PATH
#define SOLVER_REL_PATH "../../third_party/cuckoo/src/cuckatoo"
#endif

// EDGEBITS is set at compile time (-DEDGEBITS=N). The solver binary name encodes it.
#define STR2(x) #x
#define STR(x) STR2(x)

// Default solver binary name. The Makefile overrides this (-DSOLVER_BIN_NAME) with
// the actual EDGEBITS-appropriate name (e.g. lean29x4 for EDGEBITS=29, which uses
// NSIPHASH=4 + AVX2). CUCKOO_SOLVER env still takes precedence at runtime.
#ifndef SOLVER_BIN_NAME
#define SOLVER_BIN_NAME "lean" STR(EDGEBITS) "x1"
#endif

static std::string dirname_of(const std::string& path) {
  size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) return ".";
  return path.substr(0, slash);
}

// Peak RSS (KB) of terminated child processes (the solver runs out-of-process via
// popen/pclose). ru_maxrss over RUSAGE_CHILDREN is the max over all waited-for
// children, i.e. the solver's peak memory — the real "create" memory footprint,
// which RUSAGE_SELF on this driver would NOT capture.
static long peak_child_rss_kb() {
  struct rusage ru;
  getrusage(RUSAGE_CHILDREN, &ru);
  return ru.ru_maxrss;
}

// Reconstruct siphash keys from the same fixed all-zero header + nonce the solver uses.
static void make_keys(u32 nonce, siphash_keys* keys) {
  char header[HEADERLEN];
  memset(header, 0, sizeof(header));
  ((u32*)header)[HEADERLEN / sizeof(u32) - 1] = htole32(nonce);
  setheader(header, sizeof(header), keys);
}

// Result of one solver invocation.
struct SolveResult {
  bool found = false;
  double create_ms = 0.0;
  word_t proof[PROOFSIZE];
  u32 nonce = 0;
};

// Invoke the solver at a single nonce. Parses "Time: N ms" and the "Solution ..."
// line (42 hex indices). Returns found=false if the solver printed no solution for
// that nonce.
static SolveResult run_solver_at(const std::string& solver_bin, u32 nonce) {
  SolveResult res;
  res.nonce = nonce;

  std::string cmd = solver_bin + " -n " + std::to_string(nonce) + " 2>/dev/null";
  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    fprintf(stderr, "FATAL: popen failed for solver: %s\n", cmd.c_str());
    exit(2);
  }

  char line[8192];
  while (fgets(line, sizeof(line), pipe)) {
    if (strncmp(line, "Time:", 5) == 0) {
      int ms = 0;
      if (sscanf(line, "Time: %d ms", &ms) == 1) res.create_ms = (double)ms;
    } else if (strncmp(line, "Solution", 8) == 0) {
      // "Solution <hex> <hex> ... <hex>" — exactly PROOFSIZE hex indices.
      const char* p = line + 8;
      int n = 0;
      while (n < PROOFSIZE) {
        char* end = nullptr;
        unsigned long long v = strtoull(p, &end, 16);
        if (end == p) break;  // no more numbers
        res.proof[n++] = (word_t)v;
        p = end;
      }
      if (n == PROOFSIZE) res.found = true;
    }
  }
  pclose(pipe);
  return res;
}

// Find a nonce >= start that yields a solution, returning the solve result.
// The solver's own create_ms is per-nonce; we accumulate the wall time across
// nonces that produced no solution so create_ms reflects the true cost of
// producing a proof (a real miner also burns time on non-solution nonces).
static SolveResult find_solution(const std::string& solver_bin, u32 start) {
  double accumulated_ms = 0.0;
  for (u32 nonce = start; nonce < start + 100000; nonce++) {
    SolveResult r = run_solver_at(solver_bin, nonce);
    accumulated_ms += r.create_ms;
    if (r.found) {
      r.create_ms = accumulated_ms;  // total solve time to land this proof
      return r;
    }
  }
  fprintf(stderr, "FATAL: no solution found in 100000 nonces from %u\n", start);
  exit(2);
}

int main(int argc, char** argv) {
  if (argc != 5) {
    fprintf(stderr,
            "usage: %s <param_label> <hardware_label> <runs> <verify_loops>\n",
            argv[0]);
    return 2;
  }
  const char* param_label = argv[1];
  const char* hardware_label = argv[2];
  int runs = atoi(argv[3]);
  long verify_loops = atol(argv[4]);
  if (runs <= 0 || verify_loops <= 0) {
    fprintf(stderr, "FATAL: runs and verify_loops must be positive\n");
    return 2;
  }

  // Resolve the solver binary path.
  std::string solver_bin;
  if (const char* env = getenv("CUCKOO_SOLVER")) {
    solver_bin = env;
  } else {
    std::string mydir = dirname_of(argv[0]);
    solver_bin = mydir + "/" + SOLVER_REL_PATH + "/" + SOLVER_BIN_NAME;
  }
  if (access(solver_bin.c_str(), X_OK) != 0) {
    fprintf(stderr,
            "FATAL: solver binary not found or not executable: %s\n"
            "       build it (make -C drivers/cuckoo EDGEBITS=%d) or set "
            "CUCKOO_SOLVER.\n",
            solver_bin.c_str(), EDGEBITS);
    return 2;
  }

  const long proof_bytes = (long)PROOFSIZE * sizeof(word_t);

  for (int run = 0; run < runs; run++) {
    // Deterministic, reproducible starting nonce per run.
    u32 start_nonce = 1u + (u32)run * 1000u;

    // --- CREATE phase: solve for a real proof. ---
    SolveResult sol = find_solution(solver_bin, start_nonce);
    // create_ms comes from the solver's own "Time:" measurement (solve time).
    // The solver runs out-of-process; create_peak_kb is the solver subprocess's
    // peak RSS via RUSAGE_CHILDREN (NOT this driver's RSS). Across runs this is
    // the max solver RSS seen so far, which is the meaningful create-memory cost.
    long create_peak_kb = peak_child_rss_kb();

    // --- Reconstruct keys from the SAME header+nonce the solver used. ---
    siphash_keys keys;
    make_keys(sol.nonce, &keys);

    // --- Self-validation BEFORE timing (proves keys match the solver's). ---
    int rc = verify(sol.proof, &keys);
    if (rc != POW_OK) {
      fprintf(stderr,
              "FATAL: self-validation failed: genuine proof at nonce %u did NOT "
              "verify (rc=%d: %s). header/keys mismatch with solver — do not "
              "fudge.\n",
              sol.nonce, rc, errstr[rc]);
      return 1;
    }
    // Tamper one index and confirm verify rejects it.
    word_t tampered[PROOFSIZE];
    memcpy(tampered, sol.proof, sizeof(tampered));
    // Corrupt the last index while keeping ascending order plausible-ish; any
    // change that breaks the cycle suffices. Add 2 (stays > prev, but breaks cycle).
    tampered[PROOFSIZE - 1] = tampered[PROOFSIZE - 1] + 2;
    int rc_bad = verify(tampered, &keys);
    if (rc_bad == POW_OK) {
      fprintf(stderr,
              "FATAL: self-validation failed: tampered proof at nonce %u still "
              "verified as OK. verify() is not actually checking.\n",
              sol.nonce);
      return 1;
    }

    // --- VERIFY timing: loop many times (single verify is sub-ms). ---
    // acc is volatile and written every iteration: each volatile store is
    // observable behavior the compiler may not elide, which forces verify() to
    // run on each pass (a valid microbenchmark fence).
    volatile long acc = 0;
    auto t0 = bench::clk::now();
    for (long i = 0; i < verify_loops; i++) {
      acc += verify(sol.proof, &keys);
    }
    double total_us = bench::us_since(t0);
    (void)acc;
    double verify_us = total_us / (double)verify_loops;
    // verify uses negligible memory; absolute process RSS here is a conservative
    // upper bound for the verify-memory gate (if the whole process is under the
    // cap during verify, verify itself certainly is).
    long verify_peak_kb = bench::peak_rss_kb();

    bench::emit_row("cuckatoo", param_label, hardware_label, run, sol.create_ms,
                    verify_us, proof_bytes, verify_peak_kb, create_peak_kb);
    fflush(stdout);
  }

  return 0;
}
