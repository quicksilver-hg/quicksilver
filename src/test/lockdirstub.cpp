// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// A second process for test_LockDirectory, which cannot test what it exists to
// test from inside one process.
//
// The datadir lock's whole promise is that a SECOND PROCESS cannot open a
// directory this one holds. Upstream tested that by fork()ing, and guarded the
// whole thing `#ifndef WIN32` "due to lack of fork()" -- so on Windows the case
// still ran, still passed, and silently verified nothing about the promise. That
// reason is about the MECHANISM, not the contract: Windows locks the directory
// with a real CreateFileW handle (src/util/fs.cpp), so the contract is just as
// enforceable there, and the box that most needs checking is the one where a
// stale lock is hardest to see.
//
// This is a compiled executable rather than a script for the same reason
// src/qt/test/solverprobestub.cpp is: "executable" is spelled differently per
// platform, and CreateProcess cannot launch a .bat or .cmd.
//
// Synchronisation is by FILE, not by pipe. Byte-at-a-time streaming through the
// vendored subprocess.h is exercised nowhere else in this tree -- run_command.cpp
// only ever calls communicate() -- and that header carried two Windows defects
// (it did not compile under MSVC; Popen::wait() reported success for every failed
// process) that were only fixed recently. Testing the datadir lock THROUGH an
// unproven Windows streaming path would turn a bridge bug into a confusing lock
// failure, or a hang. Spawn and wait() are all this needs, and both are proven.

#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/translation.h>

#include <chrono>
#include <iostream>
#include <fstream>
#include <string>
#include <thread>

//! Required by anything linking quicksilver_util: clientversion references it.
//! This stub prints no user-facing strings, so it needs no translator.
const TranslateFn G_TRANSLATION_FUN{nullptr};

namespace {
//! Single-character outcomes, chosen so a stray file is readable by eye.
constexpr char kSuccess{'S'};
constexpr char kErrorWrite{'W'};
constexpr char kErrorLock{'L'};

//! Generous: this bounds a hang, it is not a performance expectation. A parent
//! that died leaves the child to exit on its own rather than linger forever.
constexpr auto kWaitLimit{std::chrono::seconds(60)};

char Attempt(const fs::path& dir, const fs::path& lockname)
{
    switch (util::LockDirectory(dir, lockname)) {
    case util::LockResult::Success: return kSuccess;
    case util::LockResult::ErrorWrite: return kErrorWrite;
    case util::LockResult::ErrorLock: return kErrorLock;
    }
    return '?';
}

void Announce(const fs::path& path, char outcome)
{
    // Write to a temporary and rename, so the parent can never observe a
    // half-written marker and read '\0' as an outcome.
    const fs::path tmp{fs::PathFromString(fs::PathToString(path) + ".tmp")};
    { std::ofstream out{tmp, std::ios::binary}; out.put(outcome); }
    fs::rename(tmp, path);
}

//! Returns false if the file never appeared inside the limit.
bool AwaitFile(const fs::path& path)
{
    const auto deadline{std::chrono::steady_clock::now() + kWaitLimit};
    while (std::chrono::steady_clock::now() < deadline) {
        if (fs::exists(path)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
} // namespace

int main(int argc, char* argv[])
{
    // <dir> <lockname> <mode> <ready> [<go> [<released> <go2>]]
    if (argc < 5) {
        std::cerr << "usage: " << argv[0] << " DIR LOCKNAME MODE READY [GO [RELEASED GO2]]" << std::endl;
        return 64;
    }
    const fs::path dir{fs::PathFromString(argv[1])};
    const fs::path lockname{fs::PathFromString(argv[2])};
    const std::string mode{argv[3]};
    const fs::path ready{fs::PathFromString(argv[4])};

    if (mode == "try") {
        // Attempt and report. Exiting drops whatever was acquired.
        Announce(ready, Attempt(dir, lockname));
        return 0;
    }

    if (argc < 6) {
        std::cerr << argv[0] << ": mode '" << mode << "' needs a GO file" << std::endl;
        return 64;
    }
    const fs::path go{fs::PathFromString(argv[5])};

    if (mode == "hold-exit") {
        // Hold until released, then exit STILL HOLDING, so the parent observes
        // that process exit alone frees the lock.
        Announce(ready, Attempt(dir, lockname));
        if (!AwaitFile(go)) return 65;
        return 0;
    }

    if (mode == "hold-release") {
        if (argc < 8) {
            std::cerr << argv[0] << ": mode 'hold-release' needs RELEASED and GO2" << std::endl;
            return 64;
        }
        const fs::path released{fs::PathFromString(argv[6])};
        const fs::path go2{fs::PathFromString(argv[7])};

        Announce(ready, Attempt(dir, lockname));
        if (!AwaitFile(go)) return 65;
        // Explicit release WITHOUT exiting: a different contract from the one
        // hold-exit covers, and the only way to see it is from outside.
        ReleaseDirectoryLocks();
        Announce(released, kSuccess);
        if (!AwaitFile(go2)) return 65;
        return 0;
    }

    std::cerr << argv[0] << ": unknown mode '" << mode << "'" << std::endl;
    return 64;
}
