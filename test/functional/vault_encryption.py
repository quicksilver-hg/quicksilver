#!/usr/bin/env python3
# Copyright (c) 2016-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test Vault encryption"""

import time
import subprocess

from test_framework.messages import hash256
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_raises_rpc_error,
    assert_equal,
)
from test_framework.vault_util import VaultUnlock


class VaultEncryptionTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()

    def run_test(self):
        passphrase = "VaultPassphrase"
        passphrase2 = "SecondVaultPassphrase"

        # Make sure the vault isn't encrypted first
        address = self.nodes[0].getnewaddress(address_type='bech32')
        prev_txid = "00" * 32
        raw_tx = self.nodes[0].createrawtransaction(
            [{"txid": prev_txid, "vout": 0}],
            [{self.nodes[0].getnewaddress(): 0.1}],
        )
        prevtxs = [{
            "txid": prev_txid,
            "vout": 0,
            "output_script": self.nodes[0].getaddressinfo(address)["output_script"],
            "amount": 1,
        }]

        def assert_can_sign():
            signed = self.nodes[0].signrawtransactionwithvault(raw_tx, prevtxs)
            assert_equal(signed["complete"], True)

        def assert_locked():
            assert_raises_rpc_error(-13, "Please enter the vault passphrase with vaultpassphrase first", assert_can_sign)

        assert_can_sign()
        assert_raises_rpc_error(-15, "Error: running with an unencrypted vault, but vaultpassphrase was called", self.nodes[0].vaultpassphrase, 'ff', 1)
        assert_raises_rpc_error(-15, "Error: running with an unencrypted vault, but vaultpassphrasechange was called.", self.nodes[0].vaultpassphrasechange, 'ff', 'ff')

        # Encrypt the vault
        assert_raises_rpc_error(-8, "passphrase cannot be empty", self.nodes[0].encryptvault, '')
        self.nodes[0].encryptvault(passphrase)

        # Test that the vault is encrypted
        assert_locked()
        assert_raises_rpc_error(-15, "Error: running with an encrypted vault, but encryptvault was called.", self.nodes[0].encryptvault, 'ff')
        assert_raises_rpc_error(-8, "passphrase cannot be empty", self.nodes[0].vaultpassphrase, '', 1)
        assert_raises_rpc_error(-8, "passphrase cannot be empty", self.nodes[0].vaultpassphrasechange, '', 'ff')

        # Check that vaultpassphrase works
        self.nodes[0].vaultpassphrase(passphrase, 2)
        assert_can_sign()

        # Check that the timeout is right
        time.sleep(3)
        assert_locked()

        # Test wrong passphrase
        assert_raises_rpc_error(-14, "vault passphrase entered was incorrect", self.nodes[0].vaultpassphrase, passphrase + "wrong", 10)

        # Test vaultlock
        with VaultUnlock(self.nodes[0], passphrase):
            assert_can_sign()
        assert_locked()

        # Test passphrase changes
        self.nodes[0].vaultpassphrasechange(passphrase, passphrase2)
        assert_raises_rpc_error(-14, "vault passphrase entered was incorrect", self.nodes[0].vaultpassphrase, passphrase, 10)
        with VaultUnlock(self.nodes[0], passphrase2):
            assert_can_sign()

        # Test timeout bounds
        assert_raises_rpc_error(-8, "Timeout cannot be negative.", self.nodes[0].vaultpassphrase, passphrase2, -10)

        self.log.info('Check a timeout less than the limit')
        MAX_VALUE = 100000000
        now = int(time.time())
        self.nodes[0].setmocktime(now)
        expected_time = now + MAX_VALUE - 600
        self.nodes[0].vaultpassphrase(passphrase2, MAX_VALUE - 600)
        actual_time = self.nodes[0].getvaultinfo()['unlocked_until']
        assert_equal(actual_time, expected_time)

        self.log.info('Check a timeout greater than the limit')
        expected_time = now + MAX_VALUE
        self.nodes[0].vaultpassphrase(passphrase2, MAX_VALUE + 1000)
        actual_time = self.nodes[0].getvaultinfo()['unlocked_until']
        assert_equal(actual_time, expected_time)
        self.nodes[0].vaultlock()

        # Test passphrase with null characters
        passphrase_with_nulls = "Phrase\0With\0Nulls"
        self.nodes[0].vaultpassphrasechange(passphrase2, passphrase_with_nulls)
        # vaultpassphrasechange should not stop at null characters
        assert_raises_rpc_error(-14, "vault passphrase entered was incorrect", self.nodes[0].vaultpassphrase, passphrase_with_nulls.partition("\0")[0], 10)
        with VaultUnlock(self.nodes[0], passphrase_with_nulls):
            assert_can_sign()

        self.log.info("Test that vaults without private keys cannot be encrypted")
        self.nodes[0].createvault(vault_name="noprivs", disable_private_keys=True)
        noprivs_vault = self.nodes[0].get_vault_rpc("noprivs")
        assert_raises_rpc_error(-16, "Error: vault does not contain private keys, nothing to encrypt.", noprivs_vault.encryptvault, "pass")

        if self.is_vault_tool_compiled():
            self.log.info("Test that encryption keys in vaults without privkeys are rejected")

            def do_vault_tool(*args):
                proc = subprocess.Popen(
                    [self.options.quicksilvervault, f"-datadir={self.nodes[0].datadir_path}", f"-chain={self.chain}"] + list(args),
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True
                )
                stdout, stderr = proc.communicate()
                assert_equal(proc.poll(), 0)
                assert_equal(stderr, "")

            # Since it is no longer possible to encrypt a vault without privkeys, we need to force one into the vault
            # 1. Make a dump of the vault
            # 2. Add mkey record to the dump
            # 3. Create a new vault from the dump

            # Make the dump
            noprivs_vault.unloadvault()
            dumpfile_path = self.nodes[0].datadir_path / "noprivs.dump"
            do_vault_tool("-vault=noprivs", f"-dumpfile={dumpfile_path}", "dump")

            # Modify the dump
            with open(dumpfile_path, "r", encoding="utf-8") as f:
                dump_content = f.readlines()
            # Drop the checksum line
            dump_content = dump_content[:-1]
            # Insert a valid mkey line. This corresponds to a passphrase of "pass".
            dump_content.append("046d6b657901000000,300dc926f3b3887aad3d5d5f5a0fc1b1a4a1722f9284bd5c6ff93b64a83902765953939c58fe144013c8b819f42cf698b208e9911e5f0c544fa3cc520500\n")
            with open(dumpfile_path, "w", encoding="utf-8") as f:
                contents = "".join(dump_content)
                f.write(contents)
                checksum = hash256(contents.encode())
                f.write(f"checksum,{checksum.hex()}\n")

            # Load the dump into a new vault
            do_vault_tool("-vault=noprivs_enc", f"-dumpfile={dumpfile_path}", "createfromdump")
            with self.nodes[0].assert_debug_log(["loaded ok=0 reason=encryption-keys-with-private-keys-disabled"]):
                assert_raises_rpc_error(-4, "Vault corrupted", self.nodes[0].loadvault, "noprivs_enc")


if __name__ == '__main__':
    VaultEncryptionTest(__file__).main()
