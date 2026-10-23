// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef QUICKSILVER_CRYPTO_CUCKATOO_GPU_OPENCL_DEVICES_H
#define QUICKSILVER_CRYPTO_CUCKATOO_GPU_OPENCL_DEVICES_H

#include <crypto/cuckatoo/gpu/listing_format.h>
#include <crypto/cuckatoo/gpu/opencl_loader.h>

#include <string>
#include <string_view>
#include <vector>

namespace qsgpu {
struct ListedDevice {
    cl::PlatformId platform;
    cl::DeviceId device;
    ListingFields fields;
    // The enumeration-order id this device would have had without a slot address
    // ("opencl:<platform>:<index>"). Kept so a choice saved under an older listing still
    // selects this card; never printed -- listing_format.h is the node's contract.
    std::string enumeration_id;
};

// Where the DRM render nodes are looked up, so the walk can be tested without hardware.
struct DrmPaths {
    std::string dev_dri{"/dev/dri"};
    std::string sys_class_drm{"/sys/class/drm"};
};

struct RenderNode {
    std::string name;    // "renderD128"
    std::string pci_id;  // "pci:0000:03:00.0", empty when sysfs reports no slot name
};

// "0000:03:00.0" -> "pci:0000:03:00.0"; empty for anything that is not sysfs's own
// fixed-width domain:bus:device.function.
std::string PciIdFromSlotName(std::string_view slot_name);

// The render nodes this process can see, in node-name order, each with the PCI address
// sysfs reports for it. Only /dev/dri entries are returned: /sys/class/drm lists every
// node on the box, including cards this process was not given.
std::vector<RenderNode> ScanRenderNodes(const DrmPaths& paths = {});

// render_nodes is used only for devices whose driver reports no slot address, and only
// when the mapping is unambiguous (see MapsOntoRenderNodes in the .cpp).
std::vector<ListedDevice> EnumerateDevices(const OpenClApi& api, bool probe, const std::vector<RenderNode>& render_nodes);

// The device a --device argument names: by printed id or by enumeration-order id, or the
// first GPU when the argument is empty. nullptr when nothing matches.
const ListedDevice* SelectListedDevice(const std::vector<ListedDevice>& listed, const std::string& device);
const char* ProbeKernelSource();
std::string CompileAndProbe(const OpenClApi& api, const ListedDevice& device);
} // namespace qsgpu

#endif // QUICKSILVER_CRYPTO_CUCKATOO_GPU_OPENCL_DEVICES_H
