#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test zero-claim mining (F-578): the mining role's coinbase pays 0 to OP_RETURN.

The blocks come from the background mining role, the path the fleet runs. Each
must validate and relay both ways, book its whole ceiling (subsidy plus mint
allowance) as unclaimed_rewards, add nothing to the UTXO set and leave the next
block's ceiling its own. Block-generating RPCs still claim in full.
"""

from decimal import Decimal

from test_framework.blocktools import (
    COINBASE_MATURITY,
    NORMAL_GBT_REQUEST_PARAMS,
)
from test_framework.messages import COIN
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal, assert_greater_than
from test_framework.vault import MiniVault

ZERO = Decimal("0")


class MiningZeroClaimTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.supports_cli = False
        self.extra_args = [["-coinstatsindex", "-txpownocycle=1", "-minezeroclaim", "-minescript=6a"]] * 2

    def sync_indexes(self):
        tip = self.nodes[0].getblockcount()
        for node in self.nodes:
            self.wait_until(lambda: node.getindexinfo()["coinstatsindex"] == {"synced": True, "best_block_height": tip})

    def block_stats(self, node, height):
        return node.gettxoutsetinfo("muhash", height, True)

    def mine_zero_claim(self, node, count):
        """Run the mining role on `node` until it has mined at least `count`
        blocks, stop it, and return the heights of every block it mined."""
        start = node.getblockcount()
        assert_equal(node.startmining()["payout_script"], "6a")
        self.wait_until(lambda: node.getblockcount() >= start + count, timeout=120)
        node.stopmining()
        assert_equal(node.getminingstatus()["active"], False)
        self.sync_blocks()
        self.sync_indexes()
        return list(range(start + 1, node.getblockcount() + 1))

    def coinbase(self, node, height):
        return node.getblock(node.getblockhash(height), 2)["tx"][0]

    def assert_zero_claim_block(self, height, *, mint=ZERO):
        """The block at `height` claims 0 on both nodes, and the whole ceiling,
        the subsidy read from the node plus `mint`, is booked as unclaimed."""
        subsidy = Decimal(self.nodes[0].getblockstats(height, ["subsidy"])["subsidy"]) / COIN
        assert_greater_than(subsidy, ZERO)
        for node in self.nodes:
            cb = self.coinbase(node, height)
            assert_equal(cb["vout"][0]["output_script"]["hex"], "6a")
            assert_equal(sum((out["value"] for out in cb["vout"]), ZERO), ZERO)
            for out in cb["vout"]:
                assert_equal(out["output_script"]["hex"][:2], "6a")
            before = self.block_stats(node, height - 1)
            after = self.block_stats(node, height)
            info = after["block_info"]
            assert_equal(info["coinbase"], ZERO)
            assert_equal(info["unspendables"]["unclaimed_rewards"], subsidy + mint)
            assert_equal(info["unspendables"]["scripts"], ZERO)
            assert_equal(info["unspendable"], subsidy + mint)
            assert_equal(after["total_amount"], before["total_amount"])
            assert_equal(after["total_unspendable_amount"], before["total_unspendable_amount"] + subsidy + mint)
        return before, after

    def assert_full_claim(self, node, block_hash, expected_value):
        cb = node.getblock(block_hash, 2)["tx"][0]
        assert_equal(sum((out["value"] for out in cb["vout"]), ZERO), expected_value)
        height = node.getblock(block_hash)["height"]
        self.sync_indexes()
        info = self.block_stats(node, height)["block_info"]
        assert_equal(info["coinbase"], expected_value)
        assert_equal(info["unspendables"]["unclaimed_rewards"], ZERO)

    def full_ceiling(self, node):
        gbt = node.getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)
        assert_equal(gbt["coinbasevalue"], gbt["subsidy"] + gbt["mintvalue"])
        return gbt, Decimal(gbt["coinbasevalue"]) / COIN

    def run_test(self):
        node0, node1 = self.nodes
        vault = MiniVault(node0)

        self.log.info("The mining role reports that it claims nothing")
        for node in self.nodes:
            assert_equal(node.getminingstatus()["claims_reward"], False)

        self.log.info("Scope: with -minezeroclaim on, generatetodescriptor still claims in full")
        # These blocks also fund the vault for the transaction case below.
        for _ in range(COINBASE_MATURITY + 1):
            gbt, ceiling = self.full_ceiling(node0)
            block_hash = self.generatetodescriptor(node0, 1, vault.get_descriptor())[0]
            assert_equal(node0.getblock(block_hash)["height"], gbt["height"])
            assert_equal(self.coinbase(node0, gbt["height"])["vout"][0]["value"], ceiling)
        vault.rescan_utxos()
        self.sync_indexes()
        self.assert_full_claim(node0, node0.getbestblockhash(), ceiling)

        self.log.info("Scope: generatetoaddress and generateblock also claim in full")
        gbt, ceiling = self.full_ceiling(node0)
        block_hash = self.generatetoaddress(node0, 1, vault.get_address())[0]
        self.assert_full_claim(node0, block_hash, ceiling)
        gbt, ceiling = self.full_ceiling(node0)
        block_hash = self.generateblock(node0, vault.get_address(), [])["hash"]
        self.assert_full_claim(node0, block_hash, ceiling)
        vault.rescan_utxos()

        self.log.info("A zero-claim block mined by node0 relays to node1")
        heights = self.mine_zero_claim(node0, 1)
        assert_equal(node0.getbestblockhash(), node1.getbestblockhash())
        self.log.info("node0 mined zero-claim heights %d..%d", heights[0], heights[-1])

        self.log.info("A zero-claim block mined by node1 relays to node0")
        heights += self.mine_zero_claim(node1, 1)
        assert_equal(node0.getbestblockhash(), node1.getbestblockhash())
        self.log.info("zero-claim heights so far %d..%d", heights[0], heights[-1])

        self.log.info("Each is booked as unclaimed, not burned, and adds no output to the UTXO set")
        for height in heights:
            before, after = self.assert_zero_claim_block(height)
            assert_equal(node0.getblock(node0.getblockhash(height))["nTx"], 1)
            assert_equal(after["txouts"], before["txouts"])
            self.log.info("height=%d coinbase_outputs=%d txouts=%d->%d unclaimed_rewards=%s scripts=%s total_amount=%s",
                          height, len(self.coinbase(node0, height)["vout"]), before["txouts"], after["txouts"],
                          after["block_info"]["unspendables"]["unclaimed_rewards"],
                          after["block_info"]["unspendables"]["scripts"], after["total_amount"])

        self.log.info("No rollover: the next full-claim block claims only its own ceiling")
        gbt, ceiling = self.full_ceiling(node0)
        assert_equal(gbt["mintvalue"], 0)
        assert_equal(gbt["height"], heights[-1] + 1)
        block_hash = self.generatetodescriptor(node0, 1, vault.get_descriptor())[0]
        self.assert_full_claim(node0, block_hash, ceiling)
        assert_equal(Decimal(node0.getblockstats(gbt["height"], ["subsidy"])["subsidy"]), Decimal(gbt["subsidy"]))
        self.log.info("height=%d claimed=%s (own subsidy, mint 0)", gbt["height"], ceiling)
        vault.rescan_utxos()

        self.log.info("A long run of COINBASE_MATURITY + 10 zero-claim blocks connects on both nodes")
        run = self.mine_zero_claim(node1, COINBASE_MATURITY + 10)
        assert_greater_than(len(run), COINBASE_MATURITY + 9)
        coinbase_txids = set()
        for height in run:
            block_hash = node0.getblockhash(height)
            for node in self.nodes:
                assert_equal(node.getblockhash(height), block_hash)
                assert_greater_than(node.getblockheader(block_hash)["confirmations"], 0)
            self.assert_zero_claim_block(height)
            coinbase_txids.add(self.coinbase(node0, height)["txid"])
        assert_equal(len(coinbase_txids), len(run))
        assert_equal(node0.getbestblockhash(), node1.getbestblockhash())
        self.log.info("run heights %d..%d, %d distinct coinbase txids", run[0], run[-1], len(coinbase_txids))

        self.log.info("A zero-claim block that carries a transaction gives up its mint allowance too")
        tx = vault.send_self_transfer(from_node=node0)
        self.sync_relaypools()
        gbt, _ = self.full_ceiling(node0)
        entry = [t for t in gbt["transactions"] if t["txid"] == tx["txid"]]
        assert_equal(len(entry), 1)
        mint = Decimal(entry[0]["mintvalue"]) / COIN
        assert_greater_than(mint, ZERO)
        carried = self.mine_zero_claim(node0, 1)
        tx_height = [h for h in carried if tx["txid"] in node0.getblock(node0.getblockhash(h))["tx"]]
        assert_equal(len(tx_height), 1)
        tx_height = tx_height[0]
        for node in self.nodes:
            assert tx["txid"] in node.getblock(node.getblockhash(tx_height))["tx"]
            assert_equal(node.getrawrelaypool(), [])
        before, after = self.assert_zero_claim_block(tx_height, mint=mint)
        self.log.info("height=%d tx=%s mint=%s txouts=%d->%d unclaimed_rewards=%s coinbase=%s",
                      tx_height, tx["txid"], mint, before["txouts"], after["txouts"],
                      after["block_info"]["unspendables"]["unclaimed_rewards"], after["block_info"]["coinbase"])
        for height in carried:
            if height != tx_height:
                self.assert_zero_claim_block(height)

        self.log.info("A zero-claim block survives invalidateblock and reconsiderblock")
        # A fresh run, so the tip is a block without the transaction.
        self.mine_zero_claim(node1, 1)
        tip_height = node0.getblockcount()
        tip = node0.getbestblockhash()
        self.assert_zero_claim_block(tip_height)
        totals = [node.gettxoutsetinfo("muhash") for node in self.nodes]
        for node in self.nodes:
            node.invalidateblock(tip)
            assert_equal(node.getblockcount(), tip_height - 1)
            assert_equal(node.gettxoutsetinfo("muhash")["total_amount"], totals[0]["total_amount"])
        for node in self.nodes:
            node.reconsiderblock(tip)
            assert_equal(node.getbestblockhash(), tip)
        self.sync_indexes()
        assert_equal([node.gettxoutsetinfo("muhash") for node in self.nodes], totals)
        self.assert_zero_claim_block(tip_height)

        self.log.info("A zero-claim chain survives -reindex-chainstate")
        self.restart_node(1, extra_args=self.extra_args[1] + ["-reindex-chainstate"])
        self.connect_nodes(0, 1)
        self.sync_blocks()
        self.sync_indexes()
        reindexed = node1.gettxoutsetinfo("muhash")
        assert_equal(reindexed["muhash"], totals[0]["muhash"])
        assert_equal(reindexed, node0.gettxoutsetinfo("muhash"))
        self.assert_zero_claim_block(tip_height)

        self.log.info("Neither mining role had a block refused")
        for node in self.nodes:
            assert b"rejected reason=" not in node.debug_log_path.read_bytes()


if __name__ == '__main__':
    MiningZeroClaimTest(__file__).main()
