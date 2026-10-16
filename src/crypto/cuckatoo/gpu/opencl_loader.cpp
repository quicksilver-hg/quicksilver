// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <crypto/cuckatoo/gpu/opencl_loader.h>

#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace qsgpu {
namespace {
#ifdef _WIN32
using Handle = HMODULE;
Handle OpenLibrary(const char* name)
{
    const int wide_len = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
    std::wstring wide(wide_len > 0 ? wide_len : 0, L'\0');
    if (wide_len > 0) MultiByteToWideChar(CP_UTF8, 0, name, -1, wide.data(), wide_len);
    return LoadLibraryExW(wide.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}
void* Symbol(Handle h, const char* name) { return reinterpret_cast<void*>(GetProcAddress(h, name)); }
constexpr const char* kDefaultLibrary{"OpenCL.dll"};
#else
using Handle = void*;
Handle OpenLibrary(const char* name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }
void* Symbol(Handle h, const char* name) { return dlsym(h, name); }
constexpr const char* kDefaultLibrary{"libOpenCL.so.1"};
#endif

template <typename Fn>
bool Resolve(Handle h, const char* name, Fn& slot, std::string& error)
{
    slot = reinterpret_cast<Fn>(Symbol(h, name));
    if (slot) return true;
    error = std::string{"the OpenCL loader has no entry point "} + name;
    return false;
}
} // namespace

std::optional<OpenClApi> LoadOpenClFrom(const char* library, std::string& error)
{
    const Handle h = OpenLibrary(library);
    if (!h) {
        error = std::string{"no OpenCL loader ("} + library + " could not be loaded)";
        return std::nullopt;
    }
    OpenClApi api{};
    const bool ok =
        Resolve(h, "clGetPlatformIDs", api.GetPlatformIDs, error) &&
        Resolve(h, "clGetPlatformInfo", api.GetPlatformInfo, error) &&
        Resolve(h, "clGetDeviceIDs", api.GetDeviceIDs, error) &&
        Resolve(h, "clGetDeviceInfo", api.GetDeviceInfo, error) &&
        Resolve(h, "clCreateContext", api.CreateContext, error) &&
        Resolve(h, "clReleaseContext", api.ReleaseContext, error) &&
        Resolve(h, "clCreateCommandQueue", api.CreateCommandQueue, error) &&
        Resolve(h, "clReleaseCommandQueue", api.ReleaseCommandQueue, error) &&
        Resolve(h, "clCreateProgramWithSource", api.CreateProgramWithSource, error) &&
        Resolve(h, "clBuildProgram", api.BuildProgram, error) &&
        Resolve(h, "clGetProgramBuildInfo", api.GetProgramBuildInfo, error) &&
        Resolve(h, "clReleaseProgram", api.ReleaseProgram, error) &&
        Resolve(h, "clCreateKernel", api.CreateKernel, error) &&
        Resolve(h, "clReleaseKernel", api.ReleaseKernel, error) &&
        Resolve(h, "clSetKernelArg", api.SetKernelArg, error) &&
        Resolve(h, "clGetKernelWorkGroupInfo", api.GetKernelWorkGroupInfo, error) &&
        Resolve(h, "clCreateBuffer", api.CreateBuffer, error) &&
        Resolve(h, "clReleaseMemObject", api.ReleaseMemObject, error) &&
        Resolve(h, "clEnqueueFillBuffer", api.EnqueueFillBuffer, error) &&
        Resolve(h, "clEnqueueNDRangeKernel", api.EnqueueNDRangeKernel, error) &&
        Resolve(h, "clEnqueueReadBuffer", api.EnqueueReadBuffer, error) &&
        Resolve(h, "clFinish", api.Finish, error);
    if (!ok) return std::nullopt;
    return api;
}

std::optional<OpenClApi> LoadOpenCl(std::string& error) { return LoadOpenClFrom(kDefaultLibrary, error); }

void ApplyRusticlDefault()
{
    if (std::getenv("RUSTICL_ENABLE") != nullptr) return;
#ifdef _WIN32
    _putenv_s("RUSTICL_ENABLE", "radeonsi");
#else
    setenv("RUSTICL_ENABLE", "radeonsi", 0);
#endif
}
} // namespace qsgpu
