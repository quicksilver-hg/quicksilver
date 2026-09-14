// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <gpu/detection_service.h>

#include <boost/test/unit_test.hpp>

#include <future>
#include <vector>

BOOST_AUTO_TEST_SUITE(gpu_detection_tests)

BOOST_AUTO_TEST_CASE(nvidia_smi_count_is_parsed_strictly)
{
    BOOST_CHECK(!gpu::detail::HasGpuFromNvidiaSmiOutput(""));
    BOOST_CHECK(!gpu::detail::HasGpuFromNvidiaSmiOutput("0\n"));
    BOOST_CHECK(!gpu::detail::HasGpuFromNvidiaSmiOutput("not supported\n"));
    BOOST_CHECK(!gpu::detail::HasGpuFromNvidiaSmiOutput("1 device\n"));
    BOOST_CHECK(gpu::detail::HasGpuFromNvidiaSmiOutput("1\n"));
    BOOST_CHECK(gpu::detail::HasGpuFromNvidiaSmiOutput("  2\r\n2\r\n"));
}

BOOST_AUTO_TEST_CASE(detection_is_cached_and_configuration_is_caller_owned)
{
    const gpu::GpuState first{gpu::GpuDetectionService::detectGpu(false)};
    const gpu::GpuState second{gpu::GpuDetectionService::detectGpu(true)};
    BOOST_CHECK_EQUAL(first.has_gpu, second.has_gpu);
    BOOST_CHECK(!first.is_configured);
    BOOST_CHECK(second.is_configured);
}

BOOST_AUTO_TEST_CASE(cached_detection_is_thread_safe)
{
    const bool expected{gpu::GpuDetectionService::detectGpu().has_gpu};
    std::vector<std::future<gpu::GpuState>> calls;
    for (int i{0}; i < 8; ++i) {
        calls.emplace_back(std::async(std::launch::async, [] { return gpu::GpuDetectionService::detectGpu(); }));
    }
    for (auto& call : calls)
        BOOST_CHECK_EQUAL(call.get().has_gpu, expected);
}

BOOST_AUTO_TEST_SUITE_END()
