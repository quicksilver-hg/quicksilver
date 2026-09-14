#!/usr/bin/env python3
# Copyright (c) 2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the sendall RPC command."""

from decimal import Decimal, getcontext

from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
)

# Decorator to reset activevault to zero utxos
def cleanup(func):
    def wrapper(self):
        try:
            func(self)
        finally:
            if 0 < self.vault.getbalances()["mine"]["trusted"]:
                self.vault.sendall([self.remainder_target])
            assert_equal(0, self.vault.getbalances()["mine"]["trusted"]) # vault is empty
    return wrapper

class SendallTest(QuicksilverTestFramework):
    # Setup and helpers
    def add_options(self, parser):
        self.add_vault_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()

    def set_test_params(self):
        getcontext().prec=10
        self.num_nodes = 1
        self.setup_clean_chain = True

    def assert_balance_swept_completely(self, tx, balance):
        output_sum = sum([o["value"] for o in tx["decoded"]["vout"]])
        assert "fee" not in tx
        assert_equal(output_sum, balance)
        assert_equal(0, self.vault.getbalances()["mine"]["trusted"]) # vault is empty

    def assert_tx_has_output(self, tx, addr, value=None):
        for output in tx["decoded"]["vout"]:
            if addr == output["output_script"]["address"] and value is None or value == output["value"]:
                return
        raise AssertionError("Output to {} not present or wrong amount".format(addr))

    def assert_tx_has_outputs(self, tx, expected_outputs):
        assert_equal(len(expected_outputs), len(tx["decoded"]["vout"]))
        for eo in expected_outputs:
            self.assert_tx_has_output(tx, eo["address"], eo["value"])

    def add_utxos(self, amounts):
        for a in amounts:
            self.def_vault.sendtoaddress(self.vault.getnewaddress(), a)
        self.generate(self.nodes[0], 1)
        assert_greater_than(self.vault.getbalances()["mine"]["trusted"], 0)
        return self.vault.getbalances()["mine"]["trusted"]

    # Helper schema for success cases
    def test_sendall_success(self, sendall_args, remaining_balance = 0):
        sendall_tx_receipt = self.vault.sendall(sendall_args)
        # sendall must produce a transaction the network will actually take. It did
        # not: it skipped the per-tx proof-of-work grind entirely, so every
        # transaction it built was rejected as bad-txns-pow-anchor with anchor=0
        # while the RPC still returned {"txid": ..., "complete": true}. This whole
        # file passed throughout, because it only ever read the receipt and the
        # resulting balance -- and the balance moves either way, since the vault
        # records the transaction as its own whether or not it is broadcast.
        self.nodes[0].getrelaypoolentry(sendall_tx_receipt["txid"])
        self.generate(self.nodes[0], 1)
        # vault has remaining balance (usually empty)
        assert_equal(remaining_balance, self.vault.getbalances()["mine"]["trusted"])

        assert_equal(sendall_tx_receipt["complete"], True)
        return self.vault.gettransaction(txid = sendall_tx_receipt["txid"], verbose = True)

    @cleanup
    def gen_and_clean(self):
        self.add_utxos([15, 2, 4])

    def test_cleanup(self):
        self.log.info("Test that cleanup wrapper empties vault")
        self.gen_and_clean()
        assert_equal(0, self.vault.getbalances()["mine"]["trusted"]) # vault is empty

    # Actual tests
    @cleanup
    def sendall_two_utxos(self):
        self.log.info("Testing basic sendall case without specific amounts")
        pre_sendall_balance = self.add_utxos([10,11])
        tx_from_vault = self.test_sendall_success(sendall_args = [self.remainder_target])

        self.assert_tx_has_outputs(tx = tx_from_vault,
            expected_outputs = [
                { "address": self.remainder_target, "value": pre_sendall_balance }
            ]
        )
        self.assert_balance_swept_completely(tx_from_vault, pre_sendall_balance)

    @cleanup
    def sendall_split(self):
        self.log.info("Testing sendall where two recipients have unspecified amount")
        pre_sendall_balance = self.add_utxos([1, 2, 3, 15])
        tx_from_vault = self.test_sendall_success([self.remainder_target, self.split_target])

        half = pre_sendall_balance / 2
        self.assert_tx_has_outputs(tx_from_vault,
            expected_outputs = [
                { "address": self.split_target, "value": half },
                { "address": self.remainder_target, "value": half }
            ]
        )
        self.assert_balance_swept_completely(tx_from_vault, pre_sendall_balance)

    @cleanup
    def sendall_and_spend(self):
        self.log.info("Testing sendall in combination with paying specified amount to recipient")
        pre_sendall_balance = self.add_utxos([8, 13])
        tx_from_vault = self.test_sendall_success([{self.recipient: 5}, self.remainder_target])

        self.assert_tx_has_outputs(tx_from_vault,
            expected_outputs = [
                { "address": self.recipient, "value": 5 },
                { "address": self.remainder_target, "value": pre_sendall_balance - 5 }
            ]
        )
        self.assert_balance_swept_completely(tx_from_vault, pre_sendall_balance)

    @cleanup
    def sendall_invalid_recipient_addresses(self):
        self.log.info("Test having only recipient with specified amount, missing recipient with unspecified amount")
        self.add_utxos([12, 9])

        assert_raises_rpc_error(
                -8,
                "Must provide at least one address without a specified amount" ,
                self.vault.sendall,
                [{self.recipient: 5}]
            )

    @cleanup
    def sendall_duplicate_recipient(self):
        self.log.info("Test duplicate destination")
        self.add_utxos([1, 8, 3, 9])

        assert_raises_rpc_error(
                -8,
                "Invalid parameter, duplicated address: {}".format(self.remainder_target),
                self.vault.sendall,
                [self.remainder_target, self.remainder_target]
            )

    @cleanup
    def sendall_invalid_amounts(self):
        self.log.info("Test sending more than balance")
        pre_sendall_balance = self.add_utxos([7, 14])

        assert_raises_rpc_error(-6, "Assigned more value to outputs than available funds.", self.vault.sendall,
                [{self.recipient: pre_sendall_balance + 1}, self.remainder_target])
        exact_balance_tx = self.vault.sendall(
            recipients=[{self.recipient: pre_sendall_balance}, self.remainder_target],
            add_to_vault=False,
        )
        decoded_exact_balance = self.vault.decoderawtransaction(exact_balance_tx["hex"])
        assert_equal(True, any(
            output["output_script"]["address"] == self.recipient and output["value"] == pre_sendall_balance
            for output in decoded_exact_balance["vout"]
        ))
        one_sat_tx = self.vault.sendall(
            recipients=[{self.recipient: Decimal("0.00000001")}, self.remainder_target],
            add_to_vault=False,
        )
        decoded = self.vault.decoderawtransaction(one_sat_tx["hex"])
        assert_equal(True, any(
            output["output_script"]["address"] == self.recipient and output["value"] == Decimal("0.00000001")
            for output in decoded["vout"]
        ))

    # @cleanup not needed because different vault used
    def sendall_negative_effective_value(self):
        self.log.info("Test that small UTXOs remain spendable")
        # Use dedicated vault for tiny amounts and unload vault at end.
        self.nodes[0].createvault("smallvault")
        small_vault = self.nodes[0].get_vault_rpc("smallvault")

        self.def_vault.sendtoaddress(small_vault.getnewaddress(), 0.00000400)
        self.def_vault.sendtoaddress(small_vault.getnewaddress(), 0.00000300)
        self.generate(self.nodes[0], 1)
        assert_greater_than(small_vault.getbalances()["mine"]["trusted"], 0)

        sendall_tx_receipt = small_vault.sendall(recipients=[self.remainder_target])
        tx_from_vault = small_vault.gettransaction(txid=sendall_tx_receipt["txid"], verbose=True)
        self.assert_tx_has_outputs(tx_from_vault, [{"address": self.remainder_target, "value": Decimal("0.00000700")}])
        assert "fee" not in tx_from_vault

        small_vault.unloadvault()

    @cleanup
    def sendall_spends_uneconomic_utxos(self):
        self.log.info("Check that sendall spends every UTXO, including tiny ones")
        # Quicksilver has no fee-based uneconomic-UTXO filter, so the two small
        # UTXOs below are spent by default rather than needing an opt-in flag.
        self.add_utxos([0.00000400, 0.00000300, 1])

        sendall_tx_receipt = self.vault.sendall(recipients=[self.remainder_target])
        tx_from_vault = self.vault.gettransaction(txid = sendall_tx_receipt["txid"], verbose = True)

        assert_equal(len(tx_from_vault["decoded"]["vin"]), 3)
        self.assert_tx_has_outputs(tx_from_vault, [{"address": self.remainder_target, "value": Decimal("1.00000700")}])
        assert_equal(self.vault.getbalances()["mine"]["trusted"], 0)

    @cleanup
    def sendall_specific_inputs(self):
        self.log.info("Test sendall with a subset of UTXO pool")
        self.add_utxos([17, 4])
        utxo = self.vault.listunspent()[0]

        sendall_tx_receipt = self.vault.sendall(recipients=[self.remainder_target], inputs=[utxo])
        tx_from_vault = self.vault.gettransaction(txid = sendall_tx_receipt["txid"], verbose = True)
        assert_equal(len(tx_from_vault["decoded"]["vin"]), 1)
        assert_equal(len(tx_from_vault["decoded"]["vout"]), 1)
        assert_equal(tx_from_vault["decoded"]["vin"][0]["txid"], utxo["txid"])
        assert_equal(tx_from_vault["decoded"]["vin"][0]["vout"], utxo["vout"])
        self.assert_tx_has_output(tx_from_vault, self.remainder_target)

        self.generate(self.nodes[0], 1)
        assert_greater_than(self.vault.getbalances()["mine"]["trusted"], 0)

    @cleanup
    def sendall_fails_on_missing_input(self):
        # fails because UTXO was previously spent, and vault is empty
        self.log.info("Test sendall fails because specified UTXO is not available")
        self.add_utxos([16, 5])
        spent_utxo = self.vault.listunspent()[0]

        # fails on out of bounds vout
        assert_raises_rpc_error(-8,
                "Input not found. UTXO ({}:{}) is not part of vault.".format(spent_utxo["txid"], 1000),
                self.vault.sendall, recipients=[self.remainder_target], inputs=[{"txid": spent_utxo["txid"], "vout": 1000}])

        # fails on unconfirmed spent UTXO
        self.vault.sendall(recipients=[self.remainder_target])
        assert_raises_rpc_error(-8,
                "Input not available. UTXO ({}:{}) was already spent.".format(spent_utxo["txid"], spent_utxo["vout"]),
                self.vault.sendall, recipients=[self.remainder_target], inputs=[spent_utxo])

        # fails on specific previously spent UTXO, while other UTXOs exist
        self.generate(self.nodes[0], 1)
        self.add_utxos([19, 2])
        assert_raises_rpc_error(-8,
                "Input not available. UTXO ({}:{}) was already spent.".format(spent_utxo["txid"], spent_utxo["vout"]),
                self.vault.sendall, recipients=[self.remainder_target], inputs=[spent_utxo])

        # fails because UTXO is unknown, while other UTXOs exist
        foreign_utxo = self.def_vault.listunspent()[0]
        assert_raises_rpc_error(-8, "Input not found. UTXO ({}:{}) is not part of vault.".format(foreign_utxo["txid"],
            foreign_utxo["vout"]), self.vault.sendall, recipients=[self.remainder_target],
            inputs=[foreign_utxo])

    @cleanup
    def sendall_fails_on_no_address(self):
        self.log.info("Test sendall fails because no address is provided")
        self.add_utxos([19, 2])

        assert_raises_rpc_error(
                -8,
                "Must provide at least one address without a specified amount" ,
                self.vault.sendall,
                []
            )

    @cleanup
    def sendall_preserves_value(self):
        self.log.info("Test sendall preserves value exactly")
        self.add_utxos([21])

        sendall_tx_receipt = self.vault.sendall(recipients=[self.remainder_target])
        tx_from_vault = self.vault.gettransaction(txid=sendall_tx_receipt["txid"], verbose=True)
        assert "fee" not in tx_from_vault
        self.assert_tx_has_outputs(tx_from_vault, [{"address": self.remainder_target, "value": Decimal("21")}])

    @cleanup
    def sendall_spends_low_value(self):
        self.log.info("Test sendall can spend a low-value transaction")
        self.add_utxos([1])
        sendall_tx_receipt = self.vault.sendall(recipients=[self.recipient])
        tx_from_vault = self.vault.gettransaction(txid=sendall_tx_receipt["txid"], verbose=True)
        assert "fee" not in tx_from_vault
        self.assert_tx_has_outputs(tx_from_vault, [{"address": self.recipient, "value": Decimal("1")}])

    @cleanup
    def sendall_tracking_vault_specific_inputs(self):
        self.log.info("Test sendall with a subset of the UTXO pool in a key-disabled tracking vault")
        self.add_utxos([17, 4])
        utxo = self.vault.listunspent()[0]

        self.nodes[0].createvault(vault_name="watching", disable_private_keys=True)
        tracking_vault = self.nodes[0].get_vault_rpc("watching")

        import_req = [{
            "desc": utxo["desc"],
            "timestamp": 0,
        }]
        tracking_vault.importdescriptors(import_req)

        sendall_tx_receipt = tracking_vault.sendall(recipients=[self.remainder_target], inputs=[utxo])
        psqt = sendall_tx_receipt["psqt"]
        decoded = self.nodes[0].decodepsqt(psqt)
        assert_equal(len(decoded["inputs"]), 1)
        assert_equal(len(decoded["outputs"]), 1)
        assert_equal(decoded["tx"]["vin"][0]["txid"], utxo["txid"])
        assert_equal(decoded["tx"]["vin"][0]["vout"], utxo["vout"])
        assert_equal(decoded["tx"]["vout"][0]["output_script"]["address"], self.remainder_target)

    @cleanup
    def sendall_with_minconf(self):
        # utxo of 17 bicoin has 6 confirmations, utxo of 4 has 3
        self.add_utxos([17])
        self.generate(self.nodes[0], 2)
        self.add_utxos([4])
        self.generate(self.nodes[0], 2)

        self.log.info("Test sendall fails because minconf is negative")

        assert_raises_rpc_error(-8,
            "Invalid minconf (minconf cannot be negative): -2",
            self.vault.sendall,
            recipients=[self.remainder_target],
            options={"minconf": -2})
        self.log.info("Test sendall fails because minconf is used while specific inputs are provided")

        utxo = self.vault.listunspent()[0]
        assert_raises_rpc_error(-8,
            "Cannot combine minconf or maxconf with specific inputs.",
            self.vault.sendall,
            recipients=[self.remainder_target],
            options={"inputs": [utxo], "minconf": 2})

        self.log.info("Test sendall fails because there are no utxos with enough confirmations specified by minconf")

        assert_raises_rpc_error(-6,
            "Total value of UTXO pool too low to pay requested outputs.",
            self.vault.sendall,
            recipients=[self.remainder_target],
            options={"minconf": 7})

        self.log.info("Test sendall only spends utxos with a specified number of confirmations when minconf is used")
        self.vault.sendall(recipients=[self.remainder_target], options={"minconf": 6})

        assert_equal(len(self.vault.listunspent()), 1)
        assert_equal(self.vault.listunspent()[0]['confirmations'], 3)

        # decrease minconf and show the remaining utxo is picked up
        self.vault.sendall(recipients=[self.remainder_target], options={"minconf": 3})
        assert_equal(self.vault.getbalance(), 0)

    @cleanup
    def sendall_with_maxconf(self):
        # utxo of 17 bicoin has 6 confirmations, utxo of 4 has 3
        self.add_utxos([17])
        self.generate(self.nodes[0], 2)
        self.add_utxos([4])
        self.generate(self.nodes[0], 2)

        self.log.info("Test sendall fails because there are no utxos with enough confirmations specified by maxconf")
        assert_raises_rpc_error(-6,
            "Total value of UTXO pool too low to pay requested outputs.",
            self.vault.sendall,
            recipients=[self.remainder_target],
            options={"maxconf": 1})

        self.log.info("Test sendall only spends utxos with a specified number of confirmations when maxconf is used")
        self.vault.sendall(recipients=[self.remainder_target], options={"maxconf":4})
        assert_equal(len(self.vault.listunspent()), 1)
        assert_equal(self.vault.listunspent()[0]['confirmations'], 6)

    @cleanup
    def sendall_spends_unconfirmed_change(self):
        self.log.info("Test that sendall spends unconfirmed change")
        self.add_utxos([17])
        self.vault.sendtoaddress(self.remainder_target, 10)
        assert_greater_than(self.vault.getbalances()["mine"]["trusted"], 6)
        self.test_sendall_success(sendall_args = [self.remainder_target])

        assert_equal(self.vault.getbalance(), 0)

    @cleanup
    def sendall_spends_unconfirmed_inputs_if_specified(self):
        self.log.info("Test that sendall spends specified unconfirmed inputs")
        self.def_vault.sendtoaddress(self.vault.getnewaddress(), 17)
        self.vault.syncwithvalidationinterfacequeue()
        assert_equal(self.vault.getbalances()["mine"]["untrusted_pending"], 17)
        unspent = self.vault.listunspent(minconf=0)[0]

        self.vault.sendall(recipients=[self.remainder_target], inputs=[unspent])
        assert_equal(self.vault.getbalance(), 0)

    @cleanup
    def sendall_does_ancestor_aware_funding(self):
        self.log.info("Test that sendall does ancestor aware funding for unconfirmed inputs")

        self.def_vault.sendtoaddress(address=self.vault.getnewaddress(), amount=17)
        self.vault.syncwithvalidationinterfacequeue()

        assert_equal(self.vault.getbalances()["mine"]["untrusted_pending"], 17)
        unspent = self.vault.listunspent(minconf=0)[0]

        parent_txid = unspent["txid"]
        assert_equal(self.vault.gettransaction(parent_txid)["confirmations"], 0)

        res_1 = self.vault.sendall(recipients=[self.def_vault.getnewaddress()], inputs=[unspent], add_to_vault=False, lock_unspents=True)
        child_hex = res_1["hex"]

        child_tx = self.vault.decoderawtransaction(child_hex)
        first_child_amount = child_tx["vout"][0]["value"]

        self.def_vault.sendtoaddress(address=self.vault.getnewaddress(), amount=17)
        self.vault.syncwithvalidationinterfacequeue()
        assert_equal(self.vault.getbalances()["mine"]["untrusted_pending"], 34)
        unspent = self.vault.listunspent(minconf=0)[0]

        parent_txid = unspent["txid"]
        assert_equal(self.vault.gettransaction(parent_txid)["confirmations"], 0)

        res_2 = self.vault.sendall(recipients=[self.def_vault.getnewaddress()], inputs=[unspent], add_to_vault=False, lock_unspents=True)
        child_hex = res_2["hex"]

        child_tx = self.vault.decoderawtransaction(child_hex)
        second_child_amount = child_tx["vout"][0]["value"]

        assert_equal(first_child_amount, second_child_amount)

    # This tests needs to be the last one otherwise @cleanup will fail with "Transaction too large" error
    def sendall_fails_with_transaction_too_large(self):
        self.log.info("Test that sendall fails if resulting transaction is too large")

        # Force the vault to bulk-generate the addresses we'll need
        self.vault.keypoolrefill(1600)

        # create many inputs
        outputs = {self.vault.getnewaddress(): 0.000025 for _ in range(1600)}
        self.def_vault.sendmany(amounts=outputs)
        self.generate(self.nodes[0], 1)

        assert_raises_rpc_error(
                -4,
                "Transaction too large.",
                self.vault.sendall,
                recipients=[self.remainder_target])

    def run_test(self):
        self.nodes[0].createvault("activevault")
        self.vault = self.nodes[0].get_vault_rpc("activevault")
        self.def_vault = self.nodes[0].get_vault_rpc(self.default_vault_name)
        self.generate(self.nodes[0], 101)
        self.recipient = self.def_vault.getnewaddress() # payee for a specific amount
        self.remainder_target = self.def_vault.getnewaddress() # address that receives everything left after payments
        self.split_target = self.def_vault.getnewaddress() # 2nd target when splitting rest

        # Test cleanup
        self.test_cleanup()

        # Basic sweep: everything to one address
        self.sendall_two_utxos()

        # Split remainder to two addresses with equal amounts
        self.sendall_split()

        # Pay recipient and sweep remainder
        self.sendall_and_spend()

        # sendall fails if no recipient has unspecified amount
        self.sendall_invalid_recipient_addresses()

        # Sendall fails if same destination is provided twice
        self.sendall_duplicate_recipient()

        # Sendall fails when trying to spend more than the balance
        self.sendall_invalid_amounts()

        # Sendall spends tiny UTXOs because Quicksilver has no fee-based uneconomic filter
        self.sendall_negative_effective_value()

        # sendall spends all eligible UTXOs, including uneconomic ones
        self.sendall_spends_uneconomic_utxos()

        # Sendall succeeds with specific inputs
        self.sendall_specific_inputs()

        # Fails for the right reasons on missing or previously spent UTXOs
        self.sendall_fails_on_missing_input()

        # Sendall fails when no address is provided
        self.sendall_fails_on_no_address()


        # Sendall preserves exact value
        self.sendall_preserves_value()

        # Sendall spends low-value transactions
        self.sendall_spends_low_value()

        # Sendall succeeds with tracking vaults spending specific UTXOs
        self.sendall_tracking_vault_specific_inputs()

        # Sendall only uses outputs with at least a give number of confirmations when using minconf
        self.sendall_with_minconf()

        # Sendall only uses outputs with less than a given number of confirmation when using minconf
        self.sendall_with_maxconf()

        # Sendall spends unconfirmed change
        self.sendall_spends_unconfirmed_change()

        # Sendall spends unconfirmed inputs if they are specified
        self.sendall_spends_unconfirmed_inputs_if_specified()

        # Sendall does ancestor aware funding when spending an unconfirmed UTXO
        self.sendall_does_ancestor_aware_funding()

        # Sendall fails when many inputs result to too large transaction
        self.sendall_fails_with_transaction_too_large()

if __name__ == '__main__':
    SendallTest(__file__).main()
