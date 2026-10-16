// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef QUICKSILVER_GPU_HELPER_CONFIG_H
#define QUICKSILVER_GPU_HELPER_CONFIG_H
#include <functional>
#include <optional>
#include <string>
#include <util/fs.h>
#include <util/result.h>
class ArgsManager;
namespace gpu {
#ifdef WIN32
inline constexpr const char* kBundledHelperName{"qsgpusolve-opencl.exe"};
#else
inline constexpr const char* kBundledHelperName{"qsgpusolve-opencl"};
#endif
//! <exe_dir>/../libexec/quicksilver/<name>, then <exe_dir>/<name>. Never PATH:
//! a helper the node runs for minutes on the user's card is ours or theirs by
//! name (-cuckatoosolver), never whatever a search path turns up.
std::optional<fs::path> FindBundledHelper(const fs::path& exe_dir, const std::string& binary_name, const std::function<bool(const fs::path&)>& usable);
//! FindBundledHelper for this process.
std::optional<fs::path> LocateBundledHelper();
//! Register the shared graphics-helper arguments.
void AddGpuHelperArgs(ArgsManager& argsman);
//! Validate -cuckatoodevice and hand the configuration to the cuckatoo library.
util::Result<void> ApplyGpuHelperArgs(const ArgsManager& args, const std::optional<fs::path>& bundled);
} // namespace gpu
#endif // QUICKSILVER_GPU_HELPER_CONFIG_H
