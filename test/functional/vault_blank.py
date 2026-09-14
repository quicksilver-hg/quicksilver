#!/usr/bin/env python3
# Copyright (c) 2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php.


from test_framework.test_framework import QuicksilverTestFramework
from test_framework.address import (
    ADDRESS_SHG1_UNSPENDABLE_DESCRIPTOR,
)
from test_framework.util import (
    assert_equal,
)


class VaultBlankTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()

    def add_options(self, options):
        self.add_vault_options(options)

    def test_importdescriptors(self):
        self.log.info("Test that importdescriptors preserves the blank flag")
        self.nodes[0].createvault(vault_name="idesc", disable_private_keys=True, blank=True)
        vault = self.nodes[0].get_vault_rpc("idesc")
        info = vault.getvaultinfo()
        assert "descriptors" not in info
        assert_equal(info["blank"], True)
        vault.importdescriptors([{
            "desc": ADDRESS_SHG1_UNSPENDABLE_DESCRIPTOR,
            "timestamp": "now",
        }])
        assert_equal(vault.getvaultinfo()["blank"], True)

    def test_encrypt_descriptors(self):
        self.log.info("Test that encrypting a blank vault preserves the blank flag and descriptors remain the same")
        self.nodes[0].createvault(vault_name="encblankdesc", blank=True)
        vault = self.nodes[0].get_vault_rpc("encblankdesc")

        info = vault.getvaultinfo()
        assert "descriptors" not in info
        assert_equal(info["blank"], True)
        descs = vault.listdescriptors()

        vault.encryptvault("pass")
        assert_equal(vault.getvaultinfo()["blank"], True)
        assert_equal(descs, vault.listdescriptors())

    def run_test(self):
        self.test_importdescriptors()
        self.test_encrypt_descriptors()


if __name__ == '__main__':
    VaultBlankTest(__file__).main()
