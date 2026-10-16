// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <crypto/cuckatoo/gpu_devices.h>
#include <util/process/contained_child.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

namespace cuckatoo {
namespace {
std::mutex g_mutex;
// A detached GUI discovery thread can still be listing during process teardown.
// Keep the cache alive so that thread can never touch a destroyed map.
auto* g_cache = new std::map<std::string, GpuDiscovery>;

std::string Trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) text.remove_suffix(1);
    return std::string{text};
}

bool Decimal(std::string_view text, uint64_t& value)
{
    if (text.empty()) return false;
    value = 0;
    for (const unsigned char c : text) {
        if (c < '0' || c > '9') return false;
        const uint64_t digit = c - '0';
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    return true;
}

bool IsHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
void Sort(std::vector<GpuDevice>& devices) { std::sort(devices.begin(), devices.end(), [](const auto& a, const auto& b) { return DeviceIdLess(a.id, b.id); }); }
} // namespace

std::optional<GpuDevice> ParseDeviceLine(std::string_view line)
{
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (!line.starts_with("device ")) return std::nullopt;
    const size_t name_at = line.find(" name=");
    if (name_at == std::string_view::npos) return std::nullopt;
    GpuDevice out;
    out.name = std::string{line.substr(name_at + 6)};
    bool backend{false}, id{false}, type{false}, status{false};
    std::string_view fields = line.substr(7, name_at - 7);
    size_t pos{0};
    while (pos < fields.size()) {
        const size_t end = fields.find(' ', pos);
        const std::string_view token = fields.substr(pos, end == std::string_view::npos ? fields.size() - pos : end - pos);
        const size_t eq = token.find('=');
        if (eq == std::string_view::npos || eq == 0) return std::nullopt;
        const auto key = token.substr(0, eq), value = token.substr(eq + 1);
        if (key == "backend") { out.backend = value; backend = !value.empty(); }
        else if (key == "id") { out.id = value; id = !value.empty(); }
        else if (key == "type") { out.type = value; type = value == "gpu" || value == "cpu" || value == "accel"; }
        else if (key == "integrated") { if (value != "0" && value != "1") return std::nullopt; out.integrated = value == "1"; }
        else if (key == "mem") { if (!Decimal(value, out.mem)) return std::nullopt; }
        else if (key == "status") {
            status = true;
            if (value == "ok") out.status = DeviceStatus::kOk;
            else if (value == "no-driver") out.status = DeviceStatus::kNoDriver;
            else if (value == "compile-failed") out.status = DeviceStatus::kCompileFailed;
            else if (value == "probe-failed") out.status = DeviceStatus::kProbeFailed;
            else out.status = DeviceStatus::kOther;
        } else if (key == "reason") {
            out.reason = value; std::replace(out.reason.begin(), out.reason.end(), '_', ' ');
        }
        if (end == std::string_view::npos) break;
        pos = end + 1;
    }
    if (!backend || !id || !type || !status || out.name.empty()) return std::nullopt;
    return out;
}

std::vector<GpuDevice> ParseDeviceListing(std::string_view text)
{
    std::vector<GpuDevice> result;
    while (!text.empty()) {
        const size_t end = text.find('\n');
        if (auto device = ParseDeviceLine(text.substr(0, end)); device) result.push_back(std::move(*device));
        if (end == std::string_view::npos) break;
        text.remove_prefix(end + 1);
    }
    return result;
}

bool IsWellFormedDeviceId(std::string_view id)
{
    if (id == "cpu" || id.empty()) return false;
    if (id.starts_with("pci:")) {
        if (id.size() != 16 || id[8] != ':' || id[11] != ':' || id[14] != '.') return false;
        for (size_t i : {4U,5U,6U,7U,9U,10U,12U,13U,15U}) if (!IsHex(id[i])) return false;
        return true;
    }
    const size_t colon = id.find(':');
    if (colon == 0 || colon == std::string_view::npos || colon + 1 == id.size()) return false;
    if (!((id.front() >= 'a' && id.front() <= 'z') || (id.front() >= 'A' && id.front() <= 'Z'))) return false;
    return std::none_of(id.begin(), id.end(), [](unsigned char c) { return c <= ' ' || c == ','; });
}

bool DeviceIdLess(const std::string& a, const std::string& b)
{
    const bool ap = a.starts_with("pci:"), bp = b.starts_with("pci:");
    if (ap != bp) return ap;
    return a < b;
}

std::optional<DeviceChoice> ParseDeviceChoice(const std::vector<std::string>& values, std::string& error)
{
    DeviceChoice result;
    bool cpu{false};
    for (const auto& raw : values) {
        const std::string value = Trim(raw);
        if (value.empty()) continue;
        if (value == "cpu") { cpu = true; continue; }
        if (!IsWellFormedDeviceId(value)) {
            error = "-cuckatoodevice=" + value + " is not a device id. Use an id from the helper's --list-devices output, such as pci:0000:04:00.0, or cpu.";
            return std::nullopt;
        }
        if (std::find(result.ids.begin(), result.ids.end(), value) == result.ids.end()) result.ids.push_back(value);
    }
    if (cpu && !result.ids.empty()) { error = "-cuckatoodevice=cpu cannot be combined with a graphics card."; return std::nullopt; }
    if (cpu) result.mode = DeviceChoice::Mode::kCpuOnly;
    else if (!result.ids.empty()) result.mode = DeviceChoice::Mode::kChosen;
    return result;
}

DeviceChoice DeviceChoiceFromEnv()
{
    const char* env = std::getenv("CUCKATOO_GPU_DEVICES");
    if (!env || !env[0]) return {};
    std::vector<std::string> values; std::string_view rest{env};
    for (;;) { const size_t comma = rest.find(','); values.emplace_back(rest.substr(0, comma)); if (comma == std::string_view::npos) break; rest.remove_prefix(comma + 1); }
    std::string error; return ParseDeviceChoice(values, error).value_or(DeviceChoice{});
}

DeviceListing ListDevices(const std::string& helper, std::chrono::milliseconds limit)
{
    DeviceListing out;
    auto child = util::ContainedChild::Spawn({helper, "--list-devices"}, util::ChildStdout::kPipe);
    if (!child) { out.outcome = ListOutcome::kFailed; out.detail = "could not start " + helper; return out; }
    const auto deadline = std::chrono::steady_clock::now() + limit;
    std::string text; char buf[4096];
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) { child->KillGroup(); out.outcome = ListOutcome::kTimedOut; out.detail = "listing timed out"; return out; }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const int slice = static_cast<int>(std::min<std::chrono::milliseconds>(left, std::chrono::milliseconds{250}).count());
        size_t got{0}; const auto state = child->ReadStdout(buf, sizeof(buf), slice, got);
        if (state == util::ContainedChild::ReadStatus::kData) text.append(buf, got);
        else if (state == util::ContainedChild::ReadStatus::kClosed) break;
    }
    const int rc = child->Wait();
    out.devices = ParseDeviceListing(text);
    if (rc == 0) out.outcome = ListOutcome::kListed;
    else if (rc == 4) out.outcome = ListOutcome::kNoLoader;
    else { out.outcome = ListOutcome::kFailed; char code[32]; std::snprintf(code, sizeof(code), "exit %d", rc); out.detail = code; }
    return out;
}

DeviceSelection SelectDevices(const std::vector<GpuDevice>& listed, const DeviceChoice& choice)
{
    DeviceSelection out;
    if (choice.mode == DeviceChoice::Mode::kCpuOnly) return out;
    if (choice.mode == DeviceChoice::Mode::kAutomatic) {
        for (const auto& d : listed) if (d.type == "gpu") {
            if (d.integrated) out.integrated_unchosen.push_back(d);
            else if (d.status == DeviceStatus::kOk) out.use.push_back(d);
            else out.unusable.push_back(d);
        }
    } else {
        for (const auto& id : choice.ids) {
            const auto found = std::find_if(listed.begin(), listed.end(), [&](const auto& d) { return d.id == id; });
            if (found == listed.end()) { out.missing.push_back(id); continue; }
            GpuDevice d = *found;
            if (d.type != "gpu") { d.status = DeviceStatus::kOther; d.reason = "not a graphics card"; out.unusable.push_back(std::move(d)); }
            else if (d.status == DeviceStatus::kOk) out.use.push_back(std::move(d));
            else out.unusable.push_back(std::move(d));
        }
        for (const auto& d : listed) if (d.type == "gpu" && std::find(choice.ids.begin(), choice.ids.end(), d.id) == choice.ids.end()) {
            if (d.integrated) out.integrated_unchosen.push_back(d);
            else if (d.status == DeviceStatus::kOk) out.unchosen.push_back(d);
        }
    }
    Sort(out.use); Sort(out.unusable); Sort(out.integrated_unchosen); Sort(out.unchosen);
    return out;
}

std::optional<std::string> SolveDevice(const DeviceSelection& selection) { if (selection.use.empty()) return std::nullopt; return selection.use.front().id; }

GpuSituation ClassifySituation(const DeviceListing& listing, const DeviceChoice& choice, const DeviceSelection& selection)
{
    if (choice.mode == DeviceChoice::Mode::kCpuOnly) return GpuSituation::kCpuChosen;
    if (listing.outcome == ListOutcome::kNoHelper) return GpuSituation::kHelperMissing;
    if (listing.outcome == ListOutcome::kNoLoader) return GpuSituation::kNoLoader;
    if (listing.outcome == ListOutcome::kFailed || listing.outcome == ListOutcome::kTimedOut) return GpuSituation::kListingFailed;
    if (!selection.use.empty()) return selection.unusable.empty() && selection.missing.empty() ? GpuSituation::kReady : GpuSituation::kPartial;
    if (!selection.missing.empty()) return GpuSituation::kChosenMissing;
    if (!selection.unusable.empty()) return GpuSituation::kNoneUsable;
    if (!selection.integrated_unchosen.empty()) return GpuSituation::kIntegratedOnly;
    return GpuSituation::kNoGpu;
}

std::optional<std::string> GpuHelperFor(uint8_t edgebits)
{
    if (const char* custom = std::getenv("CUCKATOO_GPU_SOLVER"); custom && custom[0]) return custom;
    if (edgebits == kBundledHelperEdgeBits) if (const char* bundled = std::getenv("CUCKATOO_GPU_BUNDLED"); bundled && bundled[0]) return bundled;
    return std::nullopt;
}

GpuDiscovery DiscoverGpus(uint8_t edgebits)
{
    std::lock_guard<std::mutex> lock{g_mutex};
    const auto helper = GpuHelperFor(edgebits); const char* configured = std::getenv("CUCKATOO_GPU_DEVICES");
    const std::string key = helper.value_or("") + "\n" + (configured ? configured : "");
    if (const auto found = g_cache->find(key); found != g_cache->end()) return found->second;
    GpuDiscovery result; result.helper = helper; result.choice = DeviceChoiceFromEnv();
    result.listing = helper ? ListDevices(*helper) : DeviceListing{};
    result.selection = SelectDevices(result.listing.devices, result.choice);
    result.situation = ClassifySituation(result.listing, result.choice, result.selection);
    g_cache->emplace(key, result); return result;
}

void ResetGpuDiscoveryForTesting() { std::lock_guard<std::mutex> lock{g_mutex}; g_cache->clear(); }
} // namespace cuckatoo
