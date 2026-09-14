#!/usr/bin/env python3
# Copyright (c) 2023 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test fastprune mode."""
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal
)
from test_framework.vault import MiniVault


class FeatureFastpruneTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-fastprune", "-txpownocycle=1"]]

    def run_test(self):
        self.log.info("ensure that large blocks don't crash or freeze in -fastprune")
        vault = MiniVault(self.nodes[0])
        tx = vault.create_self_transfer()['tx']
        annex = b"\x50" + b"\xff" * 0x10000
        tx.wit.vtxinwit[0].scriptWitness.stack.append(annex)
        self.generateblock(self.nodes[0], output="raw(55)", transactions=[tx.serialize().hex()])
        assert_equal(self.nodes[0].getblockcount(), 201)


if __name__ == '__main__':
    FeatureFastpruneTest(__file__).main()
