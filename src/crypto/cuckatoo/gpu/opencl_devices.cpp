// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <crypto/cuckatoo/gpu/opencl_devices.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace qsgpu {
namespace {
template <typename T>
bool Get(const OpenClApi& api, cl::DeviceId device, cl::Uint property, T& value)
{
    return api.GetDeviceInfo(device, property, sizeof(value), &value, nullptr) == cl::kSuccess;
}

std::string GetString(const OpenClApi& api, cl::DeviceId device, cl::Uint property)
{
    size_t size{0};
    if (api.GetDeviceInfo(device, property, 0, nullptr, &size) != cl::kSuccess || size == 0) return {};
    std::string value(size, '\0');
    if (api.GetDeviceInfo(device, property, size, value.data(), nullptr) != cl::kSuccess) return {};
    while (!value.empty() && (value.back() == '\0' || value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n')) value.pop_back();
    const auto first = value.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? std::string{} : value.substr(first);
}

bool HasExtension(const std::string& extensions, const std::string& extension)
{
    size_t pos{0};
    while ((pos = extensions.find(extension, pos)) != std::string::npos) {
        const size_t end = pos + extension.size();
        if ((pos == 0 || extensions[pos - 1] == ' ') && (end == extensions.size() || extensions[end] == ' ')) return true;
        pos = end;
    }
    return false;
}

std::string CallFailure(const char* call, cl::Int code)
{
    char out[96];
    std::snprintf(out, sizeof(out), "probe-failed %s=%d", call, code);
    return out;
}

std::string FallbackId(size_t platform, size_t device)
{
    char out[64];
    std::snprintf(out, sizeof(out), "opencl:%zu:%zu", platform, device);
    return out;
}
} // namespace

const char* ProbeKernelSource() { return "__kernel void qs_probe(void) {}"; }

std::string CompileAndProbe(const OpenClApi& api, const ListedDevice& listed)
{
    cl::Int rc{cl::kSuccess};
    cl::Context context = api.CreateContext(nullptr, 1, &listed.device, nullptr, nullptr, &rc);
    if (!context || rc != cl::kSuccess) return CallFailure("clCreateContext", rc);
    cl::CommandQueue queue = api.CreateCommandQueue(context, listed.device, 0, &rc);
    if (!queue || rc != cl::kSuccess) {
        api.ReleaseContext(context);
        return CallFailure("clCreateCommandQueue", rc);
    }
    const char* source = ProbeKernelSource();
    cl::Program program = api.CreateProgramWithSource(context, 1, &source, nullptr, &rc);
    if (!program || rc != cl::kSuccess) {
        api.ReleaseCommandQueue(queue); api.ReleaseContext(context);
        return CallFailure("clCreateProgramWithSource", rc);
    }
    rc = api.BuildProgram(program, 1, &listed.device, nullptr, nullptr, nullptr);
    if (rc != cl::kSuccess) {
        size_t size{0};
        api.GetProgramBuildInfo(program, listed.device, cl::kProgramBuildLog, 0, nullptr, &size);
        std::string log(size, '\0');
        if (size) api.GetProgramBuildInfo(program, listed.device, cl::kProgramBuildLog, size, log.data(), nullptr);
        while (!log.empty() && log.back() == '\0') log.pop_back();
        api.ReleaseProgram(program); api.ReleaseCommandQueue(queue); api.ReleaseContext(context);
        return "compile-failed " + (log.empty() ? CallFailure("clBuildProgram", rc).substr(13) : log);
    }
    cl::Kernel kernel = api.CreateKernel(program, "qs_probe", &rc);
    if (!kernel || rc != cl::kSuccess) {
        api.ReleaseProgram(program); api.ReleaseCommandQueue(queue); api.ReleaseContext(context);
        return CallFailure("clCreateKernel", rc);
    }
    const size_t one{1};
    rc = api.EnqueueNDRangeKernel(queue, kernel, 1, nullptr, &one, &one, 0, nullptr, nullptr);
    std::string result;
    if (rc != cl::kSuccess) result = CallFailure("clEnqueueNDRangeKernel", rc);
    if (result.empty()) {
        rc = api.Finish(queue);
        if (rc != cl::kSuccess) result = CallFailure("clFinish", rc);
    }
    api.ReleaseKernel(kernel); api.ReleaseProgram(program); api.ReleaseCommandQueue(queue); api.ReleaseContext(context);
    return result;
}

std::vector<ListedDevice> EnumerateDevices(const OpenClApi& api, bool probe)
{
    cl::Uint platform_count{0};
    const cl::Int platform_rc = api.GetPlatformIDs(0, nullptr, &platform_count);
    if (platform_rc == cl::kPlatformNotFoundKhr || platform_count == 0) return {};
    if (platform_rc != cl::kSuccess) return {};
    std::vector<cl::PlatformId> platforms(platform_count);
    if (api.GetPlatformIDs(platform_count, platforms.data(), nullptr) != cl::kSuccess) return {};

    std::vector<ListedDevice> result;
    for (size_t pi{0}; pi < platforms.size(); ++pi) {
        cl::Uint device_count{0};
        const cl::Int count_rc = api.GetDeviceIDs(platforms[pi], cl::kDeviceTypeAll, 0, nullptr, &device_count);
        if (count_rc == cl::kDeviceNotFound || device_count == 0) continue;
        if (count_rc != cl::kSuccess) continue;
        std::vector<cl::DeviceId> devices(device_count);
        if (api.GetDeviceIDs(platforms[pi], cl::kDeviceTypeAll, device_count, devices.data(), nullptr) != cl::kSuccess) continue;
        for (size_t di{0}; di < devices.size(); ++di) {
            ListedDevice listed{platforms[pi], devices[di], {}};
            auto& fields = listed.fields;
            fields.backend = "opencl";
            cl::DeviceType type{0};
            Get(api, listed.device, cl::kDeviceType, type);
            fields.type = (type & cl::kDeviceTypeGpu) ? "gpu" : (type & cl::kDeviceTypeCpu) ? "cpu" : "accel";
            cl::Bool unified{0};
            fields.integrated = fields.type == "gpu" && Get(api, listed.device, cl::kDeviceHostUnifiedMemory, unified) && unified;
            Get(api, listed.device, cl::kDeviceGlobalMemSize, fields.mem);
            fields.name = GetString(api, listed.device, cl::kDeviceName);
            const std::string extensions = GetString(api, listed.device, cl::kDeviceExtensions);
            fields.id = FallbackId(pi, di);
            if (HasExtension(extensions, "cl_khr_pci_bus_info")) {
                cl::PciBusInfoKhr pci{};
                if (Get(api, listed.device, cl::kDevicePciBusInfoKhr, pci)) fields.id = FormatPciId(pci.pci_domain, pci.pci_bus, pci.pci_device, pci.pci_function);
            } else if (HasExtension(extensions, "cl_amd_device_attribute_query")) {
                cl::TopologyAmd topology{};
                if (Get(api, listed.device, cl::kDeviceTopologyAmd, topology) && topology.raw.type == cl::kDeviceTopologyTypePcieAmd) {
                    fields.id = FormatPciId(0, static_cast<unsigned char>(topology.pcie.bus), static_cast<unsigned char>(topology.pcie.device), static_cast<unsigned char>(topology.pcie.function));
                }
            } else if (HasExtension(extensions, "cl_nv_device_attribute_query")) {
                // NVIDIA packs the PCI device (bits 7-3) and function (bits 2-0) into the slot id.
                cl::Uint bus{0}, slot{0}, domain{0};
                if (Get(api, listed.device, cl::kDevicePciBusIdNv, bus) && Get(api, listed.device, cl::kDevicePciSlotIdNv, slot)) {
                    Get(api, listed.device, cl::kDevicePciDomainIdNv, domain);
                    fields.id = FormatPciId(domain, bus, slot >> 3, slot & 7);
                }
            }
            cl::Bool available{0}, compiler{0};
            if (!Get(api, listed.device, cl::kDeviceAvailable, available) || !available) {
                fields.status = "no-driver"; fields.reason = "device-unavailable";
            } else if (!Get(api, listed.device, cl::kDeviceCompilerAvailable, compiler) || !compiler) {
                fields.status = "no-driver"; fields.reason = "no-compiler";
            } else if (probe) {
                const std::string failure = CompileAndProbe(api, listed);
                if (failure.empty()) fields.status = "ok";
                else {
                    const size_t space = failure.find(' ');
                    fields.status = failure.substr(0, space);
                    fields.reason = space == std::string::npos ? failure : failure.substr(space + 1);
                }
            } else {
                fields.status = "ok";
            }
            result.push_back(std::move(listed));
        }
    }
    return result;
}
} // namespace qsgpu
