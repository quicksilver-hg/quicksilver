// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef QUICKSILVER_CRYPTO_CUCKATOO_GPU_WATCHDOG_CHECK_H
#define QUICKSILVER_CRYPTO_CUCKATOO_GPU_WATCHDOG_CHECK_H
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#ifdef _MSC_VER
#pragma comment(lib, "advapi32.lib")
#endif
#endif

namespace qsgpu {
inline void WarnIfWatchdogShort(const char* tool, const char* hint)
{
#ifdef _WIN32
    DWORD value{0}, size{sizeof(value)};
    const LSTATUS st = RegGetValueA(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", "TdrDelay",
        RRF_RT_REG_DWORD, nullptr, &value, &size);
    const unsigned long effective = st == ERROR_SUCCESS ? static_cast<unsigned long>(value) : 2UL;
    if (effective < 10UL) {
        std::fprintf(stderr, "%s: WARNING TdrDelay is %lu s%s. A Cuckatoo graph takes several seconds, so the display-driver watchdog is likely to reset the GPU mid-kernel. %s\n",
            tool, effective, st == ERROR_SUCCESS ? "" : " (unset, so the 2 s default applies)", hint);
    }
#else
    (void)tool; (void)hint;
#endif
}
} // namespace qsgpu
#endif // QUICKSILVER_CRYPTO_CUCKATOO_GPU_WATCHDOG_CHECK_H
