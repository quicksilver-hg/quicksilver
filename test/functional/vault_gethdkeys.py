#!/usr/bin/env python3
# Copyright (c) 2023 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test vault gethdkeys RPC."""

from test_framework.descriptors import descsum_create
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)
from test_framework.vault_util import VaultUnlock


class VaultGetHDKeyTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()

    def run_test(self):
        self.test_basic_gethdkeys()
        self.test_ranged_imports()
        self.test_lone_key_imports()
        self.test_ranged_multisig()
        self.test_mixed_multisig()

    def test_basic_gethdkeys(self):
        self.log.info("Test gethdkeys basics")
        self.nodes[0].createvault("basic")
        vault = self.nodes[0].get_vault_rpc("basic")
        qpub_info = vault.gethdkeys()
        assert_equal(len(qpub_info), 1)
        assert_equal(qpub_info[0]["has_private"], True)

        assert "qprv" not in qpub_info[0]
        qpub = qpub_info[0]["qpub"]

        qpub_info = vault.gethdkeys(private=True)
        qprv = qpub_info[0]["qprv"]
        assert_equal(qpub_info[0]["qpub"], qpub)
        assert_equal(qpub_info[0]["has_private"], True)

        descs = vault.listdescriptors(True)
        for desc in descs["descriptors"]:
            assert qprv in desc["desc"]

        self.log.info("HD pubkey can be retrieved from encrypted vaults")
        prev_qprv = qprv
        vault.encryptvault("pass")
        # HD key is rotated on encryption, there should now be 2 HD keys
        assert_equal(len(vault.gethdkeys()), 2)
        # New key is active, should be able to get only that one and its descriptors
        qpub_info = vault.gethdkeys(active_only=True)
        assert_equal(len(qpub_info), 1)
        assert qpub_info[0]["qpub"] != qpub
        assert "qprv" not in qpub_info[0]
        assert_equal(qpub_info[0]["has_private"], True)

        self.log.info("HD privkey can be retrieved from encrypted vaults")
        assert_raises_rpc_error(-13, "Error: Please enter the vault passphrase with vaultpassphrase first", vault.gethdkeys, private=True)
        with VaultUnlock(vault, "pass"):
            qpub_info = vault.gethdkeys(active_only=True, private=True)[0]
            assert qpub_info["qprv"] != qprv
            for desc in vault.listdescriptors(True)["descriptors"]:
                if desc["active"]:
                    # After encrypting, HD key was rotated and should appear in all active descriptors
                    assert qpub_info["qprv"] in desc["desc"]
                else:
                    # Inactive descriptors should have the previous HD key
                    assert prev_qprv in desc["desc"]

    def test_ranged_imports(self):
        self.log.info("Keys of imported ranged descriptors appear in gethdkeys")
        def_vault = self.nodes[0].get_vault_rpc(self.default_vault_name)
        self.nodes[0].createvault("imports")
        vault = self.nodes[0].get_vault_rpc("imports")

        qpub_info = vault.gethdkeys()
        assert_equal(len(qpub_info), 1)
        active_qpub = qpub_info[0]["qpub"]

        import_qpub = def_vault.gethdkeys(active_only=True)[0]["qpub"]
        desc_import = def_vault.listdescriptors(True)["descriptors"]
        for desc in desc_import:
            desc["active"] = False
        vault.importdescriptors(desc_import)
        assert_equal(vault.gethdkeys(active_only=True), qpub_info)

        qpub_info = vault.gethdkeys()
        assert_equal(len(qpub_info), 2)
        for x in qpub_info:
            if x["qpub"] == active_qpub:
                for desc in x["descriptors"]:
                    assert_equal(desc["active"], True)
            elif x["qpub"] == import_qpub:
                for desc in x["descriptors"]:
                    assert_equal(desc["active"], False)
            else:
                assert False


    def test_lone_key_imports(self):
        self.log.info("Non-HD keys do not appear in gethdkeys")
        self.nodes[0].createvault("lonekey", blank=True)
        vault = self.nodes[0].get_vault_rpc("lonekey")

        assert_equal(vault.gethdkeys(), [])
        vault.importdescriptors([{"desc": descsum_create("wpkh(cTe1f5rdT8A8DFgVWTjyPwACsDPJM9ff4QngFxUixCSvvbg1x6sh)"), "timestamp": "now"}])
        assert_equal(vault.gethdkeys(), [])

        self.log.info("HD keys of non-ranged descriptors should appear in gethdkeys")
        def_vault = self.nodes[0].get_vault_rpc(self.default_vault_name)
        qpub_info = def_vault.gethdkeys(private=True)
        qpub = qpub_info[0]["qpub"]
        qprv = qpub_info[0]["qprv"]
        prv_desc = descsum_create(f"wpkh({qprv})")
        pub_desc = descsum_create(f"wpkh({qpub})")
        assert_equal(vault.importdescriptors([{"desc": prv_desc, "timestamp": "now"}])[0]["success"], True)
        qpub_info = vault.gethdkeys()
        assert_equal(len(qpub_info), 1)
        assert_equal(qpub_info[0]["qpub"], qpub)
        assert_equal(len(qpub_info[0]["descriptors"]), 1)
        assert_equal(qpub_info[0]["descriptors"][0]["desc"], pub_desc)
        assert_equal(qpub_info[0]["descriptors"][0]["active"], False)

    def test_ranged_multisig(self):
        self.log.info("HD keys of a multisig appear in gethdkeys")
        def_vault = self.nodes[0].get_vault_rpc(self.default_vault_name)
        self.nodes[0].createvault("ranged_multisig")
        vault = self.nodes[0].get_vault_rpc("ranged_multisig")

        qpub1 = vault.gethdkeys()[0]["qpub"]
        qprv1 = vault.gethdkeys(private=True)[0]["qprv"]
        qpub2 = def_vault.gethdkeys()[0]["qpub"]

        prv_multi_desc = descsum_create(f"wsh(multi(2,{qprv1}/*,{qpub2}/*))")
        pub_multi_desc = descsum_create(f"wsh(multi(2,{qpub1}/*,{qpub2}/*))")
        assert_equal(vault.importdescriptors([{"desc": prv_multi_desc, "timestamp": "now"}])[0]["success"], True)

        qpub_info = vault.gethdkeys()
        assert_equal(len(qpub_info), 2)
        for x in qpub_info:
            if x["qpub"] == qpub1:
                found_desc = next((d for d in qpub_info[0]["descriptors"] if d["desc"] == pub_multi_desc), None)
                assert found_desc is not None
                assert_equal(found_desc["active"], False)
            elif x["qpub"] == qpub2:
                assert_equal(len(x["descriptors"]), 1)
                assert_equal(x["descriptors"][0]["desc"], pub_multi_desc)
                assert_equal(x["descriptors"][0]["active"], False)
            else:
                assert False

    def test_mixed_multisig(self):
        self.log.info("Non-HD keys of a multisig do not appear in gethdkeys")
        def_vault = self.nodes[0].get_vault_rpc(self.default_vault_name)
        self.nodes[0].createvault("single_multisig")
        vault = self.nodes[0].get_vault_rpc("single_multisig")

        qpub = vault.gethdkeys()[0]["qpub"]
        qprv = vault.gethdkeys(private=True)[0]["qprv"]
        pub = def_vault.getaddressinfo(def_vault.getnewaddress())["pubkey"]

        prv_multi_desc = descsum_create(f"wsh(multi(2,{qprv},{pub}))")
        pub_multi_desc = descsum_create(f"wsh(multi(2,{qpub},{pub}))")
        import_res = vault.importdescriptors([{"desc": prv_multi_desc, "timestamp": "now"}])
        assert_equal(import_res[0]["success"], True)

        qpub_info = vault.gethdkeys()
        assert_equal(len(qpub_info), 1)
        assert_equal(qpub_info[0]["qpub"], qpub)
        found_desc = next((d for d in qpub_info[0]["descriptors"] if d["desc"] == pub_multi_desc), None)
        assert found_desc is not None
        assert_equal(found_desc["active"], False)


if __name__ == '__main__':
    VaultGetHDKeyTest(__file__).main()
