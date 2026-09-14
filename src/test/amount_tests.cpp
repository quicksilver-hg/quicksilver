// Copyright (c) 2016-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/amount.h>

#include <limits>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(amount_tests)

BOOST_AUTO_TEST_CASE(MoneyRangeTest)
{
    BOOST_CHECK_EQUAL(MoneyRange(CAmount(-1)), false);
    BOOST_CHECK_EQUAL(MoneyRange(CAmount(0)), true);
    BOOST_CHECK_EQUAL(MoneyRange(CAmount(1)), true);
    BOOST_CHECK_EQUAL(MoneyRange(MAX_MONEY), true);
    BOOST_CHECK_EQUAL(MoneyRange(MAX_MONEY + CAmount(1)), false);
    // Quicksilver: MAX_MONEY is a 1e9-COIN overflow guard, not a 21M cap.
    static_assert(MAX_MONEY == CAmount{1'000'000'000} * COIN, "overflow guard is 1e9 COIN");
    static_assert(MAX_MONEY < std::numeric_limits<int64_t>::max() / 2,
                  "2*MAX_MONEY must not overflow int64 (incremental output-sum safety)");
    BOOST_CHECK_EQUAL(MoneyRange(CAmount{1'000'000'000} * COIN), true);          // exactly the ceiling
    BOOST_CHECK_EQUAL(MoneyRange(CAmount{1'000'000'000} * COIN + 1), false);     // one over
    BOOST_CHECK_EQUAL(MoneyRange(CAmount{21'000'000} * COIN), true);
}

BOOST_AUTO_TEST_SUITE_END()
