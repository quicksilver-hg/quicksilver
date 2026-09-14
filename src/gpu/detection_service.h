// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef QUICKSILVER_GPU_DETECTION_SERVICE_H
#define QUICKSILVER_GPU_DETECTION_SERVICE_H

#include <mutex>
#include <optional>
#include <string_view>

namespace gpu {

struct GpuState {
    bool has_gpu{false};
    bool is_configured{false};
};

namespace detail {
/** Parse the output of `nvidia-smi --query-gpu=count --format=csv,noheader`. */
bool HasGpuFromNvidiaSmiOutput(std::string_view output);
} // namespace detail

class GpuDetectionService
{
public:
    /**
     * Detect compatible graphics hardware on the first call and cache it for
     * the process lifetime. Configuration belongs to the caller and is copied
     * into the returned snapshot without changing the hardware cache.
     */
    static GpuState detectGpu(bool is_configured = false);

private:
    static bool hasNvidiaDevice();

    static std::optional<bool> s_cached_has_gpu;
    static std::mutex s_mutex;
};

} // namespace gpu

#endif // QUICKSILVER_GPU_DETECTION_SERVICE_H
