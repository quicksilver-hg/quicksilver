#!/usr/bin/env python3
# Copyright (c) 2014-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the vault accounts properly when there is a double-spend conflict."""

from test_framework.test_framework import QuicksilverTestFramework
from test_framework.txpow import prove_raw_tx_pow
from test_framework.util import (
    assert_equal,
)


class TxnMallTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.supports_cli = False
        self.extra_args = [["-txpownocycle=1"]] * self.num_nodes

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()

    def add_options(self, parser):
        self.add_vault_options(parser)
        parser.add_argument("--mineblock", dest="mine_block", default=False, action="store_true",
                            help="Test double-spend of 1-confirmed transaction")

    def setup_network(self):
        # Start with split network:
        super().setup_network()
        self.disconnect_nodes(1, 2)

    def spend_utxo(self, utxo, outputs):
        inputs = [utxo]
        tx = self.nodes[0].createrawtransaction(inputs, outputs)
        tx = self.nodes[0].fundrawtransaction(tx)
        tx = self.nodes[0].signrawtransactionwithvault(tx['hex'])
        return self.nodes[0].sendrawtransaction(tx['hex'])

    def run_test(self):
        # All nodes should be out of IBD.
        # If the nodes are not all out of IBD, that can interfere with
        # blockchain sync later in the test when nodes are connected, due to
        # timing issues.
        for n in self.nodes:
            assert n.getblockchaininfo()["initialblockdownload"] == False

        node0_starting_balance = self.nodes[0].getbalance()
        node1_starting_balance = self.nodes[1].getbalance()
        fund_foo_amount = 900
        fund_bar_amount = 200
        doublespend_amount = 1000
        tx1_amount = 400
        tx2_amount = 100

        # Assign coins to foo and bar addresses:
        node0_address_foo = self.nodes[0].getnewaddress()
        fund_foo_utxo = self.create_outpoints(self.nodes[0], outputs=[{node0_address_foo: fund_foo_amount}])[0]
        fund_foo_tx = self.nodes[0].gettransaction(fund_foo_utxo['txid'])
        self.nodes[0].lockunspent(False, [fund_foo_utxo])

        node0_address_bar = self.nodes[0].getnewaddress()
        fund_bar_utxo = self.create_outpoints(node=self.nodes[0], outputs=[{node0_address_bar: fund_bar_amount}])[0]
        fund_bar_tx = self.nodes[0].gettransaction(fund_bar_utxo['txid'])

        assert "fee" not in fund_foo_tx
        assert "fee" not in fund_bar_tx
        assert_equal(self.nodes[0].getbalance(), node0_starting_balance)

        # Coins are sent to node1_address
        node1_address = self.nodes[1].getnewaddress()

        # First: use raw transaction API to send to node1_address,
        # but don't broadcast:
        inputs = [fund_foo_utxo, fund_bar_utxo]
        change_address = self.nodes[0].getnewaddress()
        outputs = {}
        outputs[node1_address] = doublespend_amount
        outputs[change_address] = fund_foo_amount + fund_bar_amount - doublespend_amount
        rawtx = self.nodes[0].createrawtransaction(inputs, [{address: amount} for address, amount in outputs.items()])
        rawtx = self.nodes[0].fundrawtransaction(rawtx, {"add_inputs": False})["hex"]
        doublespend = self.nodes[0].signrawtransactionwithvault(rawtx)
        assert_equal(doublespend["complete"], True)

        # Create two spends using 1 50 Hg coin each
        txid1 = self.spend_utxo(fund_foo_utxo, [{node1_address: tx1_amount}])
        txid2 = self.spend_utxo(fund_bar_utxo, [{node1_address: tx2_amount}])

        # Have node0 mine a block:
        if (self.options.mine_block):
            self.generate(self.nodes[0], 1, sync_fun=lambda: self.sync_blocks(self.nodes[0:2]))

        tx1 = self.nodes[0].gettransaction(txid1)
        tx2 = self.nodes[0].gettransaction(txid2)

        assert_equal(tx1["amount"], -tx1_amount)
        assert_equal(tx2["amount"], -tx2_amount)
        assert "fee" not in tx1
        assert "fee" not in tx2

        if self.options.mine_block:
            assert_equal(tx1["confirmations"], 1)
            assert_equal(tx2["confirmations"], 1)
            # Node1's balance should be both transaction amounts:
            assert_equal(self.nodes[1].getbalance(), node1_starting_balance - tx1["amount"] - tx2["amount"])
        else:
            assert_equal(tx1["confirmations"], 0)
            assert_equal(tx2["confirmations"], 0)

        # Now give doublespend and its parents to miner:
        self.nodes[2].sendrawtransaction(fund_foo_tx["hex"])
        self.nodes[2].sendrawtransaction(fund_bar_tx["hex"])
        doublespend_txid = self.nodes[2].sendrawtransaction(prove_raw_tx_pow(doublespend["hex"], self.nodes[2]))
        # ... mine a block...
        self.generate(self.nodes[2], 1, sync_fun=self.no_op)

        # Reconnect the split network, and sync chain:
        self.connect_nodes(1, 2)
        self.generate(self.nodes[2], 1)  # Mine another block to make sure we sync
        assert_equal(self.nodes[0].gettransaction(doublespend_txid)["confirmations"], 2)

        # Re-fetch transaction info:
        tx1 = self.nodes[0].gettransaction(txid1)
        tx2 = self.nodes[0].gettransaction(txid2)

        # Both transactions should be conflicted
        assert_equal(tx1["confirmations"], -2)
        assert_equal(tx2["confirmations"], -2)

        doublespend_tx = self.nodes[0].gettransaction(doublespend_txid)
        assert_equal(doublespend_tx["amount"], -doublespend_amount)
        assert "fee" not in doublespend_tx

        # Node1's balance should be its initial balance plus the doublespend:
        assert_equal(self.nodes[1].getbalance(), node1_starting_balance + doublespend_amount)


if __name__ == '__main__':
    TxnMallTest(__file__).main()
