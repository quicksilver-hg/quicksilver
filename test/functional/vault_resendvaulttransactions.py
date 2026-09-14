#!/usr/bin/env python3
# Copyright (c) 2017-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that the vault resends transactions periodically."""
import time

from test_framework.blocktools import (
    create_block,
    create_coinbase,
)
from test_framework.messages import DEFAULT_RELAYPOOL_EXPIRY_HOURS
from test_framework.p2p import P2PTxInvStore
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

class ResendVaultTransactionsTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()

    def run_test(self):
        node = self.nodes[0]  # alias

        peer_first = node.add_p2p_connection(P2PTxInvStore())

        self.log.info("Create a new transaction and wait until it's broadcast")
        parent_utxo, indep_utxo = node.listunspent()[:2]
        addr = node.getnewaddress()
        txid = node.send(outputs=[{addr: 1}], inputs=[parent_utxo])["txid"]

        # Can take a few seconds due to transaction trickling
        peer_first.wait_for_broadcast([txid])

        # Add a second peer since txs aren't rebroadcast to the same peer (see m_tx_inventory_known_filter)
        peer_second = node.add_p2p_connection(P2PTxInvStore())

        self.log.info("Create a block")
        # Create and submit a block without the transaction.
        # Transactions are only rebroadcast if there has been a block at least five minutes
        # after the last time we tried to broadcast. Use mocktime and give an extra minute to be sure.
        block_time = int(time.time()) + 6 * 60
        node.setmocktime(block_time)
        block = create_block(int(node.getbestblockhash(), 16), create_coinbase(node.getblockcount() + 1), block_time)
        block.solve()
        node.submitblock(block.serialize().hex())

        # Set correct m_best_block_time, which is used in ResubmitVaultTransactions
        node.syncwithvalidationinterfacequeue()
        now = int(time.time())

        # Transaction should not be rebroadcast within first 12 hours
        # Leave 2 mins for buffer
        twelve_hrs = 12 * 60 * 60
        two_min = 2 * 60
        node.setmocktime(now + twelve_hrs - two_min)
        node.mockscheduler(60)  # Tell scheduler to call MaybeResendVaultTxs now
        assert_equal(int(txid, 16) in peer_second.get_invs(), False)

        self.log.info("Bump time & check that transaction is rebroadcast")
        # Transaction should be rebroadcast approximately 24 hours in the future,
        # but can range from 12-36. So bump 36 hours to be sure.
        with node.assert_debug_log(['submitted unconfirmed_txs=1 reason=resubmit']):
            node.setmocktime(now + 36 * 60 * 60)
            # Tell scheduler to call MaybeResendVaultTxs now.
            node.mockscheduler(60)
        # Give some time for trickle to occur
        node.setmocktime(now + 36 * 60 * 60 + 600)
        peer_second.wait_for_broadcast([txid])

        self.log.info("Chain of unconfirmed not-in-relaypool txs are rebroadcast")
        # Upstream additionally pinned the mapVault ordering so it could assert the
        # parent is broadcast before the child. It did that by repeatedly bump-feeing
        # the child to replace it until the ordering came out right -- a loop with no
        # Quicksilver equivalent, since a feeless chain has neither a fee to raise nor
        # "insufficient fee, rejecting replacement" to grind against. Replacement here
        # ranks on surplus proof-of-work, and re-grinding a child purely to shuffle a
        # map ordering would be a test that mines rather than a test that checks.
        #
        # What remains is the coverage that does apply: a parent and its unconfirmed
        # child, evicted from the relay pool, are BOTH resubmitted. The parent-before-
        # child ordering assertion is not covered here and wants its own test built on
        # the surplus-work ranking.
        child_inputs = [{"txid": txid, "vout": 0}]
        child_txid = node.sendall(recipients=[addr], inputs=child_inputs)["txid"]
        entry_time = node.getrelaypoolentry(child_txid)["time"]

        block_time = entry_time + 6 * 60
        node.setmocktime(block_time)
        block = create_block(int(node.getbestblockhash(), 16), create_coinbase(node.getblockcount() + 1), block_time)
        block.solve()
        node.submitblock(block.serialize().hex())
        # Set correct m_best_block_time, which is used in ResubmitVaultTransactions
        node.syncwithvalidationinterfacequeue()

        evict_time = block_time + 60 * 60 * DEFAULT_RELAYPOOL_EXPIRY_HOURS + 5
        # Flush out currently scheduled resubmit attempt now so that there can't be one right between eviction and check.
        with node.assert_debug_log(['submitted unconfirmed_txs=2 reason=resubmit']):
            node.setmocktime(evict_time)
            node.mockscheduler(60)

        # Evict these txs from the relaypool
        indep_send = node.send(outputs=[{node.getnewaddress(): 1}], inputs=[indep_utxo])
        node.getrelaypoolentry(indep_send["txid"])
        assert_raises_rpc_error(-5, "Transaction not in the relay pool", node.getrelaypoolentry, txid)
        assert_raises_rpc_error(-5, "Transaction not in the relay pool", node.getrelaypoolentry, child_txid)

        # Rebroadcast and check that parent and child are both in the relaypool
        with node.assert_debug_log(['submitted unconfirmed_txs=2 reason=resubmit']):
            node.setmocktime(evict_time + 36 * 60 * 60) # 36 hrs is the upper limit of the resend timer
            node.mockscheduler(60)
        node.getrelaypoolentry(txid)
        node.getrelaypoolentry(child_txid)


if __name__ == '__main__':
    ResendVaultTransactionsTest(__file__).main()
