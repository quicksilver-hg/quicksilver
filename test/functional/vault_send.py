#!/usr/bin/env python3
# Copyright (c) 2020-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the send RPC command."""

from decimal import Decimal, getcontext

from test_framework.authproxy import JSONRPCException
from test_framework.descriptors import descsum_create
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_greater_than_or_equal,
    assert_raises_rpc_error,
)
from test_framework.vault_util import (
    calculate_input_weight,
)


class VaultSendTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 2
        # Exercise send funding, validation, and target-priced work without making
        # this RPC contract test depend on real-cycle luck. In particular, the valid
        # 1501-output setup transaction for the weight-limit cases otherwise spends
        # minutes grinding before the assertions under test are reached.
        self.extra_args = [["-txpownocycle=1"]] * self.num_nodes
        # whitelist peers to speed up tx relay / relaypool sync
        self.noban_tx_relay = True
        getcontext().prec = 8 # Cinnabar precision for Decimal

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()

    def test_send(self, from_vault, to_vault=None, amount=None, data=None,
                  add_to_vault=None, psqt=None,
                  inputs=None, add_inputs=None, include_unsafe=None, change_address=None, change_position=None, change_type=None,
                  spends_external=None, locktime=None, lock_unspents=None,
                  expect_error=None, solving_data=None, minconf=None):
        assert (amount is None) != (data is None)

        from_balance_before = from_vault.getbalances()["mine"]["trusted"]
        if include_unsafe:
            from_balance_before += from_vault.getbalances()["mine"]["untrusted_pending"]

        if to_vault is None:
            assert amount is None
        else:
            to_untrusted_pending_before = to_vault.getbalances()["mine"]["untrusted_pending"]

        if amount:
            dest = to_vault.getnewaddress()
            outputs = [{dest: amount}]
        else:
            outputs = [{"data": data}]

        # Construct options dictionary
        options = {}
        if add_to_vault is not None:
            options["add_to_vault"] = add_to_vault
        else:
            if psqt:
                add_to_vault = False
            else:
                add_to_vault = from_vault.getvaultinfo()["private_keys_enabled"] # Default value
        if psqt is not None:
            options["psqt"] = psqt
        if inputs is not None:
            options["inputs"] = inputs
        if add_inputs is not None:
            options["add_inputs"] = add_inputs
        if include_unsafe is not None:
            options["include_unsafe"] = include_unsafe
        if change_address is not None:
            options["change_address"] = change_address
        if change_position is not None:
            options["change_position"] = change_position
        if change_type is not None:
            options["change_type"] = change_type
        if locktime is not None:
            options["locktime"] = locktime
        if lock_unspents is not None:
            options["lock_unspents"] = lock_unspents
        if solving_data is not None:
            options["solving_data"] = solving_data
        if minconf is not None:
            options["minconf"] = minconf

        if len(options.keys()) == 0:
            options = None

        if expect_error is None:
            res = from_vault.send(outputs=outputs, options=options)
        else:
            try:
                assert_raises_rpc_error(expect_error[0], expect_error[1], from_vault.send,
                    outputs=outputs, options=options)
            except AssertionError:
                # Provide debug info if the test fails
                self.log.error("Unexpected successful result:")
                self.log.error(options)
                res = from_vault.send(outputs=outputs, options=options)
                self.log.error(res)
                if "txid" in res and add_to_vault:
                    self.log.error("Transaction details:")
                    try:
                        tx = from_vault.gettransaction(res["txid"])
                        self.log.error(tx)
                        self.log.error("testrelaypoolaccept (transaction may already be in relaypool):")
                        self.log.error(from_vault.testrelaypoolaccept([tx["hex"]]))
                    except JSONRPCException as exc:
                        self.log.error(exc)

                raise

            return

        if locktime:
            return res

        # A pre-selected input belonging to another vault cannot be signed here, however
        # many keys this vault holds, so the result is a PSQT for the other vault to finish.
        if from_vault.getvaultinfo()["private_keys_enabled"] and not spends_external:
            assert_equal(res["complete"], True)
            assert "txid" in res
        else:
            assert_equal(res["complete"], False)
            assert not "txid" in res
            assert "psqt" in res

        from_balance = from_vault.getbalances()["mine"]["trusted"]
        if include_unsafe:
            from_balance += from_vault.getbalances()["mine"]["untrusted_pending"]

        if add_to_vault:
            # Ensure transaction exists in the vault:
            tx = from_vault.gettransaction(res["txid"])
            assert tx
            # Ensure transaction exists in the relaypool:
            tx = from_vault.getrawtransaction(res["txid"], 1)
            assert tx
            if amount:
                assert_greater_than_or_equal(from_balance_before - from_balance, amount)
            else:
                assert next((out for out in tx["vout"] if out["output_script"]["asm"] == "OP_RETURN 35"), None)
        else:
            assert_equal(from_balance_before, from_balance)

        if to_vault:
            self.sync_relaypools()
            if add_to_vault:
                assert_equal(to_vault.getbalances()["mine"]["untrusted_pending"], to_untrusted_pending_before + Decimal(amount if amount else 0))
            else:
                assert_equal(to_vault.getbalances()["mine"]["untrusted_pending"], to_untrusted_pending_before)

        return res

    def run_test(self):
        self.log.info("Setup vaults...")
        # w0 is a vault with coinbase rewards
        w0 = self.nodes[0].get_vault_rpc(self.default_vault_name)
        # w1 is a regular vault
        self.nodes[1].createvault(vault_name="w1")
        w1 = self.nodes[1].get_vault_rpc("w1")
        # w2 contains the private keys for w3
        self.nodes[1].createvault(vault_name="w2", blank=True)
        w2 = self.nodes[1].get_vault_rpc("w2")
        xpriv = "sqrv1wkfAGt6m8urrG6yVov7wQPehYVJZJcRzZecBjZBF7HkEzqkRXX5TNBkkJZGvKGG3dwBV1VQN2Y4CPNdx2ewwy3bs5MPtEUejkvaWzSh1wr"
        xpub = "squb6UShJgvLuWbXjj8mPTaiLp3mGa1EidoLZsFPUFbUfP695V6X3vLfdrocvSYTVQMn5rKPXuaayeDF8GW2eYG43qvHTMiRjNqSULBF8QqhG6n"
        w2.importdescriptors([{
            "desc": descsum_create("wpkh(" + xpriv + "/0/0/*)"),
            "timestamp": "now",
            "range": [0, 100],
            "active": True
        },{
            "desc": descsum_create("wpkh(" + xpriv + "/0/1/*)"),
            "timestamp": "now",
            "range": [0, 100],
            "active": True,
            "internal": True
        }])

        # w3 tracks w2's scripts without holding its keys
        self.nodes[1].createvault(vault_name="w3", disable_private_keys=True)
        w3 = self.nodes[1].get_vault_rpc("w3")
        # Match the privkeys in w2 for descriptors.
        res = w3.importdescriptors([{
            "desc": descsum_create("wpkh(" + xpub + "/0/0/*)"),
            "timestamp": "now",
            "range": [0, 100],
            "keypool": True,
            "active": True
        },{
            "desc": descsum_create("wpkh(" + xpub + "/0/1/*)"),
            "timestamp": "now",
            "range": [0, 100],
            "keypool": True,
            "active": True,
            "internal": True
        }])
        assert_equal(res, [{"success": True}, {"success": True}])

        for _ in range(3):
            a2_receive = w2.getnewaddress()

        w0.sendtoaddress(a2_receive, 10) # fund w3
        self.generate(self.nodes[0], 1)

        self.log.info("Reject dict outputs...")
        assert_raises_rpc_error(-3, "JSON value of type object is not of expected type array",
                                w0.send, outputs={w1.getnewaddress(): 1})

        self.log.info("Send to address...")
        self.test_send(from_vault=w0, to_vault=w1, amount=1)
        self.test_send(from_vault=w0, to_vault=w1, amount=1, add_to_vault=True)

        self.log.info("Don't broadcast...")
        res = self.test_send(from_vault=w0, to_vault=w1, amount=1, add_to_vault=False)
        assert res["hex"]

        self.log.info("Return PSQT...")
        res = self.test_send(from_vault=w0, to_vault=w1, amount=1, psqt=True)
        assert res["psqt"]

        self.log.info("Create transaction that spends to address, but don't broadcast...")
        self.test_send(from_vault=w0, to_vault=w1, amount=1, add_to_vault=False)

        self.log.info("Create PSQT from tracking vault w3, sign with w2...")
        res = self.test_send(from_vault=w3, to_vault=w1, amount=1)
        res = w2.vaultprocesspsqt(res["psqt"])
        assert res["complete"]

        self.log.info("Create OP_RETURN...")
        self.test_send(from_vault=w0, to_vault=w1, amount=1)
        self.test_send(from_vault=w0, data="Hello World", expect_error=(-8, "Data must be hexadecimal string (not 'Hello World')"))
        self.test_send(from_vault=w0, data="23", inputs=[w0.listunspent()[0]], add_inputs=False)
        res = self.test_send(from_vault=w3, data="23", inputs=[w3.listunspent()[0]], add_inputs=False)
        res = w2.vaultprocesspsqt(res["psqt"])
        assert res["complete"]

        self.log.info("If inputs are specified, do not automatically add more...")
        utxo1 = w0.listunspent()[0]
        target_amount = utxo1["amount"] + Decimal(1)
        res = self.test_send(from_vault=w0, to_vault=w1, amount=target_amount, inputs=[], add_to_vault=False)
        assert res["complete"]
        ERR_NOT_ENOUGH_PRESET_INPUTS = "The preselected coins total amount does not cover the transaction target. " \
                                       "Please allow other inputs to be automatically selected or include more coins manually"
        self.test_send(from_vault=w0, to_vault=w1, amount=target_amount, inputs=[utxo1],
                       expect_error=(-4, ERR_NOT_ENOUGH_PRESET_INPUTS))
        self.test_send(from_vault=w0, to_vault=w1, amount=target_amount, inputs=[utxo1], add_inputs=False,
                       expect_error=(-4, ERR_NOT_ENOUGH_PRESET_INPUTS))
        res = self.test_send(from_vault=w0, to_vault=w1, amount=target_amount, inputs=[utxo1], add_inputs=True, add_to_vault=False)
        assert res["complete"]

        self.log.info("Manual change address and position...")
        self.test_send(from_vault=w0, to_vault=w1, amount=1, change_address="not an address",
                       expect_error=(-5, "Change address must be a valid Quicksilver address"))
        change_address = w0.getnewaddress()
        self.test_send(from_vault=w0, to_vault=w1, amount=1, add_to_vault=False, change_address=change_address)
        assert res["complete"]
        res = self.test_send(from_vault=w0, to_vault=w1, amount=1, add_to_vault=False, change_address=change_address, change_position=0)
        assert res["complete"]
        assert_equal(self.nodes[0].decodepsqt(res["psqt"])["tx"]["vout"][0]["output_script"]["address"], change_address)
        res = self.test_send(from_vault=w0, to_vault=w1, amount=1, add_to_vault=False, change_type="bech32", change_position=0)
        assert res["complete"]
        change_output = self.nodes[0].decodepsqt(res["psqt"])["tx"]["vout"][0]["output_script"]
        assert_equal(change_output["type"], "witness_v0_keyhash")

        self.log.info("Set lock time...")
        height = self.nodes[0].getblockchaininfo()["blocks"]
        res = self.test_send(from_vault=w0, to_vault=w1, amount=1, locktime=height + 1)
        assert res["complete"]
        assert res["txid"]
        txid = res["txid"]
        # Although the vault finishes the transaction, it can't be added to the relaypool yet:
        hex = self.nodes[0].gettransaction(res["txid"])["hex"]
        res = self.nodes[0].testrelaypoolaccept([hex])
        assert not res[0]["allowed"]
        assert_equal(res[0]["reject-reason"], "non-final")
        # It shouldn't be confirmed in the next block
        self.generate(self.nodes[0], 1)
        assert_equal(self.nodes[0].gettransaction(txid)["confirmations"], 0)
        # The relaypool should allow it now:
        res = self.nodes[0].testrelaypoolaccept([hex])
        assert res[0]["allowed"]
        # Don't wait for vault to add it to the relaypool:
        res = self.nodes[0].sendrawtransaction(hex)
        self.generate(self.nodes[0], 1)
        assert_equal(self.nodes[0].gettransaction(txid)["confirmations"], 1)

        self.log.info("Lock unspents...")
        utxo1 = w0.listunspent()[0]
        assert_greater_than(utxo1["amount"], 1)
        res = self.test_send(from_vault=w0, to_vault=w1, amount=1, inputs=[utxo1], add_to_vault=False, lock_unspents=True)
        assert res["complete"]
        locked_coins = w0.listlockunspent()
        assert_equal(len(locked_coins), 1)
        # Locked coins are automatically unlocked when manually selected
        res = self.test_send(from_vault=w0, to_vault=w1, amount=1, inputs=[utxo1], add_to_vault=False)
        assert res["complete"]

        self.log.info("Include unsafe inputs")
        self.nodes[1].createvault(vault_name="w5")
        w5 = self.nodes[1].get_vault_rpc("w5")
        self.test_send(from_vault=w0, to_vault=w5, amount=2)
        self.test_send(from_vault=w5, to_vault=w0, amount=1, expect_error=(-4, "Insufficient funds"))
        res = self.test_send(from_vault=w5, to_vault=w0, amount=1, include_unsafe=True)
        assert res["complete"]

        self.log.info("Minconf")
        self.nodes[1].createvault(vault_name="minconfw")
        minconfw= self.nodes[1].get_vault_rpc("minconfw")
        self.test_send(from_vault=w0, to_vault=minconfw, amount=2)
        self.generate(self.nodes[0], 3)
        self.test_send(from_vault=minconfw, to_vault=w0, amount=1, minconf=4, expect_error=(-4, "Insufficient funds"))
        self.test_send(from_vault=minconfw, to_vault=w0, amount=1, minconf=-4, expect_error=(-8, "Negative minconf"))
        res = self.test_send(from_vault=minconfw, to_vault=w0, amount=1, minconf=3)
        assert res["complete"]

        self.log.info("External outputs")
        privkey = self.nodes[1].get_deterministic_priv_key().key

        self.nodes[1].createvault("extsend")
        ext_vault = self.nodes[1].get_vault_rpc("extsend")
        self.nodes[1].createvault("extfund")
        ext_fund = self.nodes[1].get_vault_rpc("extfund")

        # Make a weird but signable native P2WSH script.
        desc = descsum_create("wsh(pkh({}))".format(privkey))
        res = ext_fund.importdescriptors([{"desc": desc, "timestamp": "now"}])
        assert res[0]["success"]
        addr = self.nodes[0].deriveaddresses(desc)[0]
        addr_info = ext_fund.getaddressinfo(addr)

        self.nodes[0].sendtoaddress(addr, 10)
        self.nodes[0].sendtoaddress(ext_vault.getnewaddress(), 10)
        self.generate(self.nodes[0], 6)
        ext_utxo = ext_fund.listunspent(addresses=[addr])[0]

        # An external input without solving data should result in an error
        self.test_send(from_vault=ext_vault, to_vault=self.nodes[0], amount=15, inputs=[ext_utxo], add_inputs=True, psqt=True, expect_error=(-4, "Not solvable pre-selected input COutPoint(%s, %s)" % (ext_utxo["txid"][0:10], ext_utxo["vout"])))

        # But funding should work when the solving data is provided
        res = self.test_send(from_vault=ext_vault, to_vault=self.nodes[0], amount=15, inputs=[ext_utxo], add_inputs=True, psqt=True, spends_external=True, solving_data={"pubkeys": [addr_info['pubkey']], "scripts": [addr_info["embedded"]["output_script"]]})
        signed = ext_vault.vaultprocesspsqt(res["psqt"])
        signed = ext_fund.vaultprocesspsqt(res["psqt"])
        assert signed["complete"]

        res = self.test_send(from_vault=ext_vault, to_vault=self.nodes[0], amount=15, inputs=[ext_utxo], add_inputs=True, psqt=True, spends_external=True, solving_data={"descriptors": [desc]})
        signed = ext_vault.vaultprocesspsqt(res["psqt"])
        signed = ext_fund.vaultprocesspsqt(res["psqt"])
        assert signed["complete"]

        dec = self.nodes[0].decodepsqt(signed["psqt"])
        for i, txin in enumerate(dec["tx"]["vin"]):
            if txin["txid"] == ext_utxo["txid"] and txin["vout"] == ext_utxo["vout"]:
                input_idx = i
                break
        psqt_in = dec["inputs"][input_idx]
        scriptsig_hex = psqt_in["final_scriptSig"]["hex"] if "final_scriptSig" in psqt_in else ""
        witness_stack_hex = psqt_in["final_scriptwitness"] if "final_scriptwitness" in psqt_in else None
        input_weight = calculate_input_weight(scriptsig_hex, witness_stack_hex)

        # Input weight error conditions
        assert_raises_rpc_error(
            -8,
            "Input weights should be specified in inputs rather than in options.",
            ext_vault.send,
            outputs=[{self.nodes[0].getnewaddress(): 15}],
            options={"inputs": [ext_utxo], "input_weights": [{"txid": ext_utxo["txid"], "vout": ext_utxo["vout"], "weight": 1000}]}
        )

        # Funding should also work when input weights are provided
        res = self.test_send(
            from_vault=ext_vault,
            to_vault=self.nodes[0],
            amount=15,
            inputs=[{"txid": ext_utxo["txid"], "vout": ext_utxo["vout"], "weight": input_weight}],
            add_inputs=True,
            psqt=True,
            spends_external=True,
        )
        signed = ext_vault.vaultprocesspsqt(res["psqt"])
        signed = ext_fund.vaultprocesspsqt(res["psqt"])
        assert signed["complete"]
        testres = self.nodes[0].testrelaypoolaccept([signed["hex"]])[0]
        assert_equal(testres["allowed"], True)
        assert "fees" not in testres

        # Check tx creation size limits
        self.test_weight_limits()

    def test_weight_limits(self):
        self.log.info("Test weight limits")

        self.nodes[1].createvault("test_weight_limits")
        vault = self.nodes[1].get_vault_rpc("test_weight_limits")

        # Generate future inputs; bech32 spends are smaller than Base58 spends,
        # so use enough inputs to exceed the max standard tx weight.
        oversized_inputs = 1500
        outputs = []
        for _ in range(oversized_inputs + 1):
            outputs.append({vault.getnewaddress(address_type="bech32"): 0.1})
        self.nodes[0].send(outputs=outputs)
        self.generate(self.nodes[0], 1)

        # 1) Try to fund transaction only using the preset inputs
        inputs = vault.listunspent()
        assert_raises_rpc_error(-4, "Transaction too large",
                                vault.send, outputs=[{vault.getnewaddress(): Decimal("0.1") * oversized_inputs}], options={"inputs": inputs, "add_inputs": False})

        # 2) Let the vault fund the transaction
        assert_raises_rpc_error(-4, "The inputs size exceeds the maximum weight. Please try sending a smaller amount or manually consolidating your vault's UTXOs",
                                vault.send, outputs=[{vault.getnewaddress(): Decimal("0.1") * oversized_inputs}])

        # 3) Pre-select some inputs and let the vault fill-up the remaining amount
        inputs = inputs[0:1000]
        assert_raises_rpc_error(-4, "The combination of the pre-selected inputs and the vault automatic inputs selection exceeds the transaction maximum weight. Please try sending a smaller amount or manually consolidating your vault's UTXOs",
                                vault.send, outputs=[{vault.getnewaddress(): Decimal("0.1") * oversized_inputs}], options={"inputs": inputs, "add_inputs": True})

        self.nodes[1].unloadvault("test_weight_limits")


if __name__ == '__main__':
    VaultSendTest(__file__).main()
