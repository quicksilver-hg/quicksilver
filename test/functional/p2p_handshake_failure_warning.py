#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the node warning for outbound connections that keep failing before the handshake.

A sandbox node and a publictest node cannot talk: their network magic differs.
Outbound attempts from one to the other close before the handshake, which must
raise a node warning naming the running network. A completed outbound handshake
clears it, inbound failures never raise it, and it is logged once per raise.
"""

import time

from test_framework.p2p import P2PInterface
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal,
    p2p_port,
)

WARNING = ("Connections to other nodes keep failing before they finish. Check that those nodes run "
           "the same network as this one ({}) and a current Quicksilver release.")
RAISE_LOG = "outbound connections closed before finishing the handshake"
THRESHOLD = 3
# The shipping default; the framework writes a huge peertimeout so that tests
# can jump mocktime, but the v2 case needs the handshake timer to fire.
PEER_TIMEOUT = 60


class HandshakeFailureWarningTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.disable_autoconnect = True

    def setup_network(self):
        self.add_nodes(self.num_nodes)
        public = self.nodes[1]
        public.chain = "publictest"
        public.replace_in_config([
            ("sandbox=1", "publictest=1"),
            ("[sandbox]", "[publictest]"),
        ])

    def start_pair(self, *, v2):
        args = ["-connect=0", "-dnsseed=0", "-fixedseeds=0", "-listenonion=0",
                f"-peertimeout={PEER_TIMEOUT}", f"-v2transport={int(v2)}"]
        self.start_node(0, args)
        # -prune: on start the publictest node warns on stderr when the disk can't hold the whole
        # publictest chain (128 GB), which this test never downloads; pruned, the check asks for 550 MB.
        self.start_node(1, args + ["-prune=550"])
        assert_equal(self.nodes[0].getblockchaininfo()["chain"], "sandbox")
        assert_equal(self.nodes[1].getblockchaininfo()["chain"], "publictest")

    @staticmethod
    def has_warning(node, chain):
        return WARNING.format(chain) in node.getnetworkinfo()["warnings"]

    def fail_outbound(self, src, dst_index, *, bump=None):
        """Open one manual connection to the other network and wait for it to close."""
        src.addnode(f"127.0.0.1:{p2p_port(dst_index)}", "onetry")
        if bump is not None:
            bump()
        self.wait_until(lambda: len(src.getpeerinfo()) == 0)

    def raise_count(self, node, offset):
        log = node.debug_log_path.read_bytes()[offset:].decode("utf8", errors="replace")
        return log.count(RAISE_LOG)

    def test_v1_outbound(self):
        self.log.info("v1: outbound attempts to another network raise the warning at the threshold")
        self.start_pair(v2=False)
        sandbox = self.nodes[0]
        offset = sandbox.debug_log_path.stat().st_size

        for _ in range(THRESHOLD - 1):
            self.fail_outbound(sandbox, 1)
        assert not self.has_warning(sandbox, "sandbox")

        self.fail_outbound(sandbox, 1)
        self.wait_until(lambda: self.has_warning(sandbox, "sandbox"))
        # A development build also carries the unreleased-build warning, so count ours.
        assert_equal(sandbox.getnetworkinfo()["warnings"].count(WARNING.format("sandbox")), 1)

        self.log.info("Further failures neither repeat the warning nor log it again")
        for _ in range(2):
            self.fail_outbound(sandbox, 1)
        assert_equal(sandbox.getnetworkinfo()["warnings"].count(WARNING.format("sandbox")), 1)
        assert_equal(self.raise_count(sandbox, offset), 1)

        self.log.info("A completed outbound handshake on the right network clears it")
        sandbox.add_outbound_p2p_connection(P2PInterface(), p2p_idx=0, connection_type="outbound-full-relay")
        self.wait_until(lambda: not self.has_warning(sandbox, "sandbox"))
        sandbox.disconnect_p2ps()
        self.wait_until(lambda: len(sandbox.getpeerinfo()) == 0)

        self.log.info("After a clear, the count starts again and a new raise is logged once")
        for _ in range(THRESHOLD - 1):
            self.fail_outbound(sandbox, 1)
        assert not self.has_warning(sandbox, "sandbox")
        self.fail_outbound(sandbox, 1)
        self.wait_until(lambda: self.has_warning(sandbox, "sandbox"))
        self.wait_until(lambda: self.raise_count(sandbox, offset) == 2)
        self.stop_nodes()

    def test_v1_inbound_only(self):
        self.log.info("v1: the same failures, inbound only, raise nothing on the receiving node")
        self.start_pair(v2=False)
        sandbox, public = self.nodes

        for _ in range(THRESHOLD + 1):
            self.fail_outbound(public, 0)
        # The publictest node made those attempts outbound, so it warns, naming its own network.
        self.wait_until(lambda: self.has_warning(public, "publictest"))
        self.wait_until(lambda: len(sandbox.getpeerinfo()) == 0)
        assert not self.has_warning(sandbox, "sandbox")
        self.stop_nodes()

    def test_v2_outbound(self):
        self.log.info("v2: the handshake timeout, reached by mocktime, counts as a failure")
        self.start_pair(v2=True)
        sandbox = self.nodes[0]
        sandbox.mocktime = int(time.time())
        sandbox.setmocktime(sandbox.mocktime)

        def bump():
            # Jump only once the other side's v2 bytes have arrived. Before
            # that the close is "never received", which retries with v1:
            # not the wrong-network case.
            # The other side may close first: it never finds this network's
            # garbage terminator, and when the random garbage plus what
            # follows passes its limit it drops the connection at once
            # ("missing garbage terminator"). That close is a failure too.
            self.wait_until(lambda: (not (peers := sandbox.getpeerinfo())) or any(p["bytesrecv"] > 0 for p in peers))
            if sandbox.getpeerinfo():
                sandbox.bumpmocktime(PEER_TIMEOUT + 1)

        with sandbox.assert_debug_log(expected_msgs=["handshake timeout"], unexpected_msgs=["retrying with v1"]):
            for _ in range(THRESHOLD - 1):
                self.fail_outbound(sandbox, 1, bump=bump)
            assert not self.has_warning(sandbox, "sandbox")
            self.fail_outbound(sandbox, 1, bump=bump)
            self.wait_until(lambda: self.has_warning(sandbox, "sandbox"))
        self.stop_nodes()

    def run_test(self):
        self.test_v1_outbound()
        self.test_v1_inbound_only()
        self.test_v2_outbound()


if __name__ == '__main__':
    HandshakeFailureWarningTest(__file__).main()
