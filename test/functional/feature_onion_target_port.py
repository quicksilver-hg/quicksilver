#!/usr/bin/env python3
# Copyright (c) 2024-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test conflicts between the default onion target and RPC bind ports."""

from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import p2p_port, tor_port


class OnionTargetPortTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.bind_to_localhost_only = False
        self.num_nodes = 1

    def setup_network(self):
        peer_port = p2p_port(self.num_nodes)
        self.add_nodes(
            self.num_nodes,
            self.extra_args,
            rpchost=f"127.0.0.1:{peer_port + 1}",
        )

    def run_test(self):
        node = self.nodes[0]
        # Suppress the framework's automatic -bind arguments: this test needs
        # the node to choose its default onion target.
        node.has_explicit_bind = True

        peer_port = p2p_port(self.num_nodes)
        onion_target_port = peer_port + 1
        explicit_onion_port = p2p_port(self.num_nodes + 3)
        base_args = [
            "-server=1",
            "-listen=1",
            f"-port={peer_port}",
            f"-rpcport={onion_target_port}",
            f"-torcontrol=127.0.0.1:{tor_port(self.num_nodes)}",
        ]
        expected_error = (
            f"Error: -rpcport={onion_target_port} is the onion service target port "
            "(-port + 1). Choose another -rpcport, or set -bind=...=onion to "
            "move the onion target."
        )

        self.log.info("Reject an RPC port that conflicts with the default onion target")
        node.assert_start_raises_init_error(
            extra_args=base_args + ["-listenonion=1"],
            expected_msg=expected_error,
        )

        self.log.info("Allow the same ports when the onion service is disabled")
        self.start_node(0, extra_args=base_args + ["-listenonion=0"])
        self.stop_node(0)

        self.log.info("Allow the same ports with an explicit onion target")
        self.start_node(0, extra_args=base_args + [
            "-listenonion=1",
            f"-bind=127.0.0.1:{explicit_onion_port}=onion",
        ])
        self.stop_node(0)


if __name__ == "__main__":
    OnionTargetPortTest(__file__).main()
