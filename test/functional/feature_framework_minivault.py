#!/usr/bin/env python3
# Copyright (c) 2024 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test MiniVault."""
import random
import string

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal,
)
from test_framework.vault import (
    MiniVault,
    MiniVaultMode,
)


class FeatureFrameworkMiniVaultTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-txpownocycle=1"]]

    def test_tx_padding(self):
        """Verify that MiniVault's transaction padding (`target_vsize` parameter)
           works accurately with all modes."""
        for mode_name, vault in self.vaults:
            self.log.info(f"Test tx padding with MiniVault mode {mode_name}...")
            utxo = vault.get_utxo(mark_as_spent=False)
            base_vsize = vault.create_self_transfer(utxo_to_spend=utxo)['tx'].get_vsize()
            for target_vsize in [base_vsize + 50, 500, 1250, 2500, 5000, 12500, 25000, 50000, 1000000,
                                 501, 1085, 3343, 5805, 12289, 25509, 55855,  999998]:
                tx = vault.create_self_transfer(utxo_to_spend=utxo, target_vsize=target_vsize)
                assert_equal(tx['tx'].get_vsize(), target_vsize)
                child_tx = vault.create_self_transfer_multi(utxos_to_spend=[tx["new_utxo"]], target_vsize=target_vsize)
                assert_equal(child_tx['tx'].get_vsize(), target_vsize)


    def test_vault_tagging(self):
        """Verify that tagged vault instances are able to send funds."""
        self.log.info("Test tagged vault instances...")
        node = self.nodes[0]
        untagged_vault = self.vaults[0][1]
        for i in range(10):
            tag = ''.join(random.choice(string.ascii_letters) for _ in range(20))
            self.log.debug(f"-> ({i}) tag name: {tag}")
            tagged_vault = MiniVault(node, tag_name=tag)
            untagged_vault.send_to(from_node=node, scriptPubKey=tagged_vault.get_output_script(), amount=100000)
            tagged_vault.rescan_utxos()
            tagged_vault.send_self_transfer(from_node=node)
        self.generate(node, 1)  # clear relaypool

    def run_test(self):
        node = self.nodes[0]
        self.vaults = [
            ("ADDRESS_OP_TRUE", MiniVault(node, mode=MiniVaultMode.ADDRESS_OP_TRUE)),
            ("RAW_OP_TRUE",     MiniVault(node, mode=MiniVaultMode.RAW_OP_TRUE)),
            ("RAW_P2PK",        MiniVault(node, mode=MiniVaultMode.RAW_P2PK)),
        ]
        for _, vault in self.vaults:
            self.generate(vault, 10)
        self.generate(vault, COINBASE_MATURITY)

        self.test_tx_padding()
        self.test_vault_tagging()


if __name__ == '__main__':
    FeatureFrameworkMiniVaultTest(__file__).main()
