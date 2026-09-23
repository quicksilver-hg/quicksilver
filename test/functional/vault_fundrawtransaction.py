#!/usr/bin/env python3
# Copyright (c) 2014-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the fundrawtransaction RPC."""


from decimal import Decimal
from test_framework.address import address_to_scriptpubkey

from test_framework.descriptors import descsum_create
from test_framework.messages import (
    COIN,
    CTransaction,
    CTxOut,
)
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_greater_than_or_equal,
    assert_raises_rpc_error,
)
from test_framework.vault_util import generate_keypair, VaultUnlock

ERR_NOT_ENOUGH_PRESET_INPUTS = "The preselected coins total amount does not cover the transaction target. " \
                               "Please allow other inputs to be automatically selected or include more coins manually"

def output_addresses(outputs):
    """Collect the destination addresses from a createrawtransaction outputs list."""
    return {address for output in outputs for address in output}

def get_unspent(listunspent, amount):
    for utx in listunspent:
        if utx['amount'] == amount:
            return utx
    raise AssertionError('Could not find unspent with amount={}'.format(amount))

class RawTransactionsTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 4
        # This file grinds two near-maximum-weight transactions as fixtures:
        # `test_transaction_too_large` sends 1500 outputs and `test_weight_limits`
        # sends 1472. Per-tx work scales with serialized bytes and with net UTXOs
        # created, so a 46 kB, 1473-output transaction is charged ~40x the base
        # work of an ordinary spend -- ~40 expected Cuckatoo cycle solves for one
        # setup transaction. Neither fixture asserts anything about cycle
        # validity; GrindTransactionPow names this exact case as what
        # -txpownocycle is for. The proof-hash target check stays live, so the
        # size-scaled difficulty is still exercised, without solving E19 cycles
        # for it.
        self.extra_args = [["-txpownocycle=1"] for i in range(self.num_nodes)]
        self.setup_clean_chain = True
        # whitelist peers to speed up tx relay / relaypool sync
        self.noban_tx_relay = True
        self.rpc_timeout = 90  # to prevent timeouts in `test_transaction_too_large`

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()

    def setup_network(self):
        self.setup_nodes()

        self.connect_nodes(0, 1)
        self.connect_nodes(1, 2)
        self.connect_nodes(0, 2)
        self.connect_nodes(0, 3)

    def lock_outputs_type(self, vault, outputtype):
        """
        Only allow UTXOs of the given type
        """
        if outputtype in ["p2pkh", "pkh"]:
            prefixes = ["pkh(", "sh(multi("]
        elif outputtype in ["bech32", "wpkh"]:
            prefixes = ["wpkh(", "wsh("]
        else:
            assert False, f"Unknown output type {outputtype}"

        to_lock = []
        for utxo in vault.listunspent():
            if "desc" in utxo:
                for prefix in prefixes:
                    if utxo["desc"].startswith(prefix):
                        to_lock.append({"txid": utxo["txid"], "vout": utxo["vout"]})
        vault.lockunspent(False, to_lock)

    def unlock_utxos(self, vault):
        """
        Unlock all UTXOs except the tracked one
        """
        to_keep = []
        if self.tracked_utxo is not None:
            to_keep.append(self.tracked_utxo)
        vault.lockunspent(True)
        vault.lockunspent(False, to_keep)

    def run_test(self):
        self.tracked_utxo = None
        self.log.info("Connect nodes, generate blocks, and sync")
        self.generate(self.nodes[2], 1)
        self.generate(self.nodes[0], 121)

        self.test_add_inputs_default_value()
        self.test_preset_inputs_selection()
        self.test_weight_calculation()
        self.test_weight_limits()
        self.test_change_position()
        self.test_simple()
        self.test_simple_two_coins()
        self.test_simple_two_outputs()
        self.test_change()
        self.test_no_change()
        self.test_invalid_option()
        self.test_invalid_change_address()
        self.test_valid_change_address()
        self.test_change_type()
        self.test_coin_selection()
        self.test_two_vin()
        self.test_two_vin_two_vout()
        self.test_invalid_input()
        self.test_accounting_p2pkh()
        self.test_accounting_p2pkh_multi_out()
        self.test_p2sh_funding()
        self.test_accounting_4of5()
        self.test_spend_2of2()
        self.test_locked_vault()
        self.test_many_inputs_accounting()
        self.test_many_inputs_send()
        self.test_op_return()
        self.test_tracked_funding()
        self.test_all_tracked_funds()
        self.test_feeless_result()
        self.test_address_reuse()
        self.test_transaction_too_large()
        self.test_include_unsafe()
        self.test_external_inputs()
        self.test_near_exact_funding()
        self.test_input_confs_control()
        self.test_duplicate_outputs()

    def test_duplicate_outputs(self):
        self.log.info("Test deserializing and funding a transaction with duplicate outputs")
        self.nodes[1].createvault("fundtx_duplicate_outputs")
        w = self.nodes[1].get_vault_rpc("fundtx_duplicate_outputs")

        addr = w.getnewaddress(address_type="bech32")
        self.nodes[0].sendtoaddress(addr, 5)
        self.generate(self.nodes[0], 1)

        address = self.nodes[0].getnewaddress("bech32")
        tx = CTransaction()
        tx.vin = []
        tx.vout = [CTxOut(1 * COIN, bytearray(address_to_scriptpubkey(address)))] * 2
        tx.nLockTime = 0
        tx_hex = tx.serialize().hex()
        res = w.fundrawtransaction(tx_hex, add_inputs=True)
        signed_res = w.signrawtransactionwithvault(res["hex"])
        txid = w.sendrawtransaction(signed_res["hex"])
        assert self.nodes[1].getrawtransaction(txid)

    def test_change_position(self):
        """Ensure setting change_position in fundraw with an exact match is handled properly."""
        self.log.info("Test fundrawtxn change_position option")
        rawmatch = self.nodes[2].createrawtransaction([], [{self.nodes[2].getnewaddress(): self.nodes[2].getbalance()}])
        rawmatch = self.nodes[2].fundrawtransaction(rawmatch, change_position=1)
        assert_equal(rawmatch["changepos"], -1)

        self.nodes[3].createvault(vault_name="wtrack", disable_private_keys=True)
        wtrack = self.nodes[3].get_vault_rpc('wtrack')
        tracked_address = self.nodes[0].getnewaddress()
        tracked_pubkey = self.nodes[0].getaddressinfo(tracked_address)["pubkey"]
        self.tracked_amount = Decimal(200)
        wtrack.importpubkey(tracked_pubkey, "", True)
        self.tracked_utxo = self.create_outpoints(self.nodes[0], outputs=[{tracked_address: self.tracked_amount}])[0]

        # Lock UTXO so nodes[0] doesn't accidentally spend it
        self.nodes[0].lockunspent(False, [self.tracked_utxo])

        self.nodes[0].sendtoaddress(self.nodes[3].get_vault_rpc(self.default_vault_name).getnewaddress(), self.tracked_amount / 10)

        self.nodes[0].sendtoaddress(self.nodes[2].getnewaddress(), 1.5)
        self.nodes[0].sendtoaddress(self.nodes[2].getnewaddress(), 1.0)
        self.nodes[0].sendtoaddress(self.nodes[2].getnewaddress(), 5.0)

        self.generate(self.nodes[0], 1)

        wtrack.unloadvault()

    def test_simple(self):
        self.log.info("Test fundrawtxn")
        inputs  = [ ]
        outputs = [{self.nodes[0].getnewaddress() : 1.0}]
        rawtx   = self.nodes[2].createrawtransaction(inputs, outputs)
        rawtxfund = self.nodes[2].fundrawtransaction(rawtx)
        dec_tx  = self.nodes[2].decoderawtransaction(rawtxfund['hex'])
        assert len(dec_tx['vin']) > 0  #test that we have enough inputs

    def test_simple_two_coins(self):
        self.log.info("Test fundrawtxn with 2 coins")
        inputs  = [ ]
        outputs = [{self.nodes[0].getnewaddress() : 2.2}]
        rawtx   = self.nodes[2].createrawtransaction(inputs, outputs)
        rawtxfund = self.nodes[2].fundrawtransaction(rawtx)
        dec_tx  = self.nodes[2].decoderawtransaction(rawtxfund['hex'])
        assert len(dec_tx['vin']) > 0  #test if we have enough inputs
        assert_equal(dec_tx['vin'][0]['input_script']['hex'], '')

    def test_simple_two_outputs(self):
        self.log.info("Test fundrawtxn with 2 outputs")

        inputs  = [ ]
        outputs = [{self.nodes[0].getnewaddress() : 2.6}, {self.nodes[1].getnewaddress() : 2.5}]
        rawtx   = self.nodes[2].createrawtransaction(inputs, outputs)

        rawtxfund = self.nodes[2].fundrawtransaction(rawtx)
        dec_tx  = self.nodes[2].decoderawtransaction(rawtxfund['hex'])

        assert len(dec_tx['vin']) > 0
        assert_equal(dec_tx['vin'][0]['input_script']['hex'], '')

    def test_change(self):
        self.log.info("Test fundrawtxn with a vin > required amount")
        utx = get_unspent(self.nodes[2].listunspent(), 5)

        inputs  = [ {'txid' : utx['txid'], 'vout' : utx['vout']}]
        outputs = [{self.nodes[0].getnewaddress() : 1.0}]
        rawtx   = self.nodes[2].createrawtransaction(inputs, outputs)
        dec_tx  = self.nodes[2].decoderawtransaction(rawtx)
        assert_equal(utx['txid'], dec_tx['vin'][0]['txid'])

        rawtxfund = self.nodes[2].fundrawtransaction(rawtx)
        assert "fee" not in rawtxfund
        dec_tx  = self.nodes[2].decoderawtransaction(rawtxfund['hex'])
        totalOut = 0
        for out in dec_tx['vout']:
            totalOut += out['value']

        assert_equal(totalOut, utx['amount'])

    def test_no_change(self):
        self.log.info("Test fundrawtxn not having a change output")
        utx = get_unspent(self.nodes[2].listunspent(), 5)

        inputs  = [ {'txid' : utx['txid'], 'vout' : utx['vout']}]
        outputs = [{self.nodes[0].getnewaddress(): Decimal(5.0)}]
        rawtx   = self.nodes[2].createrawtransaction(inputs, outputs)
        dec_tx  = self.nodes[2].decoderawtransaction(rawtx)
        assert_equal(utx['txid'], dec_tx['vin'][0]['txid'])

        rawtxfund = self.nodes[2].fundrawtransaction(rawtx)
        assert "fee" not in rawtxfund
        dec_tx  = self.nodes[2].decoderawtransaction(rawtxfund['hex'])
        totalOut = 0
        for out in dec_tx['vout']:
            totalOut += out['value']

        assert_equal(rawtxfund['changepos'], -1)
        assert_equal(totalOut, utx['amount'])

    def test_invalid_option(self):
        self.log.info("Test fundrawtxn with an invalid option")
        utx = get_unspent(self.nodes[2].listunspent(), 5)

        inputs  = [ {'txid' : utx['txid'], 'vout' : utx['vout']} ]
        outputs = [{self.nodes[0].getnewaddress() : Decimal(4.0)}]
        rawtx   = self.nodes[2].createrawtransaction(inputs, outputs)
        dec_tx  = self.nodes[2].decoderawtransaction(rawtx)
        assert_equal(utx['txid'], dec_tx['vin'][0]['txid'])

        assert_raises_rpc_error(-8, "Unknown named parameter foo", self.nodes[2].fundrawtransaction, rawtx, foo='bar')

        # reserveChangeKey was deprecated and is now removed
        assert_raises_rpc_error(-8, "Unknown named parameter reserveChangeKey", lambda: self.nodes[2].fundrawtransaction(hexstring=rawtx, reserveChangeKey=True))

    def test_invalid_change_address(self):
        self.log.info("Test fundrawtxn with an invalid change address")
        utx = get_unspent(self.nodes[2].listunspent(), 5)

        inputs  = [ {'txid' : utx['txid'], 'vout' : utx['vout']} ]
        outputs = [{self.nodes[0].getnewaddress() : Decimal(4.0)}]
        rawtx   = self.nodes[2].createrawtransaction(inputs, outputs)
        dec_tx  = self.nodes[2].decoderawtransaction(rawtx)
        assert_equal(utx['txid'], dec_tx['vin'][0]['txid'])

        assert_raises_rpc_error(-5, "Change address must be a valid Quicksilver address", self.nodes[2].fundrawtransaction, rawtx, change_address='foobar')
        assert_raises_rpc_error(-8, "Use change_address instead of changeAddress", self.nodes[2].fundrawtransaction, rawtx, {"changeAddress": "foobar"})
        assert_raises_rpc_error(-8, "Use change_position instead of changePosition", self.nodes[2].fundrawtransaction, rawtx, {"changePosition": 0})
        assert_raises_rpc_error(-8, "Use lock_unspents instead of lockUnspents", self.nodes[2].fundrawtransaction, rawtx, {"lockUnspents": True})

    def test_valid_change_address(self):
        self.log.info("Test fundrawtxn with a provided change address")
        utx = get_unspent(self.nodes[2].listunspent(), 5)

        inputs  = [ {'txid' : utx['txid'], 'vout' : utx['vout']} ]
        outputs = [{self.nodes[0].getnewaddress() : Decimal(4.0)}]
        rawtx   = self.nodes[2].createrawtransaction(inputs, outputs)
        dec_tx  = self.nodes[2].decoderawtransaction(rawtx)
        assert_equal(utx['txid'], dec_tx['vin'][0]['txid'])

        change = self.nodes[2].getnewaddress()
        assert_raises_rpc_error(-8, "change_position out of bounds", self.nodes[2].fundrawtransaction, rawtx, change_address=change, change_position=2)
        rawtxfund = self.nodes[2].fundrawtransaction(rawtx, change_address=change, change_position=0)
        dec_tx  = self.nodes[2].decoderawtransaction(rawtxfund['hex'])
        out = dec_tx['vout'][0]
        assert_equal(change, out['output_script']['address'])

    def test_change_type(self):
        self.log.info("Test fundrawtxn with a provided change type")
        utx = get_unspent(self.nodes[2].listunspent(), 5)

        inputs  = [ {'txid' : utx['txid'], 'vout' : utx['vout']} ]
        outputs = [{self.nodes[0].getnewaddress() : Decimal(4.0)}]
        rawtx   = self.nodes[2].createrawtransaction(inputs, outputs)
        assert_raises_rpc_error(-3, "JSON value of type null is not of expected type string", self.nodes[2].fundrawtransaction, rawtx, change_type=None)
        assert_raises_rpc_error(-5, "Unknown change type ''", self.nodes[2].fundrawtransaction, rawtx, change_type='')
        rawtx = self.nodes[2].fundrawtransaction(rawtx, change_type='bech32')
        dec_tx = self.nodes[2].decoderawtransaction(rawtx['hex'])
        assert_equal('witness_v0_keyhash', dec_tx['vout'][rawtx['changepos']]['output_script']['type'])

    def test_coin_selection(self):
        self.log.info("Test fundrawtxn with a vin < required amount")
        utx = get_unspent(self.nodes[2].listunspent(), 1)

        inputs  = [ {'txid' : utx['txid'], 'vout' : utx['vout']}]
        outputs = [{self.nodes[0].getnewaddress() : 1.0}]
        rawtx   = self.nodes[2].createrawtransaction(inputs, outputs)

        # 4-byte version + 1-byte vin count + 36-byte prevout then script_len
        rawtx = rawtx[:82] + "0100" + rawtx[84:]

        dec_tx  = self.nodes[2].decoderawtransaction(rawtx)
        assert_equal(utx['txid'], dec_tx['vin'][0]['txid'])
        assert_equal("00", dec_tx['vin'][0]['input_script']['hex'])

        # With zero fees, the preset input exactly funds the output.
        rawtxfund_no_extra = self.nodes[2].fundrawtransaction(rawtx, add_inputs=False)
        assert_equal(len(self.nodes[2].decoderawtransaction(rawtxfund_no_extra['hex'])['vin']), 1)
        # add_inputs is enabled by default
        rawtxfund = self.nodes[2].fundrawtransaction(rawtx)

        dec_tx  = self.nodes[2].decoderawtransaction(rawtxfund['hex'])
        matchingOuts = 0
        for i, out in enumerate(dec_tx['vout']):
            if out['output_script']['address'] in output_addresses(outputs):
                matchingOuts+=1
            else:
                assert_equal(i, rawtxfund['changepos'])

        assert_equal(utx['txid'], dec_tx['vin'][0]['txid'])
        assert_equal("00", dec_tx['vin'][0]['input_script']['hex'])

        assert_equal(matchingOuts, 1)
        assert_equal(rawtxfund['changepos'], -1)
        assert_equal(len(dec_tx['vout']), 1)

    def test_two_vin(self):
        self.log.info("Test fundrawtxn with 2 vins")
        utx = get_unspent(self.nodes[2].listunspent(), 1)
        utx2 = get_unspent(self.nodes[2].listunspent(), 5)

        inputs  = [ {'txid' : utx['txid'], 'vout' : utx['vout']},{'txid' : utx2['txid'], 'vout' : utx2['vout']} ]
        outputs = [{self.nodes[0].getnewaddress() : 6.0}]
        rawtx   = self.nodes[2].createrawtransaction(inputs, outputs)
        dec_tx  = self.nodes[2].decoderawtransaction(rawtx)
        assert_equal(utx['txid'], dec_tx['vin'][0]['txid'])

        rawtxfund_no_extra = self.nodes[2].fundrawtransaction(rawtx, add_inputs=False)
        assert_equal(len(self.nodes[2].decoderawtransaction(rawtxfund_no_extra['hex'])['vin']), 2)
        rawtxfund = self.nodes[2].fundrawtransaction(rawtx, add_inputs=True)
        dec_tx  = self.nodes[2].decoderawtransaction(rawtxfund['hex'])
        matchingOuts = 0
        for out in dec_tx['vout']:
            if out['output_script']['address'] in output_addresses(outputs):
                matchingOuts+=1

        assert_equal(matchingOuts, 1)
        assert_equal(rawtxfund['changepos'], -1)
        assert_equal(len(dec_tx['vout']), 1)

        matchingIns = 0
        for vinOut in dec_tx['vin']:
            for vinIn in inputs:
                if vinIn['txid'] == vinOut['txid']:
                    matchingIns+=1

        assert_equal(matchingIns, 2) #we now must see two vins identical to vins given as params

    def test_two_vin_two_vout(self):
        self.log.info("Test fundrawtxn with 2 vins and 2 vouts")
        utx = get_unspent(self.nodes[2].listunspent(), 1)
        utx2 = get_unspent(self.nodes[2].listunspent(), 5)

        inputs  = [ {'txid' : utx['txid'], 'vout' : utx['vout']},{'txid' : utx2['txid'], 'vout' : utx2['vout']} ]
        outputs = [{self.nodes[0].getnewaddress() : 6.0}, {self.nodes[0].getnewaddress() : 1.0}]
        rawtx   = self.nodes[2].createrawtransaction(inputs, outputs)
        dec_tx  = self.nodes[2].decoderawtransaction(rawtx)
        assert_equal(utx['txid'], dec_tx['vin'][0]['txid'])

        # Should fail without add_inputs:
        assert_raises_rpc_error(-4, ERR_NOT_ENOUGH_PRESET_INPUTS, self.nodes[2].fundrawtransaction, rawtx, add_inputs=False)
        rawtxfund = self.nodes[2].fundrawtransaction(rawtx, add_inputs=True)

        dec_tx  = self.nodes[2].decoderawtransaction(rawtxfund['hex'])
        matchingOuts = 0
        for out in dec_tx['vout']:
            if out['output_script']['address'] in output_addresses(outputs):
                matchingOuts+=1

        assert_equal(matchingOuts, 2)
        assert_equal(len(dec_tx['vout']), 3)

    def test_invalid_input(self):
        self.log.info("Test fundrawtxn with an invalid vin")
        txid = "1c7f966dab21119bac53213a2bc7532bff1fa844c124fd750a7d0b1332440bd1"
        vout = 0
        inputs  = [ {'txid' : txid, 'vout' : vout} ] #invalid vin!
        outputs = [{self.nodes[0].getnewaddress() : 1.0}]
        rawtx   = self.nodes[2].createrawtransaction(inputs, outputs)
        assert_raises_rpc_error(-4, "Unable to find UTXO for external input", self.nodes[2].fundrawtransaction, rawtx)

    def test_accounting_p2pkh(self):
        """Compare zero-fee accounting of a standard pubkeyhash transaction."""
        self.log.info("Test fundrawtxn p2pkh feeless accounting")
        self.lock_outputs_type(self.nodes[0], "p2pkh")
        inputs = []
        outputs = [{self.nodes[1].getnewaddress():1.1}]
        rawtx = self.nodes[0].createrawtransaction(inputs, outputs)
        fundedTx = self.nodes[0].fundrawtransaction(rawtx)

        # Create same transaction over sendtoaddress.
        txId = self.nodes[0].sendtoaddress(self.nodes[1].getnewaddress(), 1.1)
        assert "fee" not in fundedTx
        assert "fees" not in self.nodes[0].getrelaypoolentry(txId)

        self.unlock_utxos(self.nodes[0])

    def test_accounting_p2pkh_multi_out(self):
        """Compare zero-fee accounting of a standard pubkeyhash transaction with multiple outputs."""
        self.log.info("Test fundrawtxn p2pkh feeless accounting with multiple outputs")
        self.lock_outputs_type(self.nodes[0], "p2pkh")
        inputs = []
        amounts = {self.nodes[1].getnewaddress():1.1, self.nodes[1].getnewaddress():1.2, self.nodes[1].getnewaddress():0.1, self.nodes[1].getnewaddress():1.3, self.nodes[1].getnewaddress():0.2, self.nodes[1].getnewaddress():0.3}
        outputs = [{address: amount} for address, amount in amounts.items()]
        rawtx = self.nodes[0].createrawtransaction(inputs, outputs)
        fundedTx = self.nodes[0].fundrawtransaction(rawtx)

        # Create same transaction over sendtoaddress.
        txId = self.nodes[0].sendmany(amounts)
        assert "fee" not in fundedTx
        assert "fees" not in self.nodes[0].getrelaypoolentry(txId)

        self.unlock_utxos(self.nodes[0])

    def test_p2sh_funding(self):
        """Fund a 2-of-2 multisig p2sh transaction."""
        self.lock_outputs_type(self.nodes[0], "p2pkh")
        # Create 2-of-2 addr.
        addr1 = self.nodes[1].getnewaddress()
        addr2 = self.nodes[1].getnewaddress()

        addr1Obj = self.nodes[1].getaddressinfo(addr1)
        addr2Obj = self.nodes[1].getaddressinfo(addr2)

        mSigObj = self.nodes[3].createmultisig(2, [addr1Obj['pubkey'], addr2Obj['pubkey']], "base58")['address']

        inputs = []
        outputs = [{mSigObj:1.1}]
        rawtx = self.nodes[0].createrawtransaction(inputs, outputs)
        fundedTx = self.nodes[0].fundrawtransaction(rawtx)

        # Create same transaction over sendtoaddress.
        txId = self.nodes[0].sendtoaddress(mSigObj, 1.1)
        assert "fee" not in fundedTx
        assert "fees" not in self.nodes[0].getrelaypoolentry(txId)

        self.unlock_utxos(self.nodes[0])

    def test_accounting_4of5(self):
        """Compare zero-fee accounting of a multisig transaction."""
        self.log.info("Test fundrawtxn feeless accounting with 4-of-5 addresses")
        self.lock_outputs_type(self.nodes[0], "p2pkh")

        # Create 4-of-5 addr.
        addr1 = self.nodes[1].getnewaddress()
        addr2 = self.nodes[1].getnewaddress()
        addr3 = self.nodes[1].getnewaddress()
        addr4 = self.nodes[1].getnewaddress()
        addr5 = self.nodes[1].getnewaddress()

        addr1Obj = self.nodes[1].getaddressinfo(addr1)
        addr2Obj = self.nodes[1].getaddressinfo(addr2)
        addr3Obj = self.nodes[1].getaddressinfo(addr3)
        addr4Obj = self.nodes[1].getaddressinfo(addr4)
        addr5Obj = self.nodes[1].getaddressinfo(addr5)

        mSigObj = self.nodes[1].createmultisig(
            4,
            [
                addr1Obj['pubkey'],
                addr2Obj['pubkey'],
                addr3Obj['pubkey'],
                addr4Obj['pubkey'],
                addr5Obj['pubkey'],
            ]
        )['address']

        inputs = []
        outputs = [{mSigObj:1.1}]
        rawtx = self.nodes[0].createrawtransaction(inputs, outputs)
        fundedTx = self.nodes[0].fundrawtransaction(rawtx)

        # Create same transaction over sendtoaddress.
        txId = self.nodes[0].sendtoaddress(mSigObj, 1.1)
        assert "fee" not in fundedTx
        assert "fees" not in self.nodes[0].getrelaypoolentry(txId)

        self.unlock_utxos(self.nodes[0])

    def test_spend_2of2(self):
        """Spend a 2-of-2 multisig transaction over fundraw."""
        self.log.info("Test fundpsqt spending 2-of-2 multisig")

        # Create 2-of-2 addr.
        addr1 = self.nodes[2].getnewaddress()
        addr2 = self.nodes[2].getnewaddress()

        addr1Obj = self.nodes[2].getaddressinfo(addr1)
        addr2Obj = self.nodes[2].getaddressinfo(addr2)

        self.nodes[2].createvault(vault_name='wmulti', disable_private_keys=True)
        wmulti = self.nodes[2].get_vault_rpc('wmulti')
        w2 = self.nodes[2].get_vault_rpc(self.default_vault_name)
        mSigObj = wmulti.addmultisigaddress(
            2,
            [
                addr1Obj['pubkey'],
                addr2Obj['pubkey'],
            ]
        )['address']

        # Send 1.2 Hg to msig addr.
        self.nodes[0].sendtoaddress(mSigObj, 1.2)
        self.generate(self.nodes[0], 1)

        oldBalance = self.nodes[1].getbalance()
        inputs = []
        outputs = [{self.nodes[1].getnewaddress():1.1}]
        funded_psqt = wmulti.vaultcreatefundedpsqt(inputs=inputs, outputs=outputs, change_address=w2.getrawchangeaddress())['psqt']

        signed_psqt = w2.vaultprocesspsqt(funded_psqt)
        self.nodes[2].sendrawtransaction(signed_psqt['hex'])
        self.generate(self.nodes[2], 1)

        # Make sure funds are received at node1.
        assert_equal(oldBalance+Decimal('1.10000000'), self.nodes[1].getbalance())

        wmulti.unloadvault()

    def test_locked_vault(self):
        self.log.info("Test fundrawtxn with locked vault and hardened derivation")

        df_vault = self.nodes[1].get_vault_rpc(self.default_vault_name)
        self.nodes[1].createvault(vault_name="locked_vault")
        vault = self.nodes[1].get_vault_rpc("locked_vault")
        # Encrypt vault and import descriptors
        vault.encryptvault("test")

        with VaultUnlock(vault, "test"):
            vault.importdescriptors([{
                'desc': descsum_create('wpkh(sqrv1wkfAGt6m8urpXYkbTYqv26XAnqRTNEaPoALKFfQBhsoh4Q1Wn4drYJkDSigKG55NuPBcEZKUdXFkpQU3W7nx2vJ6G3dSViMmaz3NfVue16/0h/*h)'),
                'timestamp': 'now',
                'active': True
            },
            {
                'desc': descsum_create('wpkh(sqrv1wkfAGt6m8urpXYkbTYqv26XAnqRTNEaPoALKFfQBhsoh4Q1Wn4drYJkDSigKG55NuPBcEZKUdXFkpQU3W7nx2vJ6G3dSViMmaz3NfVue16/1h/*h)'),
                'timestamp': 'now',
                'active': True,
                'internal': True
            }])

        # Add some balance after descriptor import so the funded address remains tracked.
        self.nodes[0].sendtoaddress(vault.getnewaddress(), 1)
        self.generate(self.nodes[0], 1)

        # Drain the keypool.
        vault.getrawchangeaddress()

        # Choose input
        inputs = vault.listunspent()

        # Quicksilver is feeless (in == out), so spending the whole input value
        # already produces a changeless transaction.
        value = inputs[0]["amount"]

        outputs = [{self.nodes[0].getnewaddress():value}]
        rawtx = vault.createrawtransaction(inputs, outputs)
        # fund a transaction that does not require a new key for the change output
        funded_tx = vault.fundrawtransaction(rawtx)
        assert_equal(funded_tx["changepos"], -1)

        # fund a transaction that requires a new key for the change output
        # creating the key must be impossible because the vault is locked
        outputs = [{self.nodes[0].getnewaddress():value - Decimal("0.1")}]
        rawtx = vault.createrawtransaction(inputs, outputs)
        assert_raises_rpc_error(-4, "Transaction needs a change address, but we can't generate it.", vault.fundrawtransaction, rawtx)

        # Refill the keypool.
        with VaultUnlock(vault, "test"):
            vault.keypoolrefill(8) #need to refill the keypool to get an internal change address

        assert_raises_rpc_error(-13, "vaultpassphrase", vault.sendtoaddress, self.nodes[0].getnewaddress(), 1.2)

        oldBalance = self.nodes[0].getbalance()

        inputs = []
        outputs = [{self.nodes[0].getnewaddress(): Decimal("0.9")}]
        rawtx = vault.createrawtransaction(inputs, outputs)
        fundedTx = vault.fundrawtransaction(rawtx)
        assert fundedTx["changepos"] != -1

        # Now we need to unlock.
        with VaultUnlock(vault, "test"):
            signedTx = vault.signrawtransactionwithvault(fundedTx['hex'])
            vault.sendrawtransaction(signedTx['hex'])
            self.generate(self.nodes[1], 1)

            # Make sure funds are received at node1.
            assert_greater_than_or_equal(self.nodes[0].getbalance(), oldBalance + Decimal("0.9"))

            # Restore pre-test vault state
            vault.sendall(recipients=[df_vault.getnewaddress(), df_vault.getnewaddress(), df_vault.getnewaddress()])
        vault.unloadvault()
        self.generate(self.nodes[1], 1)

    def test_many_inputs_accounting(self):
        """Multiple (~19) inputs tx test | Compare zero-fee accounting."""
        self.log.info("Test fundrawtxn feeless accounting with many inputs")

        # Empty node1, send some small coins from node0 to node1.
        self.nodes[1].sendall(recipients=[self.nodes[0].getnewaddress()])
        self.generate(self.nodes[1], 1)

        for _ in range(20):
            self.nodes[0].sendtoaddress(self.nodes[1].getnewaddress(), 0.01)
        self.generate(self.nodes[0], 1)

        # Fund a tx with ~20 small inputs.
        inputs = []
        amounts = {self.nodes[0].getnewaddress():0.15, self.nodes[0].getnewaddress():0.04}
        outputs = [{address: amount} for address, amount in amounts.items()]
        rawtx = self.nodes[1].createrawtransaction(inputs, outputs)
        fundedTx = self.nodes[1].fundrawtransaction(rawtx)

        # Create same transaction over sendtoaddress.
        txId = self.nodes[1].sendmany(amounts)
        assert "fee" not in fundedTx
        assert "fees" not in self.nodes[1].getrelaypoolentry(txId)

    def test_many_inputs_send(self):
        """Multiple (~19) inputs tx test | sign/send."""
        self.log.info("Test fundrawtxn sign+send with many inputs")

        # Again, empty node1, send some small coins from node0 to node1.
        self.nodes[1].sendall(recipients=[self.nodes[0].getnewaddress()])
        self.generate(self.nodes[1], 1)

        for _ in range(20):
            self.nodes[0].sendtoaddress(self.nodes[1].getnewaddress(), 0.01)
        self.generate(self.nodes[0], 1)

        # Fund a tx with ~20 small inputs.
        oldBalance = self.nodes[0].getbalance()

        inputs = []
        outputs = [{self.nodes[0].getnewaddress():0.15}, {self.nodes[0].getnewaddress():0.04}]
        rawtx = self.nodes[1].createrawtransaction(inputs, outputs)
        fundedTx = self.nodes[1].fundrawtransaction(rawtx)
        fundedAndSignedTx = self.nodes[1].signrawtransactionwithvault(fundedTx['hex'])
        self.nodes[1].sendrawtransaction(fundedAndSignedTx['hex'])
        self.generate(self.nodes[1], 1)
        assert_greater_than_or_equal(self.nodes[0].getbalance(), oldBalance + Decimal("0.19"))

    def test_op_return(self):
        self.log.info("Test fundrawtxn with OP_RETURN and no vin")

        rawtx = self.nodes[2].createrawtransaction([], [{"data": "74657374"}, {self.nodes[2].getnewaddress(): 1}])
        dec_tx  = self.nodes[2].decoderawtransaction(rawtx)

        assert_equal(len(dec_tx['vin']), 0)
        assert_equal(len(dec_tx['vout']), 2)

        rawtxfund = self.nodes[2].fundrawtransaction(rawtx)
        dec_tx  = self.nodes[2].decoderawtransaction(rawtxfund['hex'])

        assert_greater_than(len(dec_tx['vin']), 0) # at least one vin
        assert_greater_than_or_equal(len(dec_tx['vout']), 2)
        assert any(out["output_script"]["type"] == "nulldata" for out in dec_tx["vout"])

    def test_tracked_funding(self):
        self.log.info("Test fundrawtxn from a key-disabled tracking vault")

        inputs = []
        outputs = [{self.nodes[2].getnewaddress(): self.tracked_amount / 2}]
        rawtx = self.nodes[3].createrawtransaction(inputs, outputs)

        self.nodes[3].loadvault('wtrack')
        wtrack = self.nodes[3].get_vault_rpc('wtrack')
        # Setup change addresses for the tracking vault
        desc_import = [{
            "desc": descsum_create("wpkh(squb6UShJgvLuWbXjMRbSTY6wi5iU8y2qWsaoDXsoeUy4X3Mgt6KGWHzuJQt7HZWA3zovAPBjp2eUqx1rHY3TDMehr5xWqunHpMftHsZkJ72Lwq/1/*)"),
            "timestamp": "now",
            "internal": True,
            "active": True,
            "keypool": True,
            "range": [0, 100],
        }]
        wtrack.importdescriptors(desc_import)

        result = wtrack.fundrawtransaction(rawtx)
        res_dec = self.nodes[0].decoderawtransaction(result["hex"])
        assert_equal(len(res_dec["vin"]), 1)
        assert_equal(res_dec["vin"][0]["txid"], self.tracked_utxo['txid'])

        assert "fee" not in result
        assert_greater_than(result["changepos"], -1)

        wtrack.unloadvault()

    def test_all_tracked_funds(self):
        self.log.info("Test fundrawtxn using the entirety of the tracked funds")

        inputs = []
        outputs = [{self.nodes[2].getnewaddress(): self.tracked_amount}]
        rawtx = self.nodes[3].createrawtransaction(inputs, outputs)

        self.nodes[3].loadvault('wtrack')
        wtrack = self.nodes[3].get_vault_rpc('wtrack')
        w3 = self.nodes[3].get_vault_rpc(self.default_vault_name)
        result = wtrack.fundrawtransaction(rawtx, change_address=w3.getrawchangeaddress())
        res_dec = self.nodes[0].decoderawtransaction(result["hex"])
        assert_equal(len(res_dec["vin"]), 1)
        assert res_dec["vin"][0]["txid"] == self.tracked_utxo['txid']

        assert_equal(result["changepos"], -1)
        assert "fee" not in result
        assert_equal(res_dec["vout"][0]["value"], self.tracked_amount)

        signedtx = wtrack.signrawtransactionwithvault(result["hex"])
        assert not signedtx["complete"]
        signedtx = self.nodes[0].signrawtransactionwithvault(signedtx["hex"])
        assert signedtx["complete"]
        self.nodes[0].sendrawtransaction(signedtx["hex"])
        self.generate(self.nodes[0], 1)

        wtrack.unloadvault()

    def test_feeless_result(self):
        self.log.info("Test fundrawtxn creates feeless transactions")
        node = self.nodes[3]
        # Make sure there is exactly one input so coin selection can't skew the result.
        assert_equal(len(self.nodes[3].listunspent(1)), 1)
        inputs = []
        outputs = [{node.getnewaddress() : 1}]
        rawtx = node.createrawtransaction(inputs, outputs)

        result = node.fundrawtransaction(rawtx)

        assert "fee" not in result
        assert "fee" not in node.fundrawtransaction(rawtx)

    def test_address_reuse(self):
        """Test no address reuse occurs."""
        self.log.info("Test fundrawtxn does not reuse addresses")

        rawtx = self.nodes[3].createrawtransaction(inputs=[], outputs=[{self.nodes[3].getnewaddress(): 1}])
        result3 = self.nodes[3].fundrawtransaction(rawtx)
        res_dec = self.nodes[0].decoderawtransaction(result3["hex"])
        changeaddress = ""
        for out in res_dec['vout']:
            if out['value'] > 1.0:
                changeaddress += out['output_script']['address']
        assert changeaddress != ""
        nextaddr = self.nodes[3].getnewaddress()
        # Now the change address key should be removed from the keypool.
        assert changeaddress != nextaddr

    def test_transaction_too_large(self):
        self.log.info("Test fundrawtx where an exact-value solution would be too large")
        self.nodes[0].createvault("large")
        vault = self.nodes[0].get_vault_rpc(self.default_vault_name)
        recipient = self.nodes[0].get_vault_rpc("large")
        outputs = {}
        rawtx = recipient.createrawtransaction([], [{vault.getnewaddress(): 147.99899260}])

        # Make 1500 0.1 Hg outputs. The amount targeted for funding has an exact-value
        # solution using these outputs, but selecting them would make the transaction too
        # large. For now we just check that we get an error.
        # First, force the vault to bulk-generate the addresses we'll need.
        recipient.keypoolrefill(1500)
        for _ in range(1500):
            outputs[recipient.getnewaddress()] = 0.1
        vault.sendmany(outputs)
        self.generate(self.nodes[0], 10)
        assert_raises_rpc_error(-4, "The inputs size exceeds the maximum weight. "
                                    "Please try sending a smaller amount or manually consolidating your vault's UTXOs",
                                recipient.fundrawtransaction, rawtx)
        self.nodes[0].unloadvault("large")

    def test_external_inputs(self):
        self.log.info("Test funding with external inputs")
        privkey, _ = generate_keypair(wif=True)
        self.nodes[2].createvault("extfund")
        vault = self.nodes[2].get_vault_rpc("extfund")

        # Make a weird but signable script. sh(pkh()) descriptor accomplishes this
        desc = descsum_create("sh(pkh({}))".format(privkey))
        res = self.nodes[0].importdescriptors([{"desc": desc, "timestamp": "now"}])
        assert res[0]["success"]
        addr = self.nodes[0].deriveaddresses(desc)[0]
        addr_info = self.nodes[0].getaddressinfo(addr)

        self.nodes[0].sendtoaddress(addr, 10)
        self.nodes[0].sendtoaddress(vault.getnewaddress(), 10)
        self.generate(self.nodes[0], 6)
        ext_utxo = self.nodes[0].listunspent(addresses=[addr])[0]

        # An external input without solving data should result in an error
        raw_tx = vault.createrawtransaction([ext_utxo], [{self.nodes[0].getnewaddress(): ext_utxo["amount"] / 2}])
        assert_raises_rpc_error(-4, "Not solvable pre-selected input COutPoint(%s, %s)" % (ext_utxo["txid"][0:10], ext_utxo["vout"]), vault.fundrawtransaction, raw_tx)

        # Error conditions
        assert_raises_rpc_error(-5, 'Pubkey "not a pubkey" must be a hex string', vault.fundrawtransaction, raw_tx, solving_data={"pubkeys":["not a pubkey"]})
        assert_raises_rpc_error(-5, 'Pubkey "01234567890a0b0c0d0e0f" must have a length of either 33 or 65 bytes', vault.fundrawtransaction, raw_tx, solving_data={"pubkeys":["01234567890a0b0c0d0e0f"]})
        assert_raises_rpc_error(-5, "'not a script' is not hex", vault.fundrawtransaction, raw_tx, solving_data={"scripts":["not a script"]})
        assert_raises_rpc_error(-8, "Unable to parse descriptor 'not a descriptor'", vault.fundrawtransaction, raw_tx, solving_data={"descriptors":["not a descriptor"]})
        assert_raises_rpc_error(-8, "Invalid parameter, missing vout key", vault.fundrawtransaction, raw_tx, input_weights=[{"txid": ext_utxo["txid"]}])
        assert_raises_rpc_error(-8, "Invalid parameter, vout cannot be negative", vault.fundrawtransaction, raw_tx, input_weights=[{"txid": ext_utxo["txid"], "vout": -1}])
        assert_raises_rpc_error(-8, "Invalid parameter, missing weight key", vault.fundrawtransaction, raw_tx, input_weights=[{"txid": ext_utxo["txid"], "vout": ext_utxo["vout"]}])
        assert_raises_rpc_error(-8, "Invalid parameter, weight cannot be less than 165", vault.fundrawtransaction, raw_tx, input_weights=[{"txid": ext_utxo["txid"], "vout": ext_utxo["vout"], "weight": 164}])
        assert_raises_rpc_error(-8, "Invalid parameter, weight cannot be less than 165", vault.fundrawtransaction, raw_tx, input_weights=[{"txid": ext_utxo["txid"], "vout": ext_utxo["vout"], "weight": -1}])
        assert_raises_rpc_error(-8, "Invalid parameter, weight cannot be greater than", vault.fundrawtransaction, raw_tx, input_weights=[{"txid": ext_utxo["txid"], "vout": ext_utxo["vout"], "weight": 400001}])

        # But funding should work when the solving data is provided
        funded_tx = vault.fundrawtransaction(raw_tx, solving_data={"pubkeys": [addr_info['pubkey']], "scripts": [addr_info["embedded"]["output_script"]]})
        signed_tx = vault.signrawtransactionwithvault(funded_tx['hex'])
        assert not signed_tx['complete']
        signed_tx = self.nodes[0].signrawtransactionwithvault(signed_tx['hex'])
        assert signed_tx['complete']

        funded_tx = vault.fundrawtransaction(raw_tx, solving_data={"descriptors": [desc]})
        signed_tx1 = vault.signrawtransactionwithvault(funded_tx['hex'])
        assert not signed_tx1['complete']
        signed_tx2 = self.nodes[0].signrawtransactionwithvault(signed_tx1['hex'])
        assert signed_tx2['complete']

        unsigned_weight = self.nodes[0].decoderawtransaction(signed_tx1["hex"])["weight"]
        signed_weight = self.nodes[0].decoderawtransaction(signed_tx2["hex"])["weight"]
        # Input's weight is difference between weight of signed and unsigned,
        # and the weight of stuff that didn't change (prevout, sequence, 1 byte of scriptSig)
        input_weight = signed_weight - unsigned_weight + (41 * 4)
        low_input_weight = input_weight // 2
        high_input_weight = input_weight * 2

        # Funding should also work if the input weight is provided
        funded_tx = vault.fundrawtransaction(raw_tx, input_weights=[{"txid": ext_utxo["txid"], "vout": ext_utxo["vout"], "weight": input_weight}])
        signed_tx = vault.signrawtransactionwithvault(funded_tx["hex"])
        signed_tx = self.nodes[0].signrawtransactionwithvault(signed_tx["hex"])
        assert_equal(self.nodes[0].testrelaypoolaccept([signed_tx["hex"]])[0]["allowed"], True)
        assert_equal(signed_tx["complete"], True)
        assert "fee" not in funded_tx

        # Different provided weights are accepted but do not create fees.
        funded_tx2 = vault.fundrawtransaction(raw_tx, input_weights=[{"txid": ext_utxo["txid"], "vout": ext_utxo["vout"], "weight": low_input_weight}])
        assert "fee" not in funded_tx2
        funded_tx2 = vault.fundrawtransaction(raw_tx, input_weights=[{"txid": ext_utxo["txid"], "vout": ext_utxo["vout"], "weight": high_input_weight}])
        assert "fee" not in funded_tx2
        # The provided weight should override the calculated weight when solving data is provided
        funded_tx3 = vault.fundrawtransaction(raw_tx, solving_data={"descriptors": [desc]}, input_weights=[{"txid": ext_utxo["txid"], "vout": ext_utxo["vout"], "weight": high_input_weight}])
        assert "fee" not in funded_tx3
        funded_tx4 = vault.fundrawtransaction(raw_tx, input_weights=[{"txid": ext_utxo["txid"], "vout": ext_utxo["vout"], "weight": high_input_weight}])
        assert "fee" not in funded_tx4

        # Funding with weight at csuint boundaries should not cause problems
        funded_tx = vault.fundrawtransaction(raw_tx, input_weights=[{"txid": ext_utxo["txid"], "vout": ext_utxo["vout"], "weight": 255}])
        funded_tx = vault.fundrawtransaction(raw_tx, input_weights=[{"txid": ext_utxo["txid"], "vout": ext_utxo["vout"], "weight": 65539}])

        self.nodes[2].unloadvault("extfund")

    def test_add_inputs_default_value(self):
        self.log.info("Test 'add_inputs' default value")

        # Create and fund the vault with 5 Hg
        self.nodes[2].createvault("test_preset_inputs")
        vault = self.nodes[2].get_vault_rpc("test_preset_inputs")
        addr1 = vault.getnewaddress(address_type="bech32")
        self.nodes[0].sendtoaddress(addr1, 5)
        self.generate(self.nodes[0], 1)

        # Covered cases:
        # 1. Default add_inputs value with no preset inputs (add_inputs=true):
        #       Expect: automatically add coins from the vault to the tx.
        # 2. Default add_inputs value with preset inputs (add_inputs=false):
        #       Expect: disallow automatic coin selection.
        # 3. Explicit add_inputs=true and preset inputs (with preset inputs not-covering the target amount).
        #       Expect: include inputs from the vault.
        # 4. Explicit add_inputs=true and preset inputs (with preset inputs covering the target amount).
        #       Expect: only preset inputs are used.
        # 5. Explicit add_inputs=true, no preset inputs (same as (1) but with an explicit set):
        #       Expect: include inputs from the vault.
        # 6. Explicit add_inputs=false, no preset inputs:
        #       Expect: failure as we did not provide inputs and the process cannot automatically select coins.

        # Case (1), 'send' command
        # 'add_inputs' value is true unless "inputs" are specified, in such case, add_inputs=false.
        # So, the vault will automatically select coins and create the transaction if only the outputs are provided.
        tx = vault.send(outputs=[{addr1: 3}])
        assert tx["complete"]

        # Case (2), 'send' command
        # Select an input manually, which doesn't cover the entire output amount and
        # verify that the dynamically set 'add_inputs=false' value works.

        # Fund vault with 2 outputs, 5 Hg each.
        addr2 = vault.getnewaddress(address_type="bech32")
        source_tx = self.nodes[0].send(outputs=[{addr1: 5}, {addr2: 5}], change_position=0)
        self.generate(self.nodes[0], 1)

        # Select only one input.
        options = {
            "inputs": [
                {
                    "txid": source_tx["txid"],
                    "vout": 1  # change position was hardcoded to index 0
                }
            ]
        }
        assert_raises_rpc_error(-4, ERR_NOT_ENOUGH_PRESET_INPUTS, vault.send, outputs=[{addr1: 8}], **options)

        # Case (3), Explicit add_inputs=true and preset inputs (with preset inputs not-covering the target amount)
        options["add_inputs"] = True
        options["add_to_vault"] = False
        tx = vault.send(outputs=[{addr1: 8}], **options)
        assert tx["complete"]

        # Case (4), Explicit add_inputs=true and preset inputs (with preset inputs covering the target amount)
        options["inputs"].append({
            "txid": source_tx["txid"],
            "vout": 2  # change position was hardcoded to index 0
        })
        tx = vault.send(outputs=[{addr1: 8}], **options)
        assert tx["complete"]
        # Check that only the preset inputs were added to the tx
        decoded_psqt_inputs = self.nodes[0].decodepsqt(tx["psqt"])['tx']['vin']
        assert_equal(len(decoded_psqt_inputs), 2)
        for input in decoded_psqt_inputs:
            assert_equal(input["txid"], source_tx["txid"])

        # Case (5), assert that inputs are added to the tx by explicitly setting add_inputs=true
        options = {"add_inputs": True, "add_to_vault": True}
        tx = vault.send(outputs=[{addr1: 8}], **options)
        assert tx["complete"]

        # 6. Explicit add_inputs=false, no preset inputs:
        options = {"add_inputs": False}
        assert_raises_rpc_error(-4, ERR_NOT_ENOUGH_PRESET_INPUTS, vault.send, outputs=[{addr1: 3}], **options)

        ################################################

        # Case (1), 'vaultcreatefundedpsqt' command
        # Default add_inputs value with no preset inputs (add_inputs=true)
        inputs = []
        outputs = [{self.nodes[1].getnewaddress(): 8}]
        assert "psqt" in vault.vaultcreatefundedpsqt(inputs=inputs, outputs=outputs)

        # Case (2), 'vaultcreatefundedpsqt' command
        # Default add_inputs value with preset inputs (add_inputs=false).
        inputs = [{
            "txid": source_tx["txid"],
            "vout": 1  # change position was hardcoded to index 0
        }]
        outputs = [{self.nodes[1].getnewaddress(): 8}]
        assert_raises_rpc_error(-4, ERR_NOT_ENOUGH_PRESET_INPUTS, vault.vaultcreatefundedpsqt, inputs=inputs, outputs=outputs)

        # Case (3), Explicit add_inputs=true and preset inputs (with preset inputs not-covering the target amount)
        options["add_inputs"] = True
        assert "psqt" in vault.vaultcreatefundedpsqt(outputs=[{addr1: 8}], inputs=inputs, **options)

        # Case (4), Explicit add_inputs=true and preset inputs (with preset inputs covering the target amount)
        inputs.append({
            "txid": source_tx["txid"],
            "vout": 2  # change position was hardcoded to index 0
        })
        psqt_tx = vault.vaultcreatefundedpsqt(outputs=[{addr1: 8}], inputs=inputs, **options)
        # Check that only the preset inputs were added to the tx
        decoded_psqt_inputs = self.nodes[0].decodepsqt(psqt_tx["psqt"])['tx']['vin']
        assert_equal(len(decoded_psqt_inputs), 2)
        for input in decoded_psqt_inputs:
            assert_equal(input["txid"], source_tx["txid"])

        # Case (5), 'vaultcreatefundedpsqt' command
        # Explicit add_inputs=true, no preset inputs
        options = {
            "add_inputs": True
        }
        assert "psqt" in vault.vaultcreatefundedpsqt(inputs=[], outputs=outputs, **options)

        # Case (6). Explicit add_inputs=false, no preset inputs:
        options = {"add_inputs": False}
        assert_raises_rpc_error(-4, ERR_NOT_ENOUGH_PRESET_INPUTS, vault.vaultcreatefundedpsqt, inputs=[], outputs=outputs, **options)

        self.nodes[2].unloadvault("test_preset_inputs")

    def test_preset_inputs_selection(self):
        self.log.info('Test vault preset inputs are not double-counted or reused in coin selection')

        # Create and fund the vault with 4 UTXO of 5 Hg each (20 Hg total)
        self.nodes[2].createvault("test_preset_inputs_selection")
        vault = self.nodes[2].get_vault_rpc("test_preset_inputs_selection")
        outputs = {}
        for _ in range(4):
            outputs[vault.getnewaddress(address_type="bech32")] = 5
        self.nodes[0].sendmany(outputs)
        self.generate(self.nodes[0], 1)

        # Select the preset inputs
        coins = vault.listunspent()
        preset_inputs = [coins[0], coins[1], coins[2]]

        # Now let's create the tx creation options
        options = {
            "inputs": preset_inputs,
            "add_inputs": True,  # automatically add coins from the vault to fulfill the target
            "add_to_vault": False
        }

        # Attempt to send 29 Hg from a vault that only has 20 Hg. The vault should exclude
        # the preset inputs from the pool of available coins, realize that there is not enough
        # money to fund the 29 Hg payment, and fail with "Insufficient funds".
        #
        # If the vault does not properly exclude preset inputs from the pool of available coins
        # prior to coin selection, it may create a transaction that does not fund the full payment
        # amount, so the recipient no longer receives the selected target of 29 Hg.

        assert_raises_rpc_error(-4, "Insufficient funds", vault.send, outputs=[{vault.getnewaddress(address_type="bech32"): 29}], options=options)

        self.nodes[2].unloadvault("test_preset_inputs_selection")

    def test_weight_calculation(self):
        self.log.info("Test weight calculation with external inputs")

        self.nodes[2].createvault("test_weight_calculation")
        vault = self.nodes[2].get_vault_rpc("test_weight_calculation")

        addr = vault.getnewaddress(address_type="bech32")
        ext_addr = self.nodes[0].getnewaddress(address_type="bech32")
        utxo, ext_utxo = self.create_outpoints(self.nodes[0], outputs=[{addr: 5}, {ext_addr: 5}])

        self.nodes[0].sendtoaddress(vault.getnewaddress(address_type="bech32"), 5)
        self.generate(self.nodes[0], 1)

        rawtx = vault.createrawtransaction([utxo], [{self.nodes[0].getnewaddress(address_type="bech32"): 8}])
        fundedtx = vault.fundrawtransaction(rawtx, change_type="bech32")
        assert "fee" not in fundedtx

        # Using the other output exercises external input weights.
        rawtx = vault.createrawtransaction([ext_utxo], [{self.nodes[0].getnewaddress(): 13}])
        ext_desc = self.nodes[0].getaddressinfo(ext_addr)["desc"]
        fundedtx = vault.fundrawtransaction(rawtx, change_type="bech32", solving_data={"descriptors": [ext_desc]})
        assert "fee" not in fundedtx

        self.nodes[2].unloadvault("test_weight_calculation")

    def test_weight_limits(self):
        self.log.info("Test weight limits")

        self.nodes[2].createvault("test_weight_limits")
        vault = self.nodes[2].get_vault_rpc("test_weight_limits")

        outputs = []
        for _ in range(1472):
            outputs.append({vault.getnewaddress(address_type="bech32"): 0.1})
        txid = self.nodes[0].send(outputs=outputs, change_position=0)["txid"]
        self.generate(self.nodes[0], 1)

        # 272 WU per input (273 when high-s); picking 1471 inputs will exceed the max standard tx weight.
        rawtx = vault.createrawtransaction([], [{vault.getnewaddress(): 0.1 * 1471}])

        # 1) Try to fund the transaction using only the preset inputs (all 1472 inputs)
        input_weights = []
        for i in range(1, 1473):  # skip first output as it is the parent tx change output
            input_weights.append({"txid": txid, "vout": i, "weight": 273})
        assert_raises_rpc_error(-4, "Transaction too large", vault.fundrawtransaction, hexstring=rawtx, input_weights=input_weights)

        # 2) Let the vault fund the transaction
        assert_raises_rpc_error(-4, "The inputs size exceeds the maximum weight. Please try sending a smaller amount or manually consolidating your vault's UTXOs",
                                vault.fundrawtransaction, hexstring=rawtx)

        # 3) Pre-select some inputs and let the vault fill-up the remaining amount
        inputs = input_weights[0:1000]
        assert_raises_rpc_error(-4, "The combination of the pre-selected inputs and the vault automatic inputs selection exceeds the transaction maximum weight. Please try sending a smaller amount or manually consolidating your vault's UTXOs",
                                vault.fundrawtransaction, hexstring=rawtx, input_weights=inputs)

        self.nodes[2].unloadvault("test_weight_limits")

    def test_include_unsafe(self):
        self.log.info("Test fundrawtxn with unsafe inputs")

        self.nodes[0].createvault("unsafe")
        vault = self.nodes[0].get_vault_rpc("unsafe")

        # We receive unconfirmed funds from external keys (unsafe outputs).
        addr = vault.getnewaddress()
        inputs = []
        for i in range(0, 2):
            utxo = self.create_outpoints(self.nodes[2], outputs=[{addr: 5}])[0]
            inputs.append((utxo['txid'], utxo['vout']))
        self.sync_relaypools()

        # Unsafe inputs are ignored by default.
        rawtx = vault.createrawtransaction([], [{self.nodes[2].getnewaddress(): 7.5}])
        assert_raises_rpc_error(-4, "Insufficient funds", vault.fundrawtransaction, rawtx)

        # But we can opt-in to use them for funding.
        fundedtx = vault.fundrawtransaction(rawtx, include_unsafe=True)
        tx_dec = vault.decoderawtransaction(fundedtx['hex'])
        assert all((txin["txid"], txin["vout"]) in inputs for txin in tx_dec["vin"])
        signedtx = vault.signrawtransactionwithvault(fundedtx['hex'])
        assert vault.testrelaypoolaccept([signedtx['hex']])[0]["allowed"]

        # And we can also use them once they're confirmed.
        self.generate(self.nodes[0], 1)
        fundedtx = vault.fundrawtransaction(rawtx, include_unsafe=False)
        tx_dec = vault.decoderawtransaction(fundedtx['hex'])
        assert all((txin["txid"], txin["vout"]) in inputs for txin in tx_dec["vin"])
        signedtx = vault.signrawtransactionwithvault(fundedtx['hex'])
        assert vault.testrelaypoolaccept([signedtx['hex']])[0]["allowed"]
        self.nodes[0].unloadvault("unsafe")

    def test_near_exact_funding(self):
        self.log.info("Test that near-exact funding does not result in an assertion")

        self.nodes[1].createvault("roundtest")
        w = self.nodes[1].get_vault_rpc("roundtest")

        addr = w.getnewaddress(address_type="bech32")
        self.nodes[0].sendtoaddress(addr, 1)
        self.generate(self.nodes[0], 1)

        # A P2WPKH input costs 68 vbytes; With a single P2WPKH output, the rest of the tx is 42 vbytes for a total of 110 vbytes.
        # Spending almost the full input exercises the near-exact funding path. If working correctly,
        # this should fail with insufficient funds rather than quicksilverd asserting.
        rawtx = w.createrawtransaction(inputs=[], outputs=[{self.nodes[0].getnewaddress(address_type="bech32"): 1 - 0.00000202}])
        funded_tx = w.fundrawtransaction(rawtx)
        assert "fee" not in funded_tx

    def test_input_confs_control(self):
        self.nodes[0].createvault("minconf")
        vault = self.nodes[0].get_vault_rpc("minconf")

        # Fund the vault with different chain heights
        for _ in range(2):
            self.nodes[2].sendmany({vault.getnewaddress():1, vault.getnewaddress():1})
            self.generate(self.nodes[2], 1)

        unconfirmed_txid = vault.sendtoaddress(vault.getnewaddress(), 0.5)

        self.log.info("Crafting TX using an unconfirmed input")
        target_address = self.nodes[2].getnewaddress()
        raw_tx1 = vault.createrawtransaction([], [{target_address: 0.1}], 0)
        funded_tx1 = vault.fundrawtransaction(raw_tx1, {'maxconf': 0})['hex']

        # Make sure we only had the one input
        tx1_inputs = self.nodes[0].decoderawtransaction(funded_tx1)['vin']
        assert_equal(len(tx1_inputs), 1)

        utxo1 = tx1_inputs[0]
        assert unconfirmed_txid == utxo1['txid']

        final_tx1 = vault.signrawtransactionwithvault(funded_tx1)['hex']
        txid1 = self.nodes[0].sendrawtransaction(final_tx1)

        relaypool = self.nodes[0].getrawrelaypool()
        assert txid1 in relaypool

        self.log.info("Fail to craft a new TX with minconf above highest one")
        # Create a replacement tx to 'final_tx1' that has 1 Hg target instead of 0.1.
        raw_tx2 = vault.createrawtransaction([{'txid': utxo1['txid'], 'vout': utxo1['vout']}], [{target_address: 1}])
        assert_raises_rpc_error(-4, "Insufficient funds", vault.fundrawtransaction, raw_tx2, {'add_inputs': True, 'minconf': 3})

        self.log.info("Craft a new TX with maxconf 0 and verify it chooses unconfirmed outputs")
        # Now fund 'raw_tx2' to fulfill the total target (1 Hg) by using all the vault unconfirmed outputs.
        # As it was created with the first unconfirmed output, 'raw_tx2' only has 0.1 Hg covered (need to fund 0.9 Hg more).
        # So, the selection process, to cover the amount, will pick up the 'final_tx1' output as well, which is an
        # output of the tx that this new tx conflicts with. Replacement accept/reject depends on the freshly ground
        # tx PoW surplus; this vault test only needs to verify input confirmation selection and signing.
        funded_unconfirmed = vault.fundrawtransaction(raw_tx2, {'add_inputs': True, 'maxconf': 0})['hex']
        funded_unconfirmed_inputs = self.nodes[0].decoderawtransaction(funded_unconfirmed)['vin']
        for vin in funded_unconfirmed_inputs:
            assert_equal(vault.gettransaction(vin['txid'])['confirmations'], 0)
        final_unconfirmed = vault.signrawtransactionwithvault(funded_unconfirmed)
        assert final_unconfirmed["complete"]

        self.log.info("Craft a replacement adding inputs with highest depth possible")
        funded_tx2 = vault.fundrawtransaction(raw_tx2, {'add_inputs': True, 'minconf': 2})['hex']
        tx2_inputs = self.nodes[0].decoderawtransaction(funded_tx2)['vin']
        assert_greater_than_or_equal(len(tx2_inputs), 2)
        for vin in tx2_inputs:
            if vin['txid'] != unconfirmed_txid:
                assert_greater_than_or_equal(self.nodes[0].gettxout(vin['txid'], vin['vout'])['confirmations'], 2)

        # Replacement accept/reject depends on the freshly ground tx PoW surplus;
        # this test only needs to prove input confirmation selection and signing.
        final_tx2 = vault.signrawtransactionwithvault(funded_tx2)
        assert final_tx2["complete"]

        relaypool = self.nodes[0].getrawrelaypool()
        assert txid1 in relaypool

        vault.unloadvault()

if __name__ == '__main__':
    RawTransactionsTest(__file__).main()
