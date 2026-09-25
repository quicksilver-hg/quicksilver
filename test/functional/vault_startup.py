#!/usr/bin/env python3
# Copyright (c) 2017-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test vault load on startup.

Verify that a quicksilver-daemon node can maintain the list of vaults loading on
startup. An unnamed vault is not auto-loaded; it must be on the startup list.
"""
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal,
)


class VaultStartupTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.supports_cli = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()

    def setup_nodes(self):
        self.add_nodes(self.num_nodes)
        self.start_nodes()

    def run_test(self):
        self.log.info('Should start without any vaults')
        assert_equal(self.nodes[0].listvaults(), [])
        assert_equal(self.nodes[0].listvaultdir(), {'vaults': []})

        self.log.info('Unnamed vault with load_on_startup=False is created but does not auto-load')
        self.nodes[0].createvault(vault_name='', load_on_startup=False)
        assert_equal(self.nodes[0].listvaults(), [''])
        self.restart_node(0)
        assert_equal(self.nodes[0].listvaults(), [])
        assert_equal(self.nodes[0].listvaultdir(), {'vaults': [{'name': ''}]})

        self.log.info('Test load on startup behavior')
        self.nodes[0].loadvault(filename='', load_on_startup=True)
        self.nodes[0].createvault(vault_name='w0', load_on_startup=True)
        self.nodes[0].createvault(vault_name='w1', load_on_startup=False)
        self.nodes[0].createvault(vault_name='w2', load_on_startup=True)
        self.nodes[0].createvault(vault_name='w3', load_on_startup=False)
        self.nodes[0].createvault(vault_name='w4', load_on_startup=False)
        self.nodes[0].unloadvault(vault_name='w0', load_on_startup=False)
        self.nodes[0].unloadvault(vault_name='w4', load_on_startup=False)
        self.nodes[0].loadvault(filename='w4', load_on_startup=True)
        assert_equal(set(self.nodes[0].listvaults()), set(('', 'w1', 'w2', 'w3', 'w4')))
        self.restart_node(0)
        assert_equal(set(self.nodes[0].listvaults()), set(('', 'w2', 'w4')))
        self.nodes[0].unloadvault(vault_name='', load_on_startup=False)
        self.nodes[0].unloadvault(vault_name='w4', load_on_startup=False)
        self.nodes[0].loadvault(filename='w3', load_on_startup=True)
        self.nodes[0].loadvault(filename='')
        self.restart_node(0)
        assert_equal(set(self.nodes[0].listvaults()), set(('w2', 'w3')))

if __name__ == '__main__':
    VaultStartupTest(__file__).main()
