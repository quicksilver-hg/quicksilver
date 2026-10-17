// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <crypto/cuckatoo/gpu/listing_format.h>
#include <crypto/cuckatoo/gpu/opencl_devices.h>
#include <crypto/cuckatoo/gpu_devices.h>

#include <boost/test/unit_test.hpp>

#include <cstring>

namespace {
struct FakeDeviceInfo {
    std::string extensions;
    std::string device_name;
    std::string board_name;
    bool board_query_fails{false};
};

FakeDeviceInfo g_device;
int g_board_queries{0};

template <typename T>
qsgpu::cl::Int ReturnValue(const T& value, size_t size, void* out, size_t* size_out)
{
    if (size_out) *size_out = sizeof(value);
    if (out) {
        if (size < sizeof(value)) return -30;
        std::memcpy(out, &value, sizeof(value));
    }
    return qsgpu::cl::kSuccess;
}

qsgpu::cl::Int ReturnString(const std::string& value, size_t size, void* out, size_t* size_out)
{
    if (size_out) *size_out = value.size();
    if (out) {
        if (size < value.size()) return -30;
        std::memcpy(out, value.data(), value.size());
    }
    return qsgpu::cl::kSuccess;
}

qsgpu::cl::Int QS_CL_API_CALL GetPlatformIDs(qsgpu::cl::Uint count, qsgpu::cl::PlatformId* platforms, qsgpu::cl::Uint* count_out)
{
    if (count_out) *count_out = 1;
    if (platforms && count > 0) platforms[0] = reinterpret_cast<qsgpu::cl::PlatformId>(1);
    return qsgpu::cl::kSuccess;
}

qsgpu::cl::Int QS_CL_API_CALL GetDeviceIDs(qsgpu::cl::PlatformId, qsgpu::cl::DeviceType, qsgpu::cl::Uint count,
                                            qsgpu::cl::DeviceId* devices, qsgpu::cl::Uint* count_out)
{
    if (count_out) *count_out = 1;
    if (devices && count > 0) devices[0] = reinterpret_cast<qsgpu::cl::DeviceId>(1);
    return qsgpu::cl::kSuccess;
}

qsgpu::cl::Int QS_CL_API_CALL GetDeviceInfo(qsgpu::cl::DeviceId, qsgpu::cl::Uint property, size_t size, void* out, size_t* size_out)
{
    using namespace qsgpu;
    if (property == cl::kDeviceType) return ReturnValue(cl::kDeviceTypeGpu, size, out, size_out);
    if (property == cl::kDeviceHostUnifiedMemory || property == cl::kDeviceAvailable || property == cl::kDeviceCompilerAvailable) {
        const cl::Bool value{property == cl::kDeviceHostUnifiedMemory ? 0u : cl::kTrue};
        return ReturnValue(value, size, out, size_out);
    }
    if (property == cl::kDeviceGlobalMemSize) return ReturnValue(cl::Ulong{8589934592ULL}, size, out, size_out);
    if (property == cl::kDeviceName) return ReturnString(g_device.device_name, size, out, size_out);
    if (property == cl::kDeviceExtensions) return ReturnString(g_device.extensions, size, out, size_out);
    if (property == 0x4038) {
        ++g_board_queries;
        if (g_device.board_query_fails) return -30;
        return ReturnString(g_device.board_name, size, out, size_out);
    }
    if (property == cl::kDeviceTopologyAmd) {
        cl::TopologyAmd topology{};
        topology.raw.type = cl::kDeviceTopologyTypePcieAmd;
        topology.pcie.bus = 2;
        return ReturnValue(topology, size, out, size_out);
    }
    return -30;
}

qsgpu::OpenClApi FakeApi()
{
    qsgpu::OpenClApi api{};
    api.GetPlatformIDs = GetPlatformIDs;
    api.GetDeviceIDs = GetDeviceIDs;
    api.GetDeviceInfo = GetDeviceInfo;
    return api;
}

std::string ListedName(const FakeDeviceInfo& device)
{
    g_device = device;
    g_board_queries = 0;
    const auto listed = qsgpu::EnumerateDevices(FakeApi(), false);
    BOOST_REQUIRE_EQUAL(listed.size(), 1U);
    return listed.front().fields.name;
}
} // namespace

BOOST_AUTO_TEST_SUITE(opencl_devices_tests)

BOOST_AUTO_TEST_CASE(pci_id_is_fixed_width_lowercase)
{
    BOOST_CHECK_EQUAL(qsgpu::FormatPciId(0, 4, 0, 0), "pci:0000:04:00.0");
    BOOST_CHECK_EQUAL(qsgpu::FormatPciId(0x1, 0xab, 0x1f, 7), "pci:0001:ab:1f.7");
}

BOOST_AUTO_TEST_CASE(listing_line_puts_name_last_and_omits_reason_when_ok)
{
    qsgpu::ListingFields d{};
    d.backend = "opencl"; d.id = "pci:0000:04:00.0"; d.type = "gpu";
    d.mem = 8589934592ULL; d.status = "ok";
    d.name = "AMD Radeon RX 480 Graphics (radeonsi, polaris10)";
    BOOST_CHECK_EQUAL(qsgpu::FormatListingLine(d),
        "device backend=opencl id=pci:0000:04:00.0 type=gpu integrated=0 mem=8589934592 status=ok "
        "name=AMD Radeon RX 480 Graphics (radeonsi, polaris10)");
}

BOOST_AUTO_TEST_CASE(failure_reason_is_one_token)
{
    qsgpu::ListingFields d{};
    d.backend = "opencl"; d.id = "opencl:1:0"; d.type = "gpu";
    d.integrated = true; d.status = "compile-failed";
    d.reason = "error: use of undeclared identifier\nline 2";
    d.name = "Intel(R) HD Graphics 4600";
    BOOST_CHECK_EQUAL(qsgpu::FormatListingLine(d),
        "device backend=opencl id=opencl:1:0 type=gpu integrated=1 mem=0 status=compile-failed "
        "reason=error:_use_of_undeclared_identifier_line_2 name=Intel(R) HD Graphics 4600");
}

BOOST_AUTO_TEST_CASE(newlines_in_a_device_name_cannot_forge_a_line)
{
    qsgpu::ListingFields d{};
    d.backend = "opencl"; d.id = "pci:0000:01:00.0"; d.type = "gpu"; d.status = "ok";
    d.name = "Evil\ndevice backend=opencl id=pci:0000:09:00.0";
    const std::string line = qsgpu::FormatListingLine(d);
    BOOST_CHECK(line.find('\n') == std::string::npos);
    BOOST_CHECK(line.find('\r') == std::string::npos);
}

BOOST_AUTO_TEST_CASE(amd_board_name_is_preferred_and_round_trips_with_spaces)
{
    std::string board{"Radeon RX 570 Series"};
    board.append("\0  ", 3);
    g_device = {"cl_amd_device_attribute_query\0", "Ellesmere\0", board, false};
    g_board_queries = 0;
    const auto listed = qsgpu::EnumerateDevices(FakeApi(), false);
    BOOST_REQUIRE_EQUAL(listed.size(), 1U);
    BOOST_CHECK_EQUAL(g_board_queries, 2);
    BOOST_CHECK_EQUAL(listed.front().fields.name, "Radeon RX 570 Series");

    const auto parsed = cuckatoo::ParseDeviceLine(qsgpu::FormatListingLine(listed.front().fields));
    BOOST_REQUIRE(parsed.has_value());
    BOOST_CHECK_EQUAL(parsed->name, "Radeon RX 570 Series");
}

BOOST_AUTO_TEST_CASE(amd_empty_or_failed_board_name_falls_back_to_device_name)
{
    BOOST_CHECK_EQUAL(ListedName({"cl_amd_device_attribute_query\0", "Ellesmere\0", "\0   ", false}), "Ellesmere");
    BOOST_CHECK_EQUAL(g_board_queries, 1);
    BOOST_CHECK_EQUAL(ListedName({"cl_amd_device_attribute_query\0", "Ellesmere\0", {}, true}), "Ellesmere");
    BOOST_CHECK_EQUAL(g_board_queries, 1);
}

BOOST_AUTO_TEST_CASE(device_without_amd_extension_uses_device_name)
{
    BOOST_CHECK_EQUAL(ListedName({"cl_khr_pci_bus_info\0", "Intel Arc A770\0", "Ignored board\0", false}), "Intel Arc A770");
    BOOST_CHECK_EQUAL(g_board_queries, 0);
}

BOOST_AUTO_TEST_SUITE_END()
