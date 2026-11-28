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

namespace util { class ContainedChild; }

namespace cuckatoo {
inline constexpr uint8_t kBundledHelperEdgeBits{28};
inline constexpr std::chrono::milliseconds kListingLimit{15000};
//! How long a helper may live on after closing its stdout. A healthy helper exits
//! as it closes the pipe, so this only ever runs out on one that is broken.
inline constexpr std::chrono::milliseconds kReapLimit{5000};
enum class DeviceStatus { kOk, kNoDriver, kCompileFailed, kProbeFailed, kOther };
struct GpuDevice {
    std::string backend, id, alias, type;
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
//! The choice the node installed in CUCKATOO_GPU_DEVICES. A value ParseDeviceChoice
//! refuses (only possible when someone sets the variable by hand) is not read as
//! "automatic": that would let every card mine past a CPU-only setting. It comes
//! back as kChosen naming the raw value, which no listed card matches, so it
//! reports as a chosen card that is missing and no card is used.
DeviceChoice DeviceChoiceFromEnv();
//! Reap a helper that has closed its stdout. Waits up to the reap limit for it to
//! exit; one still running then is killed with everything it started, so a helper
//! that closes the pipe and keeps going cannot hang the caller in Wait(). Returns
//! the exit code as ContainedChild::Wait does (a killed helper is never 4 or 5).
int ReapClosedHelper(util::ContainedChild& child);
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
//! Last observed listing, without launching a helper. For status display only.
std::optional<GpuDiscovery> LastGpuDiscovery(uint8_t edgebits);
void ResetGpuDiscoveryForTesting();
//! Shorten the listing limit DiscoverGpus passes to ListDevices, so a test can
//! reach kTimedOut without sleeping out kListingLimit. ResetGpuDiscoveryForTesting
//! restores the default.
void SetListingLimitForTesting(std::chrono::milliseconds limit);
//! Shorten the reap limit so a test of a helper that outlives its stdout need not
//! sleep out kReapLimit. ResetGpuDiscoveryForTesting restores the default.
void SetReapLimitForTesting(std::chrono::milliseconds limit);
//! Kill every listing child still running and refuse new ones. Registered with atexit the first time a
//! listing starts, so every normal exit of the daemon and the GUI (return from main, exit()) runs it; call it
//! directly only from a test. Does not take the discovery lock (a listing holds it for up to 15 s).
//! ResetGpuDiscoveryForTesting re-opens listing.
void KillOutstandingListings();
} // namespace cuckatoo
#endif // QUICKSILVER_CRYPTO_CUCKATOO_GPU_DEVICES_H
