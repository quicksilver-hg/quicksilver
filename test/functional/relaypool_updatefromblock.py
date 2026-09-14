#!/usr/bin/env python3
# Copyright (c) 2020-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test relaypool descendants/ancestors information update.

Test relaypool update of transaction descendants/ancestors information (count, size)
when transactions have been re-added from a disconnected block to the relaypool.
"""
import time

from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal
from test_framework.vault import MiniVault


class RelayPoolUpdateFromBlockTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [['-limitdescendantsize=1000', '-limitancestorsize=1000', '-limitancestorcount=100', '-txpownocycle=1']]

    def transaction_graph_test(self, size, n_tx_to_mine=None):
        """Create an acyclic tournament (a type of directed graph) of transactions and use it for testing.

        Keyword arguments:
        size -- the order N of the tournament which is equal to the number of the created transactions
        n_tx_to_mine -- the number of transaction that should be mined into a block

        If all of the N created transactions tx[0]..tx[N-1] reside in the relaypool,
        the following holds:
            the tx[K] transaction:
            - has N-K descendants (including this one), and
            - has K+1 ancestors (including this one)

        More details: https://en.wikipedia.org/wiki/Tournament_(graph_theory)
        """
        vault = MiniVault(self.nodes[0])
        first_block_hash = ''
        tx_id = []
        tx_size = []
        self.log.info('Creating {} transactions...'.format(size))
        for i in range(0, size):
            self.log.debug('Preparing transaction #{}...'.format(i))
            # Prepare inputs.
            if i == 0:
                inputs = [vault.get_utxo()]  # let MiniVault provide a start UTXO
            else:
                inputs = []
                for j, tx in enumerate(tx_id[0:i]):
                    # Transaction tx[K] is a child of each of previous transactions tx[0]..tx[K-1] at their output K-1.
                    vout = i - j - 1
                    inputs.append(vault.get_utxo(txid=tx_id[j], vout=vout))

            # Prepare outputs.
            tx_count = i + 1
            if tx_count < size:
                # Transaction tx[K] is an ancestor of each of subsequent transactions tx[K+1]..tx[N-1].
                n_outputs = size - tx_count
            else:
                n_outputs = 1

            # Create a new transaction.
            new_tx = vault.send_self_transfer_multi(
                from_node=self.nodes[0],
                utxos_to_spend=inputs,
                num_outputs=n_outputs,
            )
            tx_id.append(new_tx['txid'])
            tx_size.append(new_tx['tx'].get_vsize())

            if tx_count in n_tx_to_mine:
                # The created transactions are mined into blocks by batches.
                self.log.info('The batch of {} transactions has been accepted into the relaypool.'.format(len(self.nodes[0].getrawrelaypool())))
                block_hash = self.generate(self.nodes[0], 1)[0]
                if not first_block_hash:
                    first_block_hash = block_hash
                assert_equal(len(self.nodes[0].getrawrelaypool()), 0)
                self.log.info('All of the transactions from the current batch have been mined into a block.')
            elif tx_count == size:
                # At the end all of the mined blocks are invalidated, and all of the created
                # transactions should be re-added from disconnected blocks to the relaypool.
                self.log.info('The last batch of {} transactions has been accepted into the relaypool.'.format(len(self.nodes[0].getrawrelaypool())))
                start = time.time()
                self.nodes[0].invalidateblock(first_block_hash)
                end = time.time()
                assert_equal(len(self.nodes[0].getrawrelaypool()), size)
                self.log.info('All of the recently mined transactions have been re-added into the relaypool in {} seconds.'.format(end - start))

        self.log.info('Checking descendants/ancestors properties of all of the in-relaypool transactions...')
        for k, tx in enumerate(tx_id):
            self.log.debug('Check transaction #{}.'.format(k))
            entry = self.nodes[0].getrelaypoolentry(tx)
            assert_equal(entry['descendantcount'], size - k)
            assert_equal(entry['descendantsize'], sum(tx_size[k:size]))
            assert_equal(entry['ancestorcount'], k + 1)
            assert_equal(entry['ancestorsize'], sum(tx_size[0:(k + 1)]))

    def run_test(self):
        # Use batch size limited by DEFAULT_ANCESTOR_LIMIT = 25 to not fire "too many unconfirmed parents" error.
        self.transaction_graph_test(size=100, n_tx_to_mine=[25, 50, 75])


if __name__ == '__main__':
    RelayPoolUpdateFromBlockTest(__file__).main()
