// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <crypto/cuckatoo/gpu_devices.h>
#include <crypto/cuckatoo/gpu_kernel_limit.h>
#include <test/util/setup_common.h>
#include <util/fs.h>
#include <util/string.h>

#include <boost/test/unit_test.hpp>

#include <fstream>
#include <memory>
#include <optional>
#include <string>

using namespace cuckatoo;

namespace {
// The fake /sys below names its device directory after the PCI slot ("0000:04:00.0"), and a colon is not legal in
// a Windows file name. The advisory itself reads Linux sysfs only, so these cases run where that tree can exist.
#ifndef WIN32
constexpr const char* kSlot{"0000:04:00.0"};

GpuDevice Card(const std::string& id = "pci:0000:04:00.0")
{
    GpuDevice device;
    device.backend = "opencl";
    device.id = id;
    device.type = "gpu";
    device.status = DeviceStatus::kOk;
    device.name = "Radeon RX 580";
    return device;
}

// A fake /sys: <root>/bus/pci/devices/<slot>/uevent naming the driver, and the module
// parameter file. `lockup` is written verbatim; nullopt writes no file at all.
// `modinfo` is what the fake `modinfo -p amdgpu` prints; the default (nullopt) is a modinfo that
// cannot run. No test here ever executes the real one.
KernelPaths FakeKernel(const fs::path& root, const std::string& driver, const std::optional<std::string>& lockup,
                       const std::string& slot = kSlot, std::shared_ptr<ModinfoDefault> modinfo = nullptr)
{
    const fs::path device_dir{root / "bus" / "pci" / "devices" / fs::u8path(slot)};
    fs::create_directories(device_dir);
    std::ofstream{device_dir / "uevent"} << "DRIVER=" << driver << "\nPCI_CLASS=30000\nPCI_SLOT_NAME=" << slot << "\n";
    KernelPaths paths;
    paths.modinfo = modinfo ? modinfo : std::make_shared<ModinfoDefault>([] { return std::optional<std::string>{}; });
    paths.pci_devices = fs::PathToString(root / "bus" / "pci" / "devices");
    paths.amdgpu_lockup_timeout = fs::PathToString(root / "lockup_timeout");
    if (lockup) std::ofstream{root / "lockup_timeout"} << *lockup;
    return paths;
}

// `modinfo -p amdgpu | grep lockup`, read live 2026-11-05. The 6.17 line is from this class of box
// (parameter unset); the 7.0 line is a fixture from a 7.0.0-42 box, not run where the tests are.
const std::string kModinfo617{
    "lockup_timeout:GPU lockup timeout in ms (default: 10000 for all jobs. 0: keep default value. negative: infinity timeout), "
    "format: for bare metal [Non-Compute] or [GFX,Compute,SDMA,Video]; for passthrough or sriov [all jobs] or "
    "[GFX,Compute,SDMA,Video]. (string)\n"};
const std::string kModinfo70{
    "lockup_timeout:GPU lockup timeout in ms (default: 2000. 0: keep default value. negative: infinity timeout), "
    "format: [single value for all] or [GFX,Compute,SDMA,Video]. (string)\n"};

struct CountingModinfo {
    std::shared_ptr<int> calls{std::make_shared<int>(0)};
    std::shared_ptr<ModinfoDefault> object;
    explicit CountingModinfo(std::optional<std::string> output)
    {
        auto count = calls;
        object = std::make_shared<ModinfoDefault>([count, output] {
            ++*count;
            return output;
        });
    }
};
#endif // WIN32
} // namespace

BOOST_FIXTURE_TEST_SUITE(gpu_kernel_limit_tests, BasicTestingSetup)

#ifndef WIN32
// (a) an amdgpu card with a 2000 ms compute limit gets the advisory
BOOST_AUTO_TEST_CASE(amdgpu_card_with_2000_compute_limit_advises)
{
    const auto paths = FakeKernel(m_path_root / "a", "amdgpu", "2000,2000,2000,2000\n");
    BOOST_CHECK_EQUAL(AmdgpuLowComputeLimit(Card(), paths), std::optional<long>{2000});
}

// (b) the setting the docs recommend clears it
BOOST_AUTO_TEST_CASE(a_60000_compute_limit_does_not_advise)
{
    const auto paths = FakeKernel(m_path_root / "b", "amdgpu", "2000,60000,2000,2000\n");
    BOOST_CHECK(!AmdgpuLowComputeLimit(Card(), paths));
}

// (c) the limit is amdgpu's: a card on another driver never advises
BOOST_AUTO_TEST_CASE(a_card_on_another_driver_does_not_advise)
{
    for (const char* driver : {"nvidia", "i915", "radeon"}) {
        const auto paths = FakeKernel(m_path_root / fs::u8path("c_" + std::string{driver}), driver, "2000,2000,2000,2000\n");
        BOOST_CHECK_MESSAGE(!AmdgpuLowComputeLimit(Card(), paths), driver);
    }
    // And an id that names no PCI slot (an enumeration-order id) cannot be placed on any driver.
    const auto paths = FakeKernel(m_path_root / "c_enum", "amdgpu", "2000,2000,2000,2000\n");
    BOOST_CHECK(!AmdgpuLowComputeLimit(Card("opencl:0:0"), paths));
}

// (d) missing, empty or unparsable input is "no advisory", and never a crash
BOOST_AUTO_TEST_CASE(a_missing_or_unparsable_parameter_does_not_advise)
{
    BOOST_CHECK(!AmdgpuLowComputeLimit(Card(), FakeKernel(m_path_root / "d_missing", "amdgpu", std::nullopt)));
    // A kernel with the parameter unset reads back as an empty line (measured on 6.17).
    BOOST_CHECK(!AmdgpuLowComputeLimit(Card(), FakeKernel(m_path_root / "d_empty", "amdgpu", "\n")));
    int n{0};
    for (const char* text : {"garbage", "2000,abc,2000,2000", "2000,,2000,2000", "2000,2000", "2000", "2000,2000,2000,2000,2000",
                             "2000,-1,2000,2000", "2000,0,2000,2000", "2000,99999999999999999999,2000,2000", "2000,20 00,2000,2000"}) {
        const auto paths = FakeKernel(m_path_root / fs::u8path("d_" + util::ToString(n++)), "amdgpu", std::string{text} + "\n");
        BOOST_CHECK_MESSAGE(!AmdgpuLowComputeLimit(Card(), paths), text);
    }
    // No slot directory for the card at all.
    BOOST_CHECK(!AmdgpuLowComputeLimit(Card("pci:0000:09:00.0"), FakeKernel(m_path_root / "d_noslot", "amdgpu", "2000,2000,2000,2000\n")));
}

// (e) the list is read by position: only the second number is the compute limit
BOOST_AUTO_TEST_CASE(the_four_value_list_is_read_by_position)
{
    BOOST_CHECK_EQUAL(ParseAmdgpuComputeLimit("1,2,3,4"), std::optional<long>{2});
    BOOST_CHECK_EQUAL(ParseAmdgpuComputeLimit("60000,2500,60000,60000\n"), std::optional<long>{2500});
    // Low first, third and fourth values must not advise when compute is fine ...
    BOOST_CHECK(!AmdgpuLowComputeLimit(Card(), FakeKernel(m_path_root / "e1", "amdgpu", "100,60000,100,100\n")));
    // ... and a high first value must not hide a low compute one.
    BOOST_CHECK_EQUAL(AmdgpuLowComputeLimit(Card(), FakeKernel(m_path_root / "e2", "amdgpu", "60000,2500,60000,60000\n")), std::optional<long>{2500});
}

// (f) the threshold: 9999 advises, 10000 does not
BOOST_AUTO_TEST_CASE(the_threshold_is_below_10000)
{
    BOOST_CHECK_EQUAL(AmdgpuLowComputeLimit(Card(), FakeKernel(m_path_root / "f1", "amdgpu", "2000,9999,2000,2000\n")), std::optional<long>{9999});
    BOOST_CHECK(!AmdgpuLowComputeLimit(Card(), FakeKernel(m_path_root / "f2", "amdgpu", "2000,10000,2000,2000\n")));
}

// (g) a blank file means "nothing set": the driver's own default decides. Kernel 7.0 says 2000.
BOOST_AUTO_TEST_CASE(a_blank_file_uses_the_module_default_of_kernel_7_0)
{
    CountingModinfo modinfo{kModinfo70};
    const auto paths = FakeKernel(m_path_root / "g", "amdgpu", "\n", kSlot, modinfo.object);
    BOOST_CHECK_EQUAL(AmdgpuLowComputeLimit(Card(), paths), std::optional<long>{2000});
    BOOST_CHECK_EQUAL(ParseModinfoLockupDefault(kModinfo70), std::optional<long>{2000});
}

// (h) ... and kernel 6.17's default of 10000 does not advise
BOOST_AUTO_TEST_CASE(a_blank_file_uses_the_module_default_of_kernel_6_17)
{
    CountingModinfo modinfo{kModinfo617};
    const auto paths = FakeKernel(m_path_root / "h", "amdgpu", "\n", kSlot, modinfo.object);
    BOOST_CHECK(!AmdgpuLowComputeLimit(Card(), paths));
    BOOST_CHECK_EQUAL(*modinfo.calls, 1); // it was consulted, and said 10000
    BOOST_CHECK_EQUAL(ParseModinfoLockupDefault(kModinfo617), std::optional<long>{10000});
}

// (i) a blank file and a modinfo that gives no usable default is "no advisory", and never a crash
BOOST_AUTO_TEST_CASE(a_blank_file_and_an_unusable_modinfo_does_not_advise)
{
    // The positive control: this same fixture advises when the runner answers.
    BOOST_CHECK_EQUAL(AmdgpuLowComputeLimit(Card(), FakeKernel(m_path_root / "i_ok", "amdgpu", "  \n", kSlot,
                                                                CountingModinfo{kModinfo70}.object)),
                      std::optional<long>{2000});
    int n{0};
    for (const std::optional<std::string>& output : std::initializer_list<std::optional<std::string>>{
             std::nullopt, std::string{}, std::string{"garbage\n"}, std::string{"lockup_timeout:\n"},
             std::string{"lockup_timeout:no default stated here (string)\n"},
             std::string{"lockup_timeout:GPU lockup timeout (default: 0. keep) (string)\n"},
             std::string{"lockup_timeout:GPU lockup timeout (default: -1. infinity) (string)\n"},
             std::string{"lockup_timeout:GPU lockup timeout (default: abc) (string)\n"},
             std::string{"lockup_timeout:GPU lockup timeout (default: 99999999999999999999) (string)\n"},
             std::string{"other_param:x (default: 2000)\n"}}) {
        CountingModinfo modinfo{output};
        const auto paths = FakeKernel(m_path_root / fs::u8path("i_" + util::ToString(n++)), "amdgpu", "\n", kSlot, modinfo.object);
        BOOST_CHECK_MESSAGE(!AmdgpuLowComputeLimit(Card(), paths), output.value_or("<no output>"));
    }
}

// (j) a parseable file value wins over the module default
BOOST_AUTO_TEST_CASE(a_parseable_file_value_wins_over_the_module_default)
{
    CountingModinfo seventy{kModinfo70};
    BOOST_CHECK(!AmdgpuLowComputeLimit(Card(), FakeKernel(m_path_root / "j1", "amdgpu", "20000,60000,20000,20000\n", kSlot, seventy.object)));
    CountingModinfo old{kModinfo617};
    BOOST_CHECK_EQUAL(AmdgpuLowComputeLimit(Card(), FakeKernel(m_path_root / "j2", "amdgpu", "2000,2000,2000,2000\n", kSlot, old.object)),
                      std::optional<long>{2000});
    // A file with text that does not parse is not "blank": it does not fall back to the default.
    BOOST_CHECK(!AmdgpuLowComputeLimit(Card(), FakeKernel(m_path_root / "j3", "amdgpu", "garbage\n", kSlot, seventy.object)));
    // And modinfo is not asked when the file already answered.
    BOOST_CHECK_EQUAL(*seventy.calls, 0);
    BOOST_CHECK_EQUAL(*old.calls, 0);
}

// (k) only an amdgpu card ever causes modinfo to run
BOOST_AUTO_TEST_CASE(modinfo_is_never_run_for_a_card_on_another_driver)
{
    // The positive control: an amdgpu card on the same fixture does call it.
    CountingModinfo amd{kModinfo70};
    BOOST_CHECK(AmdgpuLowComputeLimit(Card(), FakeKernel(m_path_root / "k_amd", "amdgpu", "\n", kSlot, amd.object)));
    BOOST_CHECK_EQUAL(*amd.calls, 1);
    for (const char* driver : {"nvidia", "i915", "radeon"}) {
        CountingModinfo modinfo{kModinfo70};
        const auto paths = FakeKernel(m_path_root / fs::u8path("k_" + std::string{driver}), driver, "\n", kSlot, modinfo.object);
        BOOST_CHECK_MESSAGE(!AmdgpuLowComputeLimit(Card(), paths), driver);
        BOOST_CHECK_MESSAGE(*modinfo.calls == 0, driver);
    }
    CountingModinfo enumerated{kModinfo70};
    BOOST_CHECK(!AmdgpuLowComputeLimit(Card("opencl:0:0"), FakeKernel(m_path_root / "k_enum", "amdgpu", "\n", kSlot, enumerated.object)));
    BOOST_CHECK_EQUAL(*enumerated.calls, 0);
}

// (l) the kernel does not change under a running process: modinfo runs once
BOOST_AUTO_TEST_CASE(modinfo_runs_at_most_once_across_many_queries)
{
    CountingModinfo modinfo{kModinfo70};
    const auto paths = FakeKernel(m_path_root / "l", "amdgpu", "\n", kSlot, modinfo.object);
    for (int i = 0; i < 25; ++i) BOOST_CHECK_EQUAL(AmdgpuLowComputeLimit(Card(), paths), std::optional<long>{2000});
    BOOST_CHECK_EQUAL(*modinfo.calls, 1);
    // A failure is cached too: a modinfo that cannot run is not retried on every redraw.
    CountingModinfo failing{std::nullopt};
    const auto failing_paths = FakeKernel(m_path_root / "l2", "amdgpu", "\n", kSlot, failing.object);
    for (int i = 0; i < 25; ++i) BOOST_CHECK(!AmdgpuLowComputeLimit(Card(), failing_paths));
    BOOST_CHECK_EQUAL(*failing.calls, 1);
}

#endif // WIN32

// The signed wording (plans/2026-11-05-f590-wording-sheet-b1-b2.md), with [N] filled in.
BOOST_AUTO_TEST_CASE(the_signed_wording_is_used_verbatim)
{
    BOOST_CHECK_EQUAL(AmdgpuAdvisoryText(2000),
                      "This GPU may be reset while mining. Your kernel limits GPU compute jobs to 2000 ms, shorter than a Cuckatoo solve "
                      "can take. If mining stalls or the machine freezes, see \"AMD cards on Linux kernel 7.0\" in the GPU solver guide.");
    BOOST_CHECK_EQUAL(AmdgpuLogLine(2000),
                      "amdgpu compute limit is 2000 ms; Cuckatoo jobs may exceed it and the GPU may be reset (see doc/gpu-solver.md)");
}

BOOST_AUTO_TEST_SUITE_END()
