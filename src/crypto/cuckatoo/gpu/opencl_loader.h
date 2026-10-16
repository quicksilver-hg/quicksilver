// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef QUICKSILVER_CRYPTO_CUCKATOO_GPU_OPENCL_LOADER_H
#define QUICKSILVER_CRYPTO_CUCKATOO_GPU_OPENCL_LOADER_H

#include <crypto/cuckatoo/gpu/opencl_api.h>

#include <optional>
#include <string>

namespace qsgpu {
inline constexpr int kExitOk{0};
inline constexpr int kExitUsage{2};
inline constexpr int kExitEdgeBits{3};
inline constexpr int kExitNoDevice{4};
inline constexpr int kExitDeviceFault{5};

struct OpenClApi {
    cl::Int (QS_CL_API_CALL* GetPlatformIDs)(cl::Uint, cl::PlatformId*, cl::Uint*);
    cl::Int (QS_CL_API_CALL* GetPlatformInfo)(cl::PlatformId, cl::Uint, size_t, void*, size_t*);
    cl::Int (QS_CL_API_CALL* GetDeviceIDs)(cl::PlatformId, cl::DeviceType, cl::Uint, cl::DeviceId*, cl::Uint*);
    cl::Int (QS_CL_API_CALL* GetDeviceInfo)(cl::DeviceId, cl::Uint, size_t, void*, size_t*);
    cl::Context (QS_CL_API_CALL* CreateContext)(const cl::ContextProperties*, cl::Uint, const cl::DeviceId*, void*, void*, cl::Int*);
    cl::Int (QS_CL_API_CALL* ReleaseContext)(cl::Context);
    cl::CommandQueue (QS_CL_API_CALL* CreateCommandQueue)(cl::Context, cl::DeviceId, cl::QueueProperties, cl::Int*);
    cl::Int (QS_CL_API_CALL* ReleaseCommandQueue)(cl::CommandQueue);
    cl::Program (QS_CL_API_CALL* CreateProgramWithSource)(cl::Context, cl::Uint, const char**, const size_t*, cl::Int*);
    cl::Int (QS_CL_API_CALL* BuildProgram)(cl::Program, cl::Uint, const cl::DeviceId*, const char*, void*, void*);
    cl::Int (QS_CL_API_CALL* GetProgramBuildInfo)(cl::Program, cl::DeviceId, cl::Uint, size_t, void*, size_t*);
    cl::Int (QS_CL_API_CALL* ReleaseProgram)(cl::Program);
    cl::Kernel (QS_CL_API_CALL* CreateKernel)(cl::Program, const char*, cl::Int*);
    cl::Int (QS_CL_API_CALL* ReleaseKernel)(cl::Kernel);
    cl::Int (QS_CL_API_CALL* SetKernelArg)(cl::Kernel, cl::Uint, size_t, const void*);
    cl::Int (QS_CL_API_CALL* GetKernelWorkGroupInfo)(cl::Kernel, cl::DeviceId, cl::Uint, size_t, void*, size_t*);
    cl::Mem (QS_CL_API_CALL* CreateBuffer)(cl::Context, cl::MemFlags, size_t, void*, cl::Int*);
    cl::Int (QS_CL_API_CALL* ReleaseMemObject)(cl::Mem);
    cl::Int (QS_CL_API_CALL* EnqueueFillBuffer)(cl::CommandQueue, cl::Mem, const void*, size_t, size_t, size_t, cl::Uint, const cl::Event*, cl::Event*);
    cl::Int (QS_CL_API_CALL* EnqueueNDRangeKernel)(cl::CommandQueue, cl::Kernel, cl::Uint, const size_t*, const size_t*, const size_t*, cl::Uint, const cl::Event*, cl::Event*);
    cl::Int (QS_CL_API_CALL* EnqueueReadBuffer)(cl::CommandQueue, cl::Mem, cl::Bool, size_t, size_t, void*, cl::Uint, const cl::Event*, cl::Event*);
    cl::Int (QS_CL_API_CALL* Finish)(cl::CommandQueue);
};

std::optional<OpenClApi> LoadOpenCl(std::string& error);
std::optional<OpenClApi> LoadOpenClFrom(const char* library, std::string& error);
void ApplyRusticlDefault();
} // namespace qsgpu

#endif // QUICKSILVER_CRYPTO_CUCKATOO_GPU_OPENCL_LOADER_H
