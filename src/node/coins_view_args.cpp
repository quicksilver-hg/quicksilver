// Copyright (c) 2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/coins_view_args.h>

#include <common/args.h>
#include <txdb.h>
#include <util/result.h>
#include <util/translation.h>

#include <limits>

namespace node {
util::Result<void> ReadCoinsViewArgs(const ArgsManager& args, CoinsViewOptions& options)
{
    if (auto value = args.GetIntArg("-dbbatchsize")) {
        if (*value <= 0 || static_cast<uint64_t>(*value) > std::numeric_limits<size_t>::max()) {
            return util::Error{Untranslated("-dbbatchsize must be a positive value within the supported range")};
        }
        options.batch_write_bytes = static_cast<size_t>(*value);
    }
    if (auto value = args.GetIntArg("-dbcrashratio")) {
        if (*value < 0 || *value > std::numeric_limits<int>::max()) {
            return util::Error{Untranslated("-dbcrashratio must be between 0 and 2147483647")};
        }
        options.simulate_crash_ratio = static_cast<int>(*value);
    }
    return {};
}
} // namespace node
