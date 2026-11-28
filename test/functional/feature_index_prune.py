#!/usr/bin/env python3
# Copyright (c) 2020-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test indices in conjunction with prune."""
import concurrent.futures
import os
from test_framework.blocktools import KEEP_WINDOW_GROWTH
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
)


# A pruned node keeps MIN_BLOCKS_TO_KEEP (2016, F-703) blocks below its tip; upstream sized this
# test for 288. Each stretch of chain a prune request must reach under grows by the difference,
# G. Heights are written as the original height plus a multiple of G so the mapping stays
# readable: block files below a request are laid out as before, so expected answers that only
# depend on them carry over; the ones that moved were re-derived from the block files.
G = KEEP_WINDOW_GROWTH
# Re-derived for F-703 by reading the block files just before each prune (control: the same
# method gives the unchanged 306 above). Fastprune files here hold 107 blocks, so the boundaries
# run 306 + 107k before the -reindex (upstream-sized 734 = 306 + 4 * 107) and 106 + 107k after it
# (upstream-sized 2139 = 106 + 19 * 107).
PRUNEHEIGHT_2 = 2446  # 306 + 20 * 107: the last boundary at or below 751 + G = 2479
PRUNEHEIGHT_REINDEXED = 5563  # 106 + 51 * 107: the last boundary at or below 2500 + 3G - 2016 = 5668


class FeatureIndexPruneTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 4
        self.extra_args = [
            ["-fastprune", "-prune=1", "-blockfilterindex=1"],
            ["-fastprune", "-prune=1", "-coinstatsindex=1"],
            ["-fastprune", "-prune=1", "-blockfilterindex=1", "-coinstatsindex=1"],
            [],
        ]

    def setup_network(self):
        self.setup_nodes()  # No P2P connection, so that linear_sync works

    def linear_sync(self, node_from, *, height_from=None):
        # Linear sync over RPC, because P2P sync may not be linear
        to_height = node_from.getblockcount()
        if height_from is None:
            height_from = min([n.getblockcount() for n in self.nodes]) + 1
        with concurrent.futures.ThreadPoolExecutor(max_workers=self.num_nodes) as rpc_threads:
            for i in range(height_from, to_height + 1):
                b = node_from.getblock(blockhash=node_from.getblockhash(i), verbosity=0)
                list(rpc_threads.map(lambda n: n.submitblock(b), self.nodes))

    def generate(self, node, num_blocks, sync_fun=None):
        return super().generate(node, num_blocks, sync_fun=sync_fun or (lambda: self.linear_sync(node)))

    def sync_index(self, height):
        expected_filter = {
            'basic block filter index': {'synced': True, 'best_block_height': height},
        }
        self.wait_until(lambda: self.nodes[0].getindexinfo() == expected_filter)

        expected_stats = {
            'coinstatsindex': {'synced': True, 'best_block_height': height}
        }
        self.wait_until(lambda: self.nodes[1].getindexinfo() == expected_stats, timeout=150)

        expected = {**expected_filter, **expected_stats}
        self.wait_until(lambda: self.nodes[2].getindexinfo() == expected)

    def restart_without_indices(self):
        for i in range(3):
            self.restart_node(i, extra_args=["-fastprune", "-prune=1"])

    def run_test(self):
        filter_nodes = [self.nodes[0], self.nodes[2]]
        stats_nodes = [self.nodes[1], self.nodes[2]]

        self.log.info("check if we can access blockfilters and coinstats when pruning is enabled but no blocks are actually pruned")
        self.sync_index(height=200)
        tip = self.nodes[0].getbestblockhash()
        for node in filter_nodes:
            assert_greater_than(len(node.getblockfilter(tip)['filter']), 0)
        for node in stats_nodes:
            assert node.gettxoutsetinfo(hash_type="muhash", hash_or_height=tip)['muhash']

        self.generate(self.nodes[0], 500 + G)
        self.sync_index(height=700 + G)

        self.log.info("prune some blocks")
        for node in self.nodes[:2]:
            with node.assert_debug_log([f'limited pruning to height {689 + G}']):
                pruneheight_new = node.pruneblockchain(400)
                # the prune heights used here and below are magic numbers that are determined by the
                # thresholds at which block files wrap, so they depend on disk serialization and default block file size.
                # Recalibrated for Quicksilver: a sandbox block is 601 bytes here (measured on
                # this chain, not derived on paper), well over the size the upstream numbers
                # assumed, so -fastprune's 64 KiB files hold ~109 blocks rather than upstream's
                # count and the wrap points differ. The cached chain in file 0 is written without
                # -fastprune, so file 0 holds all 200 cached blocks and file 1 covers the next
                # run. Pruning to 400 drops files 0 and 1 whole, which makes 306 the last pruned
                # block. These must be recomputed whenever block serialization changes -- most
                # recently when nCongestion entered the header (header-field congestion pricing, +4 bytes/block).
                # The G blocks added for F-703 all sit above 400, so files 0 and 1 are unchanged.
                assert_equal(pruneheight_new, 306)

        self.log.info("check if we can access the tips blockfilter and coinstats when we have pruned some blocks")
        tip = self.nodes[0].getbestblockhash()
        for node in filter_nodes:
            assert_greater_than(len(node.getblockfilter(tip)['filter']), 0)
        for node in stats_nodes:
            assert node.gettxoutsetinfo(hash_type="muhash", hash_or_height=tip)['muhash']

        self.log.info("check if we can access the blockfilter and coinstats of a pruned block")
        height_hash = self.nodes[0].getblockhash(2)
        for node in filter_nodes:
            assert_greater_than(len(node.getblockfilter(height_hash)['filter']), 0)
        for node in stats_nodes:
            assert node.gettxoutsetinfo(hash_type="muhash", hash_or_height=height_hash)['muhash']

        # mine and sync index up to a height that will later be the pruneheight
        self.generate(self.nodes[0], 51)
        self.sync_index(height=751 + G)

        self.restart_without_indices()

        self.log.info("make sure trying to access the indices throws errors")
        for node in filter_nodes:
            msg = "Index is not enabled for filtertype basic"
            assert_raises_rpc_error(-1, msg, node.getblockfilter, height_hash)
        for node in stats_nodes:
            msg = "Querying specific block heights requires coinstatsindex"
            assert_raises_rpc_error(-8, msg, node.gettxoutsetinfo, "muhash", height_hash)

        self.generate(self.nodes[0], 749 + G)

        self.log.info("prune exactly up to the indices best blocks while the indices are disabled")
        for i in range(3):
            # Upstream requested 1000 here and file granularity happened to stop the prune at 750,
            # one below the indices' best block of 751. Quicksilver's 64 KiB files hold ~109 blocks,
            # so a request of 1000 would prune well past the indices' best block and destroy blocks
            # they still need -- that is the "prune further than the indices" case tested below, not
            # this one. Request the indices' best block instead, which keeps the scenario: the prune
            # stops at the file ending at PRUNEHEIGHT_2 and every block the indices have not seen yet
            # survives. With F-703's G the indices' best block is 751 + G; re-derived from the block
            # files (last block of the highest file ending at or below the request).
            pruneheight_2 = self.nodes[i].pruneblockchain(751 + G)
            assert_equal(pruneheight_2, PRUNEHEIGHT_2)
            # Restart the nodes again with the indices activated
            self.restart_node(i, extra_args=self.extra_args[i])

        self.log.info("make sure that we can continue with the partially synced indices after having pruned up to the index height")
        self.sync_index(height=1500 + 2 * G)

        self.log.info("prune further than the indices best blocks while the indices are disabled")
        self.restart_without_indices()
        self.generate(self.nodes[0], 1000 + G)

        for i in range(3):
            # 500 past the indices' best block, as upstream's 2000 was past 1500, and below the
            # MIN_BLOCKS_TO_KEEP clamp at the tip (2500 + 3G - 2016).
            pruneheight_3 = self.nodes[i].pruneblockchain(2000 + 2 * G)
            assert_greater_than(pruneheight_3, pruneheight_2)
            self.stop_node(i)

        self.log.info("make sure we get an init error when starting the nodes again with the indices")
        filter_msg = "Error: basic block filter index best block of the index goes beyond pruned data. Please disable the index or reindex (which will download the whole blockchain again)"
        stats_msg = "Error: coinstatsindex best block of the index goes beyond pruned data. Please disable the index or reindex (which will download the whole blockchain again)"
        end_msg = f"{os.linesep}Error: A fatal internal error occurred, see debug.log for details: Failed to start indexes, shutting down.."
        for i, msg in enumerate([filter_msg, stats_msg, filter_msg]):
            self.nodes[i].assert_start_raises_init_error(extra_args=self.extra_args[i], expected_msg=msg+end_msg)

        self.log.info("make sure the nodes start again with the indices and an additional -reindex arg")
        for i in range(3):
            restart_args = self.extra_args[i] + ["-reindex"]
            self.restart_node(i, extra_args=restart_args)

        self.linear_sync(self.nodes[3])
        tip_height = 2500 + 3 * G
        self.sync_index(height=tip_height)

        for node in self.nodes[:2]:
            with node.assert_debug_log([f'limited pruning to height {tip_height - 11}']):
                pruneheight_new = node.pruneblockchain(tip_height)
                # The request is clamped to the tip minus MIN_BLOCKS_TO_KEEP (2500 + 3G - 2016), not to
                # the index prune lock at the tip minus 11, so the answer is the last block of the
                # highest block file ending at or below the clamp. After the -reindex above every file
                # is a 64 KiB fastprune file holding ~109 blocks; re-derived from the block files.
                assert_equal(pruneheight_new, PRUNEHEIGHT_REINDEXED)

        self.log.info("ensure that prune locks don't prevent indices from failing in a reorg scenario")
        reorg_height = tip_height - 20
        with self.nodes[0].assert_debug_log([f'basic block filter index prune lock moved back to {reorg_height}']):
            self.nodes[3].invalidateblock(self.nodes[0].getblockhash(reorg_height))
            self.generate(self.nodes[3], 30, sync_fun=lambda: self.linear_sync(self.nodes[3], height_from=reorg_height))


if __name__ == '__main__':
    FeatureIndexPruneTest(__file__).main()
