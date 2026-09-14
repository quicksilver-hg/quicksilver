#!/usr/bin/env python3
# Copyright (c) 2016-2021 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that a feeless node does not speak BIP133 feefilter (#6).

Quicksilver is feeless (#5b): there is no relay-fee floor, so an advertised
feefilter would always be 0 and carries no information. The type is not a
registered protocol message. BIP324 short id 5 is an unused slot so later
short ids stay put. MaybeSendFeefilter is gone, so NO peer -- default,
block-relay-only, or in blocksonly mode -- ever receives a feefilter. An
inbound feefilter is unknown traffic counted under *other*, not a known
command this node ignores.

This replaces the upstream fee-based feefilter test: the original asserted that
default peers DO receive a feefilter and exercised fee-rate-based relay
filtering, both of which have no meaning on a feeless chain. An earlier
Quicksilver pass kept the type registered for BIP324 id stability and only
checked that it was never sent.

The test framework no longer models feefilter as a protocol message either:
there is no msg_feefilter and no b"feefilter" entry in p2p.MESSAGEMAP, so the
outbound direction is now covered twice over. The node-side assertion below is
the explicit one -- CNode::mapSendBytesPerMsgType keys by the raw message type
with no *other* bucket on the send side (src/net.h), so a resurrected feefilter
would show up as a "feefilter" key. On top of that, P2PConnection._on_data
raises "Received unknown msgtype" for anything outside MESSAGEMAP, which now
makes an inbound feefilter fatal to EVERY p2p test rather than only to one that
opted in via an on_feefilter callback. Sending one still needs no registered
type: msg_generic puts arbitrary bytes on the wire under an arbitrary name.
"""

from test_framework.messages import msg_generic
from test_framework.p2p import P2PInterface
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal, assert_greater_than


def msg_feefilter(feerate=0):
    """A BIP133 feefilter on the wire, built without a registered message type."""
    return msg_generic(b"feefilter", feerate.to_bytes(8, "little"))


class FeeFilterTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        # No funded chain needed -- this test only checks p2p feefilter behaviour, and the
        # cached-chain init generates to a hardcoded address invalid for this fork.
        self.setup_clean_chain = True

    def assert_no_feefilter_sent(self, peer):
        """Assert the node has sent no feefilter, from its own byte accounting.

        Also assert the connection is still up: the framework tears a peer down
        with "Received unknown msgtype" the moment a feefilter arrives, so a
        live connection is itself evidence that none did.
        """
        assert peer.is_connected
        for info in self.nodes[0].getpeerinfo():
            assert "feefilter" not in info["bytessent_per_msg"]

    def run_test(self):
        self.log.info("A default inbound peer never receives a feefilter")
        # The former code armed m_next_send_feefilter=0, so a feefilter would be
        # sent on the very first SendMessages tick after connect. sync_with_ping
        # forces a full message-processing round-trip; if sending were re-enabled
        # this would put a "feefilter" key in the node's send accounting.
        peer = self.nodes[0].add_p2p_connection(P2PInterface())
        peer.sync_with_ping()
        self.assert_no_feefilter_sent(peer)

        self.log.info("An inbound feefilter is unknown traffic, not a known ignored command")
        peer.send_message(msg_feefilter(0))
        peer.sync_with_ping()
        self.assert_no_feefilter_sent(peer)
        recv = self.nodes[0].getpeerinfo()[0]["bytesrecv_per_msg"]
        assert "feefilter" not in recv
        assert_greater_than(recv["*other*"], 0)

        self.log.info("A block-relay-only peer never receives a feefilter")
        bro = self.nodes[0].add_outbound_p2p_connection(
            P2PInterface(), p2p_idx=0, connection_type="block-relay-only")
        bro.sync_with_ping()
        assert_equal(
            [i["connection_type"] for i in self.nodes[0].getpeerinfo()].count("block-relay-only"), 1)
        self.assert_no_feefilter_sent(bro)

        self.log.info("A peer of a blocksonly node never receives a feefilter")
        self.restart_node(0, ["-blocksonly"])
        bo_peer = self.nodes[0].add_p2p_connection(P2PInterface())
        bo_peer.sync_with_ping()
        self.assert_no_feefilter_sent(bo_peer)

        self.log.info("Feeless node never broadcasts a feefilter: PASS")


if __name__ == '__main__':
    FeeFilterTest(__file__).main()
