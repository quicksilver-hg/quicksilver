#!/usr/bin/env python3
# Copyright (c) 2014-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test relaypool persistence.

By default, quicksilverd will dump relaypool on shutdown and
then reload it on startup. This can be overridden with
the -persistrelaypool=0 command line option.

Test is as follows:

  - start node0, node1 and node2. node1 has -persistrelaypool=0
  - create 5 transactions on node2 to its own address. Note that these
    are not sent to node0 or node1 addresses because we don't want
    them to be saved in the vault.
  - check that node0 and node1 have 5 transactions in their relaypools
  - shutdown all nodes.
  - startup node0. Verify that it still has 5 transactions
    in its relaypool. Shutdown node0. This tests that by default the
    relaypool is persistent.
  - startup node1. Verify that its relaypool is empty. Shutdown node1.
    This tests that with -persistrelaypool=0, the relaypool is not
    dumped to disk when the node is shut down.
  - Restart node0 with -persistrelaypool=0. Verify that its relaypool is
    empty. Shutdown node0. This tests that with -persistrelaypool=0,
    the relaypool is not loaded from disk on start up.
  - Restart node0 with -persistrelaypool. Verify that it has 5
    transactions in its relaypool. This tests that -persistrelaypool=0
    does not overwrite a previously valid relaypool stored on disk.
  - Remove node0 relaypool.dat and verify saverelaypool RPC recreates it
    and verify that node1 can load it and has 5 transactions in its
    relaypool.
  - Verify that saverelaypool throws when the RPC is called if
    node1 can't write to disk.

"""
import os
import time

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.p2p import P2PTxInvStore
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than_or_equal,
    assert_raises_rpc_error,
)
from test_framework.vault import MiniVault


class RelayPoolPersistTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.extra_args = [["-txpownocycle=1"], ["-persistrelaypool=0", "-txpownocycle=1"], ["-txpownocycle=1"]]

    def run_test(self):
        self.mini_vault = MiniVault(self.nodes[2])
        self.generate(self.mini_vault, COINBASE_MATURITY + 30)
        if self.is_sqlite_compiled():
            self.nodes[2].createvault(
                vault_name="watch",
                disable_private_keys=True,
                load_on_startup=False,
            )
            vault_watch = self.nodes[2].get_vault_rpc("watch")
            assert_equal([{'success': True}], vault_watch.importdescriptors([{'desc': self.mini_vault.get_descriptor(), 'timestamp': 0}]))

        self.log.debug("Send 5 transactions from node2 (to its own address)")
        tx_creation_time_lower = int(time.time())
        for _ in range(5):
            last_txid = self.mini_vault.send_self_transfer(from_node=self.nodes[2], confirmed_only=True)["txid"]
        if self.is_sqlite_compiled():
            self.nodes[2].syncwithvalidationinterfacequeue()  # Flush relaypool to vault
            node2_balance = vault_watch.getbalance()
        self.sync_all()
        tx_creation_time_higher = int(time.time())

        self.log.debug("Verify that node0 and node1 have 5 transactions in their relaypools")
        assert_equal(len(self.nodes[0].getrawrelaypool()), 5)
        assert_equal(len(self.nodes[1].getrawrelaypool()), 5)

        last_entry = self.nodes[0].getrelaypoolentry(txid=last_txid)
        assert "fees" not in last_entry
        tx_creation_time = last_entry['time']
        assert_greater_than_or_equal(tx_creation_time, tx_creation_time_lower)
        assert_greater_than_or_equal(tx_creation_time_higher, tx_creation_time)

        # disconnect nodes & make a txn that remains in the unbroadcast set.
        self.disconnect_nodes(0, 1)
        assert_equal(len(self.nodes[0].getpeerinfo()), 0)
        assert_equal(len(self.nodes[0].p2ps), 0)
        self.mini_vault.send_self_transfer(from_node=self.nodes[0], confirmed_only=True)

        # Create a tx but don't submit until after the restart.
        tx_not_submitted = self.mini_vault.create_self_transfer(confirmed_only=True)

        self.log.debug("Stop-start the nodes. Verify that node0 has the transactions in its relaypool and node1 does not. Verify that node2 calculates its balance correctly after loading vault transactions.")
        self.stop_nodes()
        # Give this node a head-start, so we can be "extra-sure" that it didn't load anything later
        # Also don't store the relaypool, to keep the datadir clean
        self.start_node(1, extra_args=["-persistrelaypool=0", "-txpownocycle=1"])
        self.start_node(0)
        self.start_node(2)
        assert self.nodes[0].getrelaypoolinfo()["loaded"]  # start_node is blocking on the relaypool being loaded
        assert self.nodes[2].getrelaypoolinfo()["loaded"]
        assert_equal(len(self.nodes[0].getrawrelaypool()), 6)
        assert_equal(len(self.nodes[2].getrawrelaypool()), 5)
        # The others have loaded their relaypool. If node_1 loaded anything, we'd probably notice by now:
        assert_equal(len(self.nodes[1].getrawrelaypool()), 0)

        self.log.debug('Verify all fields are loaded correctly')
        assert_equal(last_entry, self.nodes[0].getrelaypoolentry(txid=last_txid))
        self.nodes[0].sendrawtransaction(tx_not_submitted['hex'])
        entry_before_restart = self.nodes[0].getrelaypoolentry(txid=tx_not_submitted['txid'])
        assert "fees" not in entry_before_restart

        # Verify accounting of relaypool transactions after restart is correct
        if self.is_sqlite_compiled():
            self.nodes[2].loadvault("watch")
            vault_watch = self.nodes[2].get_vault_rpc("watch")
            self.nodes[2].syncwithvalidationinterfacequeue()  # Flush relaypool to vault
            assert_equal(node2_balance, vault_watch.getbalance())

        relaypooldat0 = os.path.join(self.nodes[0].chain_path, 'relaypool.dat')
        relaypooldat1 = os.path.join(self.nodes[1].chain_path, 'relaypool.dat')

        self.log.debug("Force -persistrelaypool=0 node1 to saverelaypool to disk via RPC")
        assert not os.path.exists(relaypooldat1)
        result1 = self.nodes[1].saverelaypool()
        assert os.path.isfile(relaypooldat1)
        assert_equal(result1['filename'], relaypooldat1)
        os.remove(relaypooldat1)

        self.log.debug("Stop-start node0 with -persistrelaypool=0. Verify that it doesn't load its relaypool.dat file.")
        self.stop_nodes()
        self.start_node(0, extra_args=["-persistrelaypool=0", "-txpownocycle=1"])
        assert self.nodes[0].getrelaypoolinfo()["loaded"]
        assert_equal(len(self.nodes[0].getrawrelaypool()), 0)

        self.log.debug("Import relaypool at runtime to node0.")
        assert_equal({}, self.nodes[0].importrelaypool(relaypooldat0))
        assert_equal(len(self.nodes[0].getrawrelaypool()), 7)
        assert "fees" not in self.nodes[0].getrelaypoolentry(txid=last_txid)
        assert_equal({}, self.nodes[0].importrelaypool(relaypooldat0, {"apply_unbroadcast_set": True}))
        assert_equal(2, self.nodes[0].getrelaypoolinfo()["unbroadcastcount"])
        assert "fees" not in self.nodes[0].getrelaypoolentry(txid=last_txid)

        self.log.debug("Stop-start node0. Verify that it has the transactions in its relaypool.")
        self.stop_nodes()
        self.start_node(0)
        assert self.nodes[0].getrelaypoolinfo()["loaded"]
        assert_equal(len(self.nodes[0].getrawrelaypool()), 7)

        self.log.debug("Remove the relaypool.dat file. Verify that saverelaypool to disk via RPC re-creates it")
        os.remove(relaypooldat0)
        result0 = self.nodes[0].saverelaypool()
        assert os.path.isfile(relaypooldat0)
        assert_equal(result0['filename'], relaypooldat0)

        self.log.debug("Stop nodes, make node1 use relaypool.dat from node0. Verify it has 7 transactions")
        os.rename(relaypooldat0, relaypooldat1)
        self.stop_nodes()
        self.start_node(1, extra_args=["-persistrelaypool", "-txpownocycle=1"])
        assert self.nodes[1].getrelaypoolinfo()["loaded"]
        assert_equal(len(self.nodes[1].getrawrelaypool()), 7)

        self.log.debug("Prevent quicksilverd from writing relaypool.dat to disk. Verify that `saverelaypool` fails")
        # to test the exception we are creating a tmp folder called relaypool.dat.new
        # which is an implementation detail that could change and break this test
        relaypooldotnew1 = relaypooldat1 + '.new'
        os.mkdir(relaypooldotnew1)
        assert_raises_rpc_error(-1, "Unable to dump relay pool to disk", self.nodes[1].saverelaypool)
        os.rmdir(relaypooldotnew1)

        self.test_importrelaypool_union()
        self.test_persist_unbroadcast()
        self.test_legacy_filename_is_ignored()

    def test_legacy_filename_is_ignored(self):
        self.log.debug("Verify the pre-rename pool file is ignored, not migrated")
        self.stop_nodes()
        chain_path = self.nodes[0].chain_path
        current = chain_path / "relaypool.dat"
        assert os.path.isfile(current), "the pool must persist to relaypool.dat"
        # The pre-rename filename, spelled indirectly so a future tree-wide
        # rename sweep cannot silently turn this into a no-op.
        legacy = chain_path / ("mem" + "pool.dat")
        os.rename(current, legacy)
        self.start_node(0)
        assert self.nodes[0].getrelaypoolinfo()["loaded"]
        assert_equal(len(self.nodes[0].getrawrelaypool()), 0)
        assert os.path.isfile(legacy), "the legacy file must be left alone, not consumed"

    def test_persist_unbroadcast(self):
        node0 = self.nodes[0]
        self.start_node(0)
        self.start_node(2)

        # clear out relaypool
        self.generate(node0, 1, sync_fun=self.no_op)

        # ensure node0 doesn't have any connections
        # make a transaction that will remain in the unbroadcast set
        assert_equal(len(node0.getpeerinfo()), 0)
        assert_equal(len(node0.p2ps), 0)
        self.mini_vault.send_self_transfer(from_node=node0, confirmed_only=True)

        # shutdown, then startup with vault disabled
        self.restart_node(0, extra_args=["-disablevault", "-txpownocycle=1"])

        # check that txn gets broadcast due to unbroadcast logic
        conn = node0.add_p2p_connection(P2PTxInvStore())
        node0.mockscheduler(16 * 60)  # 15 min + 1 for buffer
        self.wait_until(lambda: len(conn.get_invs()) == 1)

    def test_importrelaypool_union(self):
        self.log.debug("Submit different transactions to node0 and node1's relaypools")
        self.start_node(0)
        self.start_node(2)
        tx_node0 = self.mini_vault.send_self_transfer(from_node=self.nodes[0], confirmed_only=True)
        tx_node1 = self.mini_vault.send_self_transfer(from_node=self.nodes[1], confirmed_only=True)
        tx_node01 = self.mini_vault.create_self_transfer(confirmed_only=True)
        tx_node01_secret = self.mini_vault.create_self_transfer(confirmed_only=True)
        self.nodes[0].sendrawtransaction(tx_node01["hex"])
        self.nodes[1].sendrawtransaction(tx_node01["hex"])
        assert tx_node0["txid"] in self.nodes[0].getrawrelaypool()
        assert not tx_node0["txid"] in self.nodes[1].getrawrelaypool()
        assert not tx_node1["txid"] in self.nodes[0].getrawrelaypool()
        assert tx_node1["txid"] in self.nodes[1].getrawrelaypool()
        assert tx_node01["txid"] in self.nodes[0].getrawrelaypool()
        assert tx_node01["txid"] in self.nodes[1].getrawrelaypool()

        self.log.debug("Check that importrelaypool can add txns without replacing the entire relaypool")
        relaypooldat0 = str(self.nodes[0].chain_path / "relaypool.dat")
        result0 = self.nodes[0].saverelaypool()
        assert_equal(relaypooldat0, result0["filename"])
        assert_equal({}, self.nodes[1].importrelaypool(relaypooldat0))
        # All transactions should be in node1's relaypool now.
        assert tx_node0["txid"] in self.nodes[1].getrawrelaypool()
        assert tx_node1["txid"] in self.nodes[1].getrawrelaypool()
        assert not tx_node1["txid"] in self.nodes[0].getrawrelaypool()
        entry_node01 = self.nodes[1].getrelaypoolentry(tx_node01["txid"])
        assert "fees" not in entry_node01
        self.nodes[1].sendrawtransaction(tx_node01_secret["hex"])
        entry_node01_secret = self.nodes[1].getrelaypoolentry(tx_node01_secret["txid"])
        assert "fees" not in entry_node01_secret
        self.stop_nodes()


if __name__ == "__main__":
    RelayPoolPersistTest(__file__).main()
