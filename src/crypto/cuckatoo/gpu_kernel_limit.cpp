// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <crypto/cuckatoo/gpu_kernel_limit.h>

#include <util/fs.h>
#include <util/process/contained_child.h>
#include <util/string.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace cuckatoo {
namespace {
//! The text of a small file, or nullopt when it cannot be opened.
std::optional<std::string> ReadSmallFile(const std::string& path)
{
    std::ifstream stream{fs::PathFromString(path)};
    if (!stream) return std::nullopt;
    // These are one-line sysfs files; a stray large file is not one.
    std::string text(4096, '\0');
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<size_t>(stream.gcount()));
    return text;
}

std::string_view Trim(std::string_view text)
{
    constexpr std::string_view space{" \t\r\n"};
    const auto first = text.find_first_not_of(space);
    if (first == std::string_view::npos) return {};
    return text.substr(first, text.find_last_not_of(space) - first + 1);
}

//! A positive decimal integer and nothing else.
std::optional<long> ParsePositive(std::string_view text)
{
    if (text.empty()) return std::nullopt;
    long value{0};
    for (const char c : text) {
        if (c < '0' || c > '9') return std::nullopt;
        const long digit{c - '0'};
        if (value > (std::numeric_limits<long>::max() - digit) / 10) return std::nullopt;
        value = value * 10 + digit;
    }
    if (value == 0) return std::nullopt;
    return value;
}

//! "pci:0000:04:00.0" -> "0000:04:00.0"; empty for any other id (an enumeration-order id names no slot).
std::string SlotOf(const GpuDevice& device)
{
    constexpr std::string_view prefix{"pci:"};
    if (device.id.compare(0, prefix.size(), prefix) != 0) return {};
    return device.id.substr(prefix.size());
}

bool DrivenByAmdgpu(const std::string& pci_devices, const std::string& slot)
{
    // The slot comes from a listing line; it must be a single path component.
    if (slot.empty() || slot.find_first_of("/\\") != std::string::npos || slot.find("..") != std::string::npos) return false;
    std::ifstream uevent{fs::PathFromString(pci_devices) / fs::u8path(slot) / "uevent"};
    if (!uevent) return false;
    constexpr std::string_view key{"DRIVER="};
    std::string line;
    while (std::getline(uevent, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.compare(0, key.size(), key) == 0) return line.substr(key.size()) == "amdgpu";
    }
    return false;
}
} // namespace

std::optional<long> ParseAmdgpuComputeLimit(std::string_view text)
{
    text = Trim(text);
    long fields[4]{};
    size_t count{0};
    while (true) {
        const auto comma = text.find(',');
        const auto value = ParsePositive(text.substr(0, comma));
        if (!value || count == 4) return std::nullopt;
        fields[count++] = *value;
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
    }
    if (count != 4) return std::nullopt;
    return fields[1];
}

ModinfoDefault::ModinfoDefault(Runner runner) : m_runner{std::move(runner)} {}

std::optional<long> ModinfoDefault::Get()
{
    std::lock_guard lock{m_mutex};
    if (!m_done) {
        m_done = true; // a failed run is remembered too: a redraw must not exec again
        const auto output = m_runner ? m_runner() : RunModinfoAmdgpu();
        if (output) m_value = ParseModinfoLockupDefault(*output);
    }
    return m_value;
}

std::optional<long> ParseModinfoLockupDefault(std::string_view output)
{
    constexpr std::string_view param{"lockup_timeout:"};
    constexpr std::string_view marker{"default:"};
    while (!output.empty()) {
        const auto eol = output.find('\n');
        std::string_view line = output.substr(0, eol);
        output = eol == std::string_view::npos ? std::string_view{} : output.substr(eol + 1);
        if (line.compare(0, param.size(), param) != 0) continue;
        const auto at = line.find(marker);
        if (at == std::string_view::npos) return std::nullopt;
        line.remove_prefix(at + marker.size());
        while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
        size_t digits{0};
        while (digits < line.size() && line[digits] >= '0' && line[digits] <= '9') ++digits;
        // "-1" has no leading digit, so a negative (infinity) default is rejected here, as is 0.
        return ParsePositive(line.substr(0, digits));
    }
    return std::nullopt;
}

std::optional<std::string> RunModinfoAmdgpu()
{
    // modinfo is in /usr/sbin or /sbin, which a normal user's PATH lacks on Debian.
    std::string program;
    for (const char* dir : {"/usr/sbin", "/sbin", "/usr/bin", "/bin"}) {
        const fs::path candidate{fs::PathFromString(dir) / fs::u8path("modinfo")};
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) {
            program = fs::PathToString(candidate);
            break;
        }
    }
    if (program.empty()) return std::nullopt;

    auto child = util::ContainedChild::Spawn({program, "-p", "amdgpu"}, util::ChildStdout::kPipe);
    if (!child) return std::nullopt;

    // Bounded as a whole: never longer than this, whatever the child does. ~ContainedChild kills it.
    constexpr auto budget{std::chrono::seconds{2}};
    constexpr size_t max_output{1 << 20};
    const auto deadline = std::chrono::steady_clock::now() + budget;
    std::string output;
    char buf[4096];
    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            child->KillGroup();
            return std::nullopt;
        }
        size_t got{0};
        const auto status = child->ReadStdout(buf, sizeof(buf), static_cast<int>(std::min<long long>(left.count(), 250)), got);
        if (status == util::ContainedChild::ReadStatus::kClosed) break;
        if (status == util::ContainedChild::ReadStatus::kData) {
            output.append(buf, got);
            if (output.size() > max_output) {
                child->KillGroup();
                return std::nullopt;
            }
        }
    }
    if (child->Wait() != 0) return std::nullopt;
    return output;
}

std::optional<long> AmdgpuLowComputeLimit(const GpuDevice& device, const KernelPaths& paths)
{
    if (!DrivenByAmdgpu(paths.pci_devices, SlotOf(device))) return std::nullopt;
    const auto text = ReadSmallFile(paths.amdgpu_lockup_timeout);
    if (!text) return std::nullopt;
    std::optional<long> limit;
    if (Trim(*text).empty()) {
        // Nothing set the parameter: the file reads blank and the module's own default applies.
        static ModinfoDefault process_wide; // the kernel does not change under a running process
        limit = (paths.modinfo ? *paths.modinfo : process_wide).Get();
    } else {
        limit = ParseAmdgpuComputeLimit(*text);
    }
    if (!limit || *limit >= kAmdgpuComputeLimitAdviseBelowMs) return std::nullopt;
    return limit;
}

std::string AmdgpuAdvisoryText(long limit_ms)
{
    return "This GPU may be reset while mining. Your kernel limits GPU compute jobs to " + util::ToString(limit_ms) +
           " ms, shorter than a Cuckatoo solve can take. If mining stalls or the machine freezes, see "
           "\"AMD cards on Linux kernel 7.0\" in the GPU solver guide.";
}

std::string AmdgpuLogLine(long limit_ms)
{
    return "amdgpu compute limit is " + util::ToString(limit_ms) +
           " ms; Cuckatoo jobs may exceed it and the GPU may be reset (see doc/gpu-solver.md)";
}
} // namespace cuckatoo
