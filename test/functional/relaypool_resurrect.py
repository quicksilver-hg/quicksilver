#!/usr/bin/env python3
# Copyright (c) 2014-2021 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test resurrection of mined transactions when the blockchain is re-organized."""

from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal
from test_framework.vault import MiniVault


class RelayPoolCoinbaseTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-txpownocycle=1"]]

    def run_test(self):
        node = self.nodes[0]
        vault = MiniVault(node)

        # Spend block 1/2/3's coinbase transactions
        # Mine a block
        # Create three more transactions, spending the spends
        # Mine another block
        # ... make sure all the transactions are confirmed
        # Invalidate both blocks
        # ... make sure all the transactions are put back in the relaypool
        # Mine a new block
        # ... make sure all the transactions are confirmed again
        blocks = []
        spends1 = [vault.send_self_transfer(from_node=node, confirmed_only=True) for _ in range(3)]
        spends1_ids = [tx['txid'] for tx in spends1]
        blocks.extend(self.generate(node, 1))
        spends2 = [vault.send_self_transfer(from_node=node, utxo_to_spend=tx["new_utxo"]) for tx in spends1]
        spends2_ids = [tx['txid'] for tx in spends2]
        blocks.extend(self.generate(node, 1))

        spends_ids = set(spends1_ids + spends2_ids)

        # relaypool should be empty, all txns confirmed
        assert_equal(set(node.getrawrelaypool()), set())
        confirmed_txns = set(node.getblock(blocks[0])['tx'] + node.getblock(blocks[1])['tx'])
        # Checks that all spend txns are contained in the mined blocks
        assert spends_ids < confirmed_txns

        # Use invalidateblock to re-org back
        invalidated_time = node.getblockheader(blocks[0])["time"]
        node.invalidateblock(blocks[0])

        # All txns should be back in relaypool with 0 confirmations
        assert_equal(set(node.getrawrelaypool()), spends_ids)

        # Generate another block, they should all get mined
        node.setmocktime(invalidated_time + 1)
        blocks = self.generate(node, 2)
        # relaypool should be empty, all txns confirmed
        assert_equal(set(node.getrawrelaypool()), set())
        confirmed_txns = set()
        for block in blocks:
            confirmed_txns.update(node.getblock(block)['tx'])
        assert spends_ids < confirmed_txns


if __name__ == '__main__':
    RelayPoolCoinbaseTest(__file__).main()
