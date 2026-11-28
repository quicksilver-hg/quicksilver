// Copyright (c) 2012-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <httpserver.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(httpserver_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(request_tracker_closes_before_empty_barrier)
{
    http_detail::HTTPRequestTracker tracker;
    const auto* first = reinterpret_cast<const evhttp_connection*>(1);
    const auto* refused = reinterpret_cast<const evhttp_connection*>(2);
    BOOST_REQUIRE(tracker.AddRequest(first));
    tracker.Close();
    BOOST_CHECK(!tracker.AddRequest(refused));
    tracker.RemoveRequest(first);
    tracker.WaitUntilEmpty();
    BOOST_CHECK_EQUAL(tracker.CountActiveConnections(), 0U);
    BOOST_CHECK(!tracker.AddRequest(first));
    tracker.Open();
    BOOST_CHECK(tracker.AddRequest(first));
    tracker.RemoveRequest(first);
}

BOOST_AUTO_TEST_SUITE_END()
