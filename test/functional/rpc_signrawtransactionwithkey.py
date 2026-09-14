#!/usr/bin/env python3
# Copyright (c) 2015-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test transaction signing using the signrawtransactionwithkey RPC."""

from test_framework.messages import (
    COIN,
)
from test_framework.blocktools import COINBASE_MATURITY
from test_framework.address import (
    address_to_scriptpubkey,
    p2a,
    script_to_p2sh,
    script_to_p2wsh,
)
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.txpow import prove_raw_tx_pow
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)
from test_framework.script_util import (
    key_to_p2pk_script,
    key_to_p2pkh_script,
    script_to_p2wsh_script,
)
from test_framework.vault import (
    getnewdestination,
    MiniVault,
)
from test_framework.vault_util import (
    generate_keypair,
)

from decimal import (
    Decimal,
)

INPUTS = [
    # Valid pay-to-pubkey scripts
    {'txid': '9b907ef1e3c26fc71fe4a4b3580bc75264112f95050014157059c736f0202e71', 'vout': 0,
     'output_script': '76a91460baa0f494b38ce3c940dea67f3804dc52d1fb9488ac'},
    {'txid': '83a4f6a6b73660e13ee6cb3c6063fa3759c50c9b7521d0536022961898f4fb02', 'vout': 0,
     'output_script': '76a914669b857c03a5ed269d5d85a1ffac9ed5d663072788ac'},
]
OUTPUTS = [{'SW7TUT1H4vs8wgNLBAWwE9fK9carPLD31X': 0.1}]

class SignRawTransactionWithKeyTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-txpownocycle=1"]]

    def send_to_address(self, addr, amount):
        script_pub_key = address_to_scriptpubkey(addr)
        tx = self.vault.send_to(from_node=self.nodes[0], scriptPubKey=script_pub_key, amount=int(amount * COIN))
        return tx["txid"], tx["sent_vout"]

    def assert_signing_completed_successfully(self, signed_tx):
        assert 'errors' not in signed_tx
        assert 'complete' in signed_tx
        assert_equal(signed_tx['complete'], True)

    def successful_signing_test(self):
        """Create and sign a valid raw transaction with one input.

        Expected results:

        1) The transaction has a complete set of signatures
        2) No script verification error occurred"""
        self.log.info("Test valid raw transaction with one input")
        privKeys = ['SGdoqVDjnPEZYHDNzbjMVDm4hY2V4pAVzFjZwqiCbAATquHRDKAH', 'SHKJwXdFMhjqbnuDzMiJbMzJAJLZUrNjUk8KVegwZot9Pcart1NG']
        rawTx = self.nodes[0].createrawtransaction(INPUTS, OUTPUTS)
        rawTxSigned = self.nodes[0].signrawtransactionwithkey(rawTx, privKeys, INPUTS)

        self.assert_signing_completed_successfully(rawTxSigned)

    def witness_script_test(self):
        self.log.info("Test signing transaction to native witness-script addresses without vault")
        # Create a new native witness-script 1-of-1 multisig address:
        embedded_privkey, embedded_pubkey = generate_keypair(wif=True)
        p2wsh_address = self.nodes[0].createmultisig(1, [embedded_pubkey.hex()])
        self.send_to_address(p2wsh_address["address"], Decimal("0.999"))
        self.generate(self.nodes[0], 1)
        # Get the UTXO info from scantxoutset
        unspent_output = self.nodes[0].scantxoutset('start', [p2wsh_address['descriptor']])['unspents'][0]
        spk = script_to_p2wsh_script(p2wsh_address['redeem_script']).hex()
        unspent_output['witness_script'] = p2wsh_address['redeem_script']
        assert_equal(spk, unspent_output['output_script'])
        # Now create and sign a transaction spending that output on node[0], which doesn't know the scripts or keys
        spending_tx = self.nodes[0].createrawtransaction([unspent_output], [{getnewdestination()[2]: unspent_output["amount"]}])
        spending_tx_signed = self.nodes[0].signrawtransactionwithkey(spending_tx, [embedded_privkey], [unspent_output])
        self.assert_signing_completed_successfully(spending_tx_signed)

        # Now test with P2PKH and P2PK scripts as the witnessScript
        for tx_type in ['P2PKH', 'P2PK']:  # these tests are order-independent
            self.verify_txn_with_witness_script(tx_type)

    def keyless_signing_test(self):
        self.log.info("Test that keyless 'signing' of pay-to-anchor input succeeds")
        [txid, vout] = self.send_to_address(p2a(), Decimal("0.999"))
        spending_tx = self.nodes[0].createrawtransaction(
            [{"txid": txid, "vout": vout}],
            [{getnewdestination()[2]: Decimal("0.999")}])
        spending_tx = prove_raw_tx_pow(spending_tx, self.nodes[0])
        spending_tx_signed = self.nodes[0].signrawtransactionwithkey(spending_tx, [], [])
        self.assert_signing_completed_successfully(spending_tx_signed)
        assert self.nodes[0].testrelaypoolaccept([spending_tx_signed["hex"]])[0]["allowed"]
        # 'signing' a P2A prevout is a no-op, so signed and unsigned txs shouldn't differ
        assert_equal(spending_tx, spending_tx_signed["hex"])

    def verify_txn_with_witness_script(self, tx_type):
        self.log.info("Test with a {} script as the witnessScript".format(tx_type))
        embedded_privkey, embedded_pubkey = generate_keypair(wif=True)
        witness_script = {
            'P2PKH': key_to_p2pkh_script(embedded_pubkey).hex(),
            'P2PK': key_to_p2pk_script(embedded_pubkey).hex()
        }.get(tx_type, "Invalid tx_type")
        addr = script_to_p2wsh(witness_script)
        script_pub_key = address_to_scriptpubkey(addr).hex()
        # Fund that address
        [txid, vout] = self.send_to_address(addr, Decimal("0.999"))
        # Now create and sign a transaction spending that output on node[0], which doesn't know the scripts or keys
        spending_tx = self.nodes[0].createrawtransaction([{'txid': txid, 'vout': vout}], [{getnewdestination()[2]: Decimal("0.999")}])
        spending_tx_signed = self.nodes[0].signrawtransactionwithkey(spending_tx, [embedded_privkey], [{'txid': txid, 'vout': vout, 'output_script': script_pub_key, 'witness_script': witness_script, 'amount': Decimal("0.999")}])
        self.assert_signing_completed_successfully(spending_tx_signed)
        self.nodes[0].sendrawtransaction(prove_raw_tx_pow(spending_tx_signed['hex'], self.nodes[0]))

    def wrapped_witness_rejection_test(self):
        self.log.info("Test that signing rejects a witness program embedded in P2SH")
        embedded_privkey, embedded_pubkey = generate_keypair(wif=True)
        witness_script = key_to_p2pk_script(embedded_pubkey).hex()
        redeem_script = script_to_p2wsh_script(witness_script).hex()
        addr = script_to_p2sh(redeem_script)
        script_pub_key = address_to_scriptpubkey(addr).hex()
        [txid, vout] = self.send_to_address(addr, Decimal("0.999"))
        spending_tx = self.nodes[0].createrawtransaction([{'txid': txid, 'vout': vout}], [{getnewdestination()[2]: Decimal("0.999")}])
        assert_raises_rpc_error(
            -8,
            "P2SH-wrapped witness programs are not supported",
            self.nodes[0].signrawtransactionwithkey,
            spending_tx,
            [embedded_privkey],
            [{'txid': txid, 'vout': vout, 'output_script': script_pub_key, 'redeem_script': redeem_script, 'amount': Decimal("0.999")}],
        )

    def invalid_sighashtype_test(self):
        self.log.info("Test signing transaction with invalid sighashtype")
        tx = self.nodes[0].createrawtransaction(INPUTS, OUTPUTS)
        privkeys = [self.nodes[0].get_deterministic_priv_key().key]
        assert_raises_rpc_error(-8, "'all' is not a valid sighash parameter.", self.nodes[0].signrawtransactionwithkey, tx, privkeys, sighashtype="all")

    def invalid_private_key_and_tx(self):
        self.log.info("Test signing transaction with an invalid private key")
        tx = self.nodes[0].createrawtransaction(INPUTS, OUTPUTS)
        privkeys = ["123"]
        assert_raises_rpc_error(-5, "Invalid private key", self.nodes[0].signrawtransactionwithkey, tx, privkeys)
        self.log.info("Test signing transaction with an invalid tx hex")
        privkeys = [self.nodes[0].get_deterministic_priv_key().key]
        assert_raises_rpc_error(-22, "TX decode failed. Make sure the tx has at least one input.", self.nodes[0].signrawtransactionwithkey, tx + "00", privkeys)

    def combine_partial_multisig_transactions(self):
        self.log.info("Combine complementary partial multisig signatures")
        node = self.nodes[0]
        key_a = generate_keypair(wif=True)
        key_b = generate_keypair(wif=True)
        multisig = node.createmultisig(2, [key_a[1].hex(), key_b[1].hex()], "bech32")
        output_script = address_to_scriptpubkey(multisig["address"])
        amount = Decimal("1.25")
        funding = self.vault.send_to(from_node=node, scriptPubKey=output_script, amount=int(amount * COIN))
        self.generate(node, 1)

        prevout = {
            "txid": funding["txid"],
            "vout": funding["sent_vout"],
            "output_script": output_script.hex(),
            "witness_script": multisig["redeem_script"],
            "amount": amount,
        }
        unsigned = node.createrawtransaction(
            [{"txid": funding["txid"], "vout": funding["sent_vout"]}],
            [{getnewdestination()[2]: amount}],
        )
        partial_a = node.signrawtransactionwithkey(unsigned, [key_a[0]], [prevout])
        partial_b = node.signrawtransactionwithkey(unsigned, [key_b[0]], [prevout])
        assert_equal(partial_a["complete"], False)
        assert_equal(partial_b["complete"], False)

        assert_raises_rpc_error(-22, "Missing transactions", node.combinerawtransaction, [])
        assert_raises_rpc_error(-22, "TX decode failed", node.combinerawtransaction, [partial_a["hex"] + "00"])
        combined = node.combinerawtransaction([partial_a["hex"], partial_b["hex"]])
        assert_equal(node.signrawtransactionwithkey(combined, [], [prevout])["complete"], True)

    def run_test(self):
        self.vault = MiniVault(self.nodes[0])
        self.generate(self.vault, COINBASE_MATURITY + 50)
        self.successful_signing_test()
        self.witness_script_test()
        self.wrapped_witness_rejection_test()
        self.keyless_signing_test()
        self.invalid_sighashtype_test()
        self.invalid_private_key_and_tx()
        self.combine_partial_multisig_transactions()


if __name__ == '__main__':
    SignRawTransactionWithKeyTest(__file__).main()
