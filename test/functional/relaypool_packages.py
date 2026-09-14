#!/usr/bin/env python3
# Copyright (c) 2014-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test descendant package tracking code."""

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.messages import (
    DEFAULT_ANCESTOR_LIMIT,
    DEFAULT_DESCENDANT_LIMIT,
)
from test_framework.p2p import P2PTxInvStore
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)
from test_framework.vault import MiniVault

# custom limits for node1
CUSTOM_ANCESTOR_LIMIT = 5
CUSTOM_DESCENDANT_LIMIT = 10
assert CUSTOM_DESCENDANT_LIMIT >= CUSTOM_ANCESTOR_LIMIT


class RelayPoolPackagesTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        # whitelist peers to speed up tx relay / relaypool sync
        self.noban_tx_relay = True
        self.extra_args = [
            [
                "-txpownocycle=1",
            ],
            [
                "-limitancestorcount={}".format(CUSTOM_ANCESTOR_LIMIT),
                "-limitdescendantcount={}".format(CUSTOM_DESCENDANT_LIMIT),
                "-txpownocycle=1",
            ],
        ]

    def run_test(self):
        self.vault = MiniVault(self.nodes[0])
        self.generate(self.vault, COINBASE_MATURITY + 50)

        peer_inv_store = self.nodes[0].add_p2p_connection(P2PTxInvStore()) # keep track of invs

        # DEFAULT_ANCESTOR_LIMIT transactions off a confirmed tx should be fine
        chain = self.vault.create_self_transfer_chain(chain_length=DEFAULT_ANCESTOR_LIMIT)
        witness_chain = [t["wtxid"] for t in chain]
        ancestor_vsize = 0

        for i, t in enumerate(chain):
            ancestor_vsize += t["tx"].get_vsize()
            self.vault.sendrawtransaction(from_node=self.nodes[0], tx_hex=t["hex"])

        # Wait until relaypool transactions have passed initial broadcast (sent inv and received getdata)
        # Otherwise, getrawrelaypool may be inconsistent with getrelaypoolentry if unbroadcast changes in between
        peer_inv_store.wait_for_broadcast(witness_chain)

        # Check relaypool has DEFAULT_ANCESTOR_LIMIT transactions in it, and descendant and ancestor
        # count and sizes should look correct.
        relaypool = self.nodes[0].getrawrelaypool(True)
        assert_equal(len(relaypool), DEFAULT_ANCESTOR_LIMIT)
        descendant_count = 1
        descendant_vsize = 0

        assert_equal(ancestor_vsize, sum([relaypool[tx]['vsize'] for tx in relaypool]))
        ancestor_count = DEFAULT_ANCESTOR_LIMIT

        # Adding one more transaction on to the chain should fail.
        next_hop = self.vault.create_self_transfer(utxo_to_spend=chain[-1]["new_utxo"])["hex"]
        assert_raises_rpc_error(-26, "too-long-relaypool-chain", lambda: self.nodes[0].sendrawtransaction(next_hop))

        descendants = []
        ancestors = [t["txid"] for t in chain]
        chain = [t["txid"] for t in chain]
        chain_vsize = {txid: relaypool[txid]["vsize"] for txid in chain}
        for x in reversed(chain):
            # Check that getrelaypoolentry is consistent with getrawrelaypool
            entry = self.nodes[0].getrelaypoolentry(x)
            assert_equal(entry, relaypool[x])

            # Check that gettxspendingprevout is consistent with getrawrelaypool
            witnesstx = self.nodes[0].getrawtransaction(txid=x, verbosity=1)
            for tx_in in witnesstx["vin"]:
                spending_result = self.nodes[0].gettxspendingprevout([ {'txid' : tx_in["txid"], 'vout' : tx_in["vout"]} ])
                assert_equal(spending_result, [ {'txid' : tx_in["txid"], 'vout' : tx_in["vout"], 'spendingtxid' : x} ])

            # Check that the descendant calculations are correct
            assert "fees" not in entry
            assert_equal(entry['descendantcount'], descendant_count)
            descendant_vsize += entry['vsize']
            assert_equal(entry['descendantsize'], descendant_vsize)
            descendant_count += 1

            # Check that ancestor calculations are correct
            assert_equal(entry['ancestorcount'], ancestor_count)
            assert_equal(entry['ancestorsize'], ancestor_vsize)
            ancestor_vsize -= entry['vsize']
            ancestor_count -= 1

            # Check that parent/child list is correct
            assert_equal(entry['spentby'], descendants[-1:])
            assert_equal(entry['depends'], ancestors[-2:-1])

            # Check that getrelaypooldescendants is correct
            assert_equal(sorted(descendants), sorted(self.nodes[0].getrelaypooldescendants(x)))

            # Check getrelaypooldescendants verbose output is correct
            for descendant, dinfo in self.nodes[0].getrelaypooldescendants(x, True).items():
                assert_equal(dinfo['depends'], [chain[chain.index(descendant)-1]])
                if dinfo['descendantcount'] > 1:
                    assert_equal(dinfo['spentby'], [chain[chain.index(descendant)+1]])
                else:
                    assert_equal(dinfo['spentby'], [])
            descendants.append(x)

            # Check that getrelaypoolancestors is correct
            ancestors.remove(x)
            assert_equal(sorted(ancestors), sorted(self.nodes[0].getrelaypoolancestors(x)))

            # Check that getrelaypoolancestors verbose output is correct
            for ancestor, ainfo in self.nodes[0].getrelaypoolancestors(x, True).items():
                assert_equal(ainfo['spentby'], [chain[chain.index(ancestor)+1]])
                if ainfo['ancestorcount'] > 1:
                    assert_equal(ainfo['depends'], [chain[chain.index(ancestor)-1]])
                else:
                    assert_equal(ainfo['depends'], [])


        # Check that getrelaypoolancestors/getrelaypooldescendants correctly handle verbose=true
        v_ancestors = self.nodes[0].getrelaypoolancestors(chain[-1], True)
        assert_equal(len(v_ancestors), len(chain)-1)
        for x in v_ancestors.keys():
            assert_equal(relaypool[x], v_ancestors[x])
        assert chain[-1] not in v_ancestors.keys()

        v_descendants = self.nodes[0].getrelaypooldescendants(chain[0], True)
        assert_equal(len(v_descendants), len(chain)-1)
        for x in v_descendants.keys():
            assert_equal(relaypool[x], v_descendants[x])
        assert chain[0] not in v_descendants.keys()

        # Clear the relaypool by mining a block, then invalidate it to verify
        # re-added transactions preserve ancestor/descendant size tracking.
        self.generate(self.nodes[0], 1)
        assert_equal(len(self.nodes[0].getrawrelaypool()), 0)
        self.nodes[0].invalidateblock(self.nodes[0].getbestblockhash())
        # Keep node1's tip synced with node0
        self.nodes[1].invalidateblock(self.nodes[1].getbestblockhash())

        # Now check that the transaction is back in the relaypool with exact package metadata.
        descendant_count = 1
        descendant_vsize = 0
        ancestor_count = DEFAULT_ANCESTOR_LIMIT
        ancestor_vsize = sum(chain_vsize.values())
        for x in reversed(chain):
            entry = self.nodes[0].getrelaypoolentry(x)
            assert "fees" not in entry
            assert_equal(entry['descendantcount'], descendant_count)
            descendant_vsize += chain_vsize[x]
            assert_equal(entry['descendantsize'], descendant_vsize)
            descendant_count += 1
            assert_equal(entry['ancestorcount'], ancestor_count)
            assert_equal(entry['ancestorsize'], ancestor_vsize)
            ancestor_vsize -= chain_vsize[x]
            ancestor_count -= 1

        # Check that node1's relaypool is as expected (-> custom ancestor limit)
        relaypool0 = self.nodes[0].getrawrelaypool(False)
        relaypool1 = self.nodes[1].getrawrelaypool(False)
        assert_equal(len(relaypool1), CUSTOM_ANCESTOR_LIMIT)
        assert set(relaypool1).issubset(set(relaypool0))
        for tx in chain[:CUSTOM_ANCESTOR_LIMIT]:
            assert tx in relaypool1
            entry0 = self.nodes[0].getrelaypoolentry(tx)
            entry1 = self.nodes[1].getrelaypoolentry(tx)
            assert not entry0['unbroadcast']
            assert not entry1['unbroadcast']
            assert "fees" not in entry0
            assert "fees" not in entry1
            assert_equal(entry1['vsize'], entry0['vsize'])
            assert_equal(entry1['depends'], entry0['depends'])

        # Now test descendant chain limits

        tx_children = []
        # First create one parent tx with 10 children
        tx_with_children = self.vault.send_self_transfer_multi(from_node=self.nodes[0], num_outputs=10, confirmed_only=True)
        parent_transaction = tx_with_children["txid"]
        transaction_package = tx_with_children["new_utxos"]

        # Sign and send up to MAX_DESCENDANT transactions chained off the parent tx
        chain = [] # save sent txs for the purpose of checking node1's relaypool later (see below)
        for _ in range(DEFAULT_DESCENDANT_LIMIT - 1):
            utxo = transaction_package.pop(0)
            new_tx = self.vault.send_self_transfer_multi(from_node=self.nodes[0], num_outputs=10, utxos_to_spend=[utxo])
            txid = new_tx["txid"]
            chain.append(txid)
            if utxo['txid'] is parent_transaction:
                tx_children.append(txid)
            transaction_package.extend(new_tx["new_utxos"])

        relaypool = self.nodes[0].getrawrelaypool(True)
        assert_equal(relaypool[parent_transaction]['descendantcount'], DEFAULT_DESCENDANT_LIMIT)
        assert_equal(sorted(relaypool[parent_transaction]['spentby']), sorted(tx_children))

        for child in tx_children:
            assert_equal(relaypool[child]['depends'], [parent_transaction])

        # Sending one more chained transaction will fail
        next_hop = self.vault.create_self_transfer(utxo_to_spend=transaction_package.pop(0))["hex"]
        assert_raises_rpc_error(-26, "too-long-relaypool-chain", lambda: self.nodes[0].sendrawtransaction(next_hop))

        # Check that node1's relaypool is as expected, containing:
        # - txs from previous ancestor test (-> custom ancestor limit)
        # - parent tx for descendant test
        # - txs chained off parent tx (-> custom descendant limit)
        self.wait_until(lambda: len(self.nodes[1].getrawrelaypool()) ==
                                CUSTOM_ANCESTOR_LIMIT + 1 + CUSTOM_DESCENDANT_LIMIT, timeout=10)
        relaypool0 = self.nodes[0].getrawrelaypool(False)
        relaypool1 = self.nodes[1].getrawrelaypool(False)
        assert set(relaypool1).issubset(set(relaypool0))
        assert parent_transaction in relaypool1
        for tx in chain[:CUSTOM_DESCENDANT_LIMIT]:
            assert tx in relaypool1
        for tx in chain[CUSTOM_DESCENDANT_LIMIT:]:
            assert tx not in relaypool1
        for tx in relaypool1:
            entry0 = self.nodes[0].getrelaypoolentry(tx)
            entry1 = self.nodes[1].getrelaypoolentry(tx)
            assert not entry0['unbroadcast']
            assert not entry1['unbroadcast']
            assert "fees" not in entry0
            assert "fees" not in entry1
            assert_equal(entry1['vsize'], entry0['vsize'])
            assert_equal(entry1['depends'], entry0['depends'])
        # Test reorg handling
        # First, the basics:
        self.generate(self.nodes[0], 1)
        self.nodes[1].invalidateblock(self.nodes[0].getbestblockhash())
        self.nodes[1].reconsiderblock(self.nodes[0].getbestblockhash())

        # Now test the case where node1 has a transaction T in its relaypool that
        # depends on transactions A and B which are in a mined block, and the
        # block containing A and B is disconnected, AND B is not accepted back
        # into node1's relaypool because its ancestor count is too high.

        # Create 8 transactions, like so:
        # Tx0 -> Tx1 (vout0)
        #   \--> Tx2 (vout1) -> Tx3 -> Tx4 -> Tx5 -> Tx6 -> Tx7
        #
        # Mine them in the next block, then generate a new tx8 that spends
        # Tx1 and Tx7, and add to node1's relaypool, then disconnect the
        # last block.

        # Create tx0 with 2 outputs
        tx0 = self.vault.send_self_transfer_multi(from_node=self.nodes[0], num_outputs=2, confirmed_only=True)

        # Create tx1
        tx1 = self.vault.send_self_transfer(from_node=self.nodes[0], utxo_to_spend=tx0["new_utxos"][0])

        # Create tx2-7
        tx7 = self.vault.send_self_transfer_chain(from_node=self.nodes[0], utxo_to_spend=tx0["new_utxos"][1], chain_length=6)[-1]

        # Mine these in a block
        self.generate(self.nodes[0], 1)

        # Now generate tx8, spending both branches.
        self.vault.send_self_transfer_multi(from_node=self.nodes[0], utxos_to_spend=[tx1["new_utxo"], tx7["new_utxo"]])
        self.sync_relaypools()

        # Now try to disconnect the tip on each node...
        self.nodes[1].invalidateblock(self.nodes[1].getbestblockhash())
        self.nodes[0].invalidateblock(self.nodes[0].getbestblockhash())
        self.sync_blocks()

if __name__ == '__main__':
    RelayPoolPackagesTest(__file__).main()
