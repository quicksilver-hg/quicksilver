#!/usr/bin/env python3
# Copyright (c) 2017-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test relaypool acceptance of raw transactions."""

from copy import deepcopy
import math

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.cuckatoo import solve_trivial
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.messages import (
    SEQUENCE_NONFINAL,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxInWitness,
    CTxOut,
    MAX_BLOCK_WEIGHT,
    WITNESS_SCALE_FACTOR,
    MAX_MONEY,
    SEQUENCE_FINAL,
    tx_from_hex,
)
from test_framework.script import (
    CScript,
    OP_0,
    OP_HASH160,
    OP_RETURN,
    OP_TRUE,
    SIGHASH_ALL,
    sign_input_legacy,
)
from test_framework.script_util import (
    keys_to_multisig_script,
    PAY_TO_ANCHOR,
    script_to_p2sh_script,
    script_to_p2wsh_script,
)
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)
from test_framework.vault import MiniVault
from test_framework.vault_util import generate_keypair


class RelayPoolAcceptanceTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [[
            '-txindex', '-permitbaremultisig=0', '-txpownocycle=1',
        ]] * self.num_nodes
        self.supports_cli = False

    def check_relaypool_result(self, result_expected, *args, **kwargs):
        """Wrapper to check result of testrelaypoolaccept on node_0's relaypool"""
        result_test = self.nodes[0].testrelaypoolaccept(*args, **kwargs)
        for r in result_test:
            # Skip these checks for now
            r.pop('wtxid')
            if "reject-details" in r:
                r.pop("reject-details")
        assert_equal(result_expected, result_test)
        assert_equal(self.nodes[0].getrelaypoolinfo()['size'], self.relaypool_size)  # Must not change relaypool state

    def prove_tx(self, tx):
        tx.nAnchorHeight = self.nodes[0].getblockcount()
        tx.nPowNonce = 0
        tx.nCycle = solve_trivial((1 << 248) - 1)
        tx.sha256 = None
        tx.hash = None
        return tx

    def run_test(self):
        node = self.nodes[0]
        self.vault = MiniVault(node)

        self.log.info('Start with empty relaypool, and 200 blocks')
        self.vault.generate(COINBASE_MATURITY + 1)
        self.relaypool_size = 0
        assert_equal(node.getblockcount(), 200 + COINBASE_MATURITY + 1)
        assert_equal(node.getrelaypoolinfo()['size'], self.relaypool_size)

        self.log.info('Should not accept garbage to testrelaypoolaccept')
        assert_raises_rpc_error(-3, 'JSON value of type string is not of expected type array', lambda: node.testrelaypoolaccept(rawtxs='ff00baar'))
        assert_raises_rpc_error(-8, 'Array must contain between 1 and 25 transactions.', lambda: node.testrelaypoolaccept(rawtxs=['ff22']*26))
        assert_raises_rpc_error(-8, 'Array must contain between 1 and 25 transactions.', lambda: node.testrelaypoolaccept(rawtxs=[]))
        assert_raises_rpc_error(-22, 'TX decode failed', lambda: node.testrelaypoolaccept(rawtxs=['ff00baar']))

        self.log.info('A transaction already in the blockchain')
        tx = self.vault.create_self_transfer()['tx']  # Pick a random coin(base) to spend
        split_input_value = tx.vout[0].nValue
        split_first_value = split_input_value // 3
        split_second_value = split_input_value - split_first_value
        tx.vout.append(deepcopy(tx.vout[0]))
        tx.vout[0].nValue = split_first_value
        tx.vout[1].nValue = split_second_value
        raw_tx_in_block = tx.serialize().hex()
        txid_in_block = self.vault.sendrawtransaction(from_node=node, tx_hex=raw_tx_in_block)
        self.generate(node, 1)
        self.relaypool_size = 0
        self.check_relaypool_result(
            result_expected=[{'txid': txid_in_block, 'allowed': False, 'reject-reason': 'txn-already-known'}],
            rawtxs=[raw_tx_in_block],
        )

        self.log.info('A transaction not in the relaypool')
        utxo_to_spend = self.vault.get_utxo(txid=txid_in_block)
        tx = self.vault.create_self_transfer(utxo_to_spend=utxo_to_spend, sequence=SEQUENCE_NONFINAL)['tx']
        raw_tx_0 = tx.serialize().hex()
        txid_0 = tx.rehash()
        self.check_relaypool_result(
            result_expected=[{'txid': txid_0, 'allowed': True, 'vsize': tx.get_vsize()}],
            rawtxs=[raw_tx_0],
        )

        self.log.info('A final transaction not in the relaypool')
        tx = self.vault.create_self_transfer(
            sequence=SEQUENCE_FINAL,
            locktime=node.getblockcount() + 2000,  # Can be anything
        )['tx']
        raw_tx_final = tx.serialize().hex()
        tx = tx_from_hex(raw_tx_final)
        self.check_relaypool_result(
            result_expected=[{'txid': tx.rehash(), 'allowed': True, 'vsize': tx.get_vsize()}],
            rawtxs=[tx.serialize().hex()],
        )
        node.sendrawtransaction(hexstring=raw_tx_final)
        self.relaypool_size += 1

        self.log.info('A transaction in the relaypool')
        node.sendrawtransaction(hexstring=raw_tx_0)
        self.relaypool_size += 1
        self.check_relaypool_result(
            result_expected=[{'txid': txid_0, 'allowed': False, 'reject-reason': 'txn-already-in-relaypool'}],
            rawtxs=[raw_tx_0],
        )

        self.log.info('A transaction that replaces a relaypool transaction')
        tx = tx_from_hex(raw_tx_0)
        tx.nCycle = solve_trivial((1 << 240) - 1)
        tx.sha256 = None
        tx.hash = None
        raw_tx_0 = tx.serialize().hex()
        txid_0 = tx.rehash()
        self.check_relaypool_result(
            result_expected=[{'txid': txid_0, 'allowed': True, 'vsize': tx.get_vsize()}],
            rawtxs=[raw_tx_0],
        )
        node.sendrawtransaction(hexstring=tx.serialize().hex())

        self.log.info('A transaction with missing inputs, that never existed')
        tx = tx_from_hex(raw_tx_0)
        tx.vin[0].prevout = COutPoint(hash=int('ff' * 32, 16), n=14)
        self.check_relaypool_result(
            result_expected=[{'txid': tx.rehash(), 'allowed': False, 'reject-reason': 'missing-inputs'}],
            rawtxs=[tx.serialize().hex()],
        )

        self.log.info('A transaction with missing inputs, that existed once in the past')
        tx = tx_from_hex(raw_tx_0)
        tx.vin[0].prevout.n = 1  # Set vout to 1, to spend the other outpoint (49 coins) of the in-chain-tx we want to double spend
        tx.vout[0].nValue = split_second_value
        raw_tx_1 = tx.serialize().hex()
        txid_1 = node.sendrawtransaction(hexstring=raw_tx_1)
        # Now spend both to "clearly hide" the outputs, ie. remove the coins from the utxo set by spending them
        tx = self.vault.create_self_transfer()['tx']
        tx.vin.append(deepcopy(tx.vin[0]))
        tx.wit.vtxinwit.append(deepcopy(tx.wit.vtxinwit[0]))
        tx.vin[0].prevout = COutPoint(hash=int(txid_0, 16), n=0)
        tx.vin[1].prevout = COutPoint(hash=int(txid_1, 16), n=0)
        tx.vout[0].nValue = split_input_value
        raw_tx_spend_both = tx.serialize().hex()
        txid_spend_both = self.vault.sendrawtransaction(from_node=node, tx_hex=raw_tx_spend_both)
        self.generate(node, 1)
        self.relaypool_size = 0
        # Now see if we can add the coins back to the utxo set by sending the exact txs again
        self.check_relaypool_result(
            result_expected=[{'txid': txid_0, 'allowed': False, 'reject-reason': 'missing-inputs'}],
            rawtxs=[raw_tx_0],
        )
        self.check_relaypool_result(
            result_expected=[{'txid': txid_1, 'allowed': False, 'reject-reason': 'missing-inputs'}],
            rawtxs=[raw_tx_1],
        )

        self.log.info('Create a "reference" tx for later use')
        utxo_to_spend = self.vault.get_utxo(txid=txid_spend_both)
        tx = self.vault.create_self_transfer(utxo_to_spend=utxo_to_spend, sequence=SEQUENCE_FINAL)['tx']
        raw_tx_reference = tx.serialize().hex()
        # Reference tx should be valid on itself
        self.check_relaypool_result(
            result_expected=[{'txid': tx.rehash(), 'allowed': True, 'vsize': tx.get_vsize()}],
            rawtxs=[tx.serialize().hex()],
        )

        self.log.info('A transaction with no outputs')
        tx = tx_from_hex(raw_tx_reference)
        tx.vout = []
        self.check_relaypool_result(
            result_expected=[{'txid': tx.rehash(), 'allowed': False, 'reject-reason': 'bad-txns-vout-empty'}],
            rawtxs=[tx.serialize().hex()],
        )

        self.log.info('A really large transaction')
        tx = tx_from_hex(raw_tx_reference)
        tx.vin = [tx.vin[0]] * math.ceil((MAX_BLOCK_WEIGHT // WITNESS_SCALE_FACTOR) / len(tx.vin[0].serialize()))
        self.check_relaypool_result(
            result_expected=[{'txid': tx.rehash(), 'allowed': False, 'reject-reason': 'bad-txns-oversize'}],
            rawtxs=[tx.serialize().hex()],
        )

        self.log.info('A transaction with negative output value')
        tx = tx_from_hex(raw_tx_reference)
        tx.vout[0].nValue *= -1
        self.check_relaypool_result(
            result_expected=[{'txid': tx.rehash(), 'allowed': False, 'reject-reason': 'bad-txns-vout-negative'}],
            rawtxs=[tx.serialize().hex()],
        )

        # The following two validations prevent overflow of the output amounts (see CVE-2010-5139).
        self.log.info('A transaction with too large output value')
        tx = tx_from_hex(raw_tx_reference)
        tx.vout[0].nValue = MAX_MONEY + 1
        self.check_relaypool_result(
            result_expected=[{'txid': tx.rehash(), 'allowed': False, 'reject-reason': 'bad-txns-vout-toolarge'}],
            rawtxs=[tx.serialize().hex()],
        )

        self.log.info('A transaction with too large sum of output values')
        tx = tx_from_hex(raw_tx_reference)
        tx.vout = [tx.vout[0]] * 2
        tx.vout[0].nValue = MAX_MONEY
        self.check_relaypool_result(
            result_expected=[{'txid': tx.rehash(), 'allowed': False, 'reject-reason': 'bad-txns-txouttotal-toolarge'}],
            rawtxs=[tx.serialize().hex()],
        )

        self.log.info('A transaction with duplicate inputs')
        tx = tx_from_hex(raw_tx_reference)
        tx.vin = [tx.vin[0]] * 2
        self.check_relaypool_result(
            result_expected=[{'txid': tx.rehash(), 'allowed': False, 'reject-reason': 'bad-txns-inputs-duplicate'}],
            rawtxs=[tx.serialize().hex()],
        )

        self.log.info('A non-coinbase transaction with coinbase-like outpoint')
        tx = tx_from_hex(raw_tx_reference)
        tx.vin.append(CTxIn(COutPoint(hash=0, n=0xffffffff)))
        self.check_relaypool_result(
            result_expected=[{'txid': tx.rehash(), 'allowed': False, 'reject-reason': 'bad-txns-prevout-null'}],
            rawtxs=[tx.serialize().hex()],
        )

        self.log.info('A coinbase transaction')
        # Pick the input of the first tx we created, so it has to be a coinbase tx
        raw_tx_coinbase_spent = node.getrawtransaction(txid=node.decoderawtransaction(hexstring=raw_tx_in_block)['vin'][0]['txid'])
        tx = tx_from_hex(raw_tx_coinbase_spent)
        self.check_relaypool_result(
            result_expected=[{'txid': tx.rehash(), 'allowed': False, 'reject-reason': 'coinbase'}],
            rawtxs=[tx.serialize().hex()],
        )

        self.log.info('Some nonstandard transactions')
        tx = tx_from_hex(raw_tx_reference)
        tx.version = 4  # A version currently non-standard
        self.check_relaypool_result(
            result_expected=[{'txid': tx.rehash(), 'allowed': False, 'reject-reason': 'version'}],
            rawtxs=[tx.serialize().hex()],
        )
        tx = tx_from_hex(raw_tx_reference)
        tx.vout[0].scriptPubKey = CScript([OP_0])  # Some non-standard script
        self.check_relaypool_result(
            result_expected=[{'txid': tx.rehash(), 'allowed': False, 'reject-reason': 'scriptpubkey'}],
            rawtxs=[tx.serialize().hex()],
        )
        tx = tx_from_hex(raw_tx_reference)
        _, pubkey = generate_keypair()
        tx.vout[0].scriptPubKey = keys_to_multisig_script([pubkey] * 3, k=2)  # Some bare multisig script (2-of-3)
        self.check_relaypool_result(
            result_expected=[{'txid': tx.rehash(), 'allowed': False, 'reject-reason': 'bare-multisig'}],
            rawtxs=[tx.serialize().hex()],
        )
        tx = tx_from_hex(raw_tx_reference)
        tx.vin[0].scriptSig = CScript([OP_HASH160])  # Some not-pushonly scriptSig
        self.check_relaypool_result(
            result_expected=[{'txid': tx.rehash(), 'allowed': False, 'reject-reason': 'scriptsig-not-pushonly'}],
            rawtxs=[tx.serialize().hex()],
        )
        tx = tx_from_hex(raw_tx_reference)
        tx.vin[0].scriptSig = CScript([b'a' * 1648]) # Some too large scriptSig (>1650 bytes)
        self.check_relaypool_result(
            result_expected=[{'txid': tx.rehash(), 'allowed': False, 'reject-reason': 'scriptsig-size'}],
            rawtxs=[tx.serialize().hex()],
        )
        tx = tx_from_hex(raw_tx_reference)
        output_p2sh_burn = CTxOut(nValue=540, scriptPubKey=script_to_p2sh_script(b'burn'))
        num_scripts = 100000 // len(output_p2sh_burn.serialize())  # Use enough outputs to make the tx too large for our policy
        tx.vout = [output_p2sh_burn] * num_scripts
        self.check_relaypool_result(
            result_expected=[{'txid': tx.rehash(), 'allowed': False, 'reject-reason': 'tx-size'}],
            rawtxs=[tx.serialize().hex()],
        )
        tx = tx_from_hex(raw_tx_reference)
        tx.vout[0].scriptPubKey = CScript([OP_RETURN, b'\xff'])
        tx.vout = [tx.vout[0]] * 2
        self.check_relaypool_result(
            result_expected=[{'txid': tx.rehash(), 'allowed': False, 'reject-reason': 'multi-op-return'}],
            rawtxs=[tx.serialize().hex()],
        )

        self.log.info('A timelocked transaction')
        tx = tx_from_hex(raw_tx_reference)
        tx.vin[0].nSequence -= 1  # Should be non-max, so locktime is not ignored
        tx.nLockTime = node.getblockcount() + 1
        self.check_relaypool_result(
            result_expected=[{'txid': tx.rehash(), 'allowed': False, 'reject-reason': 'non-final'}],
            rawtxs=[tx.serialize().hex()],
        )

        self.log.info('A transaction that is locked by BIP68 sequence logic')
        tx = tx_from_hex(raw_tx_reference)
        tx.vin[0].nSequence = 2  # We could include it in the second block mined from now, but not the very next one
        self.check_relaypool_result(
            result_expected=[{'txid': tx.rehash(), 'allowed': False, 'reject-reason': 'non-BIP68-final'}],
            rawtxs=[tx.serialize().hex()],
        )

        self.log.info('OP_1 <0x4e73> is able to be created and spent')
        anchor_value = 10000
        create_anchor_tx = self.vault.send_to(from_node=node, scriptPubKey=PAY_TO_ANCHOR, amount=anchor_value)
        self.generate(node, 1)

        # First spend has non-empty witness, will be rejected to prevent third party wtxid malleability
        anchor_nonempty_wit_spend = CTransaction()
        anchor_nonempty_wit_spend.vin.append(CTxIn(COutPoint(int(create_anchor_tx["txid"], 16), create_anchor_tx["sent_vout"]), b""))
        anchor_nonempty_wit_spend.vout.append(CTxOut(anchor_value, script_to_p2wsh_script(CScript([OP_TRUE]))))
        anchor_nonempty_wit_spend.wit.vtxinwit.append(CTxInWitness())
        anchor_nonempty_wit_spend.wit.vtxinwit[0].scriptWitness.stack.append(b"f")
        self.prove_tx(anchor_nonempty_wit_spend)
        anchor_nonempty_wit_spend.rehash()

        self.check_relaypool_result(
            result_expected=[{'txid': anchor_nonempty_wit_spend.rehash(), 'allowed': False, 'reject-reason': 'bad-witness-nonstandard'}],
            rawtxs=[anchor_nonempty_wit_spend.serialize().hex()],
        )

        # but is consensus-legal
        self.generateblock(node, self.vault.get_address(), [anchor_nonempty_wit_spend.serialize().hex()])

        # Without witness elements it is standard
        create_anchor_tx = self.vault.send_to(from_node=node, scriptPubKey=PAY_TO_ANCHOR, amount=anchor_value)
        self.generate(node, 1)

        anchor_spend = CTransaction()
        anchor_spend.vin.append(CTxIn(COutPoint(int(create_anchor_tx["txid"], 16), create_anchor_tx["sent_vout"]), b""))
        anchor_spend.vout.append(CTxOut(anchor_value, script_to_p2wsh_script(CScript([OP_TRUE]))))
        anchor_spend.wit.vtxinwit.append(CTxInWitness())
        self.prove_tx(anchor_spend)
        # It's "segwit" but txid == wtxid since there is no witness data
        assert_equal(anchor_spend.rehash(), anchor_spend.getwtxid())

        self.check_relaypool_result(
            result_expected=[{'txid': anchor_spend.rehash(), 'allowed': True, 'vsize': anchor_spend.get_vsize()}],
            rawtxs=[anchor_spend.serialize().hex()],
        )

        self.log.info('But cannot be spent if nested sh()')
        nested_anchor_tx = self.vault.create_self_transfer(sequence=SEQUENCE_FINAL)['tx']
        nested_anchor_tx.vout[0].scriptPubKey = script_to_p2sh_script(PAY_TO_ANCHOR)
        nested_anchor_tx.rehash()
        self.generateblock(node, self.vault.get_address(), [nested_anchor_tx.serialize().hex()])

        nested_anchor_spend = CTransaction()
        nested_anchor_spend.vin.append(CTxIn(COutPoint(nested_anchor_tx.sha256, 0), b""))
        nested_anchor_spend.vin[0].scriptSig = CScript([bytes(PAY_TO_ANCHOR)])
        nested_anchor_spend.vout.append(CTxOut(nested_anchor_tx.vout[0].nValue, script_to_p2wsh_script(CScript([OP_TRUE]))))
        self.prove_tx(nested_anchor_spend)
        nested_anchor_spend.rehash()

        self.check_relaypool_result(
            result_expected=[{'txid': nested_anchor_spend.rehash(), 'allowed': False, 'reject-reason': 'bad-txns-nonstandard-inputs'}],
            rawtxs=[nested_anchor_spend.serialize().hex()],
        )
        # and is consensus-invalid too: wrapped witness is rejected by a mandatory
        # script-verify flag, so it cannot be mined either
        assert_raises_rpc_error(
            -25,
            'mandatory-script-verify-flag-failed (P2SH-wrapped witness programs are not supported)',
            lambda: self.generateblock(node, self.vault.get_address(), [nested_anchor_spend.serialize().hex()]),
        )

        self.log.info('Spending a confirmed bare multisig is okay')
        address = self.vault.get_address()
        tx = tx_from_hex(raw_tx_reference)
        privkey, pubkey = generate_keypair()
        tx.vout[0].scriptPubKey = keys_to_multisig_script([pubkey] * 3, k=1)  # Some bare multisig script (1-of-3)
        tx.rehash()
        self.generateblock(node, address, [tx.serialize().hex()])
        tx_spend = CTransaction()
        tx_spend.vin.append(CTxIn(COutPoint(tx.sha256, 0), b""))
        tx_spend.vout.append(CTxOut(tx.vout[0].nValue, script_to_p2wsh_script(CScript([OP_TRUE]))))
        self.prove_tx(tx_spend)
        tx_spend.rehash()
        sign_input_legacy(tx_spend, 0, tx.vout[0].scriptPubKey, privkey, sighash_type=SIGHASH_ALL)
        tx_spend.vin[0].scriptSig = bytes(CScript([OP_0])) + tx_spend.vin[0].scriptSig
        self.check_relaypool_result(
            result_expected=[{'txid': tx_spend.rehash(), 'allowed': True, 'vsize': tx_spend.get_vsize()}],
            rawtxs=[tx_spend.serialize().hex()],
        )

if __name__ == '__main__':
    RelayPoolAcceptanceTest(__file__).main()
