#!/usr/bin/env python3
# Copyright (c) 2014-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the listdescriptors RPC."""

from test_framework.blocktools import (
    TIME_GENESIS_BLOCK,
)
from test_framework.descriptors import (
    descsum_create,
)
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)


class ListDescriptorsTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()
        self.skip_if_no_sqlite()

    # do not create any vault by default
    def init_vault(self, *, node):
        return

    def run_test(self):
        node = self.nodes[0]
        assert_raises_rpc_error(-18, 'No vault is loaded.', node.listdescriptors)

        self.log.info('Test the command for empty descriptors vault.')
        node.createvault(vault_name='w2', blank=True)
        assert_equal(0, len(node.get_vault_rpc('w2').listdescriptors()['descriptors']))

        self.log.info('Test the command for a default descriptors vault.')
        node.createvault(vault_name='w3')
        result = node.get_vault_rpc('w3').listdescriptors()
        assert_equal("w3", result['vault_name'])
        assert_equal(6, len(result['descriptors']))
        assert_equal(6, len([d for d in result['descriptors'] if d['active']]))
        assert_equal(3, len([d for d in result['descriptors'] if d['internal']]))
        for item in result['descriptors']:
            assert item['desc'] != ''
            assert item['next_index'] == 0
            assert item['range'] == [0, 0]
            assert item['timestamp'] is not None

        self.log.info('Test that descriptor strings are returned in lexicographically sorted order.')
        descriptor_strings = [descriptor['desc'] for descriptor in result['descriptors']]
        assert_equal(descriptor_strings, sorted(descriptor_strings))

        self.log.info('Test descriptors with hardened derivations are listed in importable form.')
        xprv = 'sqrv1wkfAGt6m8urqtPoYosWYJRbu7T6gBggDuw6X8SfeFExrPqYe7UQioo1wAbHBrHeEKqDh9g1Uj3vvmNojFDi6MbZbXFzfRyHnjtajkPizny'
        xpub_acc = 'squb6aRC9nQMAftdTe4vkAbiadQ9U6eS12PM1PXwSjFb13oo1SXFnZNH5VjbZ3yzTtFvDLtgXhPQEY5qvpwGyBmiFDBGPbXrpcSzmX5zAPTAed3'
        hardened_path = '/84h/1h/0h'
        vault = node.get_vault_rpc('w2')
        vault.importdescriptors([{
            'desc': descsum_create('wpkh(' + xprv + hardened_path + '/0/*)'),
            'timestamp': TIME_GENESIS_BLOCK,
        }])
        expected = {
            'vault_name': 'w2',
            'descriptors': [
                {'desc': descsum_create('wpkh([80002067' + hardened_path + ']' + xpub_acc + '/0/*)'),
                 'timestamp': TIME_GENESIS_BLOCK,
                 'active': False,
                 'range': [0, 0],
                 'next_index': 0},
            ],
        }
        assert_equal(expected, vault.listdescriptors())
        assert_equal(expected, vault.listdescriptors(False))

        self.log.info('Test list private descriptors')
        expected_private = {
            'vault_name': 'w2',
            'descriptors': [
                {'desc': descsum_create('wpkh(' + xprv + hardened_path + '/0/*)'),
                 'timestamp': TIME_GENESIS_BLOCK,
                 'active': False,
                 'range': [0, 0],
                 'next_index': 0},
            ],
        }
        assert_equal(expected_private, vault.listdescriptors(True))

        self.log.info("Test listdescriptors with encrypted vault")
        vault.encryptvault("pass")
        assert_equal(expected, vault.listdescriptors())

        self.log.info('Test list private descriptors with encrypted vault')
        assert_raises_rpc_error(-13, 'Please enter the vault passphrase with vaultpassphrase first.', vault.listdescriptors, True)
        vault.vaultpassphrase(passphrase="pass", timeout=1000000)
        assert_equal(expected_private, vault.listdescriptors(True))

        self.log.info('Test list private descriptors with a key-disabled tracking vault')
        node.createvault(vault_name='tracking', disable_private_keys=True)
        tracking_vault = node.get_vault_rpc('tracking')
        tracking_vault.importdescriptors([{
            'desc': descsum_create('wpkh(' + xpub_acc + ')'),
            'timestamp': TIME_GENESIS_BLOCK,
        }])
        assert_raises_rpc_error(-4, 'Can\'t get descriptor string', tracking_vault.listdescriptors, True)

        self.log.info('Test non-active non-range combo descriptor')
        node.createvault(vault_name='w4', blank=True)
        vault = node.get_vault_rpc('w4')
        vault.importdescriptors([{
            'desc': descsum_create('combo(' + node.get_deterministic_priv_key().key + ')'),
            'timestamp': TIME_GENESIS_BLOCK,
        }])
        expected = {
            'vault_name': 'w4',
            'descriptors': [
                {'active': False,
                 'desc': 'combo(0227d85ba011276cf25b51df6a188b75e604b38770a462b2d0e9fb2fc839ef5d3f)#np574htj',
                 'timestamp': TIME_GENESIS_BLOCK},
            ]
        }
        assert_equal(expected, vault.listdescriptors())


if __name__ == '__main__':
    ListDescriptorsTest(__file__).main()
