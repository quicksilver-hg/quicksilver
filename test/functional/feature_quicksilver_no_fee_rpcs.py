#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Quicksilver feeless: removed fee/prioritization RPCs and public fee fields stay gone.

The fee machinery is deleted because Quicksilver is feeless (in == out). This
asserts the RPCs are hard-removed, not neutered, and that representative public
RPC result shapes do not preserve zero-valued fee compatibility fields.
"""
from decimal import Decimal

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.vault import MiniVault


REMOVED_FEE_KEYS = {
    "fee",
    "fees",
    "modifiedfee",
    "ancestorfees",
    "descendantfees",
    "total_fee",
}


def assert_no_fee_keys(value, path="result"):
    if isinstance(value, dict):
        for key, inner in value.items():
            assert key not in REMOVED_FEE_KEYS, f"unexpected fee field {path}.{key}"
            assert_no_fee_keys(inner, f"{path}.{key}")
    elif isinstance(value, list):
        for index, inner in enumerate(value):
            assert_no_fee_keys(inner, f"{path}[{index}]")


class QuicksilverNoFeeRpcsTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-txpownocycle=1"]]

    def run_test(self):
        node = self.nodes[0]
        node.createvault(vault_name="w")

        self.log.info("Check removed fee and prioritization RPCs are hard-absent")
        # -32601 == RPC_METHOD_NOT_FOUND.
        removed = [
            ("estimatesmartfee", [6]),
            ("estimaterawfee", [6]),
            ("prioritise" + "transaction", ["00" * 32, 0, 0]),
            ("getprioritised" + "transactions", []),
            ("bumpfee", ["00" * 32]),
            ("psqtbumpfee", ["00" * 32]),
            ("settxfee", [0]),
        ]
        for method, args in removed:
            assert_raises_rpc_error(-32601, "Method not found", node.__getattr__(method), *args)

        self.log.info("Fund node vault and MiniVault")
        vault_addr = node.getnewaddress(address_type="bech32")
        self.generatetoaddress(node, COINBASE_MATURITY + 1, vault_addr)
        minivault = MiniVault(node)
        self.generate(minivault, COINBASE_MATURITY + 1)

        self.log.info("Check vault creation RPCs omit fee fields")
        raw_tx = node.createrawtransaction([], [{node.getnewaddress(): Decimal("1")}])
        funded = node.fundrawtransaction(raw_tx)
        assert_no_fee_keys(funded, "fundrawtransaction")
        assert_equal(set(funded.keys()), {"hex", "changepos"})

        funded_psqt = node.vaultcreatefundedpsqt([], [{node.getnewaddress(): Decimal("1")}])
        assert_no_fee_keys(funded_psqt, "vaultcreatefundedpsqt")
        assert_equal(set(funded_psqt.keys()), {"psqt", "changepos"})

        self.log.info("Check relaypool/package RPCs omit fee fields")
        test_tx = minivault.create_self_transfer()
        test_accept = node.testrelaypoolaccept([test_tx["hex"]])[0]
        assert_equal(test_accept["allowed"], True)
        assert_no_fee_keys(test_accept, "testrelaypoolaccept")

        package_tx = minivault.create_self_transfer()
        package_result = node.submitpackage([package_tx["hex"]])
        assert_equal(package_result["package_msg"], "success")
        assert_no_fee_keys(package_result, "submitpackage")

        relaypool_entry = node.getrelaypoolentry(package_tx["txid"])
        assert_no_fee_keys(relaypool_entry, "getrelaypoolentry")

        self.log.info("Check getblocktemplate transaction entries omit fee fields")
        tmpl = node.getblocktemplate({"rules": ["segwit"]})
        assert_no_fee_keys(tmpl["transactions"], "getblocktemplate.transactions")
        assert package_tx["txid"] in [tx["txid"] for tx in tmpl["transactions"]]

        self.log.info("Quicksilver removed fee/prioritization RPC surface: PASS")


if __name__ == '__main__':
    QuicksilverNoFeeRpcsTest(__file__).main()
