// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <crypto/cuckatoo/gpu/opencl_devices.h>

#include <util/fs.h>
#include <util/strencodings.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
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

bool IsHex(std::string_view text)
{
    return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

// The render minor a node name carries: "renderD128" -> 128. Nodes are named for their
// DRM minor, so this orders them the way their names do, without assuming a width.
bool RenderNodeMinor(std::string_view name, unsigned long& minor)
{
    constexpr std::string_view prefix{"renderD"};
    if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) return false;
    const std::string_view digits{name.substr(prefix.size())};
    if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) return false;
    // strtoul is locale-dependent and the lint forbids it; ToIntegral is not, and it
    // also requires the whole view to parse. The digits are checked above, so the only
    // way this fails is a minor too large for the type -- which is not a render node.
    const auto parsed = ToIntegral<unsigned long>(digits);
    if (!parsed) return false;
    minor = *parsed;
    return true;
}

std::string SlotNameFromUevent(const fs::path& uevent)
{
    std::ifstream stream{uevent};
    if (!stream) return {};
    constexpr std::string_view key{"PCI_SLOT_NAME="};
    std::string line;
    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.compare(0, key.size(), key) == 0) return line.substr(key.size());
    }
    return {};
}

// Whether this platform's devices can be paired with the render nodes one for one. The
// bar is deliberately high: a mapping that can name the wrong card would send a
// transfer's work to a card the user did not choose (F-514).
bool MapsOntoRenderNodes(const std::vector<ListedDevice>& devices, size_t first,
                         const std::vector<RenderNode>& nodes)
{
    const size_t count = devices.size() - first;
    if (count == 0 || count != nodes.size()) return false;
    for (size_t i{0}; i < nodes.size(); ++i) {
        // Every node must carry an address, and no two may carry the same one.
        if (nodes[i].pci_id.empty()) return false;
        for (size_t j{0}; j < i; ++j) if (nodes[i].pci_id == nodes[j].pci_id) return false;
    }
    for (size_t i{first}; i < devices.size(); ++i) {
        // Only cards have render nodes: a CPU device (PoCL in CI) keeps its index id.
        if (devices[i].fields.type != "gpu") return false;
        // A slot address from the driver is the better answer, and a platform where only
        // some devices answered says nothing about which node the others are.
        if (devices[i].fields.id != devices[i].enumeration_id) return false;
    }
    return true;
}

// Apply the render-node addresses to every platform that maps onto them, or to none.
// F-544: the mapping is decided per platform, but the ids it prints are global. Two
// slot-silent platforms over the same cards each map onto the one node list, so both
// would be given the same pci: ids and SelectListedDevice would hand -cuckatoodevice
// the first match rather than the platform the user meant. A printed id that can name
// the wrong card is worse than a less precise one -- the bar MapsOntoRenderNodes sets
// above -- so on a collision every platform keeps its enumeration id, which is the
// answer this code gave before F-514.
void ApplyRenderNodeIds(std::vector<ListedDevice>& devices, const std::vector<size_t>& mapped,
                        const std::vector<RenderNode>& nodes)
{
    std::vector<std::string> proposed;
    proposed.reserve(devices.size());
    for (const auto& device : devices) proposed.push_back(device.fields.id);
    std::vector<bool> from_node(devices.size(), false);
    for (const size_t first : mapped) {
        for (size_t i{0}; i < nodes.size(); ++i) {
            proposed[first + i] = nodes[i].pci_id;
            from_node[first + i] = true;
        }
    }
    // Only a duplicate the mapping would create refuses it. Ids a driver reported for
    // itself are not this code's to arbitrate: two slot-reporting platforms over the
    // same cards collided that way before F-514 too.
    for (size_t i{0}; i < proposed.size(); ++i) {
        for (size_t j{0}; j < i; ++j) {
            if (proposed[i] == proposed[j] && (from_node[i] || from_node[j])) return;
        }
    }
    for (size_t i{0}; i < devices.size(); ++i) devices[i].fields.id = proposed[i];
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

std::string PciIdFromSlotName(std::string_view slot_name)
{
    // sysfs writes exactly dddd:bb:dd.f, lowercase hex.
    if (slot_name.size() != 12 || slot_name[4] != ':' || slot_name[7] != ':' || slot_name[10] != '.') return {};
    if (!IsHex(slot_name.substr(0, 4)) || !IsHex(slot_name.substr(5, 2)) || !IsHex(slot_name.substr(8, 2)) || !IsHex(slot_name.substr(11, 1))) return {};
    std::string out{"pci:"};
    out += slot_name;
    for (char& c : out) if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

std::vector<RenderNode> ScanRenderNodes(const DrmPaths& paths)
{
    std::error_code error;
    fs::directory_iterator entries{fs::PathFromString(paths.dev_dri), error};
    if (error) return {};
    std::vector<std::pair<unsigned long, RenderNode>> found;
    for (const auto& entry : entries) {
        const std::string name{fs::PathToString(entry.path().filename())};
        unsigned long minor{0};
        if (!RenderNodeMinor(name, minor)) continue;
        const fs::path uevent{fs::PathFromString(paths.sys_class_drm) / fs::u8path(name) / "device" / "uevent"};
        found.emplace_back(minor, RenderNode{name, PciIdFromSlotName(SlotNameFromUevent(uevent))});
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<RenderNode> nodes;
    nodes.reserve(found.size());
    for (auto& [minor, node] : found) nodes.push_back(std::move(node));
    return nodes;
}

const ListedDevice* SelectListedDevice(const std::vector<ListedDevice>& listed, const std::string& device)
{
    if (device.empty()) {
        const auto first_gpu = std::find_if(listed.begin(), listed.end(), [](const ListedDevice& candidate) { return candidate.fields.type == "gpu"; });
        return first_gpu == listed.end() ? nullptr : &*first_gpu;
    }
    // Either id selects the card: the printed one, or the enumeration-order one a choice
    // saved under an older listing carries. The two cannot collide -- one starts "pci:",
    // the other "opencl:" -- so one pass is enough.
    for (const auto& candidate : listed) {
        if (candidate.fields.id == device || candidate.enumeration_id == device) return &candidate;
    }
    return nullptr;
}

std::vector<ListedDevice> EnumerateDevices(const OpenClApi& api, bool probe, const std::vector<RenderNode>& render_nodes)
{
    cl::Uint platform_count{0};
    const cl::Int platform_rc = api.GetPlatformIDs(0, nullptr, &platform_count);
    if (platform_rc == cl::kPlatformNotFoundKhr || platform_count == 0) return {};
    if (platform_rc != cl::kSuccess) return {};
    std::vector<cl::PlatformId> platforms(platform_count);
    if (api.GetPlatformIDs(platform_count, platforms.data(), nullptr) != cl::kSuccess) return {};

    std::vector<ListedDevice> result;
    // Where each mapping platform's devices start. The addresses go on after every
    // platform has been enumerated, because a duplicate id refuses all of them (F-544).
    std::vector<size_t> mapped_platforms;
    for (size_t pi{0}; pi < platforms.size(); ++pi) {
        cl::Uint device_count{0};
        const cl::Int count_rc = api.GetDeviceIDs(platforms[pi], cl::kDeviceTypeAll, 0, nullptr, &device_count);
        if (count_rc == cl::kDeviceNotFound || device_count == 0) continue;
        if (count_rc != cl::kSuccess) continue;
        std::vector<cl::DeviceId> devices(device_count);
        if (api.GetDeviceIDs(platforms[pi], cl::kDeviceTypeAll, device_count, devices.data(), nullptr) != cl::kSuccess) continue;
        const size_t platform_first{result.size()};
        for (size_t di{0}; di < devices.size(); ++di) {
            ListedDevice listed{platforms[pi], devices[di], {}, {}};
            auto& fields = listed.fields;
            fields.backend = "opencl";
            cl::DeviceType type{0};
            Get(api, listed.device, cl::kDeviceType, type);
            fields.type = (type & cl::kDeviceTypeGpu) ? "gpu" : (type & cl::kDeviceTypeCpu) ? "cpu" : "accel";
            cl::Bool unified{0};
            fields.integrated = fields.type == "gpu" && Get(api, listed.device, cl::kDeviceHostUnifiedMemory, unified) && unified;
            Get(api, listed.device, cl::kDeviceGlobalMemSize, fields.mem);
            const std::string extensions = GetString(api, listed.device, cl::kDeviceExtensions);
            fields.name = GetString(api, listed.device, cl::kDeviceName);
            if (HasExtension(extensions, "cl_amd_device_attribute_query")) {
                std::string board_name = GetString(api, listed.device, cl::kDeviceBoardNameAmd);
                if (!board_name.empty()) fields.name = std::move(board_name);
            }
            listed.enumeration_id = FallbackId(pi, di);
            fields.id = listed.enumeration_id;
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
        if (MapsOntoRenderNodes(result, platform_first, render_nodes)) mapped_platforms.push_back(platform_first);
    }
    ApplyRenderNodeIds(result, mapped_platforms, render_nodes);
    return result;
}
} // namespace qsgpu
