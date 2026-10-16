// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <common/args.h>
#include <common/executable.h>
#include <crypto/cuckatoo/gpu_devices.h>
#include <cstdlib>
#include <gpu/helper_config.h>
#include <util/string.h>
#include <util/translation.h>

namespace gpu {
namespace {
void Set(const char* name, const std::string& value)
{
#ifdef WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}
void Unset(const char* name)
{
#ifdef WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}
std::string Trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n"), last = value.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::string{} : value.substr(first, last - first + 1);
}
} // namespace

std::optional<fs::path> FindBundledHelper(const fs::path& exe_dir, const std::string& name, const std::function<bool(const fs::path&)>& usable)
{
    if (exe_dir.empty()) return std::nullopt;
    const fs::path binary{fs::PathFromString(name)};
    for (const fs::path& candidate : {exe_dir / fs::PathFromString("..") / fs::PathFromString("libexec") / fs::PathFromString("quicksilver") / binary, exe_dir / binary}) {
        if (usable(candidate)) return candidate;
    }
    return std::nullopt;
}

std::optional<fs::path> LocateBundledHelper()
{
    return FindBundledHelper(common::SelfExecutableDir(), kBundledHelperName, common::IsUsableExecutable);
}

void AddGpuHelperArgs(ArgsManager& argsman)
{
    constexpr unsigned int flags{ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION};
    argsman.AddArg("-cuckatoosolver=<path>", "Path to a custom graphics helper, used instead of the one that comes with Quicksilver (mining and transfer proofs; never consensus). Returned proofs are verified before use.", flags, OptionsCategory::OPTIONS);
    argsman.AddArg("-cuckatoodevice=<id>", "Graphics card to use, by the id its helper lists (for example pci:0000:04:00.0). Repeat for several cards. Use cpu to prepare proofs on the processor only. Default: every graphics card except built-in graphics.", flags, OptionsCategory::OPTIONS);
    argsman.AddArg("-cuckatoosolvertimeout=<sec>", "No-progress watchdog for the graphics helper, in seconds. A helper that prints nothing for this long is stopped, and work falls back as configured.", flags, OptionsCategory::OPTIONS);
}

util::Result<void> ApplyGpuHelperArgs(const ArgsManager& args, const std::optional<fs::path>& bundled)
{
    std::string error;
    const auto choice = cuckatoo::ParseDeviceChoice(args.GetArgs("-cuckatoodevice"), error);
    if (!choice) return util::Error{Untranslated(error)};
    const std::string custom = Trim(args.GetArg("-cuckatoosolver", ""));
    if (custom.empty())
        Unset("CUCKATOO_GPU_SOLVER");
    else
        Set("CUCKATOO_GPU_SOLVER", custom);
    if (bundled)
        Set("CUCKATOO_GPU_BUNDLED", fs::PathToString(*bundled));
    else
        Unset("CUCKATOO_GPU_BUNDLED");
    if (choice->mode == cuckatoo::DeviceChoice::Mode::kCpuOnly) {
        Set("CUCKATOO_GPU_DEVICES", "cpu");
    } else if (choice->mode == cuckatoo::DeviceChoice::Mode::kChosen) {
        Set("CUCKATOO_GPU_DEVICES", util::Join(choice->ids, ","));
    } else {
        Unset("CUCKATOO_GPU_DEVICES");
    }
    if (args.IsArgSet("-cuckatoosolvertimeout"))
        Set("CUCKATOO_GPU_TIMEOUT", args.GetArg("-cuckatoosolvertimeout", ""));
    else
        Unset("CUCKATOO_GPU_TIMEOUT");
    return {};
}
} // namespace gpu
