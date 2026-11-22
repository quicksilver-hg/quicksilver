#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The background miner waits for a peer's headers before it mines (F-610).

A node that starts behind the network sits at its own tip with best header ==
tip until a peer tells it otherwise. Mining on that tip produces blocks the
network has already passed, which are rejected. On main the miner waits until a
peer has sent headers and the tip has caught up with them. -minewithoutpeers
lets a lone node on a test chain mine anyway; it defaults on for sandbox and
publictest, so this test turns it off to exercise main's behaviour.
"""

import time

from test_framework.messages import msg_headers
from test_framework.p2p import P2PInterface
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal

WAIT_ARGS = ["-minewithoutpeers=0"]
# Sandbox solves a block in well under a second, so a miner that is allowed to
# mine has found several in this time.
QUIET_SECONDS = 5


class MiningWaitsForPeerHeadersTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.supports_cli = False

    def setup_network(self):
        # The two nodes start apart; each part connects them on purpose.
        self.setup_nodes()

    def assert_mines_nothing(self, node):
        height = node.getblockcount()
        time.sleep(QUIET_SECONDS)
        assert_equal(node.getblockcount(), height)
        st = node.getminingstatus()
        assert_equal(st["blocks_found"], 0)
        assert_equal(st["mines_without_peers"], False)
        assert_equal(st["waiting_for_sync"], True)

    def run_test(self):
        node0, node1 = self.nodes
        addr = node0.get_deterministic_priv_key().address

        self.log.info("A node behind the network does not mine on its stale tip")
        # node1 is the network: five blocks node0 has not seen.
        self.generate(node1, 5, sync_fun=self.no_op)
        network_tip = node1.getbestblockhash()
        self.restart_node(0, extra_args=WAIT_ARGS)
        assert_equal(node0.getblockcount(), 0)
        node0.startmining(addr)
        self.assert_mines_nothing(node0)

        self.log.info("Once a peer's headers arrive and the tip catches up, it mines on the network's tip")
        self.connect_nodes(0, 1)
        self.wait_until(lambda: node0.getminingstatus()["blocks_found"] >= 1, timeout=60)
        assert_equal(node0.getminingstatus()["waiting_for_sync"], False)
        node0.stopmining()
        assert_equal(node0.getminingstatus()["waiting_for_sync"], False)
        self.sync_blocks()
        # No block of node0's forks off below the network's tip: its index holds
        # one chain, and that chain runs through node1's height-5 block.
        assert_equal(len(node0.getchaintips()), 1)
        assert_equal(node0.getblockhash(5), network_tip)
        assert node0.getblockcount() > 5

        self.log.info("An empty headers answer from a peer counts: that peer has nothing newer")
        self.disconnect_nodes(0, 1)
        self.restart_node(0, extra_args=WAIT_ARGS)
        node0.startmining(addr)
        self.assert_mines_nothing(node0)
        peer = node0.add_p2p_connection(P2PInterface())
        peer.send_and_ping(msg_headers())
        self.wait_until(lambda: node0.getminingstatus()["blocks_found"] >= 1, timeout=60)
        node0.stopmining()
        node0.disconnect_p2ps()

        self.log.info("On sandbox the default lets a lone node mine with no peers")
        self.restart_node(0)
        assert_equal(node0.getconnectioncount(), 0)
        assert_equal(node0.getminingstatus()["mines_without_peers"], True)
        node0.startmining(addr)
        self.wait_until(lambda: node0.getminingstatus()["blocks_found"] >= 1, timeout=60)
        node0.stopmining()


if __name__ == '__main__':
    MiningWaitsForPeerHeadersTest(__file__).main()
