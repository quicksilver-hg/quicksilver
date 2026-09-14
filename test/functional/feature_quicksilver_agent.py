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
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal, assert_raises_process_error
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

    def skip_test_if_missing_module(self):
        self.skip_if_no_agent()

    def agent_process(self, datadir: str, args: Sequence[str], *, check: bool = True) -> subprocess.CompletedProcess[str]:
        cmd = [self.options.quicksilveragent, f"-datadir={datadir}", f"-chain={self.chain}", "-prove=0", *args]
        completed = subprocess.run(cmd, capture_output=True, text=True)
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
            "version": 1,
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
            "policy_status": "pending_integration",
            "backend_created": False,
        }
        return json.dumps({
            "type": "quicksilver.agent_allotment_key_bundle",
            "version": 1,
            "policy_request": policy_request,
            "funding_address": funding_address,
            "funding_secret_wif": wif,
            "funding_outputs": [{
                "txid": ORIGINAL_TXID,
                "vout": 0,
                "amount_cinnabar": str(amount),
            }],
            "policy_enforcement": "pending_integration",
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
        wif, funding_pubkey = generate_keypair(wif=True)
        _, dest_pubkey = generate_keypair()
        funding_address = key_to_p2wpkh(funding_pubkey)
        destination = key_to_p2wpkh(dest_pubkey)
        genesis = node.getblockhash(0)

        self._test_sequential_spend_uses_change(funding_address, destination, wif, genesis)
        self._test_spent_today_is_summed_from_ledger(funding_address, destination, wif, genesis)

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


if __name__ == '__main__':
    QuicksilverAgentTest(__file__).main()
