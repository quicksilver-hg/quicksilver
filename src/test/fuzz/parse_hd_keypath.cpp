// Copyright (c) 2009-2020 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <util/bip32.h>

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

FUZZ_TARGET(parse_hd_keypath)
{
    const std::string keypath_str(buffer.begin(), buffer.end());
    std::vector<uint32_t> keypath;
    if (ParseHDKeypath(keypath_str, keypath)) {
        // S0399-B-05: parse, format, parse gives the same path. The parser's
        // grammar marks hardened steps with ' only (bip32.h), so the round trip
        // uses that form; the default 'h' form is display output.
        std::vector<uint32_t> reparsed;
        assert(ParseHDKeypath(WriteHDKeypath(keypath, /*apostrophe=*/true), reparsed));
        assert(reparsed == keypath);
    }

    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());
    const std::vector<uint32_t> random_keypath = ConsumeRandomLengthIntegralVector<uint32_t>(fuzzed_data_provider);
    const std::string formatted{FormatHDKeypath(random_keypath, /*apostrophe=*/true)}; // WriteHDKeypath calls this with false
    const std::string written{WriteHDKeypath(random_keypath)};
    assert(written == "m" + FormatHDKeypath(random_keypath, /*apostrophe=*/false));
    // Format, parse: every path, hardened or not, survives the round trip.
    std::vector<uint32_t> reparsed;
    assert(ParseHDKeypath("m" + formatted, reparsed));
    assert(reparsed == random_keypath);
}
