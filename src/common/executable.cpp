// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <common/executable.h>
#ifdef WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace common {
fs::path SelfExecutableDir()
{
    // fs::path deliberately deletes its std::string constructor (src/util/fs.h:
    // "Disallow std::string arguments to avoid locale-dependent decoding on
    // windows"), so the narrow branch goes through PathFromString rather than
    // the natural-looking conversion. The wide branch has no such problem: only
    // the narrow overload is deleted, and a wide string is what a path is made
    // of on Windows.
#ifdef WIN32
    wchar_t buf[MAX_PATH + 1];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    // n == MAX_PATH means the name did not fit and was truncated -- a truncated
    // path names a different file, or none, so it is a failure, not an answer.
    if (n == 0 || n >= MAX_PATH) return {};
    return fs::path(std::wstring(buf, n)).parent_path();
#else
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf));
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(buf)) return {};
    return fs::PathFromString(std::string(buf, static_cast<size_t>(n))).parent_path();
#endif
}

bool IsUsableExecutable(const fs::path& p)
{
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return false;
#ifndef WIN32
    return ::access(fs::PathToString(p).c_str(), X_OK) == 0;
#else
    return true; // the check that would matter here is CreateProcess's own
#endif
}
} // namespace common
