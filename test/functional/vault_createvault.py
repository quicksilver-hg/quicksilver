#!/usr/bin/env python3
# Copyright (c) 2018-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test createvault arguments.
"""

from test_framework.descriptors import descsum_create
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)
from test_framework.vault_util import generate_keypair, VaultUnlock


EMPTY_PASSPHRASE_MSG = "Empty string given as passphrase, vault will not be encrypted."


class CreateVaultTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 1)

        self.log.info("Run createvault with invalid parameters.")
        # Run createvault with invalid parameters. This must not prevent a new vault with the same name from being created with the correct parameters.
        assert_raises_rpc_error(-4, "Passphrase provided but private keys are disabled. A passphrase is only used to encrypt private keys, so cannot be used for vaults with private keys disabled.",
            self.nodes[0].createvault, vault_name='w0', disable_private_keys=True, passphrase="passphrase")

        self.nodes[0].createvault(vault_name='w0')

        self.log.info("Test disableprivatekeys creation.")
        self.nodes[0].createvault(vault_name='w1', disable_private_keys=True)
        w1 = node.get_vault_rpc('w1')
        assert_raises_rpc_error(-4, "Error: This vault has no available keys", w1.getnewaddress)
        assert_raises_rpc_error(-4, "Error: This vault has no available keys", w1.getrawchangeaddress)

        self.log.info('Test that private keys cannot be imported')
        privkey, _ = generate_keypair(wif=True)
        result = w1.importdescriptors([{'desc': descsum_create('wpkh(' + privkey + ')'), 'timestamp': 'now'}])
        assert not result[0]['success']
        assert 'warnings' not in result[0]
        assert_equal(result[0]['error']['code'], -4)
        assert_equal(result[0]['error']['message'], 'Cannot import private keys to a vault with private keys disabled')

        self.log.info("Test blank creation with private keys disabled.")
        self.nodes[0].createvault(vault_name='w2', disable_private_keys=True, blank=True)
        w2 = node.get_vault_rpc('w2')
        assert_raises_rpc_error(-4, "Error: This vault has no available keys", w2.getnewaddress)
        assert_raises_rpc_error(-4, "Error: This vault has no available keys", w2.getrawchangeaddress)
        self.log.info("Test blank creation with private keys enabled.")
        self.nodes[0].createvault(vault_name='w3', disable_private_keys=False, blank=True)
        w3 = node.get_vault_rpc('w3')
        assert_equal(w3.getvaultinfo()['keypoolsize'], 0)
        assert_raises_rpc_error(-4, "Error: This vault has no available keys", w3.getnewaddress)
        assert_raises_rpc_error(-4, "Error: This vault has no available keys", w3.getrawchangeaddress)
        w3.importdescriptors([{
            'desc': descsum_create('wpkh(sqrv1wkfAGt6m8urovofJByY43b2Sty7KkgcBGByPrtBKuHCJ7skQdygqN8fsmfLtHTzRsxNjLLZVYqJnq9gqjwQ7agCtXdTe2i8bfwndnYTmz6/0h/*)'),
            'timestamp': 'now',
            'active': True
        },
        {
            'desc': descsum_create('wpkh(sqrv1wkfAGt6m8urovofJByY43b2Sty7KkgcBGByPrtBKuHCJ7skQdygqN8fsmfLtHTzRsxNjLLZVYqJnq9gqjwQ7agCtXdTe2i8bfwndnYTmz6/1h/*)'),
            'timestamp': 'now',
            'active': True,
            'internal': True
        }])
        assert_equal(w3.getvaultinfo()['keypoolsize'], 1)
        w3.getnewaddress()
        w3.getrawchangeaddress()

        self.log.info("Test blank creation with privkeys enabled and then encryption")
        self.nodes[0].createvault(vault_name='w4', disable_private_keys=False, blank=True)
        w4 = node.get_vault_rpc('w4')
        assert_equal(w4.getvaultinfo()['keypoolsize'], 0)
        assert_raises_rpc_error(-4, "Error: This vault has no available keys", w4.getnewaddress)
        assert_raises_rpc_error(-4, "Error: This vault has no available keys", w4.getrawchangeaddress)
        # Encrypt the vault. Nothing should change about the keypool
        w4.encryptvault('pass')
        assert_raises_rpc_error(-4, "Error: This vault has no available keys", w4.getnewaddress)
        assert_raises_rpc_error(-4, "Error: This vault has no available keys", w4.getrawchangeaddress)
        with VaultUnlock(w4, "pass"):
            # Now set a seed and it should work. Vault should also be encrypted
            w4.importdescriptors([{
                'desc': descsum_create('wpkh(sqrv1wkfAGt6m8urovofJByY43b2Sty7KkgcBGByPrtBKuHCJ7skQdygqN8fsmfLtHTzRsxNjLLZVYqJnq9gqjwQ7agCtXdTe2i8bfwndnYTmz6/0h/*)'),
                'timestamp': 'now',
                'active': True
            },
            {
                'desc': descsum_create('wpkh(sqrv1wkfAGt6m8urovofJByY43b2Sty7KkgcBGByPrtBKuHCJ7skQdygqN8fsmfLtHTzRsxNjLLZVYqJnq9gqjwQ7agCtXdTe2i8bfwndnYTmz6/1h/*)'),
                'timestamp': 'now',
                'active': True,
                'internal': True
            }])
            w4.getnewaddress()
            w4.getrawchangeaddress()

        self.log.info("Test blank creation with privkeys disabled and then encryption")
        self.nodes[0].createvault(vault_name='w5', disable_private_keys=True, blank=True)
        w5 = node.get_vault_rpc('w5')
        assert_equal(w5.getvaultinfo()['keypoolsize'], 0)
        assert_raises_rpc_error(-4, "Error: This vault has no available keys", w5.getnewaddress)
        assert_raises_rpc_error(-4, "Error: This vault has no available keys", w5.getrawchangeaddress)
        # Encrypt the vault
        assert_raises_rpc_error(-16, "Error: vault does not contain private keys, nothing to encrypt.", w5.encryptvault, 'pass')
        assert_raises_rpc_error(-4, "Error: This vault has no available keys", w5.getnewaddress)
        assert_raises_rpc_error(-4, "Error: This vault has no available keys", w5.getrawchangeaddress)

        self.log.info('New blank and encrypted vaults can be created')
        self.nodes[0].createvault(vault_name='wblank', disable_private_keys=False, blank=True, passphrase='thisisapassphrase')
        wblank = node.get_vault_rpc('wblank')
        with VaultUnlock(wblank, "thisisapassphrase"):
            assert_raises_rpc_error(-4, "Error: This vault has no available keys", wblank.getnewaddress)
            assert_raises_rpc_error(-4, "Error: This vault has no available keys", wblank.getrawchangeaddress)

        self.log.info('Test creating a new encrypted vault.')
        # Born encrypted vault is created (has keys)
        self.nodes[0].createvault(vault_name='w6', disable_private_keys=False, blank=False, passphrase='thisisapassphrase')
        w6 = node.get_vault_rpc('w6')
        with VaultUnlock(w6, "thisisapassphrase"):
            w6.keypoolrefill(1)
            vaultinfo = w6.getvaultinfo()
            keys = 3
            assert_equal(vaultinfo['keypoolsize'], keys)
            assert_equal(vaultinfo['keypoolsize_hd_internal'], keys)
        # Allow empty passphrase, but there should be a warning
        resp = self.nodes[0].createvault(vault_name='w7', disable_private_keys=False, blank=False, passphrase='')
        assert_equal(resp["warnings"], [EMPTY_PASSPHRASE_MSG])

        w7 = node.get_vault_rpc('w7')
        assert_raises_rpc_error(-15, 'Error: running with an unencrypted vault, but vaultpassphrase was called.', w7.vaultpassphrase, '', 60)

        self.log.info('Test making a vault with avoid reuse flag')
        self.nodes[0].createvault('w8', False, False, '', True) # Use positional arguments to check for bug where avoid_reuse could not be set for vaults without needing them to be encrypted
        w8 = node.get_vault_rpc('w8')
        assert_raises_rpc_error(-15, 'Error: running with an unencrypted vault, but vaultpassphrase was called.', w7.vaultpassphrase, '', 60)
        assert_equal(w8.getvaultinfo()["avoid_reuse"], True)

        self.log.info('Using a passphrase with private keys disabled returns error')
        assert_raises_rpc_error(-4, 'Passphrase provided but private keys are disabled. A passphrase is only used to encrypt private keys, so cannot be used for vaults with private keys disabled.', self.nodes[0].createvault, vault_name='w9', disable_private_keys=True, passphrase='thisisapassphrase')

        self.log.info("Check that the version number is being logged correctly")
        with node.assert_debug_log(expected_msgs=[], unexpected_msgs=["loaded last_client_version=", "loaded vault_file_version="]):
            node.createvault("version_check")
        vault = node.get_vault_rpc("version_check")
        vault_version = vault.getvaultinfo()["vaultversion"]
        client_version = node.getnetworkinfo()["version"]
        vault.unloadvault()
        with node.assert_debug_log(
            expected_msgs=[f"loaded last_client_version={client_version}", f"loaded vault_file_version={vault_version}"],
            unexpected_msgs=["loaded vault_file_version=10500"]
        ):
            node.loadvault("version_check")


if __name__ == '__main__':
    CreateVaultTest(__file__).main()
