// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef QUICKSILVER_CRYPTO_CUCKATOO_GPU_DEVICES_H
#define QUICKSILVER_CRYPTO_CUCKATOO_GPU_DEVICES_H

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cuckatoo {
inline constexpr uint8_t kBundledHelperEdgeBits{28};
inline constexpr std::chrono::milliseconds kListingLimit{15000};
enum class DeviceStatus { kOk, kNoDriver, kCompileFailed, kProbeFailed, kOther };
struct GpuDevice {
    std::string backend, id, type;
    bool integrated{false};
    uint64_t mem{0};
    DeviceStatus status{DeviceStatus::kOther};
    std::string reason, name;
};
std::optional<GpuDevice> ParseDeviceLine(std::string_view line);
std::vector<GpuDevice> ParseDeviceListing(std::string_view text);
bool IsWellFormedDeviceId(std::string_view id);
bool DeviceIdLess(const std::string& a, const std::string& b);
struct DeviceChoice {
    enum class Mode { kAutomatic, kChosen, kCpuOnly };
    Mode mode{Mode::kAutomatic};
    std::vector<std::string> ids;
};
std::optional<DeviceChoice> ParseDeviceChoice(const std::vector<std::string>& values, std::string& error);
DeviceChoice DeviceChoiceFromEnv();
enum class ListOutcome { kListed, kNoHelper, kNoLoader, kFailed, kTimedOut };
struct DeviceListing {
    ListOutcome outcome{ListOutcome::kNoHelper};
    std::vector<GpuDevice> devices;
    std::string detail;
};
DeviceListing ListDevices(const std::string& helper, std::chrono::milliseconds limit = kListingLimit);
struct DeviceSelection {
    std::vector<GpuDevice> use, unusable;
    std::vector<std::string> missing;
    std::vector<GpuDevice> integrated_unchosen, unchosen;
};
DeviceSelection SelectDevices(const std::vector<GpuDevice>& listed, const DeviceChoice& choice);
std::optional<std::string> SolveDevice(const DeviceSelection& selection);
enum class GpuSituation { kReady, kPartial, kNoGpu, kIntegratedOnly, kNoneUsable, kChosenMissing, kHelperMissing, kCpuChosen, kNoLoader, kListingFailed };
GpuSituation ClassifySituation(const DeviceListing&, const DeviceChoice&, const DeviceSelection&);
std::optional<std::string> GpuHelperFor(uint8_t edgebits);
struct GpuDiscovery {
    std::optional<std::string> helper;
    DeviceChoice choice;
    DeviceListing listing;
    DeviceSelection selection;
    GpuSituation situation{GpuSituation::kHelperMissing};
};
GpuDiscovery DiscoverGpus(uint8_t edgebits);
void ResetGpuDiscoveryForTesting();
//! Shorten the listing limit DiscoverGpus passes to ListDevices, so a test can
//! reach kTimedOut without sleeping out kListingLimit. ResetGpuDiscoveryForTesting
//! restores the default.
void SetListingLimitForTesting(std::chrono::milliseconds limit);
} // namespace cuckatoo
#endif // QUICKSILVER_CRYPTO_CUCKATOO_GPU_DEVICES_H
