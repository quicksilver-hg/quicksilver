#!/usr/bin/env python3
# Copyright (c) 2021 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test ThreadDNSAddressSeed logic for querying DNS seeds."""

import time

from test_framework.netutil import UNREACHABLE_PROXY_ARG
from test_framework.p2p import P2PInterface
from test_framework.test_framework import QuicksilverTestFramework


class P2PDNSSeeds(QuicksilverTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [["-dnsseed=1", UNREACHABLE_PROXY_ARG]]

    def run_test(self):
        self.init_arg_tests()
        self.existing_outbound_connections_test()
        self.existing_block_relay_connections_test()
        # No DNS-seed delay coverage: Quicksilver ships no DNS seeds (peer discovery is
        # an onion seed plus documented -addnode), so the node never bootstraps from
        # source=dnsseed and the wait_s= log those checks asserted on is never emitted.

    def init_arg_tests(self):
        fakeaddr = "fakenodeaddr.fakedomain.invalid."

        self.log.info("Check that setting -connect disables -dnsseed by default")
        self.nodes[0].stop_node()
        with self.nodes[0].assert_debug_log(expected_msgs=["ready dnsseed=disabled"]):
            self.start_node(0, extra_args=[f"-connect={fakeaddr}", UNREACHABLE_PROXY_ARG])

        self.log.info("Check that running -connect and -dnsseed means DNS logic runs.")
        with self.nodes[0].assert_debug_log(expected_msgs=["loaded source=dnsseed addresses="], timeout=12):
            self.restart_node(0, extra_args=[f"-connect={fakeaddr}", "-dnsseed=1", UNREACHABLE_PROXY_ARG])

        # Restore default quicksilver-daemon settings
        self.restart_node(0)

    def existing_outbound_connections_test(self):
        # Make sure addrman is populated to enter the conditional where we
        # delay and potentially skip DNS seeding.
        self.nodes[0].addpeeraddress("192.0.0.8", 19556)

        self.log.info("Check that we *do not* query DNS seeds if we have 2 outbound connections")

        self.restart_node(0)
        with self.nodes[0].assert_debug_log(expected_msgs=[], unexpected_msgs=["loaded source=dnsseed addresses="]):
            for i in range(2):
                self.nodes[0].add_outbound_p2p_connection(P2PInterface(), p2p_idx=i, connection_type="outbound-full-relay")
            time.sleep(12)

    def existing_block_relay_connections_test(self):
        # Make sure addrman is populated to enter the conditional where we
        # delay and potentially skip DNS seeding. No-op when run after
        # existing_outbound_connections_test.
        self.nodes[0].addpeeraddress("192.0.0.8", 19556)

        self.log.info("Check that we do not query DNS seeds if Quicksilver has no seeds configured")

        self.restart_node(0)
        with self.nodes[0].assert_debug_log(expected_msgs=[], unexpected_msgs=["loaded source=dnsseed addresses="]):
            # This mimics the "anchors" logic where nodes are likely to
            # reconnect to block-relay-only connections on startup.
            for i in range(2):
                self.nodes[0].add_outbound_p2p_connection(P2PInterface(), p2p_idx=i, connection_type="block-relay-only")
            time.sleep(12)


if __name__ == '__main__':
    P2PDNSSeeds(__file__).main()
