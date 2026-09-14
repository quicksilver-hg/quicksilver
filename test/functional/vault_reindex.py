#!/usr/bin/env python3
# Copyright (c) 2023-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php.

"""Test vault-reindex interaction"""

import time

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.descriptors import descsum_create
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal,
)
BLOCK_TIME = 60 * 10

class VaultReindexTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()

    def advance_time(self, node, secs):
        self.node_time += secs
        node.setmocktime(self.node_time)

    # Verify the vault updates the birth time accordingly when it detects a transaction
    # with a time older than the oldest descriptor timestamp.
    # This could happen when the user blindly imports a descriptor with 'timestamp=now'.
    def birthtime_test(self, node, miner_vault):
        self.log.info("Test birth time update during tx scanning")
        # Fund address to test
        vault_addr = miner_vault.getnewaddress()
        tx_id = miner_vault.sendtoaddress(vault_addr, 2)

        # Generate 50 blocks, one every 10 min to surpass the 2 hours rescan window the vault has
        for _ in range(50):
            self.generate(node, 1)
            self.advance_time(node, BLOCK_TIME)

        # Now create a new vault, and import the descriptor
        node.createvault(vault_name='tracking', disable_private_keys=True, load_on_startup=True)
        tracking_vault = node.get_vault_rpc('tracking')
        # Blank vaults don't have a birth time
        assert 'birthtime' not in tracking_vault.getvaultinfo()

        # Import the address with timestamp=now so the existing transaction is not detected.
        tracking_vault.importdescriptors([{"desc": descsum_create(f"addr({vault_addr})"), "timestamp": "now"}])
        assert_equal(len(tracking_vault.listtransactions()), 0)

        vault_birthtime = tracking_vault.getvaultinfo()['birthtime']
        # As blocks were generated every 10 min, the chain MTP timestamp is node_time - 60 min.
        assert_equal(self.node_time - BLOCK_TIME * 6, vault_birthtime)

        # Rescan the vault to detect the missing transaction
        tracking_vault.rescanblockchain()
        assert_equal(tracking_vault.gettransaction(tx_id)['confirmations'], 50)
        assert_equal(tracking_vault.getbalances()['mine']['trusted'], 2)

        # Reindex and wait for it to finish
        with node.assert_debug_log(expected_msgs=["initload thread exit"]):
            self.restart_node(0, extra_args=['-reindex=1', f'-mocktime={self.node_time}'])
        node.syncwithvalidationinterfacequeue()

        # Verify the transaction is still 'confirmed' after reindex
        tracking_vault = node.get_vault_rpc('tracking')
        tx_info = tracking_vault.gettransaction(tx_id)
        assert_equal(tx_info['confirmations'], 50)

        # Verify the vault updated the birth time to the transaction time.
        assert_equal(tx_info['time'], tracking_vault.getvaultinfo()['birthtime'])

        tracking_vault.unloadvault()

    def run_test(self):
        node = self.nodes[0]
        self.node_time = int(time.time())
        node.setmocktime(self.node_time)

        # Fund miner
        node.createvault(vault_name='miner', load_on_startup=True)
        miner_vault = node.get_vault_rpc('miner')
        self.generatetoaddress(node, COINBASE_MATURITY + 10, miner_vault.getnewaddress())

        # Tests
        self.birthtime_test(node, miner_vault)


if __name__ == '__main__':
    VaultReindexTest(__file__).main()
