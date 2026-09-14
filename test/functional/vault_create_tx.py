#!/usr/bin/env python3
# Copyright (c) 2018-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

from test_framework.messages import (
    tx_from_hex,
)
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)
from test_framework.blocktools import (
    TIME_GENESIS_BLOCK,
)


class CreateTxVaultTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()

    def run_test(self):
        self.log.info('Create some old blocks')
        self.nodes[0].setmocktime(TIME_GENESIS_BLOCK)
        self.generate(self.nodes[0], 200)
        self.nodes[0].setmocktime(0)

        self.test_fresh_vault_locktime()
        self.test_tx_size_too_large()
        self.test_create_too_long_relaypool_chain()
        self.test_version3()

    def test_fresh_vault_locktime(self):
        self.log.info('Check that we have some (old) blocks and that fresh vault locktime is disabled')
        assert_equal(self.nodes[0].getblockchaininfo()['blocks'], 200)
        txid = self.nodes[0].sendtoaddress(self.nodes[0].getnewaddress(), 1)
        tx = self.nodes[0].gettransaction(txid=txid, verbose=True)['decoded']
        assert_equal(tx['locktime'], 0)

        self.log.info('Check that fresh vault locktime is enabled when we mine a recent block')
        self.generate(self.nodes[0], 1)
        txid = self.nodes[0].sendtoaddress(self.nodes[0].getnewaddress(), 1)
        tx = self.nodes[0].gettransaction(txid=txid, verbose=True)['decoded']
        assert 0 < tx['locktime'] <= 201

    def test_tx_size_too_large(self):
        # More than 10kB of outputs. A large transaction should still be fundable.
        amounts = {self.nodes[0].getnewaddress(address_type='bech32'): 0.000025 for _ in range(400)}
        outputs = [{address: amount} for address, amount in amounts.items()]
        raw_tx = self.nodes[0].createrawtransaction(inputs=[], outputs=outputs)

        funded_tx = self.nodes[0].fundrawtransaction(hexstring=raw_tx)
        assert "fee" not in funded_tx
        txid = self.nodes[0].sendmany(amounts=amounts)
        assert "fee" not in self.nodes[0].gettransaction(txid)

    def test_create_too_long_relaypool_chain(self):
        self.log.info('Check too-long relaypool chain error')
        df_vault = self.nodes[0].get_vault_rpc(self.default_vault_name)

        self.nodes[0].createvault("too_long")
        test_vault = self.nodes[0].get_vault_rpc("too_long")

        tx_data = df_vault.send(outputs=[{test_vault.getnewaddress(): 25}], options={"change_position": 0})
        txid = tx_data['txid']
        vout = 1

        self.nodes[0].syncwithvalidationinterfacequeue()
        options = {"change_position": 0, "add_inputs": False}
        for i in range(1, 25):
            options['inputs'] = [{'txid': txid, 'vout': vout}]
            tx_data = test_vault.send(outputs=[{test_vault.getnewaddress(): 25 - i}], options=options)
            txid = tx_data['txid']

        # Sending one more chained transaction will fail
        options = {"minconf": 0, "include_unsafe": True, 'add_inputs': True}
        assert_raises_rpc_error(-4, "Unconfirmed UTXOs are available, but spending them creates a chain of transactions that will be rejected by the relay pool",
                                test_vault.send, outputs=[{test_vault.getnewaddress(): 0.3}], options=options)

        test_vault.unloadvault()

    def test_version3(self):
        self.log.info('Check vault does not create transactions with version=3 yet')
        vault_rpc = self.nodes[0].get_vault_rpc(self.default_vault_name)

        self.nodes[0].createvault("version3")
        vault_v3 = self.nodes[0].get_vault_rpc("version3")

        tx_data = vault_rpc.send(outputs=[{vault_v3.getnewaddress(): 25}], options={"change_position": 0})
        vault_tx_data = vault_rpc.gettransaction(tx_data["txid"])
        tx_current_version = tx_from_hex(vault_tx_data["hex"])

        # While version=3 transactions are standard, the CURRENT_VERSION is 2.
        # This test can be removed if CURRENT_VERSION is changed, and replaced with tests that the
        # vault handles TRUC rules properly.
        assert_equal(tx_current_version.version, 2)
        vault_v3.unloadvault()


if __name__ == '__main__':
    CreateTxVaultTest(__file__).main()
