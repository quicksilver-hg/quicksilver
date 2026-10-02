#!/usr/bin/env python3
# Copyright (c) 2014-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test blocks whose coinbase subsidy is provably unspendable."""

from decimal import Decimal

from test_framework.blocktools import (
    COINBASE_MATURITY,
    quicksilver_sandbox_subsidy,
)
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal


class UnspendableCoinbaseTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.supports_cli = False
        self.extra_args = [["-txpownocycle=1"], ["-txpownocycle=1"]]

    @staticmethod
    def comparable_stats(stats):
        return {
            key: stats[key]
            for key in (
                "height",
                "bestblock",
                "txouts",
                "transactions",
                "total_amount",
                "muhash",
            )
        }

    @staticmethod
    def assert_utxo_totals(stats, *, height, txouts, total_amount):
        assert_equal(stats["height"], height)
        assert_equal(stats["txouts"], txouts)
        assert_equal(stats["transactions"], txouts)
        assert_equal(stats["total_amount"], total_amount)

    @staticmethod
    def checked_raw_descriptor(node, script_hex):
        return node.getdescriptorinfo(f"raw({script_hex})")["descriptor"]

    def assert_burned_coinbase(self, block_hash, script_hex, expected_value):
        for node in self.nodes:
            assert_equal(node.getbestblockhash(), block_hash)
            coinbase = node.getblock(block_hash, 2)["tx"][0]
            matching_outputs = [
                output
                for output in coinbase["vout"]
                if output["output_script"]["hex"] == script_hex
            ]
            assert_equal(len(matching_outputs), 1)
            assert_equal(matching_outputs[0]["value"], expected_value)
            assert_equal(
                sum((output["value"] for output in coinbase["vout"]), Decimal("0")),
                expected_value,
            )

    def run_test(self):
        node0, node1 = self.nodes
        zero = Decimal("0")

        self.log.info("Mine a bare OP_RETURN coinbase and relay it to the peer")
        bare_script = "6a"
        bare_descriptor = self.checked_raw_descriptor(node0, bare_script)
        initial_stats = [node.gettxoutsetinfo("muhash") for node in self.nodes]
        for stats in initial_stats:
            self.assert_utxo_totals(stats, height=0, txouts=0, total_amount=zero)

        bare_hash = self.generatetodescriptor(node0, 1, bare_descriptor)[0]
        self.assert_burned_coinbase(
            bare_hash, bare_script, quicksilver_sandbox_subsidy(1)
        )
        bare_stats = [node.gettxoutsetinfo("muhash") for node in self.nodes]
        for stats in bare_stats:
            self.assert_utxo_totals(stats, height=1, txouts=0, total_amount=zero)
        assert_equal(
            self.comparable_stats(bare_stats[1]), self.comparable_stats(bare_stats[0])
        )
        self.log.info(
            "Bare OP_RETURN block %s: script=%s value=%s txouts=%d total_amount=%s",
            bare_hash,
            bare_script,
            quicksilver_sandbox_subsidy(1),
            bare_stats[0]["txouts"],
            bare_stats[0]["total_amount"],
        )

        self.log.info(
            "Mine an 80-byte OP_RETURN push whose 84-byte script exceeds relay policy"
        )
        # OP_RETURN OP_PUSHDATA2 0x0050 <80 zero bytes>. The non-minimal push is
        # deliberate: the payload remains 80 bytes while the complete script is
        # 84 bytes, one byte above MAX_OP_RETURN_RELAY.
        data_script = "6a4d5000" + "00" * 80
        assert_equal(len(bytes.fromhex(data_script)), 84)
        data_descriptor = self.checked_raw_descriptor(node0, data_script)
        data_hash = self.generatetodescriptor(node0, 1, data_descriptor)[0]
        self.assert_burned_coinbase(
            data_hash, data_script, quicksilver_sandbox_subsidy(2)
        )
        data_stats = [node.gettxoutsetinfo("muhash") for node in self.nodes]
        for stats in data_stats:
            self.assert_utxo_totals(stats, height=2, txouts=0, total_amount=zero)
        assert_equal(
            self.comparable_stats(data_stats[1]), self.comparable_stats(data_stats[0])
        )
        self.log.info(
            "80-byte push block %s: script=%s value=%s script_bytes=%d txouts=%d total_amount=%s",
            data_hash,
            data_script,
            quicksilver_sandbox_subsidy(2),
            len(bytes.fromhex(data_script)),
            data_stats[0]["txouts"],
            data_stats[0]["total_amount"],
        )

        self.log.info("Mine past coinbase maturity using spendable coinbases")
        spendable_address = node0.get_deterministic_priv_key().address
        first_spendable_height = node0.getblockcount() + 1
        spendable_count = COINBASE_MATURITY + 1
        self.generatetoaddress(node0, spendable_count, spendable_address)
        final_height = node0.getblockcount()
        expected_amount = sum(
            (
                quicksilver_sandbox_subsidy(height)
                for height in range(first_spendable_height, final_height + 1)
            ),
            zero,
        )
        for node in self.nodes:
            self.assert_utxo_totals(
                node.gettxoutsetinfo("muhash"),
                height=final_height,
                txouts=spendable_count,
                total_amount=expected_amount,
            )
            scan = node.scantxoutset("start", [bare_descriptor])
            assert_equal(scan["success"], True)
            assert_equal(scan["unspents"], [])
            assert_equal(scan["total_amount"], zero)
        self.log.info(
            "Maturity height %d: spendable coinbases=%d txouts=%d total_amount=%s burn_matches=%d",
            final_height,
            spendable_count,
            node0.gettxoutsetinfo("muhash")["txouts"],
            expected_amount,
            len(node0.scantxoutset("start", [bare_descriptor])["unspents"]),
        )

        self.log.info("Disconnect and reconnect the first burned-coinbase block")
        stats_before_disconnect = [
            node.gettxoutsetinfo("muhash") for node in self.nodes
        ]
        node0.invalidateblock(bare_hash)
        node1.invalidateblock(bare_hash)
        assert_equal(node0.getblockcount(), 0)
        assert_equal(node1.getblockcount(), 0)
        node0.reconsiderblock(bare_hash)
        node1.reconsiderblock(bare_hash)
        self.sync_blocks()
        stats_after_reconnect = [node.gettxoutsetinfo("muhash") for node in self.nodes]
        for before, after in zip(stats_before_disconnect, stats_after_reconnect):
            assert_equal(self.comparable_stats(after), self.comparable_stats(before))
        self.log.info(
            "Disconnect/reconsider restored height=%d txouts=%d total_amount=%s muhash=%s",
            stats_after_reconnect[0]["height"],
            stats_after_reconnect[0]["txouts"],
            stats_after_reconnect[0]["total_amount"],
            stats_after_reconnect[0]["muhash"],
        )

        self.log.info("Reindex the peer chainstate and compare its UTXO commitment")
        self.restart_node(1, extra_args=["-reindex-chainstate", "-txpownocycle=1"])
        node0_stats = node0.gettxoutsetinfo("muhash")
        node1_stats = node1.gettxoutsetinfo("muhash")
        assert_equal(
            self.comparable_stats(node1_stats), self.comparable_stats(node0_stats)
        )
        self.log.info("Reindexed peer UTXO muhash=%s", node1_stats["muhash"])


if __name__ == "__main__":
    UnspendableCoinbaseTest(__file__).main()
