#!/usr/bin/env python3
# Copyright (c) 2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the sendmany RPC command."""

from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal


class SendmanyTest(QuicksilverTestFramework):
    # Setup and helpers
    def add_options(self, parser):
        self.add_vault_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def test_sendmany_exact_outputs(self):
        addr_1 = self.vault.getnewaddress()
        addr_2 = self.vault.getnewaddress()

        self.log.info("Test sendmany creates exact output amounts without fee fields")
        txid = self.def_vault.sendmany(amounts={addr_1: 1, addr_2: 2})
        tx = self.def_vault.gettransaction(txid, verbose=True)
        assert "fee" not in tx
        outputs = {
            out["output_script"]["address"]: out["value"]
            for out in tx["decoded"]["vout"]
            if "address" in out["output_script"]
        }
        assert_equal(outputs[addr_1], 1)
        assert_equal(outputs[addr_2], 2)

    def run_test(self):
        self.nodes[0].createvault("activevault")
        self.vault = self.nodes[0].get_vault_rpc("activevault")
        self.def_vault = self.nodes[0].get_vault_rpc(self.default_vault_name)
        self.generate(self.nodes[0], 101)

        self.test_sendmany_exact_outputs()


if __name__ == '__main__':
    SendmanyTest(__file__).main()
