#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Determinism checks for the basic block-filter index.

NOT AN ORACLE. These cases catch nondeterminism only. They cannot certify that
a filter is correct: every node here runs the same binary, so they will agree
on a wrong filter exactly as readily as a right one. Do not cite a green run
of this file as evidence of filter correctness. The oracles for that are
vault_fast_rescan.py (fast vs slow rescan) and the BIP 158 vector test in
src/test/blockfilter_tests.cpp.

p2p_blockfilters.py cannot carry the cross-node half. Its two nodes are on
different chains by construction, so agreement there would be a category
error. This file keeps them on one chain.

Three cases, in ascending order of what they are worth:

1. Cross-node. Two nodes on one chain. The weakest case by far -- both build
   their index through BlockConnected, so it only pins that a node which
   received blocks over P2P agrees with the node that generated them.

2. Reindex. Rebuild block index, chainstate and undo data from raw blk*.dat
   and require the filters to come back byte-identical. Note what this is NOT:
   -reindex wipes the block index too, so at BaseIndex::Init the active chain
   is empty, `m_synced` latches true (src/index/base.cpp, "this will latch to
   true immediately ... indexation will happen solely via BlockConnected"),
   and ThreadSync exits without doing any work. This case is therefore
   BlockConnected against BlockConnected over a re-derived chainstate. Both
   wipe lines are asserted below so that character is pinned: if a future
   change stops wiping the block index, this case silently becomes case 3 and
   the assertion tells you.

3. Late-enabled index -- the only case with two paths in it. A node that ran
   with -blockfilterindex=0 is restarted with it on. Its index is empty while
   the chain is already at the tip, so `m_synced` latches FALSE and
   BaseIndex::Sync walks the chain calling ReadBlock from disk. That is
   ThreadSync-from-disk against the live BlockConnected values from the other
   nodes. Still one implementation -- hence still not an oracle -- but two
   genuinely different routes into it, and it is the path a user takes
   whenever they enable the index on an existing node.

There is no log line that distinguishes a ThreadSync that worked from one that
latched synced and exited: both fall through to "is enabled at height N", and
the "Syncing ... from height" line is rate-limited at 30s so a regtest chain
never reaches it. The path is pinned instead by asserting the preconditions
that decide the latch.

Measured once, so nobody has to re-derive it (2026-08-27, 104-block chain):
the case 3 index thread lived 88ms and reported "is enabled at height 103";
the case 2 thread lived 36us and reported "is enabled at height **102**". That
102 is the clearest evidence of the difference -- at reindex, Sync() saw a
chain that was still being rebuilt, latched m_synced and exited, and
BlockConnected carried the index the remaining block to the tip.
"""

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.messages import COIN
from test_framework.script import CScript, OP_RETURN
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal
from test_framework.vault import MiniVault, getnewdestination


class BlockFilterAgreementTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        self.extra_args = [
            ["-blockfilterindex=1", "-txpownocycle=1"],
            ["-blockfilterindex=1", "-txpownocycle=1"],
            # Starts WITHOUT the index so it can be enabled later, which is
            # what forces the from-disk sync path in case 3.
            ["-blockfilterindex=0", "-txpownocycle=1"],
        ]

    def run_test(self):
        node0, node1, node2 = self.nodes
        vault = MiniVault(node0)

        # Coinbases, a spend (so undo data is in the element set), and an
        # OP_RETURN output the basic filter skips. The chain has to contain
        # more than empty coinbase filters or the comparisons below would
        # pass vacuously.
        self.generate(vault, COINBASE_MATURITY + 1)
        _, spk, _ = getnewdestination()
        vault.send_to(from_node=node0, scriptPubKey=spk, amount=1 * COIN)
        self.generate(node0, 1)
        vault.send_to(from_node=node0, scriptPubKey=CScript([OP_RETURN, b"filter-agreement"]), amount=0)
        self.generate(node0, 1)
        self.sync_blocks()

        height = node0.getblockcount()
        self.wait_for_filter_index(node0, height)
        self.wait_for_filter_index(node1, height)

        before = self.read_filters(node0)
        self.assert_chain_is_worth_comparing(before)

        # CASE 1 -- DETERMINISM ONLY, not correctness. Two copies of the same
        # binary on the same chain, both indexing through BlockConnected. A
        # later session must not cite this as an oracle.
        self.log.info("Case 1: cross-node filter agreement (determinism check, not an oracle)")
        assert_equal(before, self.read_filters(node1))

        # CASE 3 -- DETERMINISM ONLY, but the only case with two code paths in
        # it. Run it before the reindex so node2 is still holding a chainstate
        # built the ordinary way.
        self.log.info("Case 3: late-enabled index rebuilds from disk (determinism check, not an oracle)")
        assert_equal(node2.getblockcount(), height)
        self.stop_node(2)
        filter_dir = node2.chain_path / "indexes" / "blockfilter"
        # These two facts ARE the latch condition at src/index/base.cpp: no
        # index on disk means the index's start block is null, and a chain
        # already at the tip means the active chain's tip is not. Unequal, so
        # m_synced starts false and Sync() reads every block from disk. Assert
        # them rather than trusting the setup, because if the node ever leaves
        # a filter index behind with -blockfilterindex=0 this case quietly
        # degenerates into a plain restart.
        assert not filter_dir.exists(), f"node2 left a filter index behind at {filter_dir}; the from-disk path would be skipped"
        assert height > 0, "an empty chain would latch m_synced true and skip the from-disk path"
        self.start_node(2, extra_args=["-blockfilterindex=1", "-txpownocycle=1"])
        self.wait_for_filter_index(node2, height)
        assert_equal(before, self.read_filters(node2))

        # CASE 2 -- DETERMINISM ONLY, and weaker than its name suggests. See
        # the module docstring: because -reindex wipes the block index as well,
        # this rebuilds through BlockConnected, not through ThreadSync. What it
        # does prove is that block index, chainstate and undo data re-derived
        # from raw blk*.dat yield byte-identical filters.
        self.log.info("Case 2: reindex filter agreement (determinism check, not an oracle)")
        blocks_index_path = node0.chain_path / "blocks" / "index"
        filter_db_path = node0.chain_path / "indexes" / "blockfilter" / "basic" / "db"
        self.stop_node(0)
        with node0.assert_debug_log(
            expected_msgs=[
                f"Wiping LevelDB in {filter_db_path}",
                # Pins case 2 as the BlockConnected-vs-BlockConnected shape.
                f"Wiping LevelDB in {blocks_index_path}",
            ],
            timeout=60,
        ):
            self.start_node(0, extra_args=self.extra_args[0] + ["-reindex"])
        self.wait_for_filter_index(node0, height)
        assert_equal(before, self.read_filters(node0))

    def wait_for_filter_index(self, node, height):
        expected = {
            "basic block filter index": {"synced": True, "best_block_height": height},
        }
        self.wait_until(lambda: node.getindexinfo() == expected)

    def read_filters(self, node):
        """Map every block hash on the active chain to (filter, header).

        getblockfilter serves this from the index (LookupFilter), not by
        recomputing from the block, so these comparisons really do read back
        what each indexing path wrote.
        """
        out = {}
        for height in range(node.getblockcount() + 1):
            blockhash = node.getblockhash(height)
            result = node.getblockfilter(blockhash, "basic")
            out[blockhash] = (result["filter"], result["header"])
        return out

    def assert_chain_is_worth_comparing(self, filters):
        """Refuse to pass if the seeded chain produced degenerate filters.

        Narrow on purpose: this checks the tip is not the empty filter and that
        the header chain moved. It does NOT prove undo data or a skipped script
        reached an element set -- every block keys its GCS on its own hash, so
        any two non-empty filters differ regardless of content. Undo data in
        the element set is guarded by vault_fast_rescan.py, which is an oracle;
        this is only here so an all-coinbase chain cannot make the comparisons
        above trivially true.
        """
        genesis = self.nodes[0].getblockhash(0)
        genesis_filter, genesis_header = filters[genesis]
        tip = self.nodes[0].getbestblockhash()
        tip_filter, tip_header = filters[tip]
        assert tip_filter != "00", "tip produced the empty filter; the comparisons would be near-vacuous"
        assert genesis_filter != tip_filter, "seeded chain produced only the genesis filter; the comparison would be vacuous"
        assert genesis_header != tip_header, "seeded chain did not move the filter-header chain"


if __name__ == "__main__":
    BlockFilterAgreementTest(__file__).main()
