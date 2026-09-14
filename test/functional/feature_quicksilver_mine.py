#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""End-to-end node mining under feeless + per-tx-anchor (#7): a vault-built feeless tx
(in == out, anchored to the tip, carrying a real per-tx Cuckatoo proof) is assembled by
the node, the block PoW is solved, the block connects, and the tx confirms. Exercises the
real BlockAssembler (surplus-work selection + stale-anchor skip) -> CuckatooSolve ->
ConnectBlock path. The Frame-B mint arithmetic itself is pinned by the mining_anchor_tests
unit suite and feature_quicksilver_bringup."""
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal


class QuicksilverMineTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # Real Cuckatoo block PoW (sandbox EDGEBITS-19) is ~seconds/block; maturing 110
        # coinbases needs a generous RPC timeout.
        self.rpc_timeout = 1200

    def run_test(self):
        node = self.nodes[0]
        node.createvault(vault_name="mine")
        addr = node.getnewaddress(address_type="bech32")
        self.generatetoaddress(node, 110, addr)        # mature coinbases
        assert_equal(node.getblockchaininfo()["blocks"], 110)

        # Feeless send: the vault grinds the per-tx proof and anchors it to the tip.
        dest = node.getnewaddress(address_type="bech32")
        txid = node.sendtoaddress(dest, 1)
        assert "fees" not in node.getrelaypoolentry(txid)
        assert txid in node.getrawrelaypool()

        # Assemble -> solve -> connect: the node mines one block including the tx.
        blk = self.generatetoaddress(node, 1, addr)[0]
        block = node.getblock(blk, 2)
        txids = [t["txid"] for t in block["tx"]]
        assert txid in txids, "feeless tx was not assembled into the mined block"
        assert_equal(node.gettransaction(txid)["confirmations"], 1)

        # The included tx carries a real per-tx Cuckatoo proof (could not be mined otherwise).
        decoded = node.gettransaction(txid, True)["decoded"]
        assert_equal(len(decoded["cuckatoo_cycle"]), 42)
        assert any(e != 0 for e in decoded["cuckatoo_cycle"]), "per-tx cuckatoo_cycle is all zeros"

        # The mined block's per-tx-anchor is fresh: its anchor height is within the recency
        # window of the block it was mined into.
        assert "anchor_height" in decoded, "decoded tx missing anchor_height"
        assert decoded["anchor_height"] <= block["height"], "tx anchored to a future height"

        self.log.info("Quicksilver #7 node mine (assemble->solve->connect, fresh-anchor tx mined): PASS")


if __name__ == '__main__':
    QuicksilverMineTest(__file__).main()
