#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""MiniVault default UTXO ranking on a feeless chain."""

from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal
from test_framework.vault import MiniVault


class MiniVaultUtxoTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-txpownocycle=1"]]

    def run_test(self):
        self.vault = MiniVault(self.nodes[0])
        confirmed = self.vault.get_utxos(mark_as_spent=False, confirmed_only=True)
        assert len(confirmed) >= 4
        # Sandbox subsidy ramps every block, so the pre-mined MiniVault
        # coinbases (heights 76-100) are unique-valued. Combine the three
        # largest into three equal confirmed outputs so the ranking methods
        # have same-value siblings.
        sources = [self.vault.get_utxo() for _ in range(3)]
        self.vault.send_self_transfer_multi(
            from_node=self.nodes[0],
            utxos_to_spend=sources,
            num_outputs=3,
        )
        self.generate(self.nodes[0], 1)
        self.vault.rescan_utxos()
        self.test_send_does_not_steal_next_pick()
        self.test_rescan_does_not_steal_next_pick()
        self.test_largest_still_wins()

    def test_send_does_not_steal_next_pick(self):
        self.log.info("A sent self-transfer must not be the next implicit get_utxo")
        sent = self.vault.send_self_transfer(from_node=self.nodes[0])
        remaining = self.vault.get_utxos(mark_as_spent=False, confirmed_only=True)
        assert any(u["value"] == sent["new_utxo"]["value"] for u in remaining)
        pick = self.vault.get_utxo(mark_as_spent=False)
        assert (pick["txid"], pick["vout"]) != (sent["new_utxo"]["txid"], sent["new_utxo"]["vout"])
        assert pick["confirmations"] > 0
        assert_equal(pick["value"], sent["new_utxo"]["value"])

    def test_rescan_does_not_steal_next_pick(self):
        self.log.info("A just-mined self-transfer must not outrank an older same-value coin")
        sent = self.vault.send_self_transfer(from_node=self.nodes[0])
        self.generate(self.nodes[0], 1)
        self.vault.rescan_utxos()
        remaining = self.vault.get_utxos(mark_as_spent=False, confirmed_only=True)
        older = [
            u for u in remaining
            if u["value"] == sent["new_utxo"]["value"]
            and (u["txid"], u["vout"]) != (sent["new_utxo"]["txid"], sent["new_utxo"]["vout"])
        ]
        assert older
        pick = self.vault.get_utxo(mark_as_spent=False)
        assert (pick["txid"], pick["vout"]) != (sent["new_utxo"]["txid"], sent["new_utxo"]["vout"])
        assert pick["confirmations"] > 0

    def test_largest_still_wins(self):
        self.log.info("An unconfirmed output that is strictly larger still wins")
        u1 = self.vault.get_utxo()
        u2 = self.vault.get_utxo()
        combined = self.vault.send_self_transfer_multi(
            from_node=self.nodes[0],
            utxos_to_spend=[u1, u2],
            num_outputs=1,
        )
        fat = combined["new_utxos"][0]
        assert fat["value"] > u1["value"]
        assert fat["value"] > u2["value"]
        pick = self.vault.get_utxo(mark_as_spent=False)
        assert_equal((pick["txid"], pick["vout"]), (fat["txid"], fat["vout"]))
        assert_equal(pick["value"], fat["value"])


if __name__ == '__main__':
    MiniVaultUtxoTest(__file__).main()
