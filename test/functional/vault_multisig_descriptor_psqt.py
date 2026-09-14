#!/usr/bin/env python3
# Copyright (c) 2021-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test a basic M-of-N multisig setup between multiple people using vaults and PSQTs, as well as a signing flow.

This is meant to be documentation as much as functional tests, so it is kept as simple and readable as possible.
"""

from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_approx,
    assert_equal,
)


class VaultMultisigDescriptorPSQTTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.vault_names = []
        self.extra_args = [["-keypool=100"]] * self.num_nodes

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()
        self.skip_if_no_sqlite()

    @staticmethod
    def _get_qpub(vault, internal):
        """Extract the vault's qpubs using `listdescriptors` and pick the one from the `pkh` descriptor since it's least likely to be accidentally reused (Base58 addresses)."""
        pkh_descriptor = next(filter(lambda d: d["desc"].startswith("pkh(") and d["internal"] == internal, vault.listdescriptors()["descriptors"]))
        # Keep all key origin information (master key fingerprint and all derivation steps) for proper support of hardware devices
        # See section 'Key origin identification' in 'doc/descriptors.md' for more details...
        return pkh_descriptor["desc"].split("pkh(")[1].split(")")[0]

    @staticmethod
    def _check_psqt(psqt, to, value, multisig):
        """Helper function for any of the N participants to check the psqt with decodepsqt and verify it is OK before signing."""
        tx = multisig.decodepsqt(psqt)["tx"]
        amount = 0
        for vout in tx["vout"]:
            address = vout["output_script"]["address"]
            assert_equal(multisig.getaddressinfo(address)["ischange"], address != to)
            if address == to:
                amount += vout["value"]
        assert_approx(amount, float(value), vspan=0.001)

    def participants_create_multisigs(self, external_qpubs, internal_qpubs):
        """The multisig is created by importing the following descriptors. The resulting vault only tracks the scripts, and every participant can do this."""
        for i, node in enumerate(self.nodes):
            node.createvault(vault_name=f"{self.name}_{i}", blank=True, disable_private_keys=True)
            multisig = node.get_vault_rpc(f"{self.name}_{i}")
            external = multisig.getdescriptorinfo(f"wsh(sortedmulti({self.M},{','.join(external_qpubs)}))")
            internal = multisig.getdescriptorinfo(f"wsh(sortedmulti({self.M},{','.join(internal_qpubs)}))")
            result = multisig.importdescriptors([
                {  # receiving addresses (internal: False)
                    "desc": external["descriptor"],
                    "active": True,
                    "internal": False,
                    "timestamp": "now",
                },
                {  # change addresses (internal: True)
                    "desc": internal["descriptor"],
                    "active": True,
                    "internal": True,
                    "timestamp": "now",
                },
            ])
            assert all(r["success"] for r in result)
            yield multisig

    def run_test(self):
        self.M = 2
        self.N = self.num_nodes
        self.name = f"{self.M}_of_{self.N}_multisig"
        self.log.info(f"Testing {self.name}...")

        participants = {
            # Every participant generates a qpub. The most straightforward way is to create a new vault.
            # This vault will be the participant's `signer` for the resulting multisig. Avoid reusing this vault for any other purpose (for privacy reasons).
            "signers": [node.get_vault_rpc(node.createvault(vault_name=f"participant_{self.nodes.index(node)}")["name"]) for node in self.nodes],
            # After participants generate and exchange their qpubs they will each create their own tracking-only multisig.
            # Note: these multisigs are all the same, this just highlights that each participant can independently verify everything on their own node.
            "multisigs": []
        }

        self.log.info("Generate and exchange qpubs...")
        external_qpubs, internal_qpubs = [[self._get_qpub(signer, internal) for signer in participants["signers"]] for internal in [False, True]]

        self.log.info("Every participant imports the following descriptors to create the tracking-only multisig...")
        participants["multisigs"] = list(self.participants_create_multisigs(external_qpubs, internal_qpubs))

        self.log.info("Check that every participant's multisig generates the same addresses...")
        for _ in range(10):  # we check that the first 10 generated addresses are the same for all participant's multisigs
            receive_addresses = [multisig.getnewaddress() for multisig in participants["multisigs"]]
            all(address == receive_addresses[0] for address in receive_addresses)
            change_addresses = [multisig.getrawchangeaddress() for multisig in participants["multisigs"]]
            all(address == change_addresses[0] for address in change_addresses)

        self.log.info("Get a mature utxo to send to the multisig...")
        coordinator_vault = participants["signers"][0]
        self.generatetoaddress(self.nodes[0], 101, coordinator_vault.getnewaddress())

        deposit_amount = 6.15
        multisig_receiving_address = participants["multisigs"][0].getnewaddress()
        self.log.info("Send funds to the resulting multisig receiving address...")
        coordinator_vault.sendtoaddress(multisig_receiving_address, deposit_amount)
        self.generate(self.nodes[0], 1)
        for participant in participants["multisigs"]:
            assert_approx(participant.getbalance(), deposit_amount, vspan=0.001)

        self.log.info("Send a transaction from the multisig!")
        to = participants["signers"][self.N - 1].getnewaddress()
        value = 1
        self.log.info("First, make a sending transaction, created using `vaultcreatefundedpsqt` (anyone can initiate this)...")
        psqt = participants["multisigs"][0].vaultcreatefundedpsqt(inputs=[], outputs=[{to: value}])

        psqts = []
        self.log.info("Now at least M users check the psqt with decodepsqt and (if OK) signs it with vaultprocesspsqt...")
        for m in range(self.M):
            signers_multisig = participants["multisigs"][m]
            self._check_psqt(psqt["psqt"], to, value, signers_multisig)
            signing_vault = participants["signers"][m]
            partially_signed_psqt = signing_vault.vaultprocesspsqt(psqt["psqt"])
            psqts.append(partially_signed_psqt["psqt"])

        self.log.info("Finally, collect the signed PSQTs with combinepsqt, finalizepsqt, then broadcast the resulting transaction...")
        combined = coordinator_vault.combinepsqt(psqts)
        finalized = coordinator_vault.finalizepsqt(combined)
        coordinator_vault.sendrawtransaction(finalized["hex"])

        self.log.info("Check that balances are correct after the transaction has been included in a block.")
        self.generate(self.nodes[0], 1)
        assert_approx(participants["multisigs"][0].getbalance(), deposit_amount - value, vspan=0.001)
        assert_equal(participants["signers"][self.N - 1].getbalance(), value)

        self.log.info("Send another transaction from the multisig, this time with a daisy chained signing flow (one after another in series)!")
        psqt = participants["multisigs"][0].vaultcreatefundedpsqt(inputs=[], outputs=[{to: value}])
        for m in range(self.M):
            signers_multisig = participants["multisigs"][m]
            self._check_psqt(psqt["psqt"], to, value, signers_multisig)
            signing_vault = participants["signers"][m]
            psqt = signing_vault.vaultprocesspsqt(psqt["psqt"])
            assert_equal(psqt["complete"], m == self.M - 1)
        coordinator_vault.sendrawtransaction(psqt["hex"])

        self.log.info("Check that balances are correct after the transaction has been included in a block.")
        self.generate(self.nodes[0], 1)
        assert_approx(participants["multisigs"][0].getbalance(), deposit_amount - (value * 2), vspan=0.001)
        assert_equal(participants["signers"][self.N - 1].getbalance(), value * 2)


if __name__ == "__main__":
    VaultMultisigDescriptorPSQTTest(__file__).main()
