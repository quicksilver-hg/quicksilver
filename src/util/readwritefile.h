// Copyright (c) 2015-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_UTIL_READWRITEFILE_H
#define QUICKSILVER_UTIL_READWRITEFILE_H

#include <util/fs.h>

#include <limits>
#include <string>
#include <utility>

/** Read full contents of a file and return them in a std::string.
 * Returns a pair <status, string>.
 * If an error occurred, status will be false, otherwise status will be true and the data will be returned in string.
 *
 * @param maxsize Puts a maximum size limit on the file that is read. If the file is larger than this, the first
 *         maxsize bytes are returned with status true: the result is indistinguishable from a file of exactly
 *         maxsize bytes, so a caller that must detect truncation has to read with a larger limit.
 */
std::pair<bool,std::string> ReadBinaryFile(const fs::path &filename, size_t maxsize=std::numeric_limits<size_t>::max());

/** Write contents of std::string to a file, atomically: the data goes to "<filename>.tmp",
 * is synced to disk and is then renamed over filename. On any failure filename keeps its
 * previous contents (or stays absent) and the temp file is removed.
 * @return true on success.
 */
bool WriteBinaryFile(const fs::path &filename, const std::string &data);

#endif // QUICKSILVER_UTIL_READWRITEFILE_H
