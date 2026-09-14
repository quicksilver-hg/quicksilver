// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Device-fault detection shared by the two nvcc-built tools in this directory.
// Include AFTER ../vendor/lean.cu -- it uses that translation unit's globals.
//
// Why this is not a one-line error check. run_solver() cannot report a device
// fault: every path returns 0, including the explicit "Error initialising
// trimmer. Aborting." branch. The vendored gpuAssert() *consumes* the CUDA
// error -- it copies the text into LAST_ERROR_REASON and then calls
// cudaDeviceReset(), which tears the context down and clears the error state --
// so a cudaGetLastError() afterwards usually reports cudaSuccess. And
// edgetrimmer::trim() feeds checkCudaErrors() (an int-returning macro) into a
// bool-returning function, so a nonzero CUDA error code converts to `true` and
// reads as "trimming succeeded".
//
// The result is that a faulted card walks the rest of the grind on stale host
// memory and finds nothing, which is indistinguishable from an unlucky nonce
// window. Checking any single signal here would be a silent no-op -- the same
// shape of mistake as the create_solver_ctx() case exit 4 already exists for.
// So arm before the call, then test two independent signals after it.
#ifndef QUICKSILVER_CRYPTO_CUCKATOO_GPU_DEVICE_HEALTH_CUH
#define QUICKSILVER_CRYPTO_CUCKATOO_GPU_DEVICE_HEALTH_CUH

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#ifdef _MSC_VER
#pragma comment(lib, "advapi32.lib")  // RegGetValueA; keeps the Makefile unchanged
#endif
#endif

//! A zero-window qsgpusolve invocation still has to prove that code compiled
//! for this helper can execute on the selected device. Runtime calls such as
//! cudaGetDeviceCount() and cudaMalloc() succeed even when the binary contains
//! no kernel image for the card, which otherwise makes a wrong GPU_ARCH look
//! exactly like a successful empty probe.
static __global__ void gpu_health_startup_kernel() {}

//! Return a CUDA error string when this binary cannot launch on the device.
static inline const char* gpu_health_startup_fault()
{
    cudaGetLastError(); // discard anything left by device discovery
    gpu_health_startup_kernel<<<1, 1>>>();
    const cudaError_t launch = cudaGetLastError();
    if (launch != cudaSuccess) return cudaGetErrorString(launch);

    const cudaError_t sync = cudaDeviceSynchronize();
    if (sync != cudaSuccess) return cudaGetErrorString(sync);

    return nullptr;
}

//! Clear both fault signals so a later reading belongs to the next solve only.
static inline void gpu_health_arm()
{
    LAST_ERROR_REASON[0] = '\0';
    cudaGetLastError();  // discard anything already pending
}

//! Human-readable fault text, or nullptr if the device still looks healthy.
static inline const char* gpu_health_fault()
{
    // 1. Anything gpuAssert() caught. This is the signal that survives the
    //    cudaDeviceReset() gpuAssert performs, so it is the primary one.
    if (LAST_ERROR_REASON[0] != '\0') return LAST_ERROR_REASON;

    // 2. Anything it did not catch: trim() launches count_node_deg and
    //    kill_leaf_edges as raw <<<>>> calls with no error check at all, so a
    //    launch failure reaches us only through the runtime's own error state.
    const cudaError_t pending = cudaGetLastError();
    if (pending != cudaSuccess) return cudaGetErrorString(pending);

    // 3. Re-test the context itself. A watchdog reset can land between kernels
    //    with nothing sticky left behind; a synchronous call is what surfaces
    //    it. run_solver() has already synchronised, so this costs nothing next
    //    to a multi-second graph.
    const cudaError_t sync = cudaDeviceSynchronize();
    if (sync != cudaSuccess) return cudaGetErrorString(sync);

    return nullptr;
}

//! Platform-specific first thing to check. Named in the exit-5 message.
static inline const char* gpu_health_hint()
{
#ifdef _WIN32
    return "on Windows this is usually the display-driver watchdog: TDR resets the driver when a "
           "kernel holds the GPU past TdrDelay (default 2 s) and a Cuckatoo graph takes seconds. "
           "Set HKLM\\SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers\\TdrDelay (DWORD) to 60, "
           "reboot, and check for Event ID 4101 from source Display";
#else
    return "check dmesg for NVRM/Xid messages and nvidia-smi for a GPU that has fallen off the bus";
#endif
}

//! Warn at startup when the Windows GPU watchdog is short enough to kill a
//! graph. Advisory only: TDR bounds single-kernel occupancy rather than total
//! graph time, so a low value is a strong suspicion, not a proof.
static inline void gpu_health_warn_if_watchdog_short(const char* tool)
{
#ifdef _WIN32
    DWORD value = 0;
    DWORD size = sizeof(value);
    const LSTATUS st = RegGetValueA(HKEY_LOCAL_MACHINE,
                                    "SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
                                    "TdrDelay", RRF_RT_REG_DWORD, nullptr, &value, &size);
    // Absent means Windows uses its 2-second default, which is the dangerous case.
    const unsigned long effective = (st == ERROR_SUCCESS) ? (unsigned long)value : 2UL;
    if (effective < 10UL) {
        std::fprintf(stderr,
                     "%s: WARNING TdrDelay is %lu s%s. A Cuckatoo graph takes several seconds, so "
                     "the display-driver watchdog is likely to reset the GPU mid-kernel. %s\n",
                     tool, effective,
                     st == ERROR_SUCCESS ? "" : " (unset, so the 2 s default applies)",
                     gpu_health_hint());
    }
#else
    (void)tool;
#endif
}

#endif // QUICKSILVER_CRYPTO_CUCKATOO_GPU_DEVICE_HEALTH_CUH
