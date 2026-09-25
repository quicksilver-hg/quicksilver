#!/usr/bin/env python3
# Copyright (c) 2017-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test external signer.

Verify that a quicksilver-daemon node can use an external signer command
See also rpc_signer.py for tests without vault context.
"""
import os
import sys

from test_framework.address import QCKS_BECH32_HRP
from test_framework.authproxy import JSONRPCException
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)


class VaultSignerTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def mock_signer_path(self):
        path = os.path.join(os.path.dirname(os.path.realpath(__file__)), 'mocks', 'signer.py')
        return sys.executable + " " + path

    def mock_invalid_signer_path(self):
        path = os.path.join(os.path.dirname(os.path.realpath(__file__)), 'mocks', 'invalid_signer.py')
        return sys.executable + " " + path

    def mock_multi_signers_path(self):
        path = os.path.join(os.path.dirname(os.path.realpath(__file__)), 'mocks', 'multi_signers.py')
        return sys.executable + " " + path

    def mock_colliding_signer_path(self):
        path = os.path.join(os.path.dirname(os.path.realpath(__file__)), 'mocks', 'colliding_signer.py')
        return sys.executable + " " + path

    def set_test_params(self):
        self.num_nodes = 2

        self.extra_args = [
            [],
            [f"-signer={self.mock_signer_path()}", '-keypool=10'],
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_external_signer()
        self.skip_if_no_vault()

    def set_mock_result(self, node, res):
        with open(os.path.join(node.cwd, "mock_result"), "w", encoding="utf8") as f:
            f.write(res)

    def clear_mock_result(self, node):
        os.remove(os.path.join(node.cwd, "mock_result"))

    def run_test(self):
        self.test_valid_signer()
        self.restart_node(1, [f"-signer={self.mock_invalid_signer_path()}", "-keypool=10"])
        self.test_invalid_signer()
        self.restart_node(1, [f"-signer={self.mock_multi_signers_path()}", "-keypool=10"])
        self.test_multiple_signers()
        self.restart_node(1, [f"-signer={self.mock_colliding_signer_path()}", "-keypool=10"])
        self.test_colliding_signer()

    def test_valid_signer(self):
        self.log.debug(f"-signer={self.mock_signer_path()}")

        # Create new vaults for an external signer.
        # disable_private_keys must be true:
        assert_raises_rpc_error(-4, "Private keys must be disabled when using an external signer", self.nodes[1].createvault, vault_name='not_hww', disable_private_keys=False, external_signer=True)

        self.nodes[1].createvault(vault_name='hww', disable_private_keys=True, external_signer=True)
        hww = self.nodes[1].get_vault_rpc('hww')
        assert_equal(hww.getvaultinfo()["external_signer"], True)

        # Flag can't be set afterwards (could be added later for non-blank descriptor based tracking vaults)
        self.nodes[1].createvault(vault_name='not_hww', disable_private_keys=True, external_signer=False)
        not_hww = self.nodes[1].get_vault_rpc('not_hww')
        assert_equal(not_hww.getvaultinfo()["external_signer"], False)
        assert_raises_rpc_error(-8, "Vault flag is immutable: external_signer", not_hww.setvaultflag, "external_signer", True)

        # assert_raises_rpc_error(-4, "Multiple signers found, please specify which to use", vault_name='not_hww', disable_private_keys=True, external_signer=True)

        # TODO: Handle error thrown by script
        # self.set_mock_result(self.nodes[1], "2")
        # assert_raises_rpc_error(-1, 'Unable to parse JSON',
        #     self.nodes[1].createvault, vault_name='not_hww2', disable_private_keys=True, external_signer=False
        # )
        # self.clear_mock_result(self.nodes[1])

        assert_equal(hww.getvaultinfo()["keypoolsize"], 30)

        address1 = hww.getnewaddress(address_type="bech32")
        assert address1.startswith(f"{QCKS_BECH32_HRP}1q")
        address_info = hww.getaddressinfo(address1)
        assert_equal(address_info['solvable'], True)
        assert_equal(address_info['ismine'], True)
        assert_equal(address_info['hdkeypath'], "m/84h/1h/0h/0/0")

        address3 = hww.getnewaddress(address_type="base58")
        address_info = hww.getaddressinfo(address3)
        assert_equal(address_info['solvable'], True)
        assert_equal(address_info['ismine'], True)
        assert_equal(address_info['hdkeypath'], "m/44h/1h/0h/0/0")

        address4 = hww.getnewaddress(address_type="bech32m")
        assert address4.startswith(f"{QCKS_BECH32_HRP}1p")
        address_info = hww.getaddressinfo(address4)
        assert_equal(address_info['solvable'], True)
        assert_equal(address_info['ismine'], True)
        assert_equal(address_info['hdkeypath'], "m/86h/1h/0h/0/0")

        self.log.info('Test vaultdisplayaddress')
        for address in [address1, address3]:
            result = hww.vaultdisplayaddress(address)
            assert_equal(result, {"address": address})

        # Handle error thrown by script
        self.set_mock_result(self.nodes[1], "2")
        assert_raises_rpc_error(-1, 'RunCommandParseJSON error',
            hww.vaultdisplayaddress, address1
        )
        self.clear_mock_result(self.nodes[1])

        # Returned address MUST match:
        address_fail = hww.getnewaddress(address_type="bech32")
        assert address_fail.startswith(f"{QCKS_BECH32_HRP}1q")
        assert_raises_rpc_error(-1, 'Signer echoed unexpected address wrong_address',
            hww.vaultdisplayaddress, address_fail
        )

        self.log.info('Prepare mock PSQT')
        self.nodes[0].sendtoaddress(address4, 1)
        self.generate(self.nodes[0], 1)

        # Load private key into vault to generate a signed PSQT for the mock
        self.nodes[1].createvault(vault_name="mock", disable_private_keys=False, blank=True)
        mock_vault = self.nodes[1].get_vault_rpc("mock")
        assert mock_vault.getvaultinfo()['private_keys_enabled']

        result = mock_vault.importdescriptors([{
            "desc": "tr([00000001/86h/1h/0']sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/0/*)#2xaxcmz8",
            "timestamp": 0,
            "range": [0,1],
            "internal": False,
            "active": True
        },
        {
            "desc": "tr([00000001/86h/1h/0']sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/1/*)#mjc89wjl",
            "timestamp": 0,
            "range": [0, 0],
            "internal": True,
            "active": True
        }])
        assert_equal(result[0], {'success': True})
        assert_equal(result[1], {'success': True})
        assert_equal(mock_vault.getvaultinfo()["txcount"], 1)
        dest = self.nodes[0].getnewaddress(address_type='bech32')
        mock_psqt = mock_vault.vaultcreatefundedpsqt([], [{dest:0.5}], 0, {}, True)['psqt']
        mock_psqt_signed = mock_vault.vaultprocesspsqt(psqt=mock_psqt, sign=True, sighashtype="ALL", bip32derivs=True)
        mock_tx = mock_psqt_signed["hex"]
        assert mock_vault.testrelaypoolaccept([mock_tx])[0]["allowed"]

        # # Create a new vault and populate with specific public keys, in order
        # # to work with the mock signed PSQT.
        # self.nodes[1].createvault(vault_name="hww4", disable_private_keys=True, external_signer=True)
        # hww4 = self.nodes[1].get_vault_rpc("hww4")
        #
        # descriptors = [{
        #     "desc": "wpkh([00000001/84h/1h/0']squb6UShJgvLuWbXhZQZ1fML1Lg1yCsWwqoXY4vG1H3hFXrcUJ3kMeYf22JbXHi8iWoZQ8d1QbA5SBPxQw68h2mUNLdXNVQ4F5kgSzf8F3ox7o6/0/*)#nk5qxdxk",
        #     "timestamp": "now",
        #     "range": [0, 1],
        #     "internal": False,
        #     "active": True
        # },
        # {
        #     "desc": "wpkh([00000001/84h/1h/0']squb6UShJgvLuWbXhZQZ1fML1Lg1yCsWwqoXY4vG1H3hFXrcUJ3kMeYf22JbXHi8iWoZQ8d1QbA5SBPxQw68h2mUNLdXNVQ4F5kgSzf8F3ox7o6/1/*)#zz3pmckw",
        #     "timestamp": "now",
        #     "range": [0, 0],
        #     "internal": True,
        #     "active": True
        # }]

        # result = hww4.importdescriptors(descriptors)
        # assert_equal(result[0], {'success': True})
        # assert_equal(result[1], {'success': True})
        assert_equal(hww.getvaultinfo()["txcount"], 1)

        assert hww.testrelaypoolaccept([mock_tx])[0]["allowed"]

        with open(os.path.join(self.nodes[1].cwd, "mock_psqt"), "w", encoding="utf8") as f:
            f.write(mock_psqt_signed["psqt"])

        self.log.info('Test send using hww1')

        # Don't broadcast transaction yet so the RPC returns the raw hex
        res = hww.send(outputs=[{dest:0.5}],add_to_vault=False)
        assert res["complete"]
        assert_equal(res["hex"], mock_tx)

        self.log.info('Test sendall using hww1')

        res = hww.sendall(recipients=[{dest:0.5}, hww.getrawchangeaddress()], add_to_vault=False)
        assert res["complete"]
        assert_equal(res["hex"], mock_tx)
        # Broadcast the transaction
        hww.sendrawtransaction(res["hex"])

        # # Handle error thrown by script
        # self.set_mock_result(self.nodes[4], "2")
        # assert_raises_rpc_error(-1, 'Unable to parse JSON',
        #     hww4.signerprocesspsqt, psqt_orig, "00000001"
        # )
        # self.clear_mock_result(self.nodes[4])

    def test_invalid_signer(self):
        self.log.debug(f"-signer={self.mock_invalid_signer_path()}")
        self.log.info('Test invalid external signer')
        assert_raises_rpc_error(-1, "Invalid descriptor", self.nodes[1].createvault, vault_name='hww_invalid', disable_private_keys=True, external_signer=True)

    def test_multiple_signers(self):
        self.log.debug(f"-signer={self.mock_multi_signers_path()}")
        self.log.info('Test multiple external signers')

        assert_raises_rpc_error(-1, "GetExternalSigner: More than one external signer found", self.nodes[1].createvault, vault_name='multi_hww', disable_private_keys=True, external_signer=True)

    def test_colliding_signer(self):
        self.log.debug(f"-signer={self.mock_colliding_signer_path()}")
        self.log.info('Test colliding pkh + sh(pkh) descriptors are refused')

        try:
            self.nodes[1].createvault(vault_name='hww_collide', disable_private_keys=True, external_signer=True)
            raise AssertionError('createvault succeeded with colliding BASE58 descriptors')
        except JSONRPCException as e:
            assert_equal(e.error['code'], -1)
            msg = e.error['message']
            if 'Multiple descriptors for output type base58' not in msg:
                raise AssertionError(f'unexpected error: {msg}') from e
            if 'pkh(' not in msg or 'sh(pkh(' not in msg:
                raise AssertionError(f'error did not name both descriptors: {msg}') from e
            if '(receive)' not in msg:
                raise AssertionError(f'error did not name the chain: {msg}') from e

        assert 'hww_collide' not in self.nodes[1].listvaults()

if __name__ == '__main__':
    VaultSignerTest(__file__).main()
