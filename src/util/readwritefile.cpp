// Copyright (c) 2015-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Copyright (c) 2017 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/readwritefile.h>

#include <util/fs.h>
#include <util/fs_helpers.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <system_error>
#include <utility>

std::pair<bool,std::string> ReadBinaryFile(const fs::path &filename, size_t maxsize)
{
    FILE *f = fsbridge::fopen(filename, "rb");
    if (f == nullptr)
        return std::make_pair(false,"");
    std::string retval;
    char buffer[128];
    do {
        const size_t n = fread(buffer, 1, std::min(sizeof(buffer), maxsize - retval.size()), f);
        // Check for reading errors so we don't return any data if we couldn't
        // read the entire file (or up to maxsize)
        if (ferror(f)) {
            fclose(f);
            return std::make_pair(false,"");
        }
        retval.append(buffer, buffer+n);
    } while (!feof(f) && retval.size() < maxsize);
    fclose(f);
    return std::make_pair(true,retval);
}

bool WriteBinaryFile(const fs::path &filename, const std::string &data)
{
    // Write a sibling temp file, sync it, then rename it over the target: a crash or a
    // failed write leaves the previous file whole (a Tor or I2P key, a peer store), never
    // a truncated one that a later read would accept.
    const fs::path tmp{filename + ".tmp"};
    FILE *f = fsbridge::fopen(tmp, "wb");
    if (f == nullptr)
        return false;
    const bool written{fwrite(data.data(), 1, data.size(), f) == data.size() && FileCommit(f)};
    if (fclose(f) != 0 || !written) {
        std::error_code ec;
        fs::remove(tmp, ec);
        return false;
    }
    if (!RenameOver(tmp, filename)) {
        std::error_code ec;
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}
