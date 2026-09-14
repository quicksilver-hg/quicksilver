// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Mining-only GPU wrapper around the vendored CUDA lean solver. NEVER on the
// validation path; built separately via this dir's Makefile (nvcc), so CUDA
// stays out of the node's CMake/consensus build. Accepts the node's
// variable-length per-tx (or fixed block) preimage and grinds the nonce at
// buf[len-4..len-1] LE with mutate_nonce=0 — byte-identical to ../solve_28.cpp.
// CLI: qsgpusolve <edgebits> <preimage_hex> <start_nonce> <max_attempts>
// stdout (on success, exactly two lines):
//   nonce=<decimal>
//   cycle=<42 space-separated lowercase hex indices>
//
// Exit codes -- the bridge in ../gpu_solver.cpp maps these, and
// test/lint/lint-cuckatoo-solver-filenames.py asserts this block stays here:
//   0  solved (output above), or no cycle in the requested nonce window
//   2  usage error: wrong argument count, or unparseable preimage hex
//   3  this binary was compiled for a different EDGEBITS than requested
//   4  no usable CUDA device, including a binary built for the wrong architecture
//   5  the CUDA device faulted while solving (see device_health.h)
//
// 0 covering both "solved" and "no cycle" is deliberate: the caller tells them
// apart by whether stdout parsed. 4 exists because it previously could not tell
// either of those from a dead GPU -- on a CUDA test node a failed card looked like an
// unlucky grind for hours. 5 is the same failure one step later in the run: 4
// covers a card that was already dead at startup, 5 a card that dies mid-grind,
// which is what the Windows display-driver watchdog does. Both were silence
// before they existed.
#define CUCKATOO_NO_MAIN
#define SQUASH_OUTPUT 1  // keep stdout to the nonce=/cycle= contract only
#include "../vendor/lean.cu"

#include "device_health.cuh"  // must follow lean.cu

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

static bool hex2bytes(const char* hex, std::vector<unsigned char>& out) {
  size_t n = std::strlen(hex);
  if (n % 2 != 0) return false;
  out.resize(n / 2);
  for (size_t i = 0; i < n / 2; ++i) {
    unsigned v;
    if (std::sscanf(hex + 2 * i, "%2x", &v) != 1) return false;
    out[i] = (unsigned char)v;
  }
  return true;
}

int main(int argc, char** argv) {
  if (argc != 5) {
    std::fprintf(stderr,
      "usage: %s <edgebits> <preimage_hex> <start_nonce> <max_attempts>\n", argv[0]);
    return 2;
  }
  int edgebits = std::atoi(argv[1]);
  if (edgebits != EDGEBITS) {  // this binary is compiled for one graph size only
    std::fprintf(stderr, "FATAL: edgebits %d != compiled EDGEBITS %d\n", edgebits, EDGEBITS);
    return 3;
  }
  std::vector<unsigned char> pre;
  if (!hex2bytes(argv[2], pre) || pre.size() < 4) {
    std::fprintf(stderr, "FATAL: bad preimage hex\n");
    return 2;
  }
  uint32_t start = (uint32_t)std::strtoul(argv[3], nullptr, 10);
  uint32_t maxn  = (uint32_t)std::strtoul(argv[4], nullptr, 10);

  // Probe the device before doing anything else. create_solver_ctx() returns
  // NULL when CUDA is unavailable and that return was never checked, so a
  // missing or broken GPU fell through to the "no cycle in window" exit.
  int device_count = 0;
  const cudaError_t count_err = cudaGetDeviceCount(&device_count);
  if (count_err != cudaSuccess || device_count == 0) {
    std::fprintf(stderr, "qsgpusolve: no CUDA device available (%s)\n",
                 count_err == cudaSuccess ? "device count is 0"
                                          : cudaGetErrorString(count_err));
    return 4;
  }

  if (const char* fault = gpu_health_startup_fault()) {
    std::fprintf(stderr, "qsgpusolve: CUDA startup probe failed (%s)\n", fault);
    return 4;
  }

  gpu_health_warn_if_watchdog_short("qsgpusolve");

  SolverParams params;
  fill_default_params(&params);
  params.mutate_nonce = 0;  // we place the nonce ourselves (alignment-safe)
  SolverCtx* ctx = create_solver_ctx(&params);
  if (ctx == NULL) {
    std::fprintf(stderr, "qsgpusolve: could not create a solver context on the CUDA device\n");
    return 4;
  }

  std::vector<char> buf(pre.begin(), pre.end());
  const size_t len = buf.size();
  std::time_t last_beat = std::time(nullptr);
  for (uint32_t i = 0; i < maxn; ++i) {
    const uint32_t nonce = start + i;
    std::time_t now = std::time(nullptr);
    if (now != last_beat) {                 // ~1s cadence; advisory only
      std::printf("progress=%u\n", nonce);
      std::fflush(stdout);
      last_beat = now;
    }
    buf[len - 4] = (char)(nonce & 0xff);
    buf[len - 3] = (char)((nonce >> 8) & 0xff);
    buf[len - 2] = (char)((nonce >> 16) & 0xff);
    buf[len - 1] = (char)((nonce >> 24) & 0xff);
    SolverSolutions sols;
    std::memset(&sols, 0, sizeof(sols));
    gpu_health_arm();
    run_solver(ctx, buf.data(), (u32)len, nonce, /*range=*/1, &sols, nullptr);
    if (const char* fault = gpu_health_fault()) {
      // Stop at the first fault rather than reporting it and continuing: the
      // vendored gpuAssert() has already called cudaDeviceReset(), so `ctx`
      // now points at freed device memory and every later graph in this run
      // would be searched on stale host bytes.
      std::fprintf(stderr, "qsgpusolve: CUDA device fault at nonce %u: %s\n", nonce, fault);
      std::fprintf(stderr, "qsgpusolve: %s\n", gpu_health_hint());
      destroy_solver_ctx(ctx);
      return 5;
    }
    if (sols.num_sols > 0) {
      std::printf("nonce=%u\n", nonce);
      std::printf("cycle=");
      for (int j = 0; j < PROOFSIZE; ++j) {
        std::printf("%llx%s", (unsigned long long)sols.sols[0].proof[j],
                    j + 1 < PROOFSIZE ? " " : "\n");
      }
      destroy_solver_ctx(ctx);
      return 0;
    }
  }
  destroy_solver_ctx(ctx);
  return 0;  // no solution in window — silence, exit 0
}
