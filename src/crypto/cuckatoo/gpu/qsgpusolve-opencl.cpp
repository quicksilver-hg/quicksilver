// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Mining and transfer-proof helper on any OpenCL device. NEVER on the
// validation path: the node verifies every cycle it reports.
//
// Usage:
//   qsgpusolve-opencl --version        (or -version)
//   qsgpusolve-opencl --list-devices
//   qsgpusolve-opencl [--device <id>] <edgebits> <preimage_hex> <start_nonce> <max_attempts>
//
// Exit codes -- the bridge in ../gpu_solver.cpp maps these, and
// test/lint/lint-cuckatoo-solver-filenames.py asserts this block stays here:
//   0  solved (output above), or no cycle in the requested nonce window;
//      for --list-devices, the runtime loaded (zero devices included)
//   2  usage error: bad arguments, or unparseable preimage hex
//   3  this binary was compiled for a different EDGEBITS than requested
//   4  no usable GPU device for this backend: no OpenCL loader, a --device that
//      is not present, a kernel that does not compile, or a failed start-up probe
//   5  the device faulted while solving (an enqueue, read or finish failed)
#include <quicksilver-build-config.h> // IWYU pragma: keep

#include <crypto/cuckatoo/gpu/opencl_loader.h>
#include <crypto/cuckatoo/gpu/opencl_devices.h>
#include <crypto/cuckatoo/gpu/watchdog_check.h>
#include <crypto/cuckatoo/vendor/lean_ocl.hpp>

#include <clientversion.h>
#include <util/translation.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <ctime>
#include <vector>

const TranslateFn G_TRANSLATION_FUN{nullptr};

namespace {
int Usage(const char* argv0)
{
    std::fprintf(stderr,
                 "usage: %s --version\n"
                 "       %s --list-devices\n"
                 "       %s [--device <id>] <edgebits> <preimage_hex> <start_nonce> <max_attempts>\n",
                 argv0, argv0, argv0);
    return qsgpu::kExitUsage;
}

int Nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ParseHex(const char* hex, std::vector<char>& out)
{
    const size_t size = std::strlen(hex);
    if ((size & 1) != 0 || size < 8) return false;
    out.resize(size / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        const int high = Nibble(hex[2 * i]), low = Nibble(hex[2 * i + 1]);
        if (high < 0 || low < 0) return false;
        out[i] = static_cast<char>((high << 4) | low);
    }
    return true;
}

bool ParseDecimal(const char* text, uint32_t& out)
{
    if (!text[0]) return false;
    uint64_t value{0};
    for (const unsigned char c : std::string_view{text}) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
        if (value > UINT32_MAX) return false;
    }
    out = static_cast<uint32_t>(value);
    return true;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "-version") == 0)) {
        std::printf("%s graphics helper version %s\n", CLIENT_NAME, FormatFullVersion().c_str());
        return qsgpu::kExitOk;
    }
    if (argc == 2 && std::strcmp(argv[1], "--list-devices") == 0) {
        qsgpu::ApplyRusticlDefault();
        std::string error;
        const auto api = qsgpu::LoadOpenCl(error);
        if (!api) {
            std::fprintf(stderr, "qsgpusolve-opencl: %s\n", error.c_str());
            return qsgpu::kExitNoDevice;
        }
        for (const auto& device : qsgpu::EnumerateDevices(*api, true, qsgpu::ScanRenderNodes())) {
            std::printf("%s\n", qsgpu::FormatListingLine(device.fields).c_str());
        }
        return qsgpu::kExitOk;
    }
    int first = 1;
    std::string device;
    if (argc >= 3 && std::strcmp(argv[1], "--device") == 0) {
        device = argv[2];
        first = 3;
    }
    if (argc - first != 4) return Usage(argv[0]);
    uint32_t edgebits{0};
    if (!ParseDecimal(argv[first], edgebits)) return Usage(argv[0]);
    if (edgebits != EDGEBITS) {
        std::fprintf(stderr, "qsgpusolve-opencl: edgebits %u != compiled EDGEBITS %d\n", edgebits, EDGEBITS);
        return qsgpu::kExitEdgeBits;
    }
    std::vector<char> preimage;
    uint32_t start{0}, attempts{0};
    if (!ParseHex(argv[first + 1], preimage) || !ParseDecimal(argv[first + 2], start) || !ParseDecimal(argv[first + 3], attempts)) return Usage(argv[0]);

    qsgpu::ApplyRusticlDefault();
    std::string error;
    const auto api = qsgpu::LoadOpenCl(error);
    if (!api) { std::fprintf(stderr, "qsgpusolve-opencl: %s\n", error.c_str()); return qsgpu::kExitNoDevice; }
    const auto devices = qsgpu::EnumerateDevices(*api, false, qsgpu::ScanRenderNodes());
    const qsgpu::ListedDevice* selected = qsgpu::SelectListedDevice(devices, device);
    if (selected == nullptr) {
        std::fprintf(stderr, "qsgpusolve-opencl: device %s not found\n", device.empty() ? "<first GPU>" : device.c_str());
        return qsgpu::kExitNoDevice;
    }
#ifdef _WIN32
    constexpr const char* hint = "on Windows this is usually the display-driver watchdog: TDR resets the driver when a kernel holds the GPU past TdrDelay (default 2 s). Set HKLM\\SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers\\TdrDelay (DWORD) to 60, reboot, and check for Event ID 4101 from source Display";
#else
    constexpr const char* hint = "check dmesg for amdgpu, i915, xe or NVRM/Xid reset messages, and clinfo for the device";
#endif
    qsgpu::WarnIfWatchdogShort("qsgpusolve-opencl", hint);
    auto solver = qsgpu::OclLean::Create(*api, *selected, error);
    if (!solver) { std::fprintf(stderr, "qsgpusolve-opencl: %s\n", error.c_str()); return qsgpu::kExitNoDevice; }

    std::time_t last_beat = std::time(nullptr);
    for (uint32_t i = 0; i < attempts; ++i) {
        const uint32_t nonce = start + i;
        const std::time_t now = std::time(nullptr);
        if (now != last_beat) { std::printf("progress=%u\n", nonce); std::fflush(stdout); last_beat = now; }
        const size_t len = preimage.size();
        preimage[len - 4] = static_cast<char>(nonce & 0xff);
        preimage[len - 3] = static_cast<char>((nonce >> 8) & 0xff);
        preimage[len - 2] = static_cast<char>((nonce >> 16) & 0xff);
        preimage[len - 1] = static_cast<char>((nonce >> 24) & 0xff);
        qsgpu::OclWord solutions[4][qsgpu::kOclProofSize]{}; bool fault{false}; std::string fault_text;
        const int found = solver->Solve(preimage.data(), static_cast<uint32_t>(len), solutions, 4, fault, fault_text);
        if (fault) {
            std::fprintf(stderr, "qsgpusolve-opencl: device fault at nonce %u: %s\n", nonce, fault_text.c_str());
            std::fprintf(stderr, "qsgpusolve-opencl: %s\n", hint);
            return qsgpu::kExitDeviceFault;
        }
        if (found > 0) {
            std::printf("nonce=%u\ncycle=", nonce);
            for (int j = 0; j < qsgpu::kOclProofSize; ++j) std::printf("%llx%s", static_cast<unsigned long long>(solutions[0][j]), j + 1 < qsgpu::kOclProofSize ? " " : "\n");
            return qsgpu::kExitOk;
        }
    }
    return qsgpu::kExitOk;
}
