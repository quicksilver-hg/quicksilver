// Copyright (c) 2021-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <banman.h>
#include <chainparams.h>
#include <netbase.h>
#include <node/interface_ui.h>
#include <streams.h>
#include <test/util/logging.h>
#include <test/util/setup_common.h>
#include <util/readwritefile.h>


#include <boost/test/unit_test.hpp>
#include <boost/signals2/connection.hpp>

BOOST_FIXTURE_TEST_SUITE(banman_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(file)
{
    SetMockTime(777s);
    const fs::path banlist_path{m_args.GetDataDirBase() / "banlist_test"};
    {
        const std::string entries_write{
            "{ \"banned_nets\": ["
            "  { \"version\": 1, \"ban_created\": 0, \"banned_until\": 778, \"address\": \"aaaaaaaaa\" },"
            "  { \"version\": 2, \"ban_created\": 0, \"banned_until\": 778, \"address\": \"bbbbbbbbb\" },"
            "  { \"version\": 1, \"ban_created\": 0, \"banned_until\": 778, \"address\": \"1.0.0.0/8\" }"
            "] }",
        };
        BOOST_REQUIRE(WriteBinaryFile(banlist_path + ".json", entries_write));
        {
            // The invalid entries will be dropped, but the valid one remains
            ASSERT_DEBUG_LOG("Dropping entry with unparseable address or subnet (aaaaaaaaa) from ban list");
            ASSERT_DEBUG_LOG("Dropping entry with unknown version (2) from ban list");
            BanMan banman{banlist_path, /*client_interface=*/nullptr, /*default_ban_time=*/0};
            banmap_t entries_read;
            banman.GetBanned(entries_read);
            BOOST_CHECK_EQUAL(entries_read.size(), 1);
        }
    }
}

BOOST_AUTO_TEST_CASE(expiry_notification_allows_reentrant_read)
{
    SetMockTime(777s);
    CClientUIInterface ui;
    BanMan banman{m_args.GetDataDirBase() / "banlist_reentry", &ui, /*default_ban_time=*/60};
    unsigned notifications{0};
    auto connection = ui.BannedListChanged_connect([&] {
        banmap_t snapshot;
        banman.GetBanned(snapshot);
        ++notifications;
    });

    banman.Ban(LookupHost("192.0.2.1", false).value(), 1);
    SetMockTime(779s);
    banmap_t expired;
    banman.GetBanned(expired);
    BOOST_CHECK(expired.empty());
    BOOST_CHECK_EQUAL(notifications, 2);
    connection.disconnect();
    SetMockTime(0s);
}

BOOST_AUTO_TEST_SUITE_END()
