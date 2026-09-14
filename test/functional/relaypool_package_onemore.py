#!/usr/bin/env python3
# Copyright (c) 2014-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test descendant package tracking carve-out allowing one final transaction in
   an otherwise-full package as long as it has only one parent and is <= 10k in
   size.
"""

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.messages import (
    DEFAULT_ANCESTOR_LIMIT,
)
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)
from test_framework.vault import MiniVault


class RelayPoolPackagesTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-txpownocycle=1"]]

    def chain_tx(self, utxos_to_spend, *, num_outputs=1):
        return self.vault.send_self_transfer_multi(
            from_node=self.nodes[0],
            utxos_to_spend=utxos_to_spend,
            num_outputs=num_outputs)['new_utxos']

    def run_test(self):
        self.vault = MiniVault(self.nodes[0])
        self.generate(self.vault, COINBASE_MATURITY + 2)

        # DEFAULT_ANCESTOR_LIMIT transactions off a confirmed tx should be fine
        chain = []
        utxo = self.vault.get_utxo()
        for _ in range(4):
            utxo, utxo2 = self.chain_tx([utxo], num_outputs=2)
            chain.append(utxo2)
        for _ in range(DEFAULT_ANCESTOR_LIMIT - 4):
            utxo, = self.chain_tx([utxo])
            chain.append(utxo)
        second_chain, = self.chain_tx([self.vault.get_utxo(confirmed_only=True)])

        # Check relaypool has DEFAULT_ANCESTOR_LIMIT + 1 transactions in it
        assert_equal(len(self.nodes[0].getrawrelaypool()), DEFAULT_ANCESTOR_LIMIT + 1)

        # Adding one more transaction on to the chain should fail.
        assert_raises_rpc_error(-26, "too-long-relaypool-chain, too many unconfirmed ancestors [limit: 25]", self.chain_tx, [utxo])
        # ... or if it chains on from some point in the middle of the chain.
        assert_raises_rpc_error(-26, "too-long-relaypool-chain, too many descendants", self.chain_tx, [chain[2]])
        assert_raises_rpc_error(-26, "too-long-relaypool-chain, too many descendants", self.chain_tx, [chain[1]])
        # ...even if it chains on to two parent transactions with one in the chain.
        assert_raises_rpc_error(-26, "too-long-relaypool-chain, too many descendants", self.chain_tx, [chain[0], second_chain])
        # ...especially if its > 40k weight
        assert_raises_rpc_error(-26, "too-long-relaypool-chain, too many descendants", self.chain_tx, [chain[0]], num_outputs=350)
        # ...even if it's submitted with other transactions
        parent_tx = self.vault.create_self_transfer_multi(utxos_to_spend=[chain[0]])
        txns = [parent_tx["tx"], self.vault.create_self_transfer_multi(utxos_to_spend=parent_tx["new_utxos"])["tx"]]
        txns_hex = [tx.serialize().hex() for tx in txns]
        assert_equal(self.nodes[0].testrelaypoolaccept(txns_hex)[0]["reject-reason"], "too-long-relaypool-chain")
        pkg_result = self.nodes[0].submitpackage(txns_hex)
        assert "too-long-relaypool-chain" in pkg_result["tx-results"][txns[0].getwtxid()]["error"]
        assert_equal(pkg_result["tx-results"][txns[1].getwtxid()]["error"], "bad-txns-inputs-missingorspent")
        # But not if it chains directly off the first transaction
        self.nodes[0].sendrawtransaction(parent_tx["hex"])
        # and the second chain should work just fine
        self.chain_tx([second_chain])

        # Finally, check that we added two transactions
        assert_equal(len(self.nodes[0].getrawrelaypool()), DEFAULT_ANCESTOR_LIMIT + 3)


if __name__ == '__main__':
    RelayPoolPackagesTest(__file__).main()
