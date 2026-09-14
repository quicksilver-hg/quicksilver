#!/usr/bin/env python3
# Copyright (c) 2017-2021 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

#
# Test getblockstats rpc call
#

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
)
import json
import os
import time

TESTSDIR = os.path.dirname(os.path.realpath(__file__))

class GetblockstatsTest(QuicksilverTestFramework):

    start_height = 101
    max_stat_pos = 2

    def add_options(self, parser):
        self.add_vault_options(parser)
        parser.add_argument('--gen-test-data', dest='gen_test_data',
                            default=False, action='store_true',
                            help='Generate test data')
        parser.add_argument('--test-data', dest='test_data',
                            default='data/rpc_getblockstats.json',
                            action='store', metavar='FILE',
                            help='Test data file')
        parser.add_argument('--use-test-data', dest='use_test_data',
                            default=False, action='store_true',
                            help='Load blocks and expected stats from --test-data')

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.supports_cli = False

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()

    def get_stats(self):
        return [self.nodes[0].getblockstats(hash_or_height=self.start_height + i) for i in range(self.max_stat_pos+1)]

    def build_test_chain(self):
        mocktime = int(time.time())
        self.nodes[0].setmocktime(mocktime)
        self.nodes[0].createvault(vault_name='test')
        vault = self.nodes[0].get_vault_rpc('test')

        address = vault.getnewaddress()
        self.generatetoaddress(self.nodes[0], COINBASE_MATURITY + 1, address)

        vault.sendtoaddress(address=address, amount=10)
        self.generate(self.nodes[0], 1)

        vault.sendtoaddress(address=address, amount=10)
        vault.sendtoaddress(address=address, amount=10)
        vault.sendtoaddress(address=address, amount=1)
        # Send to OP_RETURN output to test its exclusion from statistics
        vault.send(outputs=[{address: 1}, {"data": "21"}])
        self.sync_all()
        self.generate(self.nodes[0], 1)

        self.expected_stats = self.get_stats()

        return mocktime

    def generate_test_data(self, filename):
        mocktime = self.build_test_chain()

        blocks = []
        tip = self.nodes[0].getbestblockhash()
        blockhash = None
        height = 0
        while tip != blockhash:
            blockhash = self.nodes[0].getblockhash(height)
            blocks.append(self.nodes[0].getblock(blockhash, 0))
            height += 1

        to_dump = {
            'blocks': blocks,
            'mocktime': int(mocktime),
            'stats': self.expected_stats,
        }
        with open(filename, 'w', encoding="utf8") as f:
            json.dump(to_dump, f, sort_keys=True, indent=2)

    def load_test_data(self, filename):
        with open(filename, 'r', encoding="utf8") as f:
            d = json.load(f)
            blocks = d['blocks']
            mocktime = d['mocktime']
            self.expected_stats = d['stats']

        # Set the timestamps from the file so that the nodes can get out of Initial Block Download
        self.nodes[0].setmocktime(mocktime)
        self.sync_all()

        for b in blocks:
            self.nodes[0].submitblock(b)


    def run_test(self):
        test_data = os.path.join(TESTSDIR, self.options.test_data)
        if self.options.gen_test_data:
            self.generate_test_data(test_data)
        elif self.options.use_test_data:
            self.load_test_data(test_data)
        else:
            self.build_test_chain()

        self.sync_all()
        stats = self.get_stats()

        # Make sure all valid statistics are included but nothing else is
        expected_keys = self.expected_stats[0].keys()
        assert_equal(set(stats[0].keys()), set(expected_keys))

        assert_equal(stats[0]['height'], self.start_height)
        assert_equal(stats[self.max_stat_pos]['height'], self.start_height + self.max_stat_pos)

        for i in range(self.max_stat_pos+1):
            self.log.info('Checking block %d' % (i))
            assert_equal(stats[i], self.expected_stats[i])

            # Check selecting block by hash too
            blockhash = self.expected_stats[i]['blockhash']
            stats_by_hash = self.nodes[0].getblockstats(hash_or_height=blockhash)
            assert_equal(stats_by_hash, self.expected_stats[i])

        # Make sure each stat can be queried on its own
        for stat in expected_keys:
            for i in range(self.max_stat_pos+1):
                result = self.nodes[0].getblockstats(hash_or_height=self.start_height + i, stats=[stat])
                assert_equal(list(result.keys()), [stat])
                if result[stat] != self.expected_stats[i][stat]:
                    self.log.info('result[%s] (%d) failed, %r != %r' % (
                        stat, i, result[stat], self.expected_stats[i][stat]))
                assert_equal(result[stat], self.expected_stats[i][stat])

        # Make sure only the selected statistics are included (more than one)
        some_stats = {'mintxsize', 'maxtxsize'}
        stats = self.nodes[0].getblockstats(hash_or_height=1, stats=list(some_stats))
        assert_equal(set(stats.keys()), some_stats)

        # Test invalid parameters raise the proper json exceptions
        tip = self.start_height + self.max_stat_pos
        assert_raises_rpc_error(-8, 'Target block height %d after current tip %d' % (tip+1, tip),
                                self.nodes[0].getblockstats, hash_or_height=tip+1)
        assert_raises_rpc_error(-8, 'Target block height %d is negative' % (-1),
                                self.nodes[0].getblockstats, hash_or_height=-1)

        # Make sure not valid stats aren't allowed
        inv_sel_stat = 'asdfghjkl'
        inv_stats = [
            [inv_sel_stat],
            ['mintxsize', inv_sel_stat],
            [inv_sel_stat, 'mintxsize'],
            ['mintxsize', inv_sel_stat, 'maxtxsize'],
        ]
        for inv_stat in inv_stats:
            assert_raises_rpc_error(-8, f"Invalid selected statistic '{inv_sel_stat}'",
                                    self.nodes[0].getblockstats, hash_or_height=1, stats=inv_stat)

        # Make sure we aren't always returning inv_sel_stat as the culprit stat
        assert_raises_rpc_error(-8, f"Invalid selected statistic 'aaa{inv_sel_stat}'",
                                self.nodes[0].getblockstats, hash_or_height=1, stats=['mintxsize', f'aaa{inv_sel_stat}'])
        assert_raises_rpc_error(-8, 'Use witness_total_size instead of swtotal_size',
                                self.nodes[0].getblockstats, hash_or_height=1, stats=['swtotal_size'])
        assert_raises_rpc_error(-8, 'Use witness_total_weight instead of swtotal_weight',
                                self.nodes[0].getblockstats, hash_or_height=1, stats=['swtotal_weight'])
        assert_raises_rpc_error(-8, 'Use witness_txs instead of swtxs',
                                self.nodes[0].getblockstats, hash_or_height=1, stats=['swtxs'])
        # The main network's genesis block shouldn't be found on sandbox
        assert_raises_rpc_error(-5, 'Block not found', self.nodes[0].getblockstats,
                                hash_or_height='c9144acae20212e57e9e59f34a681bd25f55d81438001c424ebb95cd4869bcf3')

        # Invalid number of args
        assert_raises_rpc_error(-1, 'getblockstats hash_or_height ( stats )', self.nodes[0].getblockstats, '00', 1, 2)
        assert_raises_rpc_error(-1, 'getblockstats hash_or_height ( stats )', self.nodes[0].getblockstats)

        self.log.info('Test block height 0')
        genesis_stats = self.nodes[0].getblockstats(0)
        assert_equal(genesis_stats["blockhash"], self.nodes[0].getblockhash(0))
        assert_equal(genesis_stats["utxo_increase"], 1)
        # 131 = GetSerializeSize(out) + PER_UTXO_OVERHEAD(41)
        #     = (8 nValue + 1 compactsize + 81 scriptPubKey) + 41
        # The genesis mark's scriptPubKey is OP_RETURN + OP_PUSHDATA1 + len + 78
        # bytes of mark. It grew 7 (not 6) at the 2026-08-24 remint: the mark went
        # 72 -> 78 bytes and crossed 75, so the push encoding also changed from a
        # direct push to OP_PUSHDATA1. Was 124 when the mark was 72 bytes.
        assert_equal(genesis_stats["utxo_size_inc"], 131)
        assert_equal(genesis_stats["utxo_increase_actual"], 0)
        assert_equal(genesis_stats["utxo_size_inc_actual"], 0)

        self.log.info('Test tip including OP_RETURN')
        tip_stats = self.nodes[0].getblockstats(tip)
        assert_greater_than(tip_stats["utxo_increase"], tip_stats["utxo_increase_actual"])
        assert_greater_than(tip_stats["utxo_size_inc"], tip_stats["utxo_size_inc_actual"])

        self.log.info("Test when only header is known")
        block = self.generateblock(self.nodes[0], output="raw(55)", transactions=[], submit=False)
        self.nodes[0].submitheader(block["hex"])
        assert_raises_rpc_error(-1, "Block not available (not fully downloaded)", lambda: self.nodes[0].getblockstats(block['hash']))

        self.log.info('Test when block is missing')
        (self.nodes[0].blocks_path / 'blk00000.dat').rename(self.nodes[0].blocks_path / 'blk00000.dat.backup')
        assert_raises_rpc_error(-1, 'Block not found on disk', self.nodes[0].getblockstats, hash_or_height=1)
        (self.nodes[0].blocks_path / 'blk00000.dat.backup').rename(self.nodes[0].blocks_path / 'blk00000.dat')


if __name__ == '__main__':
    GetblockstatsTest(__file__).main()
