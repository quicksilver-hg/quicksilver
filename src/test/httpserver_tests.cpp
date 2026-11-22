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

BOOST_AUTO_TEST_CASE(test_query_parameters)
{
    std::string uri {};

    // No parameters
    uri = "localhost:8080/rest/headers/someresource.json";
    BOOST_CHECK(!GetQueryParameterFromUri(uri.c_str(), "p1").has_value());

    // Single parameter
    uri = "localhost:8080/rest/endpoint/someresource.json?p1=v1";
    BOOST_CHECK_EQUAL(GetQueryParameterFromUri(uri.c_str(), "p1").value(), "v1");
    BOOST_CHECK(!GetQueryParameterFromUri(uri.c_str(), "p2").has_value());

    // Multiple parameters
    uri = "/rest/endpoint/someresource.json?p1=v1&p2=v2";
    BOOST_CHECK_EQUAL(GetQueryParameterFromUri(uri.c_str(), "p1").value(), "v1");
    BOOST_CHECK_EQUAL(GetQueryParameterFromUri(uri.c_str(), "p2").value(), "v2");

    // If the query string contains duplicate keys, the first value is returned
    uri = "/rest/endpoint/someresource.json?p1=v1&p1=v2";
    BOOST_CHECK_EQUAL(GetQueryParameterFromUri(uri.c_str(), "p1").value(), "v1");

    // Invalid query string syntax is the same as not having parameters
    uri = "/rest/endpoint/someresource.json&p1=v1&p2=v2";
    BOOST_CHECK(!GetQueryParameterFromUri(uri.c_str(), "p1").has_value());

    // URI with invalid characters (%) raises a runtime error regardless of which query parameter is queried
    uri = "/rest/endpoint/someresource.json&p1=v1&p2=v2%";
    BOOST_CHECK_EXCEPTION(GetQueryParameterFromUri(uri.c_str(), "p1"), std::runtime_error, HasReason("URI parsing failed, it likely contained RFC 3986 invalid characters"));
}
BOOST_AUTO_TEST_SUITE_END()
