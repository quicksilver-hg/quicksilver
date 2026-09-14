// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/chaintype.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(chaintype_tests)

BOOST_AUTO_TEST_CASE(signet_is_not_selectable)
{
    BOOST_CHECK(!ChainTypeFromString("signet").has_value());
}

BOOST_AUTO_TEST_CASE(test_alias_is_not_selectable)
{
    BOOST_CHECK(!ChainTypeFromString("test").has_value());
}

BOOST_AUTO_TEST_SUITE_END()
