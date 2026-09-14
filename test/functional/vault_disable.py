#!/usr/bin/env python3
# Copyright (c) 2015-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test a node with the -disablevault option.

- Test that validateaddress RPC works when running with -disablevault
- Test that it is not possible to mine to an invalid address.
"""

from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_raises_rpc_error

class DisableVaultTest (QuicksilverTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [["-disablevault"]]
        self.vault_names = []

    def run_test (self):
        # Make sure vault is really disabled
        assert_raises_rpc_error(-32601, 'Method not found', self.nodes[0].getvaultinfo)
        x = self.nodes[0].validateaddress('2N3oefVeg6stiTb5Kh3ozCSkaqmx91FDbsm')
        assert x['isvalid'] == False
        x = self.nodes[0].validateaddress('SXyGazfm6S3xfcySmD6QNkZYmtfysC2jvc')
        assert x['isvalid'] == True


if __name__ == '__main__':
    DisableVaultTest(__file__).main()
