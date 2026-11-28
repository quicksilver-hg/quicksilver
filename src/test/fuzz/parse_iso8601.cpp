// Copyright (c) 2019-present The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <util/time.h>

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

FUZZ_TARGET(parse_iso8601)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());

    // S0399-B-06: the whole range the parser accepts (four-digit years,
    // 0000-01-01T00:00:00Z to 9999-12-31T23:59:59Z), not only int32_t seconds,
    // which end in 2038.
    constexpr int64_t MIN_TIME{-62167219200};
    constexpr int64_t MAX_TIME{253402300799};
    const int64_t random_time = fuzzed_data_provider.ConsumeIntegralInRange<int64_t>(MIN_TIME, MAX_TIME);
    const std::string random_string = fuzzed_data_provider.ConsumeRemainingBytesAsString();

    const std::string iso8601_datetime = FormatISO8601DateTime(random_time);
    const std::string iso8601_date = FormatISO8601Date(random_time);
    assert(iso8601_datetime.substr(0, iso8601_date.size()) == iso8601_date);
    const int64_t parsed_time_1{ParseISO8601DateTime(iso8601_datetime).value()};
    assert(parsed_time_1 == random_time);

    (void)ParseISO8601DateTime(random_string);
}
