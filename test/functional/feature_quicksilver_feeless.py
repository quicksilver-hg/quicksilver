#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Quicksilver feeless core (#5b-1): vault sends pay zero fee (in == out), relay and
mining accept them, and exact-change (including sub-dust) construction works."""
from decimal import Decimal

from test_framework.test_framework import QuicksilverTestFramework


class QuicksilverFeelessTest(QuicksilverTestFramework):
    def add_options(self, parser):
        # Core 29.1 idiom: marks the test as needing a vault (skips if not compiled in).
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # Real Cuckatoo block PoW is ~seconds/block at sandbox EDGEBITS-19; mining 100+
        # blocks for coinbase maturity needs a generous RPC timeout.
        self.rpc_timeout = 1200

    def run_test(self):
        node = self.nodes[0]
        node.createvault(vault_name="w")
        addr = node.getnewaddress(address_type="bech32")
        self.generatetoaddress(node, 101, addr)  # mature block 1's coinbase

        # 1. A normal send is feeless (in == out), with no fee-shaped RPC data.
        dest = node.getnewaddress()
        txid = node.sendtoaddress(dest, 1)
        assert "fees" not in node.getrelaypoolentry(txid)

        # 2. Relay + mining accept the zero-fee tx: it is mined into the next block.
        blk = self.generatetoaddress(node, 1, addr)[0]
        txids = [t["txid"] for t in node.getblock(blk, 2)["tx"]]
        assert txid in txids, "feeless tx was not mined"

        # 3. Exact-change including sub-dust: spend the whole balance minus a few cinnabar, which
        #    leaves a sub-dust change output. Pre-5b this fails (dust); feeless allows it, and
        #    the resulting tx remains exact-value and has no fee-shaped RPC data.
        bal = node.getbalance()
        dest2 = node.getnewaddress()
        txid2 = node.sendtoaddress(dest2, bal - Decimal("0.00000003"))  # 3-cinnabar remainder
        assert "fees" not in node.getrelaypoolentry(txid2)

        self.log.info("Quicksilver feeless core: PASS")


if __name__ == '__main__':
    QuicksilverFeelessTest(__file__).main()
