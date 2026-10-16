// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef QUICKSILVER_CRYPTO_CUCKATOO_GPU_OPENCL_DEVICES_H
#define QUICKSILVER_CRYPTO_CUCKATOO_GPU_OPENCL_DEVICES_H

#include <crypto/cuckatoo/gpu/listing_format.h>
#include <crypto/cuckatoo/gpu/opencl_loader.h>

#include <string>
#include <vector>

namespace qsgpu {
struct ListedDevice {
    cl::PlatformId platform;
    cl::DeviceId device;
    ListingFields fields;
};

std::vector<ListedDevice> EnumerateDevices(const OpenClApi& api, bool probe);
const char* ProbeKernelSource();
std::string CompileAndProbe(const OpenClApi& api, const ListedDevice& device);
} // namespace qsgpu

#endif // QUICKSILVER_CRYPTO_CUCKATOO_GPU_OPENCL_DEVICES_H
