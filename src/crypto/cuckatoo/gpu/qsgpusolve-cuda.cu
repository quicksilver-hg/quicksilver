// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Mining-only GPU wrapper around the vendored CUDA lean solver. NEVER on the
// validation path; built separately via this dir's Makefile (nvcc), so CUDA
// stays out of the node's CMake/consensus build. Accepts the node's
// variable-length per-tx (or fixed block) preimage and grinds the nonce at
// buf[len-4..len-1] LE with mutate_nonce=0 — byte-identical to ../solve_28.cpp.
// CLI:
//   qsgpusolve-cuda --version        (or -version)
//   qsgpusolve-cuda --list-devices
//   qsgpusolve-cuda [--device <id>] <edgebits> <preimage_hex> <start_nonce> <max_attempts>
// stdout (on success, exactly two lines):
//   nonce=<decimal>
//   cycle=<42 space-separated lowercase hex indices>
//
// Exit codes -- the bridge in ../gpu_solver.cpp maps these, and
// test/lint/lint-cuckatoo-solver-filenames.py asserts this block stays here:
//   0  solved (output above), or no cycle in the requested nonce window;
//      for --list-devices, the runtime loaded (zero devices included)
//   2  usage error: bad arguments, or unparseable preimage hex
//   3  this binary was compiled for a different EDGEBITS than requested
//   4  no usable GPU device for this backend: no CUDA device, a --device that
//      is not present, or a binary built for the wrong architecture
//   5  the CUDA device faulted while solving (see device_health.cuh)
//
// 0 covering both "solved" and "no cycle" is deliberate: the caller tells them
// apart by whether stdout parsed. 4 exists because it previously could not tell
// either of those from a dead GPU.

#define CUCKATOO_NO_MAIN
#define SQUASH_OUTPUT 1  // keep stdout to the nonce=/cycle= contract only
#include "../vendor/lean.cu"

#include "device_health.cuh"  // must follow lean.cu
#include "listing_format.h"

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

static void lower_ascii(char* s) {
  for (; *s; ++s) {
    if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
  }
}

// cudaDeviceGetPCIBusId returns "0000:04:00.0". The contract prefixes pci: and
// lowercases the hex. Returns false when the call itself fails.
static bool cuda_pci_id(int index, std::string& id) {
  char buf[32];
  const cudaError_t rc = cudaDeviceGetPCIBusId(buf, (int)sizeof(buf), index);
  if (rc != cudaSuccess) return false;
  lower_ascii(buf);
  id = "pci:";
  id += buf;
  return true;
}

static void usage(const char* argv0) {
  std::fprintf(stderr,
    "usage: %s --version\n"
    "       %s --list-devices\n"
    "       %s [--device <id>] <edgebits> <preimage_hex> <start_nonce> <max_attempts>\n",
    argv0, argv0, argv0);
}

// --list-devices. Exit 0 when the runtime loaded, including zero devices.
// Exit 4 when the CUDA driver itself is missing.
static int list_devices() {
  int count = 0;
  const cudaError_t err = cudaGetDeviceCount(&count);
  if (err == cudaErrorNoDevice || (err == cudaSuccess && count == 0)) return 0;
  if (err != cudaSuccess) {
    std::fprintf(stderr, "qsgpusolve-cuda: no CUDA device available (%s)\n", cudaGetErrorString(err));
    return 4;
  }
  for (int i = 0; i < count; ++i) {
    qsgpu::ListingFields fields;
    fields.backend = "cuda";
    fields.type = "gpu";
    if (!cuda_pci_id(i, fields.id)) {
      std::fprintf(stderr, "qsgpusolve-cuda: cudaDeviceGetPCIBusId(%d) failed (%s)\n",
                   i, cudaGetErrorString(cudaGetLastError()));
      continue;
    }
    cudaDeviceProp prop;
    std::memset(&prop, 0, sizeof(prop));
    const cudaError_t prop_err = cudaGetDeviceProperties(&prop, i);
    if (prop_err != cudaSuccess) {
      std::fprintf(stderr, "qsgpusolve-cuda: cudaGetDeviceProperties(%d) failed (%s)\n",
                   i, cudaGetErrorString(prop_err));
      continue;
    }
    fields.name = prop.name;
    fields.integrated = prop.integrated != 0;
    fields.mem = (uint64_t)prop.totalGlobalMem;
    const cudaError_t set_err = cudaSetDevice(i);
    const char* fault = set_err == cudaSuccess ? gpu_health_startup_fault() : cudaGetErrorString(set_err);
    if (fault) {
      fields.status = "probe-failed";
      fields.reason = fault;
    } else {
      fields.status = "ok";
    }
    std::printf("%s\n", qsgpu::FormatListingLine(fields).c_str());
    // The startup probe holds a context. Drop it before the next device, and
    // so a listing process does not leave one behind on exit.
    if (set_err == cudaSuccess) cudaDeviceReset();
  }
  return 0;
}

// Map --device <id> to an index the same way listing does. An absent id is
// exit 4. With no --device, the first device is used, as before.
// Returns the index, or -1 after printing the reason.
static int select_device(const char* wanted) {
  int count = 0;
  const cudaError_t err = cudaGetDeviceCount(&count);
  if (err != cudaSuccess || count == 0) {
    std::fprintf(stderr, "qsgpusolve-cuda: no CUDA device available (%s)\n",
                 err == cudaSuccess ? "device count is 0" : cudaGetErrorString(err));
    return -1;
  }
  int chosen = 0;
  if (wanted) {
    chosen = -1;
    for (int i = 0; i < count; ++i) {
      std::string id;
      if (!cuda_pci_id(i, id)) continue;
      if (id == wanted) { chosen = i; break; }
    }
    if (chosen < 0) {
      std::fprintf(stderr, "qsgpusolve-cuda: device %s not found\n", wanted);
      return -1;
    }
    // Reading a PCI id can initialise that device. Reset every device the
    // scan touched except the one the solve will use.
    for (int i = 0; i < count; ++i) {
      if (i == chosen) continue;
      if (cudaSetDevice(i) == cudaSuccess) cudaDeviceReset();
    }
  }
  const cudaError_t set_err = cudaSetDevice(chosen);
  if (set_err != cudaSuccess) {
    std::fprintf(stderr, "qsgpusolve-cuda: CUDA startup probe failed (%s)\n", cudaGetErrorString(set_err));
    return -1;
  }
  if (const char* fault = gpu_health_startup_fault()) {
    std::fprintf(stderr, "qsgpusolve-cuda: CUDA startup probe failed (%s)\n", fault);
    return -1;
  }
  return chosen;
}

int main(int argc, char** argv) {
  // --version must not touch CUDA, so it works on a machine with no driver.
  if (argc == 2 && (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "-version") == 0)) {
    std::printf("qsgpusolve-cuda (built by hand; not version-stamped)\n");
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--list-devices") == 0) return list_devices();

  const char* device_id = nullptr;
  int base = 1;
  if (argc >= 2 && std::strcmp(argv[1], "--device") == 0) {
    if (argc != 7) { usage(argv[0]); return 2; }
    device_id = argv[2];
    base = 3;
  } else if (argc != 5) {
    usage(argv[0]);
    return 2;
  }

  int edgebits = std::atoi(argv[base]);
  if (edgebits != EDGEBITS) {  // this binary is compiled for one graph size only
    std::fprintf(stderr, "FATAL: edgebits %d != compiled EDGEBITS %d\n", edgebits, EDGEBITS);
    return 3;
  }
  std::vector<unsigned char> pre;
  if (!hex2bytes(argv[base + 1], pre) || pre.size() < 4) {
    std::fprintf(stderr, "FATAL: bad preimage hex\n");
    return 2;
  }
  uint32_t start = (uint32_t)std::strtoul(argv[base + 2], nullptr, 10);
  uint32_t maxn  = (uint32_t)std::strtoul(argv[base + 3], nullptr, 10);

  // Probe the chosen device before doing anything else. create_solver_ctx()
  // returns NULL when CUDA is unavailable and that return was never checked,
  // so a missing or broken GPU fell through to the "no cycle in window" exit.
  const int device = select_device(device_id);
  if (device < 0) return 4;

  gpu_health_warn_if_watchdog_short("qsgpusolve-cuda");

  SolverParams params;
  fill_default_params(&params);
  params.device = device;
  params.mutate_nonce = 0;  // we place the nonce ourselves (alignment-safe)
  SolverCtx* ctx = create_solver_ctx(&params);
  if (ctx == NULL) {
    std::fprintf(stderr, "qsgpusolve-cuda: could not create a solver context on the CUDA device\n");
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
      std::fprintf(stderr, "qsgpusolve-cuda: CUDA device fault at nonce %u: %s\n", nonce, fault);
      std::fprintf(stderr, "qsgpusolve-cuda: %s\n", gpu_health_hint());
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
