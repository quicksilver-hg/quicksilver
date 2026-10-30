#!/usr/bin/env python3
# Copyright (c) 2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal


class VaultLocktimeTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()

    def run_test(self):
        node = self.nodes[0]

        mtp_tip = node.getblockheader(node.getbestblockhash())["mediantime"]

        self.log.info("Get new address with label")
        label = "timelock⌛🔓"
        address = node.getnewaddress(label=label)

        self.log.info("Send to new address with locktime")
        node.send(
            outputs=[{address: 5}],
            locktime=mtp_tip - 1,
        )
        self.generate(node, 1)

        self.log.info("Check that clock cannot change finality of confirmed txs")
        amount_before_ad = node.getreceivedbyaddress(address)
        amount_before_lb = node.getreceivedbylabel(label)
        list_before_ad = node.listreceivedbyaddress(address_filter=address)
        list_before_lb = node.listreceivedbylabel(include_empty=False)
        balance_before = node.getbalances()["mine"]["trusted"]
        coin_before = node.listunspent(maxconf=1)
        node.setmocktime(mtp_tip - 1)
        assert_equal(node.getreceivedbyaddress(address), amount_before_ad)
        assert_equal(node.getreceivedbylabel(label), amount_before_lb)
        assert_equal(node.listreceivedbyaddress(address_filter=address), list_before_ad)
        assert_equal(node.listreceivedbylabel(include_empty=False), list_before_lb)
        assert_equal(node.getbalances()["mine"]["trusted"], balance_before)
        assert_equal(node.listunspent(maxconf=1), coin_before)

        self.log.info("An accepted send carries no not-accepted warning")
        accepted = node.send(outputs=[{address: 1}])
        assert "warnings" not in accepted
        assert accepted["txid"] in node.getrawrelaypool()

        self.log.info("A send the node refuses as non-final is kept, and says so")
        kept = node.send(outputs=[{address: 1}], locktime=node.getblockcount() + 100)
        assert_equal(kept["warnings"], [
            "Not accepted by this node yet (non-final). "
            "The vault keeps the transaction and will resubmit it automatically."
        ])
        assert kept["txid"] not in node.getrawrelaypool()
        assert kept["txid"] in [tx["txid"] for tx in node.listtransactions(count=100)]


if __name__ == "__main__":
    VaultLocktimeTest(__file__).main()
