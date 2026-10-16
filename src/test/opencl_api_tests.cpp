// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// opencl_api.h declares the OpenCL types and constants the helper uses by hand,
// so the build needs no OpenCL headers (F-469 ruling R1). A wrong constant would
// query the wrong property and fail quietly, so wherever the Khronos headers ARE
// installed, every value is compared with theirs here.
#include <crypto/cuckatoo/gpu/opencl_api.h>
#include <crypto/cuckatoo/gpu/opencl_loader.h>

#include <boost/test/unit_test.hpp>

#if __has_include(<CL/cl.h>) && __has_include(<CL/cl_ext.h>)
#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>
#include <CL/cl_ext.h>
#define QS_HAVE_KHRONOS_HEADERS 1
#endif

BOOST_AUTO_TEST_SUITE(opencl_api_tests)

BOOST_AUTO_TEST_CASE(constants_match_khronos)
{
#ifdef QS_HAVE_KHRONOS_HEADERS
    static_assert(qsgpu::cl::kSuccess == CL_SUCCESS);
    static_assert(qsgpu::cl::kDeviceNotFound == CL_DEVICE_NOT_FOUND);
    static_assert(qsgpu::cl::kPlatformNotFoundKhr == CL_PLATFORM_NOT_FOUND_KHR);
    static_assert(qsgpu::cl::kDeviceTypeCpu == CL_DEVICE_TYPE_CPU);
    static_assert(qsgpu::cl::kDeviceTypeGpu == CL_DEVICE_TYPE_GPU);
    static_assert(qsgpu::cl::kDeviceTypeAccelerator == CL_DEVICE_TYPE_ACCELERATOR);
    static_assert(qsgpu::cl::kDeviceTypeAll == CL_DEVICE_TYPE_ALL);
    static_assert(qsgpu::cl::kPlatformName == CL_PLATFORM_NAME);
    static_assert(qsgpu::cl::kDeviceType == CL_DEVICE_TYPE);
    static_assert(qsgpu::cl::kDeviceVendorId == CL_DEVICE_VENDOR_ID);
    static_assert(qsgpu::cl::kDeviceGlobalMemSize == CL_DEVICE_GLOBAL_MEM_SIZE);
    static_assert(qsgpu::cl::kDeviceAvailable == CL_DEVICE_AVAILABLE);
    static_assert(qsgpu::cl::kDeviceCompilerAvailable == CL_DEVICE_COMPILER_AVAILABLE);
    static_assert(qsgpu::cl::kDeviceName == CL_DEVICE_NAME);
    static_assert(qsgpu::cl::kDeviceExtensions == CL_DEVICE_EXTENSIONS);
    static_assert(qsgpu::cl::kDeviceHostUnifiedMemory == CL_DEVICE_HOST_UNIFIED_MEMORY);
    static_assert(qsgpu::cl::kMemReadWrite == CL_MEM_READ_WRITE);
    static_assert(qsgpu::cl::kProgramBuildLog == CL_PROGRAM_BUILD_LOG);
    static_assert(qsgpu::cl::kKernelWorkGroupSize == CL_KERNEL_WORK_GROUP_SIZE);
    static_assert(qsgpu::cl::kDevicePciBusInfoKhr == CL_DEVICE_PCI_BUS_INFO_KHR);
    static_assert(qsgpu::cl::kDeviceTopologyAmd == CL_DEVICE_TOPOLOGY_AMD);
    static_assert(qsgpu::cl::kDeviceTopologyTypePcieAmd == CL_DEVICE_TOPOLOGY_TYPE_PCIE_AMD);
#ifdef CL_DEVICE_PCI_BUS_ID_NV
    static_assert(qsgpu::cl::kDevicePciBusIdNv == CL_DEVICE_PCI_BUS_ID_NV);
    static_assert(qsgpu::cl::kDevicePciSlotIdNv == CL_DEVICE_PCI_SLOT_ID_NV);
    static_assert(qsgpu::cl::kDevicePciDomainIdNv == CL_DEVICE_PCI_DOMAIN_ID_NV);
#endif
    static_assert(sizeof(qsgpu::cl::PciBusInfoKhr) == sizeof(cl_device_pci_bus_info_khr));
    static_assert(sizeof(qsgpu::cl::TopologyAmd) == sizeof(cl_device_topology_amd));
    BOOST_CHECK(true);
#else
    BOOST_TEST_MESSAGE("Khronos OpenCL headers not installed; constants compared on hosts that have them");
    BOOST_CHECK(true);
#endif
}

BOOST_AUTO_TEST_CASE(missing_library_is_reported_not_crashed)
{
    std::string error;
    const auto api = qsgpu::LoadOpenClFrom("libqs-no-such-opencl.so.1", error);
    BOOST_CHECK(!api.has_value());
    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_SUITE_END()
