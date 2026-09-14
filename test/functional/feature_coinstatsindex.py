#!/usr/bin/env python3
# Copyright (c) 2020-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test coinstatsindex across nodes.

Test that the values returned by gettxoutsetinfo are consistent
between a node running the coinstatsindex and a node without
the index.
"""

from decimal import Decimal

from test_framework.blocktools import (
    COINBASE_MATURITY,
    create_block,
    create_coinbase,
)
from test_framework.messages import (
    COIN,
    CTxOut,
)
from test_framework.script import (
    CScript,
    OP_FALSE,
    OP_RETURN,
)
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.txpow import prove_tx_pow
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)
from test_framework.vault import (
    MiniVault,
    getnewdestination,
)


class CoinStatsIndexTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.supports_cli = False
        self.extra_args = [
            ["-txpownocycle=1"],
            ["-coinstatsindex", "-txpownocycle=1"]
        ]

    def run_test(self):
        self.vault = MiniVault(self.nodes[0])
        self._test_coin_stats_index()
        self._test_use_index_option()
        self._test_reorg_index()
        self._test_init_index_after_reorg()

    def block_sanity_check(self, block_info):
        assert 'prevout_spent' in block_info
        assert 'new_outputs_ex_coinbase' in block_info
        assert 'coinbase' in block_info
        assert 'unspendable' in block_info

    def sync_index_node(self):
        self.wait_until(lambda: self.nodes[1].getindexinfo()['coinstatsindex']['synced'] is True)

    def _test_coin_stats_index(self):
        node = self.nodes[0]
        index_node = self.nodes[1]
        # Both none and muhash options allow the usage of the index
        index_hash_options = ['none', 'muhash']

        # Generate a normal transaction and mine it
        self.generate(self.vault, COINBASE_MATURITY + 1)
        self.vault.send_self_transfer(from_node=node)
        self.generate(node, 1)

        self.log.info("Test that gettxoutsetinfo() output is consistent with or without coinstatsindex option")
        res0 = node.gettxoutsetinfo('none')

        # The fields 'disk_size' and 'transactions' do not exist on the index
        del res0['disk_size'], res0['transactions']

        for hash_option in index_hash_options:
            res1 = index_node.gettxoutsetinfo(hash_option)
            # The fields 'block_info' and 'total_unspendable_amount' only exist on the index
            del res1['block_info'], res1['total_unspendable_amount']
            res1.pop('muhash', None)

            # Everything left should be the same
            assert_equal(res1, res0)

        self.log.info("Test that gettxoutsetinfo() can get fetch data on specific heights with index")

        # Generate a new tip
        self.generate(node, 5)

        for hash_option in index_hash_options:
            # Fetch old stats by height
            res2 = index_node.gettxoutsetinfo(hash_option, 102)
            del res2['block_info'], res2['total_unspendable_amount']
            res2.pop('muhash', None)
            assert_equal(res0, res2)

            # Fetch old stats by hash
            res3 = index_node.gettxoutsetinfo(hash_option, res0['bestblock'])
            del res3['block_info'], res3['total_unspendable_amount']
            res3.pop('muhash', None)
            assert_equal(res0, res3)

            # It does not work without coinstatsindex
            assert_raises_rpc_error(-8, "Querying specific block heights requires coinstatsindex", node.gettxoutsetinfo, hash_option, 102)

        self.log.info("Test gettxoutsetinfo() with index and verbose flag")

        for hash_option in index_hash_options:
            # Genesis block is unspendable
            res4 = index_node.gettxoutsetinfo(hash_option, 0)
            assert_equal(res4['total_unspendable_amount'], 50)
            assert_equal(res4['block_info'], {
                'unspendable': 50,
                'prevout_spent': 0,
                'new_outputs_ex_coinbase': 0,
                'coinbase': 0,
                'unspendables': {
                    'genesis_block': 50,
                    'scripts': 0,
                    'unclaimed_rewards': 0
                }
            })
            self.block_sanity_check(res4['block_info'])

            # Test an older block height that included a normal tx
            res5 = index_node.gettxoutsetinfo(hash_option, 102)
            assert_equal(res5['total_unspendable_amount'], 50)
            assert_equal(res5['block_info'], {
                'unspendable': 0,
                'prevout_spent': Decimal('49.67333334'),
                'new_outputs_ex_coinbase': Decimal('49.67333334'),
                'coinbase': Decimal('17.68000000'),
                'unspendables': {
                    'genesis_block': 0,
                    'scripts': 0,
                    'unclaimed_rewards': 0,
                }
            })
            self.block_sanity_check(res5['block_info'])

        # Generate and send a normal tx with two outputs
        tx1 = self.vault.send_to(
            from_node=node,
            scriptPubKey=self.vault.get_output_script(),
            amount=21 * COIN,
        )

        # Find the right position of the 21 Hg output
        tx1_out_21 = self.vault.get_utxo(txid=tx1["txid"], vout=tx1["sent_vout"])

        # Generate and send another tx with an OP_RETURN output (which is unspendable)
        tx2 = self.vault.create_self_transfer(utxo_to_spend=tx1_out_21)['tx']
        tx2_val = '21'
        tx2.vout = [CTxOut(int(Decimal(tx2_val) * COIN), CScript([OP_RETURN] + [OP_FALSE] * 30))]
        prove_tx_pow(tx2, anchor_height=node.getblockcount())
        tx2_hex = tx2.serialize().hex()
        self.nodes[0].sendrawtransaction(tx2_hex, tx2_val)

        # Include both txs in a block
        self.generate(self.nodes[0], 1)

        for hash_option in index_hash_options:
            # Check all amounts were registered correctly
            res6 = index_node.gettxoutsetinfo(hash_option, 108)
            assert_equal(res6['total_unspendable_amount'], Decimal('71.00000000'))
            assert_equal(res6['block_info'], {
                'unspendable': Decimal('21.00000000'),
                'prevout_spent': Decimal('70.67333334'),
                'new_outputs_ex_coinbase': Decimal('49.67333334'),
                'coinbase': Decimal('16.72000000'),
                'unspendables': {
                    'genesis_block': 0,
                    'scripts': Decimal('21.00000000'),
                    'unclaimed_rewards': 0,
                }
            })
            self.block_sanity_check(res6['block_info'])

        # Create a coinbase that does not claim full subsidy and also
        # has two outputs
        cb = create_coinbase(109, nValue=9)
        cb.vout.append(CTxOut(5 * COIN, CScript([OP_FALSE])))
        cb.rehash()

        # Generate a block that includes previous coinbase
        tip = self.nodes[0].getbestblockhash()
        block_time = self.nodes[0].getblock(tip)['time'] + 1
        block = create_block(int(tip, 16), cb, block_time)
        block.solve()
        self.nodes[0].submitblock(block.serialize().hex())
        self.sync_all()

        for hash_option in index_hash_options:
            res7 = index_node.gettxoutsetinfo(hash_option, 109)
            assert_equal(res7['total_unspendable_amount'], Decimal('71.39333334'))
            assert_equal(res7['block_info'], {
                'unspendable': Decimal('0.39333334'),
                'prevout_spent': 0,
                'new_outputs_ex_coinbase': 0,
                'coinbase': 14,
                'unspendables': {
                    'genesis_block': 0,
                    'scripts': 0,
                    'unclaimed_rewards': Decimal('0.39333334'),
                }
            })
            self.block_sanity_check(res7['block_info'])

        self.log.info("Test that the index is robust across restarts")

        res8 = index_node.gettxoutsetinfo('muhash')
        self.restart_node(1, extra_args=self.extra_args[1])
        res9 = index_node.gettxoutsetinfo('muhash')
        assert_equal(res8, res9)

        self.generate(index_node, 1, sync_fun=self.no_op)
        res10 = index_node.gettxoutsetinfo('muhash')
        assert res8['txouts'] < res10['txouts']

        self.log.info("Test that the index works with -reindex")

        self.restart_node(1, extra_args=["-coinstatsindex", "-txpownocycle=1", "-reindex"])
        self.sync_index_node()
        res11 = index_node.gettxoutsetinfo('muhash')
        assert_equal(res11, res10)

        self.log.info("Test that the index works with -reindex-chainstate")

        self.restart_node(1, extra_args=["-coinstatsindex", "-txpownocycle=1", "-reindex-chainstate"])
        self.sync_index_node()
        res12 = index_node.gettxoutsetinfo('muhash')
        assert_equal(res12, res10)

        self.log.info("Test obtaining info for a non-existent block hash")
        assert_raises_rpc_error(-5, "Block not found", index_node.gettxoutsetinfo, hash_type="none", hash_or_height="ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", use_index=True)

    def _test_use_index_option(self):
        self.log.info("Test use_index option for nodes running the index")

        self.connect_nodes(0, 1)
        self.nodes[0].waitforblockheight(110)
        res = self.nodes[0].gettxoutsetinfo('muhash')
        option_res = self.nodes[1].gettxoutsetinfo(hash_type='muhash', hash_or_height=None, use_index=False)
        del res['disk_size'], option_res['disk_size']
        assert_equal(res, option_res)

    def _test_reorg_index(self):
        self.log.info("Test that index can handle reorgs")

        # Generate two block, let the index catch up, then invalidate the blocks
        index_node = self.nodes[1]
        reorg_blocks = self.generatetoaddress(index_node, 2, getnewdestination()[2])
        reorg_block = reorg_blocks[1]
        self.sync_index_node()
        res_invalid = index_node.gettxoutsetinfo('muhash')
        index_node.invalidateblock(reorg_blocks[0])
        assert_equal(index_node.gettxoutsetinfo('muhash')['height'], 110)

        # Add two new blocks
        block = self.generate(index_node, 2, sync_fun=self.no_op)[1]
        res = index_node.gettxoutsetinfo(hash_type='muhash', hash_or_height=None, use_index=False)

        # Test that the result of the reorged block is not returned for its old block height
        res2 = index_node.gettxoutsetinfo(hash_type='muhash', hash_or_height=112)
        assert_equal(res["bestblock"], block)
        assert_equal(res["muhash"], res2["muhash"])
        assert res["muhash"] != res_invalid["muhash"]

        # Test that requesting reorged out block by hash is still returning correct results
        res_invalid2 = index_node.gettxoutsetinfo(hash_type='muhash', hash_or_height=reorg_block)
        assert_equal(res_invalid2["muhash"], res_invalid["muhash"])
        assert res["muhash"] != res_invalid2["muhash"]

        # Add another block, so we don't depend on reconsiderblock remembering which
        # blocks were touched by invalidateblock
        self.generate(index_node, 1)

        # Height 108 contains two valid tx-PoW transactions and therefore two
        # Frame-B mint allowances. Rewind across it and ensure both the allowance
        # and unclaimed-reward accounting return to the exact persisted values.
        mint_block = index_node.getblockhash(108)
        mint_stats_before_rewind = index_node.gettxoutsetinfo(hash_type='muhash', hash_or_height=mint_block)
        assert_equal(mint_stats_before_rewind['block_info']['unspendables']['unclaimed_rewards'], 0)

        # Build a one-block competing branch so BlockConnected forces the index
        # to rewind all the way through the mint-bearing block to height 98.
        block = index_node.getblockhash(99)
        index_node.invalidateblock(block)
        self.generatetoaddress(index_node, 1, getnewdestination()[2], sync_fun=self.no_op)
        # A tip query drains queued validation notifications when the index is
        # on a different branch; the `synced` flag alone stays latched true.
        rewound_stats = index_node.gettxoutsetinfo('muhash')
        assert_equal(rewound_stats['height'], 99)
        assert_equal(index_node.getindexinfo()['coinstatsindex']['best_block_height'], 99)
        assert_equal(
            index_node.gettxoutsetinfo(hash_type='muhash', hash_or_height=mint_block),
            mint_stats_before_rewind,
        )

        # Reconnect the original longer branch and verify the active height row
        # is rebuilt with the same mint/unclaimed accounting.
        index_node.reconsiderblock(block)
        assert_equal(index_node.gettxoutsetinfo('muhash')['height'], 113)
        mint_stats_after_rewind = index_node.gettxoutsetinfo(hash_type='muhash', hash_or_height=108)
        assert_equal(mint_stats_after_rewind, mint_stats_before_rewind)
        res3 = index_node.gettxoutsetinfo(hash_type='muhash', hash_or_height=112)
        assert_equal(res2, res3)

    def _test_init_index_after_reorg(self):
        self.log.info("Test a reorg while the index is deactivated")
        index_node = self.nodes[1]
        block = self.nodes[0].getbestblockhash()
        self.generate(index_node, 2, sync_fun=self.no_op)
        self.sync_index_node()

        # Restart without index
        self.restart_node(1, extra_args=[])
        self.connect_nodes(0, 1)
        index_node.invalidateblock(block)
        self.generatetoaddress(index_node, 5, getnewdestination()[2])
        res = index_node.gettxoutsetinfo(hash_type='muhash', hash_or_height=None, use_index=False)

        # Restart with index that still has its best block on the old chain
        self.restart_node(1, extra_args=self.extra_args[1])
        self.sync_index_node()
        res1 = index_node.gettxoutsetinfo(hash_type='muhash', hash_or_height=None, use_index=True)
        assert_equal(res["muhash"], res1["muhash"])

        self.log.info("Test index with an unclean restart after a reorg")
        self.restart_node(1, extra_args=self.extra_args[1])
        committed_height = index_node.getblockcount()
        self.generate(index_node, 2, sync_fun=self.no_op)
        self.sync_index_node()
        block2 = index_node.getbestblockhash()
        index_node.invalidateblock(block2)
        self.generatetoaddress(index_node, 1, getnewdestination()[2], sync_fun=self.no_op)
        self.sync_index_node()
        index_node.kill_process()
        self.start_node(1, extra_args=self.extra_args[1])
        self.sync_index_node()
        # Because of the unclean shutdown above, indexes reset to the point we last committed them to disk.
        assert_equal(index_node.getindexinfo()['coinstatsindex']['best_block_height'], committed_height)


if __name__ == '__main__':
    CoinStatsIndexTest(__file__).main()
