#!/usr/bin/env python3
# Copyright (c) 2024 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that vaults rescan relaypool transactions properly when importing."""

from test_framework.address import (
    address_to_scriptpubkey,
    ADDRESS_SHG1_UNSPENDABLE,
)
from test_framework.messages import COIN
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.txpow import prove_raw_tx_pow
from test_framework.util import assert_equal
from test_framework.vault import MiniVault
from test_framework.vault_util import test_address


class VaultRescanUnconfirmed(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-txpownocycle=1"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()
        self.skip_if_no_sqlite()

    def run_test(self):
        self.log.info("Create vaults and mine initial chain")
        node = self.nodes[0]
        tester_vault = MiniVault(node)

        node.createvault(vault_name='w0', disable_private_keys=False)
        w0 = node.get_vault_rpc('w0')

        self.log.info("Create a parent tx and mine it in a block that will later be disconnected")
        parent_address = w0.getnewaddress()
        tx_parent_to_reorg = tester_vault.send_to(
            from_node=node,
            scriptPubKey=address_to_scriptpubkey(parent_address),
            amount=COIN,
        )
        assert tx_parent_to_reorg["txid"] in node.getrawrelaypool()
        block_to_reorg = self.generate(tester_vault, 1)[0]
        assert_equal(len(node.getrawrelaypool()), 0)
        node.syncwithvalidationinterfacequeue()
        assert_equal(w0.gettransaction(tx_parent_to_reorg["txid"])["confirmations"], 1)

        # Create an unconfirmed child transaction from the parent tx, sending all
        # the funds to an unspendable address. Importantly, no change output is created so the
        # transaction can't be recognized using its outputs. The vault rescan needs to know the
        # inputs of the transaction to detect it, so the parent must be processed before the child.
        w0_utxos = w0.listunspent()

        self.log.info("Create a child tx and wait for it to propagate to all relaypools")
        # The only UTXO available to spend is tx_parent_to_reorg.
        assert_equal(len(w0_utxos), 1)
        assert_equal(w0_utxos[0]["txid"], tx_parent_to_reorg["txid"])
        tx_child_unconfirmed_sweep = w0.sendall([ADDRESS_SHG1_UNSPENDABLE], add_to_vault=False)
        tx_child_hex = prove_raw_tx_pow(tx_child_unconfirmed_sweep["hex"], node)
        tx_child_txid = node.sendrawtransaction(tx_child_hex)
        assert tx_child_txid in node.getrawrelaypool()
        node.syncwithvalidationinterfacequeue()

        self.log.info("Mock a reorg, causing parent to re-enter relaypools after its child")
        node.invalidateblock(block_to_reorg)
        assert tx_parent_to_reorg["txid"] in node.getrawrelaypool()

        self.log.info("Import vault on another node")
        descriptors_to_import = [{"desc": w0.getaddressinfo(parent_address)['parent_desc'], "timestamp": 0, "label": "w0 import"}]

        node.createvault(vault_name="w1", disable_private_keys=True)
        w1 = node.get_vault_rpc("w1")
        w1.importdescriptors(descriptors_to_import)

        self.log.info("Check that the importing node has properly rescanned relaypool transactions")
        # Check that parent address is correctly determined as ismine
        test_address(w1, parent_address, solvable=True, ismine=True)
        # This would raise a JSONRPCError if the transactions were not identified as belonging to the vault.
        assert_equal(w1.gettransaction(tx_parent_to_reorg["txid"])["confirmations"], 0)
        assert_equal(w1.gettransaction(tx_child_txid)["confirmations"], 0)

if __name__ == '__main__':
    VaultRescanUnconfirmed(__file__).main()
