#!/usr/bin/env python3
# Copyright (c) 2023 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test vault createvaultdescriptor RPC."""

from test_framework.descriptors import descsum_create
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)
from test_framework.vault_util import VaultUnlock


class VaultCreateDescriptorTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()

    def run_test(self):
        self.test_basic()
        self.test_imported_other_keys()
        self.test_encrypted()

    def test_basic(self):
        def_vault = self.nodes[0].get_vault_rpc(self.default_vault_name)
        self.nodes[0].createvault("blank", blank=True)
        vault = self.nodes[0].get_vault_rpc("blank")

        xpub_info = def_vault.gethdkeys(private=True)
        xpub = xpub_info[0]["qpub"]
        xprv = xpub_info[0]["qprv"]
        expected_descs = []
        for desc in def_vault.listdescriptors()["descriptors"]:
            if desc["desc"].startswith("wpkh("):
                expected_descs.append(desc["desc"])

        assert_raises_rpc_error(-5, "Unable to determine which HD key to use from active descriptors. Please specify with 'hdkey'", vault.createvaultdescriptor, "bech32")
        assert_raises_rpc_error(-5, f"Private key for {xpub} is not known", vault.createvaultdescriptor, type="bech32", hdkey=xpub)

        self.log.info("Test createvaultdescriptor after importing active descriptor to blank vault")
        # Import one active descriptor
        assert_equal(vault.importdescriptors([{"desc": descsum_create(f"pkh({xprv}/44h/2h/0h/0/0/*)"), "timestamp": "now", "active": True}])[0]["success"], True)
        assert_equal(len(vault.listdescriptors()["descriptors"]), 1)
        assert_equal(len(vault.gethdkeys()), 1)

        new_descs = vault.createvaultdescriptor("bech32")["descs"]
        assert_equal(len(new_descs), 2)
        assert_equal(len(vault.gethdkeys()), 1)
        assert_equal(new_descs, expected_descs)

        self.log.info("Test descriptor creation options")
        old_descs = set([(d["desc"], d["active"], d["internal"]) for d in vault.listdescriptors(private=True)["descriptors"]])
        vault.createvaultdescriptor(type="bech32m", internal=False)
        curr_descs = set([(d["desc"], d["active"], d["internal"]) for d in vault.listdescriptors(private=True)["descriptors"]])
        new_descs = list(curr_descs - old_descs)
        assert_equal(len(new_descs), 1)
        assert_equal(len(vault.gethdkeys()), 1)
        assert_equal(new_descs[0][0], descsum_create(f"tr({xprv}/86h/1h/0h/0/*)"))
        assert_equal(new_descs[0][1], True)
        assert_equal(new_descs[0][2], False)

        old_descs = curr_descs
        vault.createvaultdescriptor(type="bech32m", internal=True)
        curr_descs = set([(d["desc"], d["active"], d["internal"]) for d in vault.listdescriptors(private=True)["descriptors"]])
        new_descs = list(curr_descs - old_descs)
        assert_equal(len(new_descs), 1)
        assert_equal(len(vault.gethdkeys()), 1)
        assert_equal(new_descs[0][0], descsum_create(f"tr({xprv}/86h/1h/0h/1/*)"))
        assert_equal(new_descs[0][1], True)
        assert_equal(new_descs[0][2], True)

    def test_imported_other_keys(self):
        self.log.info("Test createvaultdescriptor with multiple keys in active descriptors")
        def_vault = self.nodes[0].get_vault_rpc(self.default_vault_name)
        self.nodes[0].createvault("multiple_keys")
        vault = self.nodes[0].get_vault_rpc("multiple_keys")

        vault_xpub = vault.gethdkeys()[0]["qpub"]

        xpub_info = def_vault.gethdkeys(private=True)
        xpub = xpub_info[0]["qpub"]
        xprv = xpub_info[0]["qprv"]

        assert_equal(vault.importdescriptors([{"desc": descsum_create(f"wpkh({xprv}/0/0/*)"), "timestamp": "now", "active": True}])[0]["success"], True)
        assert_equal(len(vault.gethdkeys()), 2)

        assert_raises_rpc_error(-5, "Unable to determine which HD key to use from active descriptors. Please specify with 'hdkey'", vault.createvaultdescriptor, "bech32")
        assert_raises_rpc_error(-4, "Descriptor already exists", vault.createvaultdescriptor, type="bech32m", hdkey=vault_xpub)
        assert_raises_rpc_error(-5, "Unable to parse HD key. Please provide a valid qpub", vault.createvaultdescriptor, type="bech32m", hdkey=xprv)

        # Able to replace tr() descriptor with other hd key
        vault.createvaultdescriptor(type="bech32m", hdkey=xpub)

    def test_encrypted(self):
        self.log.info("Test createvaultdescriptor with encrypted vaults")
        def_vault = self.nodes[0].get_vault_rpc(self.default_vault_name)
        self.nodes[0].createvault("encrypted", blank=True, passphrase="pass")
        vault = self.nodes[0].get_vault_rpc("encrypted")

        xpub_info = def_vault.gethdkeys(private=True)
        xprv = xpub_info[0]["qprv"]

        with VaultUnlock(vault, "pass"):
            assert_equal(vault.importdescriptors([{"desc": descsum_create(f"wpkh({xprv}/0/0/*)"), "timestamp": "now", "active": True}])[0]["success"], True)
        assert_equal(len(vault.gethdkeys()), 1)

        assert_raises_rpc_error(-13, "Error: Please enter the vault passphrase with vaultpassphrase first.", vault.createvaultdescriptor, type="bech32m")

        with VaultUnlock(vault, "pass"):
            vault.createvaultdescriptor(type="bech32m")



if __name__ == '__main__':
    VaultCreateDescriptorTest(__file__).main()
