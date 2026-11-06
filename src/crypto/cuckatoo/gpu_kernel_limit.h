// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef QUICKSILVER_CRYPTO_CUCKATOO_GPU_KERNEL_LIMIT_H
#define QUICKSILVER_CRYPTO_CUCKATOO_GPU_KERNEL_LIMIT_H

#include <crypto/cuckatoo/gpu_devices.h>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace cuckatoo {
//! Below this compute-job limit (milliseconds) the amdgpu driver may reset a card in
//! the middle of a Cuckatoo solve (F-590). Kernel 6.17 defaults to 10000.
inline constexpr long kAmdgpuComputeLimitAdviseBelowMs{10000};

//! The driver's own default for the compute job, as `modinfo -p amdgpu` states it. The
//! `lockup_timeout` parameter file reads back blank when nothing set it (measured on 6.17),
//! so the kernel's default is only found in the module description. Asked at most once per
//! object: the kernel does not change under a running process.
class ModinfoDefault
{
public:
    //! Returns the text of `modinfo -p amdgpu`, or nullopt when it could not be run.
    using Runner = std::function<std::optional<std::string>()>;
    //! An empty runner runs the real modinfo.
    explicit ModinfoDefault(Runner runner = {});
    //! The default compute limit in ms; nullopt for any failure, 0, negative or no number.
    std::optional<long> Get();

private:
    Runner m_runner;
    std::mutex m_mutex;
    bool m_done{false};
    std::optional<long> m_value;
};

//! The compute limit stated after `default:` on the `lockup_timeout:` line of `modinfo -p amdgpu`
//! output. A default given for "all jobs" or as one value applies to the compute job.
std::optional<long> ParseModinfoLockupDefault(std::string_view modinfo_output);

//! Runs the real `modinfo -p amdgpu`: argv vector, no shell, /usr/sbin, /sbin, /usr/bin and
//! /bin searched explicitly, stopped after a short time. nullopt on any failure.
std::optional<std::string> RunModinfoAmdgpu();

//! Where the kernel's answers are read, so the check can be tested without hardware.
struct KernelPaths {
    //! Where the module default is read when the parameter file is blank. Null means the
    //! process-wide instance that runs the real modinfo.
    std::shared_ptr<ModinfoDefault> modinfo;
    std::string pci_devices{"/sys/bus/pci/devices"};
    std::string amdgpu_lockup_timeout{"/sys/module/amdgpu/parameters/lockup_timeout"};
};

//! The compute value of amdgpu's `lockup_timeout` ("gfx,compute,sdma,video", milliseconds),
//! read by position. nullopt for anything that is not four positive integers.
std::optional<long> ParseAmdgpuComputeLimit(std::string_view text);

//! The compute limit to warn about: set only when `device` is driven by amdgpu and its
//! effective compute limit is below kAmdgpuComputeLimitAdviseBelowMs. The effective limit is
//! the parameter file's compute value when that parses, and the module default when the
//! file is blank. Every failure
//! to read or parse is nullopt, never an error.
std::optional<long> AmdgpuLowComputeLimit(const GpuDevice& device, const KernelPaths& paths = {});

//! The Hardware page advisory and the arming log line, for a limit AmdgpuLowComputeLimit returned.
std::string AmdgpuAdvisoryText(long limit_ms);
std::string AmdgpuLogLine(long limit_ms);
} // namespace cuckatoo
#endif // QUICKSILVER_CRYPTO_CUCKATOO_GPU_KERNEL_LIMIT_H
