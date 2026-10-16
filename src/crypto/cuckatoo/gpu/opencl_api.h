// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// The OpenCL 1.2 declarations qsgpusolve-opencl uses, and nothing else.
#ifndef QUICKSILVER_CRYPTO_CUCKATOO_GPU_OPENCL_API_H
#define QUICKSILVER_CRYPTO_CUCKATOO_GPU_OPENCL_API_H

#include <cstddef>
#include <cstdint>

#if defined(_WIN32) && !defined(_WIN64)
#define QS_CL_API_CALL __stdcall
#else
#define QS_CL_API_CALL
#endif

namespace qsgpu::cl {
using Int = int32_t;
using Uint = uint32_t;
using Ulong = uint64_t;
using Bitfield = Ulong;
using Bool = Uint;
using DeviceType = Bitfield;
using MemFlags = Bitfield;
using QueueProperties = Bitfield;
using ContextProperties = intptr_t;
using PlatformId = struct _cl_platform_id*;
using DeviceId = struct _cl_device_id*;
using Context = struct _cl_context*;
using CommandQueue = struct _cl_command_queue*;
using Mem = struct _cl_mem*;
using Program = struct _cl_program*;
using Kernel = struct _cl_kernel*;
using Event = struct _cl_event*;

inline constexpr Int kSuccess{0};
inline constexpr Int kDeviceNotFound{-1};
inline constexpr Int kPlatformNotFoundKhr{-1001};
inline constexpr DeviceType kDeviceTypeCpu{1u << 1};
inline constexpr DeviceType kDeviceTypeGpu{1u << 2};
inline constexpr DeviceType kDeviceTypeAccelerator{1u << 3};
inline constexpr DeviceType kDeviceTypeAll{0xFFFFFFFFu};
inline constexpr Uint kPlatformName{0x0902};
inline constexpr Uint kDeviceType{0x1000};
inline constexpr Uint kDeviceVendorId{0x1001};
inline constexpr Uint kDeviceGlobalMemSize{0x101F};
inline constexpr Uint kDeviceAvailable{0x1027};
inline constexpr Uint kDeviceCompilerAvailable{0x1028};
inline constexpr Uint kDeviceName{0x102B};
inline constexpr Uint kDeviceExtensions{0x1030};
inline constexpr Uint kDeviceHostUnifiedMemory{0x1035};
inline constexpr MemFlags kMemReadWrite{1u << 0};
inline constexpr Uint kProgramBuildLog{0x1183};
inline constexpr Uint kKernelWorkGroupSize{0x11B0};
inline constexpr Bool kTrue{1};
inline constexpr Uint kDevicePciBusInfoKhr{0x410F};
struct PciBusInfoKhr { Uint pci_domain; Uint pci_bus; Uint pci_device; Uint pci_function; };
inline constexpr Uint kDeviceTopologyAmd{0x4037};
inline constexpr Uint kDeviceTopologyTypePcieAmd{1};
union TopologyAmd {
    struct { Uint type; Uint data[5]; } raw;
    struct { Uint type; char unused[17]; char bus; char device; char function; } pcie;
};
inline constexpr Uint kDevicePciBusIdNv{0x4008};
inline constexpr Uint kDevicePciSlotIdNv{0x4009};
inline constexpr Uint kDevicePciDomainIdNv{0x400A};
} // namespace qsgpu::cl

#endif // QUICKSILVER_CRYPTO_CUCKATOO_GPU_OPENCL_API_H
