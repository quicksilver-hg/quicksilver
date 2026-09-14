#!/usr/bin/env python3
# Copyright (c) 2021-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""RPCs that handle raw transaction packages."""

from decimal import Decimal
import random

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.messages import (
    SEQUENCE_NONFINAL,
    tx_from_hex,
)
from test_framework.p2p import P2PTxInvStore
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)
from test_framework.vault import MiniVault


MAX_PACKAGE_COUNT = 25


class RPCPackagesTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-txpownocycle=1"]]
        # whitelist peers to speed up tx relay / relaypool sync
        self.noban_tx_relay = True

    def assert_testres_equal(self, package_hex, testres_expected):
        """Shuffle package_hex and assert that the testrelaypoolaccept result matches testres_expected. This should only
        be used to test packages where the order does not matter. The ordering of transactions in package_hex and
        testres_expected must match.
        """
        shuffled_indeces = list(range(len(package_hex)))
        random.shuffle(shuffled_indeces)
        shuffled_package = [package_hex[i] for i in shuffled_indeces]
        shuffled_testres = [testres_expected[i] for i in shuffled_indeces]
        assert_equal(shuffled_testres, self.nodes[0].testrelaypoolaccept(shuffled_package))

    def run_test(self):
        node = self.nodes[0]

        # get an UTXO that requires signature to be spent
        deterministic_address = node.get_deterministic_priv_key().address
        blockhash = self.generatetoaddress(node, 1, deterministic_address)[0]
        coinbase = node.getblock(blockhash=blockhash, verbosity=2)["tx"][0]
        coin = {
                "txid": coinbase["txid"],
                "amount": coinbase["vout"][0]["value"],
                "output_script": coinbase["vout"][0]["output_script"],
                "vout": 0,
                "height": 0
            }

        self.vault = MiniVault(self.nodes[0])
        self.generate(self.vault, COINBASE_MATURITY + 100)  # blocks generated for inputs

        self.log.info("Create some transactions")
        # Create some transactions that can be reused throughout the test. Never submit these to relaypool.
        self.independent_txns_hex = []
        self.independent_txns_testres = []
        for _ in range(3):
            tx_hex = self.vault.create_self_transfer()["hex"]
            testres = self.nodes[0].testrelaypoolaccept([tx_hex])
            assert testres[0]["allowed"]
            self.independent_txns_hex.append(tx_hex)
            # testrelaypoolaccept returns a list of length one, avoid creating a 2D list
            self.independent_txns_testres.append(testres[0])
        self.independent_txns_testres_blank = [{
            "txid": res["txid"], "wtxid": res["wtxid"]} for res in self.independent_txns_testres]

        self.test_independent(coin)
        self.test_chain()
        self.test_multiple_children()
        self.test_multiple_parents()
        self.test_conflicting()
        self.test_submitpackage()
        self.test_maxburn_submitpackage()

    def test_independent(self, coin):
        self.log.info("Test multiple independent transactions in a package")
        node = self.nodes[0]
        # For independent transactions, order doesn't matter.
        self.assert_testres_equal(self.independent_txns_hex, self.independent_txns_testres)

        self.log.info("Test an otherwise valid package with an extra garbage tx appended")
        address = node.get_deterministic_priv_key().address
        garbage_tx = node.createrawtransaction([{"txid": "00" * 32, "vout": 5}], [{address: 1}])
        tx = tx_from_hex(garbage_tx)
        self.vault._prove_tx_pow(tx)
        garbage_tx = tx.serialize().hex()
        # Only the txid and wtxids are returned because validation is incomplete for the independent txns.
        # Package validation is atomic: if the node cannot find a UTXO for any single tx in the package,
        # it terminates immediately to avoid unnecessary, expensive signature verification.
        package_bad = self.independent_txns_hex + [garbage_tx]
        testres_bad = self.independent_txns_testres_blank + [{"txid": tx.rehash(), "wtxid": tx.getwtxid(), "allowed": False, "reject-reason": "missing-inputs"}]
        self.assert_testres_equal(package_bad, testres_bad)

        self.log.info("Check testrelaypoolaccept tells us when some transactions completed validation successfully")
        tx_bad_sig_hex = node.createrawtransaction([{"txid": coin["txid"], "vout": coin["vout"]}],[{address : coin["amount"]}])
        tx_bad_sig = tx_from_hex(tx_bad_sig_hex)
        self.vault._prove_tx_pow(tx_bad_sig)
        tx_bad_sig_hex = tx_bad_sig.serialize().hex()
        testres_bad_sig = node.testrelaypoolaccept(self.independent_txns_hex + [tx_bad_sig_hex])
        # By the time the signature for the last transaction is checked, all the other transactions
        # have been fully validated, which is why the node returns full validation results for all
        # transactions here but empty results in other cases.
        tx_bad_sig_txid = tx_bad_sig.rehash()
        tx_bad_sig_wtxid = tx_bad_sig.getwtxid()
        assert_equal(testres_bad_sig, self.independent_txns_testres + [{
            "txid": tx_bad_sig_txid,
            "wtxid": tx_bad_sig_wtxid, "allowed": False,
            "reject-reason": "mandatory-script-verify-flag-failed (Operation not valid with the current stack size)",
            "reject-details": "mandatory-script-verify-flag-failed (Operation not valid with the current stack size), " +
                              f"input 0 of {tx_bad_sig_txid} (wtxid {tx_bad_sig_wtxid}), spending {coin['txid']}:{coin['vout']}"
        }])

        self.log.info("Check testrelaypoolaccept reports non-feeless txns in packages")
        tx_non_feeless = self.vault.create_self_transfer(surplus=Decimal("0.999"))
        testres_non_feeless = node.testrelaypoolaccept([tx_non_feeless["hex"]])
        assert_equal(testres_non_feeless[0]["txid"], tx_non_feeless["txid"])
        assert_equal(testres_non_feeless[0]["wtxid"], tx_non_feeless["wtxid"])
        assert_equal(testres_non_feeless[0]["allowed"], False)
        assert_equal(testres_non_feeless[0]["reject-reason"], "bad-txns-not-feeless")
        package_non_feeless = [tx_non_feeless["hex"]] + self.independent_txns_hex
        testres_package_non_feeless = node.testrelaypoolaccept(package_non_feeless)
        assert_equal(testres_package_non_feeless, testres_non_feeless + self.independent_txns_testres_blank)

    def test_chain(self):
        node = self.nodes[0]

        chain = self.vault.create_self_transfer_chain(chain_length=25)
        chain_hex = [t["hex"] for t in chain]
        chain_txns = [t["tx"] for t in chain]

        self.log.info("Check that testrelaypoolaccept requires packages to be sorted by dependency")
        assert_equal(node.testrelaypoolaccept(rawtxs=chain_hex[::-1]),
                [{"txid": tx.rehash(), "wtxid": tx.getwtxid(), "package-error": "package-not-sorted"} for tx in chain_txns[::-1]])

        self.log.info("Testrelaypoolaccept a chain of 25 transactions")
        testres_multiple = node.testrelaypoolaccept(rawtxs=chain_hex)

        testres_single = []
        # Test accept and then submit each one individually, which should be identical to package test accept
        for rawtx in chain_hex:
            testres = node.testrelaypoolaccept([rawtx])
            testres_single.append(testres[0])
            # Submit the transaction now so its child should have no problem validating
            node.sendrawtransaction(rawtx)
        assert_equal(testres_single, testres_multiple)

        # Clean up by clearing the relaypool
        self.generate(node, 1)

    def test_multiple_children(self):
        node = self.nodes[0]
        self.log.info("Testrelaypoolaccept a package in which a transaction has two children within the package")

        parent_tx = self.vault.create_self_transfer_multi(num_outputs=2)
        assert node.testrelaypoolaccept([parent_tx["hex"]])[0]["allowed"]

        # Child A
        child_a_tx = self.vault.create_self_transfer(utxo_to_spend=parent_tx["new_utxos"][0])
        assert not node.testrelaypoolaccept([child_a_tx["hex"]])[0]["allowed"]

        # Child B
        child_b_tx = self.vault.create_self_transfer(utxo_to_spend=parent_tx["new_utxos"][1])
        assert not node.testrelaypoolaccept([child_b_tx["hex"]])[0]["allowed"]

        self.log.info("Testrelaypoolaccept with entire package, should work with children in either order")
        testres_multiple_ab = node.testrelaypoolaccept(rawtxs=[parent_tx["hex"], child_a_tx["hex"], child_b_tx["hex"]])
        testres_multiple_ba = node.testrelaypoolaccept(rawtxs=[parent_tx["hex"], child_b_tx["hex"], child_a_tx["hex"]])
        assert all([testres["allowed"] for testres in testres_multiple_ab + testres_multiple_ba])

        testres_single = []
        # Test accept and then submit each one individually, which should be identical to package testaccept
        for rawtx in [parent_tx["hex"], child_a_tx["hex"], child_b_tx["hex"]]:
            testres = node.testrelaypoolaccept([rawtx])
            testres_single.append(testres[0])
            # Submit the transaction now so its child should have no problem validating
            node.sendrawtransaction(rawtx)
        assert_equal(testres_single, testres_multiple_ab)

    def test_multiple_parents(self):
        node = self.nodes[0]
        self.log.info("Testrelaypoolaccept a package in which a transaction has multiple parents within the package")

        for num_parents in [2, 10, 24]:
            # Test a package with num_parents parents and 1 child transaction.
            parent_coins = []
            package_hex = []

            for _ in range(num_parents):
                # Package accept should work with the parents in any order (as long as parents come before child)
                parent_tx = self.vault.create_self_transfer()
                parent_coins.append(parent_tx["new_utxo"])
                package_hex.append(parent_tx["hex"])

            child_tx = self.vault.create_self_transfer_multi(utxos_to_spend=parent_coins)
            for _ in range(10):
                random.shuffle(package_hex)
                testres_multiple = node.testrelaypoolaccept(rawtxs=package_hex + [child_tx['hex']])
                assert all([testres["allowed"] for testres in testres_multiple])

            testres_single = []
            # Test accept and then submit each one individually, which should be identical to package testaccept
            for rawtx in package_hex + [child_tx["hex"]]:
                testres_single.append(node.testrelaypoolaccept([rawtx])[0])
                # Submit the transaction now so its child should have no problem validating
                node.sendrawtransaction(rawtx)
            assert_equal(testres_single, testres_multiple)

    def test_conflicting(self):
        node = self.nodes[0]
        coin = self.vault.get_utxo()

        # tx1 and tx2 share the same inputs
        tx1 = self.vault.create_self_transfer(utxo_to_spend=coin)
        tx2 = self.vault.create_self_transfer(utxo_to_spend=coin, sequence=SEQUENCE_NONFINAL)

        # Ensure tx1 and tx2 are valid by themselves
        assert node.testrelaypoolaccept([tx1["hex"]])[0]["allowed"]
        assert node.testrelaypoolaccept([tx2["hex"]])[0]["allowed"]

        self.log.info("Test duplicate transactions in the same package")
        testres = node.testrelaypoolaccept([tx1["hex"], tx1["hex"]])
        assert_equal(testres, [
            {"txid": tx1["txid"], "wtxid": tx1["wtxid"], "package-error": "package-contains-duplicates"},
            {"txid": tx1["txid"], "wtxid": tx1["wtxid"], "package-error": "package-contains-duplicates"}
        ])

        self.log.info("Test conflicting transactions in the same package")
        testres = node.testrelaypoolaccept([tx1["hex"], tx2["hex"]])
        assert_equal(testres, [
            {"txid": tx1["txid"], "wtxid": tx1["wtxid"], "package-error": "conflict-in-package"},
            {"txid": tx2["txid"], "wtxid": tx2["wtxid"], "package-error": "conflict-in-package"}
        ])

        # Add a child that spends both, to submit via submitpackage
        tx_child = self.vault.create_self_transfer_multi(
            utxos_to_spend=[tx1["new_utxo"], tx2["new_utxo"]],
        )

        testres = node.testrelaypoolaccept([tx1["hex"], tx2["hex"], tx_child["hex"]])

        assert_equal(testres, [
            {"txid": tx1["txid"], "wtxid": tx1["wtxid"], "package-error": "conflict-in-package"},
            {"txid": tx2["txid"], "wtxid": tx2["wtxid"], "package-error": "conflict-in-package"},
            {"txid": tx_child["txid"], "wtxid": tx_child["wtxid"], "package-error": "conflict-in-package"}
        ])

        submitres = node.submitpackage([tx1["hex"], tx2["hex"], tx_child["hex"]])
        assert_equal(submitres, {'package_msg': 'conflict-in-package', 'tx-results': {}, 'replaced-transactions': []})

        # Submit tx1 to relaypool, then try the same package again
        node.sendrawtransaction(tx1["hex"])

        submitres = node.submitpackage([tx1["hex"], tx2["hex"], tx_child["hex"]])
        assert_equal(submitres, {'package_msg': 'conflict-in-package', 'tx-results': {}, 'replaced-transactions': []})
        assert tx_child["txid"] not in node.getrawrelaypool()

        # ... and without the in-relaypool ancestor tx1 included in the call
        submitres = node.submitpackage([tx2["hex"], tx_child["hex"]])
        assert_equal(submitres, {'package_msg': 'package-not-child-with-unconfirmed-parents', 'tx-results': {}, 'replaced-transactions': []})

        # Regardless of error type, the child can never enter the relaypool
        assert tx_child["txid"] not in node.getrawrelaypool()

    def assert_equal_package_results(self, node, testrelaypoolaccept_result, submitpackage_result):
        """Assert that a successful submitpackage result is consistent with testrelaypoolaccept
        results and getrelaypoolentry info.
        """
        for testres_tx in testrelaypoolaccept_result:
            # Grab this result from the submitpackage_result
            submitres_tx = submitpackage_result["tx-results"][testres_tx["wtxid"]]
            assert_equal(submitres_tx["txid"], testres_tx["txid"])
            # No "allowed" if the tx was already in the relaypool
            if "allowed" in testres_tx and testres_tx["allowed"]:
                assert_equal(submitres_tx["vsize"], testres_tx["vsize"])
                assert "fees" not in testres_tx
            assert "fees" not in submitres_tx
            entry_info = node.getrelaypoolentry(submitres_tx["txid"])
            assert_equal(submitres_tx["vsize"], entry_info["vsize"])
            assert "fees" not in entry_info

    def test_submit_child_with_parents(self, num_parents, partial_submit):
        node = self.nodes[0]
        peer = node.add_p2p_connection(P2PTxInvStore())

        package_txns = []
        presubmitted_wtxids = set()
        for _ in range(num_parents):
            parent_tx = self.vault.create_self_transfer()
            package_txns.append(parent_tx)
            if partial_submit and random.choice([True, False]):
                node.sendrawtransaction(parent_tx["hex"])
                presubmitted_wtxids.add(parent_tx["wtxid"])
        child_tx = self.vault.create_self_transfer_multi(utxos_to_spend=[tx["new_utxo"] for tx in package_txns])
        package_txns.append(child_tx)

        testrelaypoolaccept_result = node.testrelaypoolaccept(rawtxs=[tx["hex"] for tx in package_txns])
        submitpackage_result = node.submitpackage(package=[tx["hex"] for tx in package_txns])

        # Check that each result is present with the correct size and no fee field.
        assert_equal(submitpackage_result["package_msg"], "success")
        for package_txn in package_txns:
            tx = package_txn["tx"]
            assert tx.getwtxid() in submitpackage_result["tx-results"]
            wtxid = tx.getwtxid()
            assert wtxid in submitpackage_result["tx-results"]
            tx_result = submitpackage_result["tx-results"][wtxid]
            assert_equal(tx_result["txid"], tx.rehash())
            assert_equal(tx_result["vsize"], tx.get_vsize())
            assert "fees" not in tx_result

        # submitpackage result should be consistent with testrelaypoolaccept and getrelaypoolentry
        self.assert_equal_package_results(node, testrelaypoolaccept_result, submitpackage_result)

        # The node should announce each transaction. No guarantees for propagation.
        peer.wait_for_broadcast([tx["tx"].getwtxid() for tx in package_txns])
        self.generate(node, 1)

    def test_submitpackage(self):
        node = self.nodes[0]

        self.log.info("Submitpackage only allows valid hex inputs")
        valid_tx_list = self.vault.create_self_transfer_chain(chain_length=2)
        hex_list = [valid_tx_list[0]["hex"][:-1] + 'X', valid_tx_list[1]["hex"]]
        txid_list = [valid_tx_list[0]["txid"], valid_tx_list[1]["txid"]]
        assert_raises_rpc_error(-22, "TX decode failed:", node.submitpackage, hex_list)
        assert txid_list[0] not in node.getrawrelaypool()
        assert txid_list[1] not in node.getrawrelaypool()

        self.log.info("Submitpackage valid packages with 1 child and some number of parents (or none)")
        for num_parents in [0, 1, 2, 24]:
            self.test_submit_child_with_parents(num_parents, False)
            self.test_submit_child_with_parents(num_parents, True)

        self.log.info("Submitpackage only allows packages of 1 child with its parents")
        # Chain of 3 transactions has too many generations
        legacy_pool = node.getrawrelaypool()
        chain_hex = [t["hex"] for t in self.vault.create_self_transfer_chain(chain_length=3)]
        assert_raises_rpc_error(-25, "package topology disallowed", node.submitpackage, chain_hex)
        assert_equal(legacy_pool, node.getrawrelaypool())

        assert_raises_rpc_error(-8, f"Array must contain between 1 and {MAX_PACKAGE_COUNT} transactions.", node.submitpackage, [])
        assert_raises_rpc_error(
            -8, f"Array must contain between 1 and {MAX_PACKAGE_COUNT} transactions.",
            node.submitpackage, [chain_hex[0]] * (MAX_PACKAGE_COUNT + 1)
        )

        # Create a transaction chain such as only the parent gets accepted (by making the child's
        # version non-standard). Make sure the parent does get broadcast.
        self.log.info("If a package is partially submitted, transactions included in relaypool get broadcast")
        peer = node.add_p2p_connection(P2PTxInvStore())
        txs = self.vault.create_self_transfer_chain(chain_length=2)
        bad_child = tx_from_hex(txs[1]["hex"])
        bad_child.version = 0xffffffff
        hex_partial_acceptance = [txs[0]["hex"], bad_child.serialize().hex()]
        res = node.submitpackage(hex_partial_acceptance)
        assert_equal(res["package_msg"], "transaction failed")
        first_wtxid = txs[0]["tx"].getwtxid()
        assert "error" not in res["tx-results"][first_wtxid]
        sec_wtxid = bad_child.getwtxid()
        assert_equal(res["tx-results"][sec_wtxid]["error"], "version")
        peer.wait_for_broadcast([first_wtxid])
        self.generate(node, 1)

    def test_maxburn_submitpackage(self):
        node = self.nodes[0]

        assert_equal(node.getrawrelaypool(), [])

        self.log.info("Submitpackage maxburnamount arg testing")
        chained_txns_burn = self.vault.create_self_transfer_chain(
            chain_length=2,
            utxo_to_spend=self.vault.get_utxo(confirmed_only=True),
        )
        chained_burn_hex = [t["hex"] for t in chained_txns_burn]

        tx = tx_from_hex(chained_burn_hex[1])
        tx.vout[-1].scriptPubKey = b'a' * 10001 # scriptPubKey bigger than 10k IsUnspendable
        chained_burn_hex = [chained_burn_hex[0], tx.serialize().hex()]
        # burn test is run before any package evaluation; nothing makes it in and we get broader exception
        assert_raises_rpc_error(-25, "Unspendable output exceeds maximum configured by user", node.submitpackage, chained_burn_hex, chained_txns_burn[1]["new_utxo"]["value"] - Decimal("0.00000001"))
        assert_equal(node.getrawrelaypool(), [])

        # Relax the restrictions for both and send it; parent gets through as own subpackage
        pkg_result = node.submitpackage(chained_burn_hex, maxburnamount=chained_txns_burn[1]["new_utxo"]["value"])
        assert "error" not in pkg_result["tx-results"][chained_txns_burn[0]["wtxid"]]
        assert_equal(pkg_result["tx-results"][tx.getwtxid()]["error"], "scriptpubkey")
        assert_equal(node.getrawrelaypool(), [chained_txns_burn[0]["txid"]])

if __name__ == "__main__":
    RPCPackagesTest(__file__).main()
