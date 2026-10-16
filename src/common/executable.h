// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef QUICKSILVER_COMMON_EXECUTABLE_H
#define QUICKSILVER_COMMON_EXECUTABLE_H
#include <util/fs.h>
namespace common {
//! The directory our own executable lives in, or an empty path when the platform
//! will not say. Only the search that consumes this is platform-neutral and
//! tested; asking the OS where we are cannot be.
fs::path SelfExecutableDir();
//! A regular file this process may execute (POSIX: X_OK; Windows: a regular file).
bool IsUsableExecutable(const fs::path& p);
} // namespace common
#endif // QUICKSILVER_COMMON_EXECUTABLE_H
