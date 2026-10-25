// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <crypto/cuckatoo/gpu/listing_format.h>
#include <crypto/cuckatoo/gpu/opencl_devices.h>
#include <crypto/cuckatoo/gpu_devices.h>
#include <test/util/setup_common.h>
#include <util/fs.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {
struct FakeDeviceInfo {
    std::string extensions;
    std::string device_name;
    std::string board_name;
    bool board_query_fails{false};
    qsgpu::cl::DeviceType type{qsgpu::cl::kDeviceTypeGpu};
    unsigned pci_bus{0};
};

FakeDeviceInfo g_device;
// Only for the several-device cases; when it is empty the fakes answer for g_device alone.
std::vector<FakeDeviceInfo> g_devices;
int g_board_queries{0};
// How many platforms the fake loader reports. Every platform enumerates the same
// devices, which is the shape F-544 is about: two loaders over one set of cards.
int g_platforms{1};

//! Report several platforms for one case, and put the count back however the case ends:
//! a BOOST_REQUIRE failure throws, and a leaked count would reach the next case.
struct PlatformCountGuard {
    explicit PlatformCountGuard(int count) { g_platforms = count; }
    ~PlatformCountGuard() { g_platforms = 1; }
};

const FakeDeviceInfo& DeviceAt(qsgpu::cl::DeviceId device)
{
    if (g_devices.empty()) return g_device;
    const auto index = reinterpret_cast<uintptr_t>(device) - 1;
    BOOST_REQUIRE(index < g_devices.size());
    return g_devices[index];
}

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
    const qsgpu::cl::Uint total = static_cast<qsgpu::cl::Uint>(g_platforms);
    if (count_out) *count_out = total;
    if (platforms) {
        for (qsgpu::cl::Uint i = 0; i < count && i < total; ++i) {
            platforms[i] = reinterpret_cast<qsgpu::cl::PlatformId>(static_cast<uintptr_t>(i) + 1);
        }
    }
    return qsgpu::cl::kSuccess;
}

qsgpu::cl::Int QS_CL_API_CALL GetDeviceIDs(qsgpu::cl::PlatformId, qsgpu::cl::DeviceType, qsgpu::cl::Uint count,
                                            qsgpu::cl::DeviceId* devices, qsgpu::cl::Uint* count_out)
{
    const qsgpu::cl::Uint total = g_devices.empty() ? 1 : static_cast<qsgpu::cl::Uint>(g_devices.size());
    if (count_out) *count_out = total;
    if (devices) {
        // DeviceAt() decodes an id back through uintptr_t, so encode through it too. Casting the 32-bit
        // Uint straight to a pointer is C4312 under MSVC /WX, where DeviceId is 64 bits wide.
        for (qsgpu::cl::Uint i = 0; i < count && i < total; ++i) {
            devices[i] = reinterpret_cast<qsgpu::cl::DeviceId>(static_cast<uintptr_t>(i) + 1);
        }
    }
    return qsgpu::cl::kSuccess;
}

bool HasFakeExtension(const std::string& extensions, const std::string& extension)
{
    return extensions.find(extension) != std::string::npos;
}

qsgpu::cl::Int QS_CL_API_CALL GetDeviceInfo(qsgpu::cl::DeviceId device, qsgpu::cl::Uint property, size_t size, void* out, size_t* size_out)
{
    using namespace qsgpu;
    const FakeDeviceInfo& info = DeviceAt(device);
    if (property == cl::kDeviceType) return ReturnValue(info.type, size, out, size_out);
    if (property == cl::kDevicePciBusInfoKhr) {
        if (!HasFakeExtension(info.extensions, "cl_khr_pci_bus_info")) return -30;
        cl::PciBusInfoKhr pci{};
        pci.pci_domain = 0;
        pci.pci_bus = info.pci_bus;
        pci.pci_device = 0;
        pci.pci_function = 0;
        return ReturnValue(pci, size, out, size_out);
    }
    if (property == cl::kDeviceHostUnifiedMemory || property == cl::kDeviceAvailable || property == cl::kDeviceCompilerAvailable) {
        const cl::Bool value{property == cl::kDeviceHostUnifiedMemory ? 0u : cl::kTrue};
        return ReturnValue(value, size, out, size_out);
    }
    if (property == cl::kDeviceGlobalMemSize) return ReturnValue(cl::Ulong{8589934592ULL}, size, out, size_out);
    if (property == cl::kDeviceName) return ReturnString(info.device_name, size, out, size_out);
    if (property == cl::kDeviceExtensions) return ReturnString(info.extensions, size, out, size_out);
    if (property == 0x4038) {
        ++g_board_queries;
        if (info.board_query_fails) return -30;
        return ReturnString(info.board_name, size, out, size_out);
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

const std::vector<qsgpu::RenderNode> kNoRenderNodes{};

std::string ListedName(const FakeDeviceInfo& device)
{
    g_device = device;
    g_devices.clear();
    g_board_queries = 0;
    const auto listed = qsgpu::EnumerateDevices(FakeApi(), false, kNoRenderNodes);
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
    g_devices.clear();
    g_board_queries = 0;
    const auto listed = qsgpu::EnumerateDevices(FakeApi(), false, kNoRenderNodes);
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

// F-514: where no slot extension answers (Mesa clover on 22.04), the id comes from the
// device's DRM render node. Measured on precision 2026-10-23: clover enumerates its
// devices in render-node *name* order, not PCI order and not /dev/dri readdir order
// (gates/f514-364a1597/list-devices-measurement.log, cases 1, 4 and 5).

namespace {
// Writes <root>/dev/dri/<node> and <root>/sys/class/drm/<node>/device/uevent for each
// node, as the container measurement found them: /dev/dri holds only the nodes the
// process can see, while /sys/class/drm lists every node on the box.
qsgpu::DrmPaths FakeDrmTree(const fs::path& root,
                            const std::vector<std::pair<std::string, std::string>>& dev_nodes,
                            const std::vector<std::pair<std::string, std::string>>& sysfs_only = {})
{
    const fs::path dev_dri{root / "dev" / "dri"};
    fs::create_directories(dev_dri);
    for (const auto& [node, slot] : dev_nodes) {
        std::ofstream{dev_dri / fs::u8path(node)} << "not a device node, but it is named like one\n";
    }
    auto write_sysfs = [&](const std::string& node, const std::string& slot) {
        const fs::path device_dir{root / "sys" / "class" / "drm" / fs::u8path(node) / "device"};
        fs::create_directories(device_dir);
        std::ofstream uevent{device_dir / "uevent"};
        uevent << "DRIVER=amdgpu\nPCI_CLASS=30000\nPCI_ID=1002:67DF\n";
        if (!slot.empty()) uevent << "PCI_SLOT_NAME=" << slot << "\n";
        uevent << "MODALIAS=pci:v00001002d000067DF\n";
    };
    for (const auto& [node, slot] : dev_nodes) write_sysfs(node, slot);
    for (const auto& [node, slot] : sysfs_only) write_sysfs(node, slot);

    qsgpu::DrmPaths paths;
    paths.dev_dri = fs::PathToString(dev_dri);
    paths.sys_class_drm = fs::PathToString(root / "sys" / "class" / "drm");
    return paths;
}

FakeDeviceInfo NoExtensionGpu()
{
    FakeDeviceInfo device{};
    device.extensions = "cl_khr_byte_addressable_store cl_khr_fp64";
    device.device_name = "AMD Radeon RX 480 Graphics (polaris10, LLVM 15.0.7)";
    return device;
}

std::vector<std::string> ListedIds(const std::vector<qsgpu::RenderNode>& nodes)
{
    g_board_queries = 0;
    std::vector<std::string> ids;
    for (const auto& listed : qsgpu::EnumerateDevices(FakeApi(), false, nodes)) ids.push_back(listed.fields.id);
    return ids;
}

const qsgpu::RenderNode kNode128{"renderD128", "pci:0000:03:00.0"};
const qsgpu::RenderNode kNode129{"renderD129", "pci:0000:04:00.0"};
} // namespace

BOOST_AUTO_TEST_CASE(a_slot_name_becomes_a_pci_id_and_a_malformed_one_is_refused)
{
    BOOST_CHECK_EQUAL(qsgpu::PciIdFromSlotName("0000:03:00.0"), "pci:0000:03:00.0");
    BOOST_CHECK_EQUAL(qsgpu::PciIdFromSlotName("0001:ab:1f.7"), "pci:0001:ab:1f.7");
    // Anything that is not domain:bus:device.function in sysfs's own fixed width is refused,
    // rather than passed through into an id the node would later parse.
    BOOST_CHECK_EQUAL(qsgpu::PciIdFromSlotName(""), "");
    BOOST_CHECK_EQUAL(qsgpu::PciIdFromSlotName("03:00.0"), "");
    BOOST_CHECK_EQUAL(qsgpu::PciIdFromSlotName("0000:03:00"), "");
    BOOST_CHECK_EQUAL(qsgpu::PciIdFromSlotName("0000:03:00.0 name=evil"), "");
    BOOST_CHECK_EQUAL(qsgpu::PciIdFromSlotName("zzzz:03:00.0"), "");
}

BOOST_FIXTURE_TEST_CASE(render_nodes_are_scanned_from_dev_dri_in_name_order, BasicTestingSetup)
{
    const auto paths = FakeDrmTree(m_path_root / "two-cards",
                                   {{"renderD129", "0000:04:00.0"}, {"renderD128", "0000:03:00.0"}});
    const auto nodes = qsgpu::ScanRenderNodes(paths);
    BOOST_REQUIRE_EQUAL(nodes.size(), 2U);
    BOOST_CHECK_EQUAL(nodes[0].name, "renderD128");
    BOOST_CHECK_EQUAL(nodes[0].pci_id, "pci:0000:03:00.0");
    BOOST_CHECK_EQUAL(nodes[1].name, "renderD129");
    BOOST_CHECK_EQUAL(nodes[1].pci_id, "pci:0000:04:00.0");
}

BOOST_FIXTURE_TEST_CASE(a_node_only_sysfs_knows_about_is_not_scanned, BasicTestingSetup)
{
    // The container measurement: with one card handed to the process, /dev/dri holds one
    // node while /sys/class/drm still lists both. The walk must follow /dev/dri.
    const auto paths = FakeDrmTree(m_path_root / "one-of-two",
                                   {{"renderD128", "0000:03:00.0"}}, {{"renderD129", "0000:04:00.0"}});
    const auto nodes = qsgpu::ScanRenderNodes(paths);
    BOOST_REQUIRE_EQUAL(nodes.size(), 1U);
    BOOST_CHECK_EQUAL(nodes[0].name, "renderD128");
    BOOST_CHECK_EQUAL(nodes[0].pci_id, "pci:0000:03:00.0");
}

BOOST_FIXTURE_TEST_CASE(a_node_without_a_slot_name_scans_with_no_pci_id, BasicTestingSetup)
{
    const auto paths = FakeDrmTree(m_path_root / "no-slot-name", {{"renderD128", ""}});
    const auto nodes = qsgpu::ScanRenderNodes(paths);
    BOOST_REQUIRE_EQUAL(nodes.size(), 1U);
    BOOST_CHECK_EQUAL(nodes[0].name, "renderD128");
    BOOST_CHECK_EQUAL(nodes[0].pci_id, "");
}

BOOST_FIXTURE_TEST_CASE(a_missing_dev_dri_scans_to_nothing, BasicTestingSetup)
{
    qsgpu::DrmPaths paths;
    paths.dev_dri = fs::PathToString(m_path_root / "absent" / "dri");
    paths.sys_class_drm = fs::PathToString(m_path_root / "absent" / "drm");
    BOOST_CHECK(qsgpu::ScanRenderNodes(paths).empty());
}

BOOST_AUTO_TEST_CASE(a_card_with_no_slot_extension_is_listed_by_its_render_nodes_address)
{
    g_device = NoExtensionGpu();
    g_devices.clear();
    const auto ids = ListedIds({kNode128});
    BOOST_REQUIRE_EQUAL(ids.size(), 1U);
    BOOST_CHECK_EQUAL(ids[0], "pci:0000:03:00.0");
}

BOOST_AUTO_TEST_CASE(two_cards_with_no_slot_extension_take_their_nodes_in_name_order)
{
    g_devices = {NoExtensionGpu(), NoExtensionGpu()};
    const auto ids = ListedIds({kNode128, kNode129});
    BOOST_REQUIRE_EQUAL(ids.size(), 2U);
    BOOST_CHECK_EQUAL(ids[0], "pci:0000:03:00.0");
    BOOST_CHECK_EQUAL(ids[1], "pci:0000:04:00.0");
}

BOOST_AUTO_TEST_CASE(a_card_with_no_render_node_keeps_its_enumeration_id)
{
    g_device = NoExtensionGpu();
    g_devices.clear();
    const auto ids = ListedIds({});
    BOOST_REQUIRE_EQUAL(ids.size(), 1U);
    BOOST_CHECK_EQUAL(ids[0], "opencl:0:0");
}

BOOST_AUTO_TEST_CASE(counts_that_disagree_keep_the_enumeration_id)
{
    // Two devices, one node: nothing says which device the node belongs to, so neither
    // takes it. An id that can name the wrong card is worse than an honest index.
    g_devices = {NoExtensionGpu(), NoExtensionGpu()};
    const auto two_devices_one_node = ListedIds({kNode128});
    BOOST_REQUIRE_EQUAL(two_devices_one_node.size(), 2U);
    BOOST_CHECK_EQUAL(two_devices_one_node[0], "opencl:0:0");
    BOOST_CHECK_EQUAL(two_devices_one_node[1], "opencl:0:1");

    g_devices = {NoExtensionGpu()};
    const auto one_device_two_nodes = ListedIds({kNode128, kNode129});
    BOOST_REQUIRE_EQUAL(one_device_two_nodes.size(), 1U);
    BOOST_CHECK_EQUAL(one_device_two_nodes[0], "opencl:0:0");
}

BOOST_AUTO_TEST_CASE(a_node_with_no_address_or_a_repeated_address_keeps_the_enumeration_id)
{
    g_devices = {NoExtensionGpu(), NoExtensionGpu()};
    const auto with_a_blank = ListedIds({kNode128, {"renderD129", ""}});
    BOOST_REQUIRE_EQUAL(with_a_blank.size(), 2U);
    BOOST_CHECK_EQUAL(with_a_blank[0], "opencl:0:0");
    BOOST_CHECK_EQUAL(with_a_blank[1], "opencl:0:1");

    g_devices = {NoExtensionGpu(), NoExtensionGpu()};
    const auto with_a_repeat = ListedIds({kNode128, {"renderD129", "pci:0000:03:00.0"}});
    BOOST_REQUIRE_EQUAL(with_a_repeat.size(), 2U);
    BOOST_CHECK_EQUAL(with_a_repeat[0], "opencl:0:0");
    BOOST_CHECK_EQUAL(with_a_repeat[1], "opencl:0:1");
}

BOOST_AUTO_TEST_CASE(a_cpu_device_keeps_its_enumeration_id_even_beside_a_render_node)
{
    // PoCL's platform in CI: CPU devices have no render node, and their ids must not move.
    FakeDeviceInfo cpu{NoExtensionGpu()};
    cpu.type = qsgpu::cl::kDeviceTypeCpu;
    cpu.device_name = "pthread-POWER";
    g_devices = {cpu};
    const auto one_cpu = ListedIds({kNode128});
    BOOST_REQUIRE_EQUAL(one_cpu.size(), 1U);
    BOOST_CHECK_EQUAL(one_cpu[0], "opencl:0:0");

    g_devices = {cpu, cpu};
    const auto two_cpus = ListedIds({kNode128, kNode129});
    BOOST_REQUIRE_EQUAL(two_cpus.size(), 2U);
    BOOST_CHECK_EQUAL(two_cpus[0], "opencl:0:0");
    BOOST_CHECK_EQUAL(two_cpus[1], "opencl:0:1");
}

BOOST_AUTO_TEST_CASE(a_slot_extension_still_wins_over_the_render_nodes_address)
{
    FakeDeviceInfo with_slot{NoExtensionGpu()};
    with_slot.extensions = "cl_khr_pci_bus_info cl_khr_fp64";
    with_slot.pci_bus = 0x0c;
    g_devices = {with_slot};
    const auto ids = ListedIds({kNode128});
    BOOST_REQUIRE_EQUAL(ids.size(), 1U);
    BOOST_CHECK_EQUAL(ids[0], "pci:0000:0c:00.0");
}

BOOST_AUTO_TEST_CASE(the_enumeration_id_stays_on_the_device_without_being_printed)
{
    g_devices = {NoExtensionGpu(), NoExtensionGpu()};
    const auto listed = qsgpu::EnumerateDevices(FakeApi(), false, {kNode128, kNode129});
    BOOST_REQUIRE_EQUAL(listed.size(), 2U);
    BOOST_CHECK_EQUAL(listed[0].enumeration_id, "opencl:0:0");
    BOOST_CHECK_EQUAL(listed[1].enumeration_id, "opencl:0:1");
    // The printed contract keeps its fields: the enumeration id is not one of them.
    const std::string line = qsgpu::FormatListingLine(listed[1].fields);
    BOOST_CHECK_EQUAL(line.find("opencl:0:1"), std::string::npos);
    const auto parsed = cuckatoo::ParseDeviceLine(line);
    BOOST_REQUIRE(parsed.has_value());
    BOOST_CHECK_EQUAL(parsed->id, "pci:0000:04:00.0");
}

BOOST_AUTO_TEST_CASE(a_saved_choice_selects_its_card_by_either_id)
{
    g_devices = {NoExtensionGpu(), NoExtensionGpu()};
    const auto listed = qsgpu::EnumerateDevices(FakeApi(), false, {kNode128, kNode129});
    BOOST_REQUIRE_EQUAL(listed.size(), 2U);

    // A 22.04 user who chose the second card under the old listing still gets that card.
    const qsgpu::ListedDevice* by_old_id = qsgpu::SelectListedDevice(listed, "opencl:0:1");
    BOOST_REQUIRE(by_old_id != nullptr);
    BOOST_CHECK_EQUAL(by_old_id->fields.id, "pci:0000:04:00.0");

    const qsgpu::ListedDevice* by_new_id = qsgpu::SelectListedDevice(listed, "pci:0000:04:00.0");
    BOOST_REQUIRE(by_new_id != nullptr);
    BOOST_CHECK_EQUAL(by_new_id->fields.id, "pci:0000:04:00.0");

    // No -device at all: the first GPU, as before.
    const qsgpu::ListedDevice* first_gpu = qsgpu::SelectListedDevice(listed, "");
    BOOST_REQUIRE(first_gpu != nullptr);
    BOOST_CHECK_EQUAL(first_gpu->fields.id, "pci:0000:03:00.0");

    BOOST_CHECK(qsgpu::SelectListedDevice(listed, "pci:0000:09:00.0") == nullptr);
    BOOST_CHECK(qsgpu::SelectListedDevice(listed, "opencl:0:7") == nullptr);
}

BOOST_AUTO_TEST_CASE(two_slot_silent_platforms_over_one_card_list_keep_their_enumeration_ids)
{
    // F-544: the render-node mapping is decided per platform, but the id namespace is
    // global. Two platforms that each enumerate the same slot-silent cards each map onto
    // the one render-node list, so both would print the same pci: ids -- and
    // SelectListedDevice would then hand -cuckatoodevice the first match rather than the
    // platform the user meant. A duplicate id refuses the mapping for every platform:
    // every device keeps its enumeration id, which is the pre-F-514 answer and names
    // exactly one device.
    PlatformCountGuard platforms{2};
    g_devices = {NoExtensionGpu(), NoExtensionGpu()};
    const auto listed = qsgpu::EnumerateDevices(FakeApi(), false, {kNode128, kNode129});
    BOOST_REQUIRE_EQUAL(listed.size(), 4U);
    std::vector<std::string> ids;
    for (const auto& device : listed) {
        BOOST_CHECK_EQUAL(device.fields.id, device.enumeration_id);
        ids.push_back(device.fields.id);
    }
    BOOST_CHECK_EQUAL(ids[0], "opencl:0:0");
    BOOST_CHECK_EQUAL(ids[1], "opencl:0:1");
    BOOST_CHECK_EQUAL(ids[2], "opencl:1:0");
    BOOST_CHECK_EQUAL(ids[3], "opencl:1:1");
    // Every printed id names one card: that is the property the refusal protects.
    for (size_t i{0}; i < ids.size(); ++i) {
        for (size_t j{0}; j < i; ++j) BOOST_CHECK(ids[i] != ids[j]);
        BOOST_CHECK(qsgpu::SelectListedDevice(listed, ids[i]) == &listed[i]);
    }
}

BOOST_AUTO_TEST_SUITE_END()
