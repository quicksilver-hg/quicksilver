#!/usr/bin/env python3
# Copyright (c) 2020-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests that a relaypool transaction expires after a given timeout and that its
children are removed as well.

Both the default expiry timeout defined by DEFAULT_RELAYPOOL_EXPIRY_HOURS and a user
definable expiry timeout via the '-relaypoolexpiry=<n>' command line argument
(<n> is the timeout in hours) are tested.
"""

from datetime import timedelta

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.messages import DEFAULT_RELAYPOOL_EXPIRY_HOURS
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)
from test_framework.vault import MiniVault

CUSTOM_RELAYPOOL_EXPIRY = 10  # hours


class RelayPoolExpiryTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-txpownocycle=1"]]

    def test_transaction_expiry(self, timeout):
        """Tests that a transaction expires after the expiry timeout and its
        children are removed as well."""
        node = self.nodes[0]

        # Send a parent transaction that will expire.
        parent = self.vault.send_self_transfer(from_node=node)
        parent_txid = parent["txid"]
        parent_utxo = self.vault.get_utxo(txid=parent_txid)
        independent_utxo = self.vault.get_utxo()

        # Ensure the transactions we send to trigger the relaypool check spend utxos that are independent of
        # the transactions being tested for expiration.
        trigger_utxo1 = self.vault.get_utxo()
        trigger_utxo2 = self.vault.get_utxo()

        # Set the mocktime to the arrival time of the parent transaction.
        entry_time = node.getrelaypoolentry(parent_txid)['time']
        node.setmocktime(entry_time)

        # Let half of the timeout elapse and broadcast the child transaction spending the parent transaction.
        half_expiry_time = entry_time + int(60 * 60 * timeout/2)
        node.setmocktime(half_expiry_time)
        child_txid = self.vault.send_self_transfer(from_node=node, utxo_to_spend=parent_utxo)['txid']
        assert_equal(parent_txid, node.getrelaypoolentry(child_txid)['depends'][0])
        self.log.info('Broadcast child transaction after {} hours.'.format(
            timedelta(seconds=(half_expiry_time-entry_time))))

        # Broadcast another (independent) transaction.
        independent_txid = self.vault.send_self_transfer(from_node=node, utxo_to_spend=independent_utxo)['txid']

        # Let most of the timeout elapse and check that the parent tx is still
        # in the relaypool.
        nearly_expiry_time = entry_time + 60 * 60 * timeout - 5
        node.setmocktime(nearly_expiry_time)
        # Broadcast a transaction as the expiry of transactions in the relaypool is only checked
        # when a new transaction is added to the relaypool.
        self.vault.send_self_transfer(from_node=node, utxo_to_spend=trigger_utxo1)
        self.log.info('Test parent tx not expired after {} hours.'.format(
            timedelta(seconds=(nearly_expiry_time-entry_time))))
        assert_equal(entry_time, node.getrelaypoolentry(parent_txid)['time'])

        # Transaction should be evicted from the relaypool after the expiry time
        # has passed.
        expiry_time = entry_time + 60 * 60 * timeout + 5
        node.setmocktime(expiry_time)
        # Again, broadcast a transaction so the expiry of transactions in the relaypool is checked.
        self.vault.send_self_transfer(from_node=node, utxo_to_spend=trigger_utxo2)
        self.log.info('Test parent tx expiry after {} hours.'.format(
            timedelta(seconds=(expiry_time-entry_time))))
        assert_raises_rpc_error(-5, 'Transaction not in the relay pool',
                                node.getrelaypoolentry, parent_txid)

        # The child transaction should be removed from the relaypool as well.
        self.log.info('Test child tx is evicted as well.')
        assert_raises_rpc_error(-5, 'Transaction not in the relay pool',
                                node.getrelaypoolentry, child_txid)

        # Check that the independent tx is still in the relaypool.
        self.log.info('Test the independent tx not expired after {} hours.'.format(
            timedelta(seconds=(expiry_time-half_expiry_time))))
        assert_equal(half_expiry_time, node.getrelaypoolentry(independent_txid)['time'])

    def run_test(self):
        self.vault = MiniVault(self.nodes[0])
        self.generate(self.vault, COINBASE_MATURITY + 10)

        self.log.info('Test default relaypool expiry timeout of %d hours.' %
                      DEFAULT_RELAYPOOL_EXPIRY_HOURS)
        self.test_transaction_expiry(DEFAULT_RELAYPOOL_EXPIRY_HOURS)

        self.log.info('Test custom relaypool expiry timeout of %d hours.' %
                      CUSTOM_RELAYPOOL_EXPIRY)
        self.restart_node(0, ['-relaypoolexpiry=%d' % CUSTOM_RELAYPOOL_EXPIRY, "-txpownocycle=1"])
        self.vault = MiniVault(self.nodes[0])
        self.generate(self.vault, COINBASE_MATURITY + 10)
        self.test_transaction_expiry(CUSTOM_RELAYPOOL_EXPIRY)


if __name__ == '__main__':
    RelayPoolExpiryTest(__file__).main()
