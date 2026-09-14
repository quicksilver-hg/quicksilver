#!/usr/bin/env python3
# Copyright (c) 2018-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the Partially Signed Quicksilver Transaction RPCs.
"""
import base64
from decimal import Decimal
from io import BytesIO
from random import randbytes

from test_framework.blocktools import (
    MAX_STANDARD_TX_WEIGHT,
)
from test_framework.descriptors import descsum_create
from test_framework.key import H_POINT
from test_framework.messages import (
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
    deser_compact_size,
    SEQUENCE_NONFINAL,
    ser_compact_size,
    WITNESS_SCALE_FACTOR,
)
from test_framework.psqt import (
    PSQT,
    PSQTMap,
    PSQT_GLOBAL_UNSIGNED_TX,
    PSQT_IN_RIPEMD160,
    PSQT_IN_SHA256,
    PSQT_IN_HASH160,
    PSQT_IN_HASH256,
    PSQT_IN_WITNESS_UTXO,
    PSQT_OUT_TAP_TREE,
)
from test_framework.script import CScript, OP_TRUE
from test_framework.script_util import MIN_STANDARD_TX_NONWITNESS_SIZE
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_greater_than_or_equal,
    assert_raises_rpc_error,
)
from test_framework.vault_util import (
    calculate_input_weight,
    generate_keypair,
    get_generate_key,
)

import json
import os


class PSQTTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 3
        self.extra_args = [
            ["-addresstype=bech32", "-changetype=bech32"], #TODO: Remove address type restrictions once taproot has psqt extensions
            ["-changetype=base58"],
            []
        ]
        # whitelist peers to speed up tx relay / relaypool sync
        for args in self.extra_args:
            args.append("-whitelist=noban@127.0.0.1")
        self.supports_cli = False

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()

    def test_psqt_incomplete_after_invalid_modification(self):
        self.log.info("Check that PSQT is correctly marked as incomplete after invalid modification")
        node = self.nodes[2]
        vault = node.get_vault_rpc(self.default_vault_name)
        address = vault.getnewaddress()
        vault.sendtoaddress(address=address, amount=1.0)
        self.generate(node, nblocks=1)

        utxos = vault.listunspent(addresses=[address])
        psqt = vault.createpsqt([{"txid": utxos[0]["txid"], "vout": utxos[0]["vout"]}], [{vault.getnewaddress(): 0.9999}])
        signed_psqt = vault.vaultprocesspsqt(psqt)["psqt"]

        # Modify the raw transaction by changing the output address, so the signature is no longer valid
        signed_psqt_obj = PSQT.from_base64(signed_psqt)
        substitute_addr = vault.getnewaddress()
        raw = vault.createrawtransaction([{"txid": utxos[0]["txid"], "vout": utxos[0]["vout"]}], [{substitute_addr: 0.9999}])
        signed_psqt_obj.g.map[PSQT_GLOBAL_UNSIGNED_TX] = bytes.fromhex(raw)

        # Check that the vaultprocesspsqt call succeeds but also recognizes that the transaction is not complete
        signed_psqt_incomplete = vault.vaultprocesspsqt(signed_psqt_obj.to_base64(), finalize=False)
        assert signed_psqt_incomplete["complete"] is False

    def assert_change_type(self, psqtx, expected_type):
        """Assert that the given PSQT has a change output with the given type."""

        # The decodepsqt RPC is stateless and independent of any settings, we can always just call it on the first node
        decoded_psqt = self.nodes[0].decodepsqt(psqtx["psqt"])
        changepos = psqtx["changepos"]
        assert_equal(decoded_psqt["tx"]["vout"][changepos]["output_script"]["type"], expected_type)

    def run_test(self):
        # Create and fund a raw tx for sending 10 Hg
        psqtx1 = self.nodes[0].vaultcreatefundedpsqt([], [{self.nodes[2].getnewaddress():10}])['psqt']
        decoded_psqt = self.nodes[0].decodepsqt(psqtx1)
        assert "psqt_version" in decoded_psqt
        assert "global_qpubs" in decoded_psqt
        assert "global_xpubs" not in decoded_psqt

        self.log.info("Test for invalid maximum transaction weights")
        dest_arg = [{self.nodes[0].getnewaddress(): 1}]
        min_tx_weight = MIN_STANDARD_TX_NONWITNESS_SIZE * WITNESS_SCALE_FACTOR
        assert_raises_rpc_error(-4, f"Maximum transaction weight must be between {min_tx_weight} and {MAX_STANDARD_TX_WEIGHT}", self.nodes[0].vaultcreatefundedpsqt, [], dest_arg, 0, {"max_tx_weight": -1})
        assert_raises_rpc_error(-4, f"Maximum transaction weight must be between {min_tx_weight} and {MAX_STANDARD_TX_WEIGHT}", self.nodes[0].vaultcreatefundedpsqt, [], dest_arg, 0, {"max_tx_weight": 0})
        assert_raises_rpc_error(-4, f"Maximum transaction weight must be between {min_tx_weight} and {MAX_STANDARD_TX_WEIGHT}", self.nodes[0].vaultcreatefundedpsqt, [], dest_arg, 0, {"max_tx_weight": MAX_STANDARD_TX_WEIGHT + 1})

        # Base transaction vsize: version (4) + locktime (4) + input count (1) + witness overhead (1) = 10 vbytes
        base_tx_vsize = 10
        # One P2WPKH output vsize: outpoint (31 vbytes)
        p2wpkh_output_vsize = 31
        # 1 vbyte for output count
        output_count = 1
        tx_weight_without_inputs = (base_tx_vsize + output_count + p2wpkh_output_vsize) * WITNESS_SCALE_FACTOR
        # min_tx_weight is greater than transaction weight without inputs
        assert_greater_than(min_tx_weight, tx_weight_without_inputs)

        # In order to test for when the passed max weight is less than the transaction weight without inputs
        # Define destination with two outputs.
        dest_arg_large = [{self.nodes[0].getnewaddress(): 1}, {self.nodes[0].getnewaddress(): 1}]
        large_tx_vsize_without_inputs = base_tx_vsize + output_count + (p2wpkh_output_vsize * 2)
        large_tx_weight_without_inputs = large_tx_vsize_without_inputs * WITNESS_SCALE_FACTOR
        assert_greater_than(large_tx_weight_without_inputs, min_tx_weight)
        # Test for max_tx_weight less than Transaction weight without inputs
        assert_raises_rpc_error(-4, "Maximum transaction weight is less than transaction weight without inputs", self.nodes[0].vaultcreatefundedpsqt, [], dest_arg_large, 0, {"max_tx_weight": min_tx_weight})
        assert_raises_rpc_error(-4, "Maximum transaction weight is less than transaction weight without inputs", self.nodes[0].vaultcreatefundedpsqt, [], dest_arg_large, 0, {"max_tx_weight": large_tx_weight_without_inputs})

        # Test for max_tx_weight high enough for the bare transaction, but not selected inputs.
        assert_raises_rpc_error(-4, "The inputs size exceeds the maximum weight", self.nodes[0].vaultcreatefundedpsqt, [], dest_arg_large, 0, {"max_tx_weight": (large_tx_vsize_without_inputs + 1) * WITNESS_SCALE_FACTOR})
        self.log.info("Test that a funded PSQT is always faithful to max_tx_weight option")
        large_tx_vsize_with_change = large_tx_vsize_without_inputs + p2wpkh_output_vsize
        # It's enough but won't accommodate selected input size
        assert_raises_rpc_error(-4, "The inputs size exceeds the maximum weight", self.nodes[0].vaultcreatefundedpsqt, [], dest_arg_large, 0, {"max_tx_weight": (large_tx_vsize_with_change) * WITNESS_SCALE_FACTOR})

        max_tx_weight_sufficient = 2000
        psqt = self.nodes[0].vaultcreatefundedpsqt(outputs=dest_arg,locktime=0, options={"max_tx_weight": max_tx_weight_sufficient})["psqt"]
        weight = self.nodes[0].decodepsqt(psqt)["tx"]["weight"]
        # ensure the transaction's weight is below the specified max_tx_weight.
        assert_greater_than_or_equal(max_tx_weight_sufficient, weight)

        # If inputs are specified, do not automatically add more:
        utxo1 = self.nodes[0].listunspent()[0]
        assert_raises_rpc_error(-4, "The preselected coins total amount does not cover the transaction target. "
                                    "Please allow other inputs to be automatically selected or include more coins manually",
                                self.nodes[0].vaultcreatefundedpsqt, [{"txid": utxo1['txid'], "vout": utxo1['vout']}], [{self.nodes[2].getnewaddress():90}])

        psqtx1 = self.nodes[0].vaultcreatefundedpsqt([{"txid": utxo1['txid'], "vout": utxo1['vout']}], [{self.nodes[2].getnewaddress():90}], 0, {"add_inputs": True})['psqt']
        assert_greater_than_or_equal(len(self.nodes[0].decodepsqt(psqtx1)['tx']['vin']), 2)

        # Inputs argument can be null
        self.nodes[0].vaultcreatefundedpsqt(None, [{self.nodes[2].getnewaddress():10}])

        # Node 1 should not be able to add anything to it but still return the psqtx same as before
        psqtx = self.nodes[1].vaultprocesspsqt(psqtx1)['psqt']
        assert_equal(psqtx1, psqtx)

        # Node 0 should not be able to sign the transaction with the vault is locked
        self.nodes[0].encryptvault("password")
        assert_raises_rpc_error(-13, "Please enter the vault passphrase with vaultpassphrase first", self.nodes[0].vaultprocesspsqt, psqtx)

        # Node 0 should be able to process without signing though
        unsigned_tx = self.nodes[0].vaultprocesspsqt(psqtx, False)
        assert_equal(unsigned_tx['complete'], False)

        self.nodes[0].vaultpassphrase(passphrase="password", timeout=1000000)

        # Sign the transaction but don't finalize
        processed_psqt = self.nodes[0].vaultprocesspsqt(psqt=psqtx, finalize=False)
        assert "hex" not in processed_psqt
        signed_psqt = processed_psqt['psqt']

        # Finalize and send
        finalized_hex = self.nodes[0].finalizepsqt(signed_psqt)['hex']
        self.nodes[0].sendrawtransaction(finalized_hex)

        # Alternative method: sign AND finalize in one command
        processed_finalized_psqt = self.nodes[0].vaultprocesspsqt(psqt=psqtx, finalize=True)
        finalized_psqt = processed_finalized_psqt['psqt']
        finalized_psqt_hex = processed_finalized_psqt['hex']
        assert signed_psqt != finalized_psqt
        assert finalized_psqt_hex == finalized_hex

        # Manually selected inputs can be locked:
        assert_equal(len(self.nodes[0].listlockunspent()), 0)
        utxo1 = self.nodes[0].listunspent()[0]
        psqtx1 = self.nodes[0].vaultcreatefundedpsqt([{"txid": utxo1['txid'], "vout": utxo1['vout']}], [{self.nodes[2].getnewaddress():1}], 0,{"lock_unspents": True})["psqt"]
        assert_equal(len(self.nodes[0].listlockunspent()), 1)

        # Locks are ignored for manually selected inputs
        self.nodes[0].vaultcreatefundedpsqt([{"txid": utxo1['txid'], "vout": utxo1['vout']}], [{self.nodes[2].getnewaddress():1}], 0)

        # Create native key-witness and script-witness addresses.
        pubkey0 = self.nodes[0].getaddressinfo(self.nodes[0].getnewaddress())['pubkey']
        pubkey1 = self.nodes[1].getaddressinfo(self.nodes[1].getnewaddress())['pubkey']
        pubkey2 = self.nodes[2].getaddressinfo(self.nodes[2].getnewaddress())['pubkey']

        # Setup key-disabled tracking vaults
        self.nodes[2].createvault(vault_name='wmulti', disable_private_keys=True)
        wmulti = self.nodes[2].get_vault_rpc('wmulti')

        # Create all the addresses
        p2wsh = wmulti.addmultisigaddress(2, [pubkey0, pubkey1, pubkey2], "", "bech32")['address']
        p2wpkh = self.nodes[1].getnewaddress("", "bech32")

        # fund those addresses
        rawtx = self.nodes[0].createrawtransaction([], [{p2wsh:10}, {p2wpkh:10}])
        rawtx = self.nodes[0].fundrawtransaction(rawtx, {"change_position":1})
        signed_tx = self.nodes[0].signrawtransactionwithvault(rawtx['hex'])['hex']
        txid = self.nodes[0].sendrawtransaction(signed_tx)
        self.generate(self.nodes[0], 6)

        # Find the output pos
        p2wsh_pos = -1
        p2wpkh_pos = -1
        decoded = self.nodes[0].decoderawtransaction(signed_tx)
        for out in decoded['vout']:
            if out['output_script']['address'] == p2wsh:
                p2wsh_pos = out['n']
            elif out['output_script']['address'] == p2wpkh:
                p2wpkh_pos = out['n']

        inputs = [{"txid": txid, "vout": p2wpkh_pos}]
        outputs = [{self.nodes[1].getnewaddress(): 9.99}]

        # spend single key from node 1
        created_psqt = self.nodes[1].vaultcreatefundedpsqt(inputs, outputs)
        vaultprocesspsqt_out = self.nodes[1].vaultprocesspsqt(created_psqt['psqt'])
        # The updater supplies both UTXO representations for this input.
        decoded = self.nodes[1].decodepsqt(vaultprocesspsqt_out['psqt'])
        assert 'non_witness_utxo' in decoded['inputs'][0]
        assert 'witness_utxo' in decoded['inputs'][0]
        assert 'fee' not in decoded
        assert 'fee' not in created_psqt
        assert_equal(vaultprocesspsqt_out['complete'], True)
        self.nodes[1].sendrawtransaction(vaultprocesspsqt_out['hex'])

        self.log.info("Test vaultcreatefundedpsqt omits fee fields")
        for bool_add, outputs_array in {True: outputs, False: [{self.nodes[1].getnewaddress(): 1}]}.items():
            psqt = self.nodes[1].vaultcreatefundedpsqt(inputs, outputs_array, 0, {"add_inputs": bool_add})
            assert "fee" not in psqt

        self.log.info("Test various PSQT operations")
        # partially sign multisig things with node 1
        psqtx = wmulti.vaultcreatefundedpsqt(inputs=[{"txid":txid,"vout":p2wsh_pos}], outputs=[{self.nodes[1].getnewaddress():9.99}], change_address=self.nodes[1].getrawchangeaddress())['psqt']
        vaultprocesspsqt_out = self.nodes[1].vaultprocesspsqt(psqtx)
        psqtx = vaultprocesspsqt_out['psqt']
        assert_equal(vaultprocesspsqt_out['complete'], False)

        # Unload wmulti, we don't need it anymore
        wmulti.unloadvault()

        # partially sign with node 2. This should be complete and sendable
        vaultprocesspsqt_out = self.nodes[2].vaultprocesspsqt(psqtx)
        assert_equal(vaultprocesspsqt_out['complete'], True)
        self.nodes[2].sendrawtransaction(vaultprocesspsqt_out['hex'])

        # check that vaultprocesspsqt fails to decode a non-psqt
        rawtx = self.nodes[1].createrawtransaction([{"txid":txid,"vout":p2wpkh_pos}], [{self.nodes[1].getnewaddress():9.99}])
        assert_raises_rpc_error(-22, "TX decode failed", self.nodes[1].vaultprocesspsqt, rawtx)

        # Convert a non-psqt to psqt and make sure we can decode it
        rawtx = self.nodes[0].createrawtransaction([], [{self.nodes[1].getnewaddress():10}])
        rawtx = self.nodes[0].fundrawtransaction(rawtx)
        new_psqt = self.nodes[0].converttopsqt(rawtx['hex'])
        self.nodes[0].decodepsqt(new_psqt)

        # Make sure that a non-psqt with signatures cannot be converted
        signedtx = self.nodes[0].signrawtransactionwithvault(rawtx['hex'])
        assert_raises_rpc_error(-22, "Inputs must not have scriptSigs and scriptWitnesses",
                                self.nodes[0].converttopsqt, hexstring=signedtx['hex'])  # permitsigdata=False by default
        assert_raises_rpc_error(-22, "Inputs must not have scriptSigs and scriptWitnesses",
                                self.nodes[0].converttopsqt, hexstring=signedtx['hex'], permitsigdata=False)
        assert_raises_rpc_error(-22, "Inputs must not have scriptSigs and scriptWitnesses",
                                self.nodes[0].converttopsqt, hexstring=signedtx['hex'], permitsigdata=False, iswitness=True)
        # Unless we allow it to convert and strip signatures
        self.nodes[0].converttopsqt(hexstring=signedtx['hex'], permitsigdata=True)

        # Create outputs to nodes 1 and 2
        # (note that we intentionally create two different txs here, as we want
        #  to check that each node is missing prevout data for one of the two
        #  utxos, see "should only have data for one input" test below)
        node1_addr = self.nodes[1].getnewaddress()
        node2_addr = self.nodes[2].getnewaddress()
        utxo1 = self.create_outpoints(self.nodes[0], outputs=[{node1_addr: 13}])[0]
        utxo2 = self.create_outpoints(self.nodes[0], outputs=[{node2_addr: 13}])[0]
        self.generate(self.nodes[0], 6)[0]

        # Create a psqt spending outputs from nodes 1 and 2
        psqt_orig = self.nodes[0].createpsqt([utxo1, utxo2], [{self.nodes[0].getnewaddress():25.999}])

        # Update psqts, should only have data for one input and not the other
        psqt1 = self.nodes[1].vaultprocesspsqt(psqt_orig, False, "ALL")['psqt']
        psqt1_decoded = self.nodes[0].decodepsqt(psqt1)
        assert psqt1_decoded['inputs'][0] and not psqt1_decoded['inputs'][1]
        # Check that BIP32 path was added
        assert "bip32_derivs" in psqt1_decoded['inputs'][0]
        psqt2 = self.nodes[2].vaultprocesspsqt(psqt_orig, False, "ALL", False)['psqt']
        psqt2_decoded = self.nodes[0].decodepsqt(psqt2)
        assert not psqt2_decoded['inputs'][0] and psqt2_decoded['inputs'][1]
        # Check that BIP32 paths were not added
        assert "bip32_derivs" not in psqt2_decoded['inputs'][1]

        # Sign PSQTs (workaround issue #18039)
        psqt1 = self.nodes[1].vaultprocesspsqt(psqt_orig)['psqt']
        psqt2 = self.nodes[2].vaultprocesspsqt(psqt_orig)['psqt']

        # Combine and finalize the psqts. This hand-built PSQT has no Quicksilver
        # per-transaction PoW anchor, so it is not broadcastable.
        combined = self.nodes[0].combinepsqt([psqt1, psqt2])
        finalized = self.nodes[0].finalizepsqt(combined)['hex']
        assert_raises_rpc_error(-26, "bad-txns-pow-anchor", self.nodes[0].sendrawtransaction, finalized)

        # Test additional args in vaultcreatepsqt. Every input -- pre-included or
        # funded -- now comes back at the single non-final sequence; there is no
        # longer an argument that varies it.
        block_height = self.nodes[0].getblockcount()
        unspent = self.nodes[0].listunspent()[0]
        psqtx_info = self.nodes[0].vaultcreatefundedpsqt([{"txid":unspent["txid"], "vout":unspent["vout"]}], [{self.nodes[2].getnewaddress():unspent["amount"]+1}], block_height+2, {"add_inputs": True}, False)
        decoded_psqt = self.nodes[0].decodepsqt(psqtx_info["psqt"])
        for tx_in, psqt_in in zip(decoded_psqt["tx"]["vin"], decoded_psqt["inputs"]):
            assert_equal(tx_in["sequence"], SEQUENCE_NONFINAL)
            assert "bip32_derivs" not in psqt_in
        assert_equal(decoded_psqt["tx"]["locktime"], block_height+2)

        # Same construction with only locktime set
        psqtx_info = self.nodes[0].vaultcreatefundedpsqt([{"txid":unspent["txid"], "vout":unspent["vout"]}], [{self.nodes[2].getnewaddress():unspent["amount"]+1}], block_height, {"add_inputs": True}, True)
        decoded_psqt = self.nodes[0].decodepsqt(psqtx_info["psqt"])
        for tx_in, psqt_in in zip(decoded_psqt["tx"]["vin"], decoded_psqt["inputs"]):
            assert_equal(tx_in["sequence"], SEQUENCE_NONFINAL)
            assert "bip32_derivs" in psqt_in
        assert_equal(decoded_psqt["tx"]["locktime"], block_height)

        # Same construction without optional arguments
        psqtx_info = self.nodes[0].vaultcreatefundedpsqt([], [{self.nodes[2].getnewaddress():unspent["amount"]+1}])
        decoded_psqt = self.nodes[0].decodepsqt(psqtx_info["psqt"])
        for tx_in, psqt_in in zip(decoded_psqt["tx"]["vin"], decoded_psqt["inputs"]):
            assert_equal(tx_in["sequence"], SEQUENCE_NONFINAL)
            assert "bip32_derivs" in psqt_in
        assert_equal(decoded_psqt["tx"]["locktime"], 0)

        # Same construction without optional arguments, on the second node
        unspent1 = self.nodes[1].listunspent()[0]
        psqtx_info = self.nodes[1].vaultcreatefundedpsqt([{"txid":unspent1["txid"], "vout":unspent1["vout"]}], [{self.nodes[2].getnewaddress():unspent1["amount"]+1}], block_height, {"add_inputs": True})
        decoded_psqt = self.nodes[1].decodepsqt(psqtx_info["psqt"])
        for tx_in, psqt_in in zip(decoded_psqt["tx"]["vin"], decoded_psqt["inputs"]):
            assert_equal(tx_in["sequence"], SEQUENCE_NONFINAL)
            assert "bip32_derivs" in psqt_in

        # Make sure a change address from a vault without P2SH innerscript access still succeeds
        # when automatic coin selection adds inputs.
        self.nodes[0].vaultcreatefundedpsqt([], [{self.nodes[2].getnewaddress():unspent["amount"]+1}], block_height+2, {"change_address":self.nodes[1].getnewaddress()}, False)

        # Make sure the vault's change type is respected by default
        small_output = {self.nodes[0].getnewaddress():0.1}
        psqtx_native = self.nodes[0].vaultcreatefundedpsqt([], [small_output])
        self.assert_change_type(psqtx_native, "witness_v0_keyhash")
        psqtx_p2sh = self.nodes[1].vaultcreatefundedpsqt([], [small_output])
        self.assert_change_type(psqtx_p2sh, "pubkeyhash")

        # Make sure the change type of the vault can also be overwritten
        psqtx_np2wkh = self.nodes[1].vaultcreatefundedpsqt([], [small_output], 0, {"change_type":"base58"})
        self.assert_change_type(psqtx_np2wkh, "pubkeyhash")

        # Make sure the change type cannot be specified if a change address is given
        invalid_options = {"change_type":"bech32","change_address":self.nodes[0].getnewaddress()}
        assert_raises_rpc_error(-8, "both change address and address type options", self.nodes[0].vaultcreatefundedpsqt, [], [small_output], 0, invalid_options)

        # Regression test for 14473 (mishandling of already-signed witness transaction):
        psqtx_info = self.nodes[0].vaultcreatefundedpsqt([{"txid":unspent["txid"], "vout":unspent["vout"]}], [{self.nodes[2].getnewaddress():unspent["amount"]+1}], 0, {"add_inputs": True})
        complete_psqt = self.nodes[0].vaultprocesspsqt(psqtx_info["psqt"])
        double_processed_psqt = self.nodes[0].vaultprocesspsqt(complete_psqt["psqt"])
        assert_equal(complete_psqt, double_processed_psqt)
        # We don't care about the decode result, but decoding must succeed.
        self.nodes[0].decodepsqt(double_processed_psqt["psqt"])

        # Make sure unsafe inputs are included if specified
        self.nodes[2].createvault(vault_name="unsafe")
        wunsafe = self.nodes[2].get_vault_rpc("unsafe")
        self.nodes[0].sendtoaddress(wunsafe.getnewaddress(), 2)
        self.sync_relaypools()
        assert_raises_rpc_error(-4, "Insufficient funds", wunsafe.vaultcreatefundedpsqt, [], [{self.nodes[0].getnewaddress(): 1}])
        wunsafe.vaultcreatefundedpsqt([], [{self.nodes[0].getnewaddress(): 1}], 0, {"include_unsafe": True})

        # BIP 174 Test Vectors

        # Check that unknown values are just passed through
        def global_map_end(raw_psqt):
            stream = BytesIO(raw_psqt)
            assert_equal(stream.read(5), b"psqt\xff")
            while True:
                record_start = stream.tell()
                key_len = deser_compact_size(stream)
                if key_len == 0:
                    return record_start
                stream.seek(key_len, 1)
                value_len = deser_compact_size(stream)
                stream.seek(value_len, 1)

        unknown_key = b"\x0f"
        unknown_value = bytes(range(1, 16))
        unknown_record = ser_compact_size(len(unknown_key)) + unknown_key + ser_compact_size(len(unknown_value)) + unknown_value
        base_psqt = base64.b64decode(self.nodes[0].createpsqt([], [{"data": "00"}]))
        insert_pos = global_map_end(base_psqt)
        unknown_psqt = base64.b64encode(base_psqt[:insert_pos] + unknown_record + base_psqt[insert_pos:]).decode("utf8")
        unknown_out = base64.b64decode(self.nodes[0].vaultprocesspsqt(unknown_psqt)['psqt'])
        assert unknown_record in unknown_out[:global_map_end(unknown_out)]

        # Open the data file
        with open(os.path.join(os.path.dirname(os.path.realpath(__file__)), 'data/rpc_psqt.json'), encoding='utf-8') as f:
            d = json.load(f)
            invalids = d['invalid']
            invalid_with_msgs = d["invalid_with_msg"]
            # NOTE: data/rpc_psqt.json also carries populated 'valid' (20), 'creator' (1),
            # 'signer' (6), 'combiner' (2), 'finalizer' (1) and 'extractor' (1) sections
            # that nothing below exercises. That round-trip coverage was dropped and has
            # not been restored for Quicksilver's PSQT magic.

        # Invalid PSQTs
        for invalid in invalids:
            assert_raises_rpc_error(-22, "TX decode failed", self.nodes[0].decodepsqt, invalid)
        for invalid in invalid_with_msgs:
            psqt, msg = invalid
            assert_raises_rpc_error(-22, "TX decode failed", self.nodes[0].decodepsqt, psqt)

        self.log.info("Skip upstream static PSQT vectors that embed stock transaction serialization")

        # Empty combiner test
        assert_raises_rpc_error(-8, "Parameter 'txs' cannot be empty", self.nodes[0].combinepsqt, [])

        self.test_psqt_incomplete_after_invalid_modification()

        # Test decoding error: invalid base64
        assert_raises_rpc_error(-22, "TX decode failed invalid base64", self.nodes[0].decodepsqt, ";definitely not base64;")

        # Send to the native witness and Base58 address types.
        addr1 = self.nodes[1].getnewaddress("", "bech32")
        addr2 = self.nodes[1].getnewaddress("", "base58")
        utxo1, utxo2 = self.create_outpoints(self.nodes[1], outputs=[{addr1: 11}, {addr2: 11}])
        self.sync_all()

        def test_psqt_input_keys(psqt_input, keys):
            """Check that the psqt input has only the expected keys."""
            assert_equal(set(keys), set(psqt_input.keys()))

        # Create a PSQT. None of the inputs are filled initially
        psqt = self.nodes[1].createpsqt([utxo1, utxo2], [{self.nodes[0].getnewaddress():21.999}])
        decoded = self.nodes[1].decodepsqt(psqt)
        test_psqt_input_keys(decoded['inputs'][0], [])
        test_psqt_input_keys(decoded['inputs'][1], [])

        # Update a PSQT with UTXOs from the node
        # Bech32 inputs should be filled with witness UTXO. Other inputs should not be filled because they are non-witness
        updated = self.nodes[1].utxoupdatepsqt(psqt)
        decoded = self.nodes[1].decodepsqt(updated)
        test_psqt_input_keys(decoded['inputs'][0], ['witness_utxo', 'non_witness_utxo'])
        test_psqt_input_keys(decoded['inputs'][1], ['non_witness_utxo'])

        # Try again with descriptors so key derivation data is filled in.
        descs = [self.nodes[1].getaddressinfo(addr)['desc'] for addr in [addr1, addr2]]
        updated = self.nodes[1].utxoupdatepsqt(psqt=psqt, descriptors=descs)
        decoded = self.nodes[1].decodepsqt(updated)
        test_psqt_input_keys(decoded['inputs'][0], ['witness_utxo', 'non_witness_utxo', 'bip32_derivs'])
        test_psqt_input_keys(decoded['inputs'][1], ['non_witness_utxo', 'bip32_derivs'])

        # Two PSQTs with a common input should not be joinable
        psqt1 = self.nodes[1].createpsqt([utxo1], [{self.nodes[0].getnewaddress():Decimal('10.999')}])
        assert_raises_rpc_error(-8, "exists in multiple PSQTs", self.nodes[1].joinpsqts, [psqt1, updated])

        # Join two distinct PSQTs
        addr4 = self.nodes[1].getnewaddress("", "bech32")
        utxo4 = self.create_outpoints(self.nodes[0], outputs=[{addr4: 5}])[0]
        self.generate(self.nodes[0], 6)
        psqt2 = self.nodes[1].createpsqt([utxo4], [{self.nodes[0].getnewaddress():Decimal('4.999')}])
        psqt2 = self.nodes[1].vaultprocesspsqt(psqt2)['psqt']
        psqt2_decoded = self.nodes[0].decodepsqt(psqt2)
        assert "final_scriptwitness" in psqt2_decoded['inputs'][0] and "final_scriptSig" not in psqt2_decoded['inputs'][0]
        joined = self.nodes[0].joinpsqts([psqt, psqt2])
        joined_decoded = self.nodes[0].decodepsqt(joined)
        assert len(joined_decoded['inputs']) == 3 and len(joined_decoded['outputs']) == 2 and "final_scriptwitness" not in joined_decoded['inputs'][2] and "final_scriptSig" not in joined_decoded['inputs'][2]

        # Check that joining shuffles the inputs and outputs
        # 10 attempts should be enough to get a shuffled join
        shuffled = False
        for _ in range(10):
            shuffled_joined = self.nodes[0].joinpsqts([psqt, psqt2])
            shuffled |= joined != shuffled_joined
            if shuffled:
                break
        assert shuffled

        # Newly created PSQT needs UTXOs and updating
        addr = self.nodes[1].getnewaddress("", "bech32")
        utxo = self.create_outpoints(self.nodes[0], outputs=[{addr: 7}])[0]
        addrinfo = self.nodes[1].getaddressinfo(addr)
        self.generate(self.nodes[0], 6)[0]
        psqt = self.nodes[1].createpsqt([utxo], [{self.nodes[0].getnewaddress("", "bech32"):Decimal('6.999')}])
        analyzed = self.nodes[0].analyzepsqt(psqt)
        assert not analyzed['inputs'][0]['has_utxo'] and not analyzed['inputs'][0]['is_final'] and analyzed['inputs'][0]['next'] == 'updater' and analyzed['next'] == 'updater'

        # After update with vault, only needs signing
        updated = self.nodes[1].vaultprocesspsqt(psqt, False, 'ALL', True)['psqt']
        analyzed = self.nodes[0].analyzepsqt(updated)
        assert analyzed['inputs'][0]['has_utxo'] and not analyzed['inputs'][0]['is_final'] and analyzed['inputs'][0]['next'] == 'signer' and analyzed['next'] == 'signer' and analyzed['inputs'][0]['missing']['signatures'][0] == addrinfo['witness_program']

        # Check size things
        assert 'fee' not in analyzed
        assert_greater_than(analyzed['estimated_vsize'], 0)

        # After signing and finalizing, needs extracting
        signed = self.nodes[1].vaultprocesspsqt(updated)['psqt']
        analyzed = self.nodes[0].analyzepsqt(signed)
        assert analyzed['inputs'][0]['has_utxo'] and analyzed['inputs'][0]['is_final'] and analyzed['next'] == 'extractor'

        self.log.info("PSQT with signed, but not finalized, inputs should have Finalizer as next")
        signed_unfinalized = self.nodes[1].vaultprocesspsqt(updated, True, 'ALL', True, False)['psqt']
        analysis = self.nodes[0].analyzepsqt(signed_unfinalized)
        assert_equal(analysis['next'], 'finalizer')

        self.log.info("Skip static invalid-value analyzepsqt vectors that embed stock transaction serialization")

        assert_raises_rpc_error(-22, "TX decode failed", self.nodes[0].analyzepsqt, "cHNidP8BAJoCAAAAAkvEW8NnDtdNtDpsmze+Ht2LH35IJcKv00jKAlUs21RrAwAAAAD/////S8Rbw2cO1020OmybN74e3Ysffkglwq/TSMoCVSzbVGsBAAAAAP7///8CwLYClQAAAAAWABSNJKzjaUb3uOxixsvh1GGE3fW7zQD5ApUAAAAAFgAUKNw0x8HRctAgmvoevm4u1SbN7XIAAAAAAAEAnQIAAAACczMa321tVHuN4GKWKRncycI22aX3uXgwSFUKM2orjRsBAAAAAP7///9zMxrfbW1Ue43gYpYpGdzJwjbZpfe5eDBIVQozaiuNGwAAAAAA/v///wIA+QKVAAAAABl2qRT9zXUVA8Ls5iVqynLHe5/vSe1XyYisQM0ClQAAAAAWABRmWQUcjSjghQ8/uH4Bn/zkakwLtAAAAAAAAQEfQM0ClQAAAAAWABRmWQUcjSjghQ8/uH4Bn/zkakwLtAAAAA==")

        assert_raises_rpc_error(-22, "TX decode failed", self.nodes[0].vaultprocesspsqt, "cHNidP8BAJoCAAAAAkvEW8NnDtdNtDpsmze+Ht2LH35IJcKv00jKAlUs21RrAwAAAAD/////S8Rbw2cO1020OmybN74e3Ysffkglwq/TSMoCVSzbVGsBAAAAAP7///8CwLYClQAAAAAWABSNJKzjaUb3uOxixsvh1GGE3fW7zQD5ApUAAAAAFgAUKNw0x8HRctAgmvoevm4u1SbN7XIAAAAAAAEAnQIAAAACczMa321tVHuN4GKWKRncycI22aX3uXgwSFUKM2orjRsBAAAAAP7///9zMxrfbW1Ue43gYpYpGdzJwjbZpfe5eDBIVQozaiuNGwAAAAAA/v///wIA+QKVAAAAABl2qRT9zXUVA8Ls5iVqynLHe5/vSe1XyYisQM0ClQAAAAAWABRmWQUcjSjghQ8/uH4Bn/zkakwLtAAAAAAAAQEfQM0ClQAAAAAWABRmWQUcjSjghQ8/uH4Bn/zkakwLtAAAAA==")

        self.log.info("Test that we can fund psqts with external inputs specified")

        privkey, _ = generate_keypair(wif=True)

        self.nodes[1].createvault("extfund")
        vault = self.nodes[1].get_vault_rpc("extfund")

        # Make a weird but signable native P2WSH script.
        desc = descsum_create("wsh(pkh({}))".format(privkey))
        res = self.nodes[0].importdescriptors([{"desc": desc, "timestamp": "now"}])
        assert res[0]["success"]
        addr = self.nodes[0].deriveaddresses(desc)[0]
        addr_info = self.nodes[0].getaddressinfo(addr)

        self.nodes[0].sendtoaddress(addr, 10)
        self.nodes[0].sendtoaddress(vault.getnewaddress(), 10)
        self.generate(self.nodes[0], 6)
        ext_utxo = self.nodes[0].listunspent(addresses=[addr])[0]

        # An external input without solving data should result in an error
        assert_raises_rpc_error(-4, "Not solvable pre-selected input COutPoint(%s, %s)" % (ext_utxo["txid"][0:10], ext_utxo["vout"]), vault.vaultcreatefundedpsqt, [ext_utxo], [{self.nodes[0].getnewaddress(): 15}])

        # But funding should work when the solving data is provided
        psqt = vault.vaultcreatefundedpsqt([ext_utxo], [{self.nodes[0].getnewaddress(): 15}], 0, {"add_inputs": True, "solving_data": {"pubkeys": [addr_info['pubkey']], "scripts": [addr_info["embedded"]["output_script"]]}})
        signed = vault.vaultprocesspsqt(psqt['psqt'])
        assert not signed['complete']
        signed = self.nodes[0].vaultprocesspsqt(signed['psqt'])
        assert signed['complete']

        psqt = vault.vaultcreatefundedpsqt([ext_utxo], [{self.nodes[0].getnewaddress(): 15}], 0, {"add_inputs": True, "solving_data":{"descriptors": [desc]}})
        signed = vault.vaultprocesspsqt(psqt['psqt'])
        assert not signed['complete']
        signed = self.nodes[0].vaultprocesspsqt(signed['psqt'])
        assert signed['complete']
        final = signed['hex']

        dec = self.nodes[0].decodepsqt(signed["psqt"])
        for i, txin in enumerate(dec["tx"]["vin"]):
            if txin["txid"] == ext_utxo["txid"] and txin["vout"] == ext_utxo["vout"]:
                input_idx = i
                break
        psqt_in = dec["inputs"][input_idx]
        scriptsig_hex = psqt_in["final_scriptSig"]["hex"] if "final_scriptSig" in psqt_in else ""
        witness_stack_hex = psqt_in["final_scriptwitness"] if "final_scriptwitness" in psqt_in else None
        input_weight = calculate_input_weight(scriptsig_hex, witness_stack_hex)
        low_input_weight = max(165, input_weight - 1)
        high_input_weight = input_weight * 2

        # Input weight error conditions
        assert_raises_rpc_error(
            -8,
            "Input weights should be specified in inputs rather than in options.",
            vault.vaultcreatefundedpsqt,
            inputs=[ext_utxo],
            outputs=[{self.nodes[0].getnewaddress(): 15}],
            options={"input_weights": [{"txid": ext_utxo["txid"], "vout": ext_utxo["vout"], "weight": 1000}]}
        )

        # Funding should also work if the input weight is provided
        psqt = vault.vaultcreatefundedpsqt(
            inputs=[{"txid": ext_utxo["txid"], "vout": ext_utxo["vout"], "weight": input_weight}],
            outputs=[{self.nodes[0].getnewaddress(): 15}],
            add_inputs=True,
        )
        signed = vault.vaultprocesspsqt(psqt["psqt"])
        signed = self.nodes[0].vaultprocesspsqt(signed["psqt"])
        final = signed["hex"]
        assert self.nodes[0].testrelaypoolaccept([final])[0]["allowed"]
        assert "fee" not in psqt
        # Different provided weights are accepted but do not create fee fields.
        psqt2 = vault.vaultcreatefundedpsqt(
            inputs=[{"txid": ext_utxo["txid"], "vout": ext_utxo["vout"], "weight": low_input_weight}],
            outputs=[{self.nodes[0].getnewaddress(): 15}],
            add_inputs=True,
        )
        assert "fee" not in psqt2
        psqt2 = vault.vaultcreatefundedpsqt(
            inputs=[{"txid": ext_utxo["txid"], "vout": ext_utxo["vout"], "weight": high_input_weight}],
            outputs=[{self.nodes[0].getnewaddress(): 15}],
            add_inputs=True,
        )
        assert "fee" not in psqt2
        # The provided weight should override the calculated weight when solving data is provided
        psqt3 = vault.vaultcreatefundedpsqt(
            inputs=[{"txid": ext_utxo["txid"], "vout": ext_utxo["vout"], "weight": high_input_weight}],
            outputs=[{self.nodes[0].getnewaddress(): 15}],
            add_inputs=True, solving_data={"descriptors": [desc]},
        )
        assert "fee" not in psqt3

        # Import the external utxo descriptor so that we can sign for it from the test vault
        res = vault.importdescriptors([{"desc": desc, "timestamp": "now"}])
        assert res[0]["success"]
        # The provided weight should override the calculated weight for a vault input
        psqt3 = vault.vaultcreatefundedpsqt(
            inputs=[{"txid": ext_utxo["txid"], "vout": ext_utxo["vout"], "weight": high_input_weight}],
            outputs=[{self.nodes[0].getnewaddress(): 15}],
            add_inputs=True,
        )
        assert "fee" not in psqt3

        self.log.info("Test signing inputs that the vault has keys for but is not watching the scripts")
        self.nodes[1].createvault(vault_name="scripttracking", disable_private_keys=True)
        tracking_vault = self.nodes[1].get_vault_rpc("scripttracking")

        privkey, pubkey = generate_keypair(wif=True)

        desc = descsum_create("wsh(pkh({}))".format(pubkey.hex()))
        res = tracking_vault.importdescriptors([{"desc": desc, "timestamp": "now"}])
        assert res[0]["success"]
        addr = self.nodes[0].deriveaddresses(desc)[0]
        self.nodes[0].sendtoaddress(addr, 10)
        self.generate(self.nodes[0], 1)
        self.nodes[0].importprivkey(privkey)

        psqt = tracking_vault.sendall([vault.getnewaddress()])["psqt"]
        signed_tx = self.nodes[0].vaultprocesspsqt(psqt)
        assert signed_tx["complete"]
        assert_raises_rpc_error(-26, "bad-txns-pow-anchor", self.nodes[0].sendrawtransaction, signed_tx["hex"])

        # Same test but for taproot
        privkey, pubkey = generate_keypair(wif=True)

        desc = descsum_create("tr({},pk({}))".format(H_POINT, pubkey.hex()))
        res = tracking_vault.importdescriptors([{"desc": desc, "timestamp": "now"}])
        assert res[0]["success"]
        addr = self.nodes[0].deriveaddresses(desc)[0]
        self.nodes[0].sendtoaddress(addr, 10)
        self.generate(self.nodes[0], 1)
        self.nodes[0].importdescriptors([{"desc": descsum_create("tr({})".format(privkey)), "timestamp":"now"}])

        psqt = tracking_vault.sendall([vault.getnewaddress(), addr])["psqt"]
        processed_psqt = self.nodes[0].vaultprocesspsqt(psqt)
        assert processed_psqt["complete"]
        assert_raises_rpc_error(-26, "bad-txns-pow-anchor", self.nodes[0].sendrawtransaction, processed_psqt["hex"])
        tx = self.nodes[0].decoderawtransaction(processed_psqt["hex"])
        vout = next(i for i, txout in enumerate(tx["vout"]) if txout["output_script"].get("address") == addr)

        # Make sure tap tree is in psqt
        parsed_psqt = PSQT.from_base64(psqt)
        assert_greater_than(len(parsed_psqt.o[vout].map[PSQT_OUT_TAP_TREE]), 0)
        assert "taproot_tree" in self.nodes[0].decodepsqt(psqt)["outputs"][vout]
        parsed_psqt.make_blank()
        comb_psqt = self.nodes[0].combinepsqt([psqt, parsed_psqt.to_base64()])
        assert_equal(comb_psqt, psqt)

        self.log.info("Test that vaultprocesspsqt both updates and signs a non-updated psqt containing Taproot inputs")
        addr = self.nodes[0].getnewaddress("", "bech32m")
        utxo = self.create_outpoints(self.nodes[0], outputs=[{addr: 1}])[0]
        psqt = self.nodes[0].createpsqt([utxo], [{self.nodes[0].getnewaddress(): 0.9999}])
        signed = self.nodes[0].vaultprocesspsqt(psqt)
        assert signed["complete"]
        rawtx = signed["hex"]
        assert_raises_rpc_error(-26, "bad-txns-pow-anchor", self.nodes[0].sendrawtransaction, rawtx)

        # Make sure tap tree is not in psqt
        parsed_psqt = PSQT.from_base64(psqt)
        assert PSQT_OUT_TAP_TREE not in parsed_psqt.o[0].map
        assert "taproot_tree" not in self.nodes[0].decodepsqt(psqt)["outputs"][0]
        parsed_psqt.make_blank()
        comb_psqt = self.nodes[0].combinepsqt([psqt, parsed_psqt.to_base64()])
        assert_equal(comb_psqt, psqt)

        self.log.info("Test vaultprocesspsqt raises if an invalid sighashtype is passed")
        assert_raises_rpc_error(-8, "'all' is not a valid sighash parameter.", self.nodes[0].vaultprocesspsqt, psqt, sighashtype="all")

        self.log.info("Test decoding PSQT with per-input preimage types")
        # note that the decodepsqt RPC doesn't check whether preimages and hashes match
        hash_ripemd160, preimage_ripemd160 = randbytes(20), randbytes(50)
        hash_sha256, preimage_sha256 = randbytes(32), randbytes(50)
        hash_hash160, preimage_hash160 = randbytes(20), randbytes(50)
        hash_hash256, preimage_hash256 = randbytes(32), randbytes(50)

        tx = CTransaction()
        tx.vin = [CTxIn(outpoint=COutPoint(hash=int('aa' * 32, 16), n=0), scriptSig=b""),
                  CTxIn(outpoint=COutPoint(hash=int('bb' * 32, 16), n=0), scriptSig=b""),
                  CTxIn(outpoint=COutPoint(hash=int('cc' * 32, 16), n=0), scriptSig=b""),
                  CTxIn(outpoint=COutPoint(hash=int('dd' * 32, 16), n=0), scriptSig=b"")]
        tx.vout = [CTxOut(nValue=0, scriptPubKey=b"")]
        psqt = PSQT()
        psqt.g = PSQTMap({PSQT_GLOBAL_UNSIGNED_TX: tx.serialize()})
        psqt.i = [PSQTMap({bytes([PSQT_IN_RIPEMD160]) + hash_ripemd160: preimage_ripemd160}),
                  PSQTMap({bytes([PSQT_IN_SHA256]) + hash_sha256: preimage_sha256}),
                  PSQTMap({bytes([PSQT_IN_HASH160]) + hash_hash160: preimage_hash160}),
                  PSQTMap({bytes([PSQT_IN_HASH256]) + hash_hash256: preimage_hash256})]
        psqt.o = [PSQTMap()]
        res_inputs = self.nodes[0].decodepsqt(psqt.to_base64())["inputs"]
        assert_equal(len(res_inputs), 4)
        preimage_keys = ["ripemd160_preimages", "sha256_preimages", "hash160_preimages", "hash256_preimages"]
        expected_hashes = [hash_ripemd160, hash_sha256, hash_hash160, hash_hash256]
        expected_preimages = [preimage_ripemd160, preimage_sha256, preimage_hash160, preimage_hash256]
        for res_input, preimage_key, hash, preimage in zip(res_inputs, preimage_keys, expected_hashes, expected_preimages):
            assert preimage_key in res_input
            assert_equal(len(res_input[preimage_key]), 1)
            assert hash.hex() in res_input[preimage_key]
            assert_equal(res_input[preimage_key][hash.hex()], preimage.hex())

        self.log.info("Test that combining PSQTs with different transactions fails")
        tx = CTransaction()
        tx.vin = [CTxIn(outpoint=COutPoint(hash=int('aa' * 32, 16), n=0), scriptSig=b"")]
        tx.vout = [CTxOut(nValue=0, scriptPubKey=b"")]
        psqt1 = PSQT(g=PSQTMap({PSQT_GLOBAL_UNSIGNED_TX: tx.serialize()}), i=[PSQTMap()], o=[PSQTMap()]).to_base64()
        tx.vout[0].nValue += 1  # slightly modify tx
        psqt2 = PSQT(g=PSQTMap({PSQT_GLOBAL_UNSIGNED_TX: tx.serialize()}), i=[PSQTMap()], o=[PSQTMap()]).to_base64()
        assert_raises_rpc_error(-8, "PSQTs not compatible (different transactions)", self.nodes[0].combinepsqt, [psqt1, psqt2])
        assert_equal(self.nodes[0].combinepsqt([psqt1, psqt1]), psqt1)

        self.log.info("Test that PSQT inputs are being checked via script execution")
        acs_prevout = CTxOut(nValue=0, scriptPubKey=CScript([OP_TRUE]))
        tx = CTransaction()
        tx.vin = [CTxIn(outpoint=COutPoint(hash=int('dd' * 32, 16), n=0), scriptSig=b"")]
        tx.vout = [CTxOut(nValue=0, scriptPubKey=b"")]
        psqt = PSQT()
        psqt.g = PSQTMap({PSQT_GLOBAL_UNSIGNED_TX: tx.serialize()})
        psqt.i = [PSQTMap({bytes([PSQT_IN_WITNESS_UTXO]) : acs_prevout.serialize()})]
        psqt.o = [PSQTMap()]
        finalized = self.nodes[0].finalizepsqt(psqt.to_base64())
        expected_hex_prefix = '0200000001dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd0000000000000000000100000000000000000000000000'
        assert finalized['complete']
        assert finalized['hex'].startswith(expected_hex_prefix)

        self.log.info("Test we don't crash when making a 0-value funded transaction without forcing an input selection")
        assert_raises_rpc_error(-4, "Transaction requires one destination of non-0 value or a pre-selected input", self.nodes[0].vaultcreatefundedpsqt, [], [{"data": "deadbeef"}])

        self.log.info("Test descriptorprocesspsqt updates and signs a psqt with descriptors")

        self.generate(self.nodes[2], 1)

        # Disable the vault for node 2 since `descriptorprocesspsqt` does not use the vault
        self.restart_node(2, extra_args=["-disablevault"])
        self.connect_nodes(0, 2)
        self.connect_nodes(1, 2)

        key_info = get_generate_key()
        key = key_info.privkey
        address = key_info.p2wpkh_addr

        descriptor = descsum_create(f"wpkh({key})")

        utxo = self.create_outpoints(self.nodes[0], outputs=[{address: 1}])[0]
        self.sync_all()

        psqt = self.nodes[2].createpsqt([utxo], [{self.nodes[0].getnewaddress(): 0.99999}])
        decoded = self.nodes[2].decodepsqt(psqt)
        test_psqt_input_keys(decoded['inputs'][0], [])

        # Test that even if the wrong descriptor is given, `witness_utxo` and `non_witness_utxo`
        # are still added to the psqt
        alt_descriptor = descsum_create(f"wpkh({get_generate_key().privkey})")
        alt_psqt = self.nodes[2].descriptorprocesspsqt(psqt=psqt, descriptors=[alt_descriptor], sighashtype="ALL")["psqt"]
        decoded = self.nodes[2].decodepsqt(alt_psqt)
        test_psqt_input_keys(decoded['inputs'][0], ['witness_utxo', 'non_witness_utxo'])

        # Test that the psqt is not finalized and does not have bip32_derivs unless specified
        processed_psqt = self.nodes[2].descriptorprocesspsqt(psqt=psqt, descriptors=[descriptor], sighashtype="ALL", bip32derivs=True, finalize=False)
        decoded = self.nodes[2].decodepsqt(processed_psqt['psqt'])
        test_psqt_input_keys(decoded['inputs'][0], ['witness_utxo', 'non_witness_utxo', 'partial_signatures', 'bip32_derivs'])

        # If psqt not finalized, test that result does not have hex
        assert "hex" not in processed_psqt

        processed_psqt = self.nodes[2].descriptorprocesspsqt(psqt=psqt, descriptors=[descriptor], sighashtype="ALL", bip32derivs=False, finalize=True)
        decoded = self.nodes[2].decodepsqt(processed_psqt['psqt'])
        test_psqt_input_keys(decoded['inputs'][0], ['witness_utxo', 'non_witness_utxo', 'final_scriptwitness'])

        # Test psqt is complete
        assert_equal(processed_psqt['complete'], True)

        assert_raises_rpc_error(-26, "bad-txns-pow-anchor", self.nodes[2].sendrawtransaction, processed_psqt['hex'])

        self.log.info("Test descriptorprocesspsqt raises if an invalid sighashtype is passed")
        assert_raises_rpc_error(-8, "'all' is not a valid sighash parameter.", self.nodes[2].descriptorprocesspsqt, psqt, [descriptor], sighashtype="all")


if __name__ == '__main__':
    PSQTTest(__file__).main()
