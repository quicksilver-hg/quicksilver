#!/usr/bin/env python3
# Copyright (c) 2019-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Test tx status in case of reorgs while vault being shutdown.

Vault txn status rely on block connection/disconnection for its
accuracy. In case of reorgs happening while vault being shutdown
block updates are not going to be received. At vault loading, we
check against chain if confirmed txn are still in chain and change
their status if block in which they have been included has been
disconnected.
"""

from decimal import Decimal
import shutil

from test_framework.test_framework import QuicksilverTestFramework
from test_framework.txpow import prove_raw_tx_pow
from test_framework.util import (
        assert_equal,
        assert_greater_than,
        assert_raises_rpc_error
)

class ReorgsRestoreTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 3
        self.extra_args = [["-txpownocycle=1"]] * self.num_nodes

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()

    def test_coinbase_automatic_abandon_during_startup(self):
        ##########################################################################################################
        # Verify the vault marks coinbase transactions, and their descendants, as abandoned during startup when #
        # the block is no longer part of the best chain.                                                         #
        ##########################################################################################################
        self.log.info("Test automatic coinbase abandonment during startup")
        # Test setup: Sync nodes for the coming test, ensuring both are at the same block, then disconnect them to
        # generate two competing chains. After disconnection, verify no other peer connection exists.
        self.connect_nodes(1, 0)
        self.sync_blocks(self.nodes[:2])
        self.disconnect_nodes(1, 0)
        assert all(len(node.getpeerinfo()) == 0 for node in self.nodes[:2])

        # Create a new block in node0, coinbase going to vault0
        self.nodes[0].createvault(vault_name="w0", load_on_startup=True)
        vault0 = self.nodes[0].get_vault_rpc("w0")
        self.generatetoaddress(self.nodes[0], 1, vault0.getnewaddress(), sync_fun=self.no_op)
        node0_coinbase_tx_hash = vault0.getblock(vault0.getbestblockhash(), verbosity=1)['tx'][0]

        # Mine 100 blocks on top to mature the coinbase and create a descendant
        self.generate(self.nodes[0], 101, sync_fun=self.no_op)
        # Make descendant, send-to-self
        descendant_tx_id = vault0.sendtoaddress(vault0.getnewaddress(), 1)

        # Verify balance
        vault0.syncwithvalidationinterfacequeue()
        assert(vault0.getbalances()['mine']['trusted'] > 0)

        # Now create a fork in node1. This will be used to replace node0's chain later.
        self.nodes[1].createvault(vault_name="w1", load_on_startup=True)
        vault1 = self.nodes[1].get_vault_rpc("w1")
        self.generatetoaddress(self.nodes[1], 1, vault1.getnewaddress(), sync_fun=self.no_op)
        vault1.syncwithvalidationinterfacequeue()

        # Verify both nodes are on a different chain
        block0_best_hash, block1_best_hash = vault0.getbestblockhash(), vault1.getbestblockhash()
        assert(block0_best_hash != block1_best_hash)

        # Stop both nodes and replace node0 chain entirely for the node1 chain
        self.stop_nodes()
        for path in ["chainstate", "blocks"]:
            shutil.rmtree(self.nodes[0].chain_path / path)
            shutil.copytree(self.nodes[1].chain_path / path, self.nodes[0].chain_path / path)

        # Start node0 and verify that now it has node1 chain and no info about its previous best block
        self.start_node(0)
        vault0 = self.nodes[0].get_vault_rpc("w0")
        assert_equal(vault0.getbestblockhash(), block1_best_hash)
        assert_raises_rpc_error(-5, "Block not found", vault0.getblock, block0_best_hash)

        # Verify the coinbase tx was marked as abandoned and balance correctly computed
        tx_info = vault0.gettransaction(node0_coinbase_tx_hash)['details'][0]
        assert_equal(tx_info['abandoned'], True)
        assert_equal(tx_info['category'], 'orphan')
        assert(vault0.getbalances()['mine']['trusted'] == 0)
        # Verify the coinbase descendant was also marked as abandoned
        assert_equal(vault0.gettransaction(descendant_tx_id)['details'][0]['abandoned'], True)

    def test_reorg_handling_during_unclean_shutdown(self):
        self.log.info("Test that vault doesn't crash due to a duplicate block disconnection event after an unclean shutdown")
        node = self.nodes[0]
        # Receive coinbase reward on a new vault
        node.createvault(vault_name="reorg_crash", load_on_startup=True)
        vault = node.get_vault_rpc("reorg_crash")
        self.generatetoaddress(node, 1, vault.getnewaddress(), sync_fun=self.no_op)

        # Restart to ensure node and vault are flushed
        self.restart_node(0)
        vault = node.get_vault_rpc("reorg_crash")
        assert_greater_than(vault.getbalances()['mine']['immature'], 0)

        # Disconnect tip and sync vault state
        tip = vault.getbestblockhash()
        vault.invalidateblock(tip)
        vault.syncwithvalidationinterfacequeue()

        # Tip was disconnected, ensure coinbase has been abandoned
        assert_equal(vault.getbalances()['mine']['immature'], 0)
        coinbase_tx_id = vault.getblock(tip, verbosity=1)["tx"][0]
        assert_equal(vault.gettransaction(coinbase_tx_id)['details'][0]['abandoned'], True)

        # Abort process abruptly to mimic an unclean shutdown (no chain state flush to disk)
        node.kill_process()

        # Restart the node and confirm that it has not persisted the last chain state changes to disk
        self.start_node(0)
        assert_equal(node.getbestblockhash(), tip)

        # Due to an existing bug, the vault incorrectly keeps the transaction in an abandoned state, even though that's
        # no longer the case (after the unclean shutdown, the node's chain returned to the pre-invalidation tip).
        # This issue blocks any future spending and results in an incorrect balance display.
        vault = node.get_vault_rpc("reorg_crash")
        assert_equal(vault.getbalances()['mine']['immature'], 0) # FIXME: #31824.

        # Previously, a bug caused the node to crash if two block disconnection events occurred consecutively.
        # Ensure this is no longer the case by simulating a new reorg.
        node.invalidateblock(tip)
        assert(node.getbestblockhash() != tip)
        # Ensure vault state is consistent now
        assert_equal(vault.gettransaction(coinbase_tx_id)['details'][0]['abandoned'], True)
        assert_equal(vault.getbalances()['mine']['immature'], 0)

        # And finally, verify the state if the block ends up being into the best chain again
        node.reconsiderblock(tip)
        assert_equal(vault.gettransaction(coinbase_tx_id)['details'][0]['abandoned'], False)
        assert_greater_than(vault.getbalances()['mine']['immature'], 0)

    def run_test(self):
        # Send a tx from which to conflict outputs later
        txid_conflict_from = self.nodes[0].sendtoaddress(self.nodes[0].getnewaddress(), Decimal("10"))
        self.generate(self.nodes[0], 1)
        nA = next(tx_out["vout"] for tx_out in self.nodes[0].gettransaction(txid_conflict_from)["details"] if tx_out["amount"] == Decimal("10"))
        self.nodes[0].lockunspent(False, [{"txid": txid_conflict_from, "vout": nA}])

        # Disconnect node1 from others to reorg its chain later
        self.disconnect_nodes(0, 1)
        self.disconnect_nodes(1, 2)
        self.connect_nodes(0, 2)

        # Send a tx to be unconfirmed later
        txid = self.nodes[0].sendtoaddress(self.nodes[0].getnewaddress(), Decimal("10"))
        tx = self.nodes[0].gettransaction(txid)
        self.generate(self.nodes[0], 4, sync_fun=self.no_op)
        self.sync_blocks([self.nodes[0], self.nodes[2]])
        tx_before_reorg = self.nodes[0].gettransaction(txid)
        assert_equal(tx_before_reorg["confirmations"], 4)

        # Disconnect node0 from node2 to broadcast a conflict on their respective chains
        self.disconnect_nodes(0, 2)
        inputs = []
        inputs.append({"txid": txid_conflict_from, "vout": nA})
        outputs_1 = {}
        outputs_2 = {}

        # Create a conflicted tx broadcast on node0 chain and conflicting tx broadcast on node1 chain. Both spend from txid_conflict_from
        outputs_1[self.nodes[0].getnewaddress()] = Decimal("10")
        outputs_2[self.nodes[0].getnewaddress()] = Decimal("10")
        conflicted = self.nodes[0].signrawtransactionwithvault(self.nodes[0].createrawtransaction(inputs, [{address: amount} for address, amount in outputs_1.items()]))
        conflicting = self.nodes[0].signrawtransactionwithvault(self.nodes[0].createrawtransaction(inputs, [{address: amount} for address, amount in outputs_2.items()]))
        conflicted["hex"] = prove_raw_tx_pow(conflicted["hex"], self.nodes[0])
        conflicting["hex"] = prove_raw_tx_pow(conflicting["hex"], self.nodes[2])

        conflicted_txid = self.nodes[0].sendrawtransaction(conflicted["hex"])
        self.generate(self.nodes[0], 1, sync_fun=self.no_op)
        conflicting_txid = self.nodes[2].sendrawtransaction(conflicting["hex"])
        self.generate(self.nodes[2], 9, sync_fun=self.no_op)

        # Reconnect node0 and node2 and check that conflicted_txid is effectively conflicted
        self.connect_nodes(0, 2)
        self.sync_blocks([self.nodes[0], self.nodes[2]])
        conflicted = self.nodes[0].gettransaction(conflicted_txid)
        conflicting = self.nodes[0].gettransaction(conflicting_txid)
        assert_equal(conflicted["confirmations"], -9)
        assert_equal(conflicted["vaultconflicts"][0], conflicting["txid"])

        # Node0 vault is shutdown
        self.restart_node(0)

        # The block chain re-orgs and the tx is included in a different block
        self.generate(self.nodes[1], 9, sync_fun=self.no_op)
        self.nodes[1].sendrawtransaction(tx["hex"])
        self.generate(self.nodes[1], 1, sync_fun=self.no_op)
        self.nodes[1].sendrawtransaction(conflicted["hex"])
        self.generate(self.nodes[1], 1, sync_fun=self.no_op)

        # Node0 vault file is loaded on longest sync'ed node1
        self.stop_node(1)
        self.nodes[0].backupvault(self.nodes[0].datadir_path / 'vault.bak')
        shutil.copyfile(self.nodes[0].datadir_path / 'vault.bak', self.nodes[1].vaults_path / self.default_vault_name / self.vault_data_filename)
        self.start_node(1)
        tx_after_reorg = self.nodes[1].gettransaction(txid)
        # Check that normal confirmed tx is confirmed again but with different blockhash
        assert_equal(tx_after_reorg["confirmations"], 2)
        assert tx_before_reorg["blockhash"] != tx_after_reorg["blockhash"]
        conflicted_after_reorg = self.nodes[1].gettransaction(conflicted_txid)
        # Check that conflicted tx is confirmed again with blockhash different than previously conflicting tx
        assert_equal(conflicted_after_reorg["confirmations"], 1)
        assert conflicting["blockhash"] != conflicted_after_reorg["blockhash"]

        # Verify we mark coinbase txs, and their descendants, as abandoned during startup
        self.test_coinbase_automatic_abandon_during_startup()

        # Verify reorg behavior during an unclean shutdown
        self.test_reorg_handling_during_unclean_shutdown()


if __name__ == '__main__':
    ReorgsRestoreTest(__file__).main()
