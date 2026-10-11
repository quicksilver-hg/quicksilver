#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""End-to-end coverage for quicksilver-agent.

F-150: the agent binary had no functional test at all. This case starts the
agent, then does two sequential signbundle calls without -spenttoday — the
shape that F-145 (spent-output reuse / unsummed daily limit) actually takes.
"""

from __future__ import annotations

import json
import os
import subprocess
from typing import Sequence

from test_framework.address import key_to_p2wpkh
from test_framework.descriptors import descsum_create
from test_framework.messages import CTxInWitness
from test_framework.psqt import PSQT, PSQT_IN_TAP_SCRIPT_SIG, PSQT_IN_TAP_LEAF_SCRIPT
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal, assert_raises_process_error, assert_raises_rpc_error, p2p_port
from test_framework.vault_util import generate_keypair


COIN = 100_000_000
ORIGINAL_TXID = "11" * 32
SPEND_AMOUNT = COIN // 4


def parse_agent_kv(text: str) -> dict[str, str]:
    parsed: dict[str, str] = {}
    for line in text.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        parsed[key] = value
    return parsed


class QuicksilverAgentTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-txpownocycle=1", "-txindex=1"]]

    def add_options(self, parser):
        self.add_vault_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_agent()
        self.skip_if_no_vault()

    def agent_process(self, datadir: str, args: Sequence[str], *, check: bool = True, input_text: str | None = None) -> subprocess.CompletedProcess[str]:
        cmd = [self.options.quicksilveragent, f"-datadir={datadir}", f"-chain={self.chain}", "-prove=0", *args]
        completed = subprocess.run(cmd, capture_output=True, text=True, input=input_text)
        if check and completed.returncode != 0:
            raise subprocess.CalledProcessError(
                completed.returncode, cmd, output=completed.stdout + completed.stderr
            )
        return completed

    def agent_output(self, datadir: str, args: Sequence[str]) -> dict[str, str]:
        completed = self.agent_process(datadir, args)
        assert_equal(completed.stderr, "")
        return parse_agent_kv(completed.stdout)

    def make_bundle(self, *, funding_address: str, wif: str, genesis: str, daily_limit: int, amount: int) -> str:
        policy_request = {
            "type": "quicksilver.agent_allotment_policy_request",
            "version": 2,
            "chain": self.chain,
            "genesis_hash": genesis,
            "id": "agent-1",
            "label": "functional-agent",
            "funding_address": funding_address,
            "funding_limit_cinnabar": str(2 * COIN),
            "funding_available_cinnabar": str(amount),
            "daily_limit_cinnabar": str(daily_limit),
            "risk_accepted_time": "123",
            "request_created_time": "456",
            "funding_descriptor": self.funding_descriptor,
        }
        return json.dumps({
            "type": "quicksilver.agent_allotment_cosign_bundle",
            "version": 1,
            "policy_request": policy_request,
            "funding_address": funding_address,
            "agent_secret_wif": wif,
            "funding_descriptor": self.funding_descriptor,
            "funding_outputs": [{
                "txid": ORIGINAL_TXID,
                "vout": 0,
                "amount_cinnabar": str(amount),
            }],
        })

    def make_receipt(self, *, funding_address: str, genesis: str, amount: int) -> str:
        return json.dumps({
            "type": "quicksilver.agent_payment_receipt",
            "version": 1,
            "chain": self.chain,
            "genesis_hash": genesis,
            "funding_address": funding_address,
            "txid": ORIGINAL_TXID,
            "vout": 0,
            "amount_cinnabar": str(amount),
            "received_time": "789",
        })

    def prepare_agent_datadir(self, name: str, receipt: str) -> str:
        datadir = os.path.join(self.options.tmpdir, name)
        os.makedirs(datadir, exist_ok=True)
        status = self.agent_output(datadir, ["status"])
        assert_equal(status["network"], self.chain)
        assert_equal(status["header_height"], "0")
        self.agent_output(datadir, ["initheaders"])
        imported = self.agent_output(datadir, [f"-paymentreceipt={receipt}", "importreceipt"])
        assert_equal(imported["imported_receipts"], "1")
        return datadir

    def signbundle_args(self, bundle: str, destination: str, spend_amount: int) -> list[str]:
        return [
            f"-policybundle={bundle}",
            f"-destination={destination}",
            f"-spendamount={spend_amount}",
            "signbundle",
        ]

    def run_test(self):
        node = self.nodes[0]
        self.funder = node.get_vault_rpc(self.default_vault_name)
        wif, funding_pubkey = generate_keypair(wif=True)
        _, dest_pubkey = generate_keypair()
        self.vault_wif, self.vault_pubkey = generate_keypair(wif=True)
        self.cosigner_wif, self.cosigner_pubkey = generate_keypair(wif=True)
        self.agent_pubkey = funding_pubkey[1:].hex()
        self.funding_descriptor = descsum_create(
            f"tr({self.vault_pubkey[1:].hex()},multi_a(2,{self.agent_pubkey},{self.cosigner_pubkey[1:].hex()}))")
        funding_address = node.deriveaddresses(self.funding_descriptor)[0]
        destination = key_to_p2wpkh(dest_pubkey)
        genesis = node.getblockhash(0)

        self._test_sequential_spend_uses_change(funding_address, destination, wif, genesis)
        self._test_spent_today_is_summed_from_ledger(funding_address, destination, wif, genesis)
        self.generatetoaddress(node, 101, node.getnewaddress())
        self._test_agent_alone_cannot_spend(funding_address, destination, wif, genesis)
        self._test_vault_cosigns_and_broadcasts(funding_address, destination, wif, genesis)
        self._test_vault_reclaims_by_key_path(funding_address, destination)
        self._test_importrecovery_finds_taproot_allotment(funding_address)

    def _test_sequential_spend_uses_change(self, funding_address: str, destination: str, wif: str, genesis: str) -> None:
        bundle = self.make_bundle(
            funding_address=funding_address,
            wif=wif,
            genesis=genesis,
            daily_limit=COIN,
            amount=COIN,
        )
        receipt = self.make_receipt(funding_address=funding_address, genesis=genesis, amount=COIN)
        datadir = self.prepare_agent_datadir("agent_reuse", receipt)

        first = self.agent_output(datadir, self.signbundle_args(bundle, destination, SPEND_AMOUNT))
        assert_equal(first["selected_input_count"], "1")
        assert_equal(first["selected_input_0"], f"{ORIGINAL_TXID}:0")
        assert_equal(first["receipt_store_saved"], "true")
        assert "psqt" in first
        assert "hex" not in first
        assert_equal(first["next_step"], "paste the psqt into the desktop's Agent spend request panel")
        change_txid = first["txid"]

        second = self.agent_output(datadir, self.signbundle_args(bundle, destination, SPEND_AMOUNT))
        assert_equal(second["selected_input_count"], "1")
        # Without the spent-output filter this selects ORIGINAL_TXID again.
        assert_equal(second["selected_input_0"], f"{change_txid}:1")

        listed = self.agent_output(datadir, ["listreceiptactivity"])
        assert_equal(listed["activity_1_type"], "spent")
        assert_equal(listed["activity_1_output"], f"{ORIGINAL_TXID}:0")

    def _test_spent_today_is_summed_from_ledger(self, funding_address: str, destination: str, wif: str, genesis: str) -> None:
        bundle = self.make_bundle(
            funding_address=funding_address,
            wif=wif,
            genesis=genesis,
            daily_limit=SPEND_AMOUNT,
            amount=COIN,
        )
        receipt = self.make_receipt(funding_address=funding_address, genesis=genesis, amount=COIN)
        datadir = self.prepare_agent_datadir("agent_daily", receipt)

        first = self.agent_output(datadir, self.signbundle_args(bundle, destination, SPEND_AMOUNT))
        assert_equal(first["selected_input_0"], f"{ORIGINAL_TXID}:0")

        checked_run = self.agent_process(
            datadir,
            [f"-policybundle={bundle}", f"-spendamount={SPEND_AMOUNT}", "checkbundle"],
            check=False,
        )
        assert_equal(checked_run.returncode, 1)
        checked = parse_agent_kv(checked_run.stdout)
        assert_equal(checked["spent_today_cinnabar"], str(SPEND_AMOUNT))
        assert_equal(checked["allowed"], "false")
        assert_equal(checked["policy_result"], "daily-limit-exceeded")

        assert_raises_process_error(
            1,
            "daily-limit-exceeded",
            self.agent_process,
            datadir,
            self.signbundle_args(bundle, destination, SPEND_AMOUNT),
        )

    def funded_request(self, name, funding_address, destination, wif, genesis):
        node = self.nodes[0]
        txid = self.funder.sendtoaddress(funding_address, 1)
        self.generate(node, 1)
        outputs = node.getrawtransaction(txid, 1)["vout"]
        vout = next(output["n"] for output in outputs if output["output_script"].get("address") == funding_address)
        bundle = json.loads(self.make_bundle(funding_address=funding_address, wif=wif, genesis=genesis, daily_limit=COIN, amount=COIN))
        bundle["funding_outputs"][0].update(txid=txid, vout=vout)
        datadir = os.path.join(self.options.tmpdir, name)
        os.makedirs(datadir)
        self.agent_output(datadir, ["initheaders"])
        synced = self.agent_output(datadir, [f"-peer=127.0.0.1:{p2p_port(node.index)}", "syncheaderspeer"])
        assert_equal(int(synced["header_peer_0_header_height"]), node.getblockcount())
        # Use a current validated header; sandbox anchors expire after 20 blocks.
        result = self.agent_output(datadir, ["-prove=1", *self.signbundle_args(json.dumps(bundle), destination, SPEND_AMOUNT)])
        assert_equal(result["proved"], "true")
        assert "hex" not in result
        return result

    def _test_agent_alone_cannot_spend(self, funding_address, destination, wif, genesis):
        node = self.nodes[0]
        result = self.funded_request("agent_alone", funding_address, destination, wif, genesis)
        assert_equal(node.finalizepsqt(result["psqt"])["complete"], False)
        partial = PSQT.from_base64(result["psqt"])
        signatures = [(key, value) for key, value in partial.i[0].map.items() if isinstance(key, bytes) and key[0] == PSQT_IN_TAP_SCRIPT_SIG]
        leaves = [(key, value) for key, value in partial.i[0].map.items() if isinstance(key, bytes) and key[0] == PSQT_IN_TAP_LEAF_SCRIPT]
        assert_equal(len(signatures), 1)
        assert_equal(signatures[0][0][1:33].hex(), self.agent_pubkey)
        assert_equal(len(signatures[0][1]), 64)
        assert_equal(len(leaves), 1)
        forced = partial.tx
        forced.wit.vtxinwit = [CTxInWitness()]
        forced.wit.vtxinwit[0].scriptWitness.stack = [b"", signatures[0][1], leaves[0][1][:-1], leaves[0][0][1:]]
        assert_raises_rpc_error(-26, "mandatory-script-verify-flag-failed", node.sendrawtransaction, forced.serialize().hex())

    def _test_vault_cosigns_and_broadcasts(self, funding_address, destination, wif, genesis):
        node = self.nodes[0]
        node.createvault("allotment_cosigner", blank=True)
        cosigner = node.get_vault_rpc("allotment_cosigner")
        # V is public here so the RPC follows the same A+C path as the desktop.
        descriptor = descsum_create(f"tr({self.vault_pubkey[1:].hex()},multi_a(2,{self.agent_pubkey},{self.cosigner_wif}))")
        assert_equal(cosigner.importdescriptors([{"desc": descriptor, "timestamp": 0}])[0]["success"], True)
        result = self.funded_request("agent_cosign", funding_address, destination, wif, genesis)
        signed = cosigner.vaultprocesspsqt(result["psqt"], finalize=False)
        half = PSQT.from_base64(result["psqt"])
        cosigned = PSQT.from_base64(signed["psqt"])
        signatures = {key[1:33].hex(): value for key, value in cosigned.i[0].map.items() if isinstance(key, bytes) and key[0] == PSQT_IN_TAP_SCRIPT_SIG}
        assert_equal(set(signatures), {self.agent_pubkey, self.cosigner_pubkey[1:].hex()})
        agent_sig = next(value for key, value in half.i[0].map.items() if isinstance(key, bytes) and key[0] == PSQT_IN_TAP_SCRIPT_SIG)
        assert_equal(signatures[self.agent_pubkey], agent_sig)
        final = node.finalizepsqt(signed["psqt"])
        assert_equal(final["complete"], True)
        decoded = node.decoderawtransaction(final["hex"])
        assert_equal(decoded["txid"], result["txid"])
        witness = decoded["vin"][0]["txinwitness"]
        assert_equal(len(witness), 4)
        assert_equal(witness[0], signatures[self.cosigner_pubkey[1:].hex()].hex())
        assert_equal(witness[1], agent_sig.hex())
        txid = node.sendrawtransaction(final["hex"])
        self.generate(node, 1)
        assert_equal(node.getrawtransaction(txid, 1)["confirmations"], 1)
        self.reclaim_descriptor = descsum_create(f"tr({self.vault_wif},multi_a(2,{self.agent_pubkey},{self.cosigner_wif}))")

    def _test_vault_reclaims_by_key_path(self, funding_address, destination):
        node = self.nodes[0]
        node.createvault("allotment_reclaim", blank=True)
        reclaim = node.get_vault_rpc("allotment_reclaim")
        assert_equal(reclaim.importdescriptors([{"desc": self.reclaim_descriptor, "timestamp": 0}])[0]["success"], True)
        txid = self.funder.sendtoaddress(funding_address, 1)
        self.generate(node, 1)
        utxo = next(u for u in reclaim.listunspent() if u["txid"] == txid)
        result = reclaim.send(outputs=[{destination: 1}], inputs=[{"txid": txid, "vout": utxo["vout"]}], add_inputs=False)
        decoded = node.getrawtransaction(result["txid"], 1)
        assert_equal(len(decoded["vin"][0]["txinwitness"]), 1)
        self.generate(node, 1)
        assert_equal(node.getrawtransaction(result["txid"], 1)["confirmations"], 1)

    def _test_importrecovery_finds_taproot_allotment(self, funding_address):
        node = self.nodes[0]
        txid = self.funder.sendtoaddress(funding_address, 1)
        self.generate(node, 1)
        scan = node.scantxoutset("start", [f"addr({funding_address})"])
        # Include just the newly funded output, rather than earlier change outputs.
        scan["unspents"] = [u for u in scan["unspents"] if u["txid"] == txid]
        assert_equal(len(scan["unspents"]), 1)
        datadir = os.path.join(self.options.tmpdir, "agent_recovery")
        os.makedirs(datadir)
        result = self.agent_process(datadir, [f"-fundingaddress={funding_address}", "-scantxoutset=-", "importrecovery"], input_text=json.dumps(scan, default=str))
        assert_equal(parse_agent_kv(result.stdout)["imported_receipts"], "1")
        listed = self.agent_output(datadir, ["listreceipts"])
        assert_equal(listed["receipt_0_output"], f"{txid}:{scan['unspents'][0]['vout']}")


if __name__ == '__main__':
    QuicksilverAgentTest(__file__).main()
