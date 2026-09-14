#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the opt-in background mining control plane (SP1a)."""

import time
from decimal import Decimal

from test_framework.test_framework import QuicksilverTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import assert_equal, assert_raises_rpc_error


class MiningServiceTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.supports_cli = False

    def run_test(self):
        node = self.nodes[0]
        addr = node.get_deterministic_priv_key().address

        self.log.info("Inactive status before starting")
        st = node.getminingstatus()
        for key in ("active", "address", "blocks_found", "coins_minted_session",
                    "graphs_attempted", "solver_ok",
                    "last_solver_error", "last_block_time", "elapsed_seconds",
                    "congestion_multiplier", "template_height",
                    "template_transactions"):
            assert key in st, f"getminingstatus missing {key}"
        # attempts_per_second is the one key that may be absent, and absence is the
        # point of it: no solver attempt has been observed, so the rate is unknown.
        # Reporting 0 here would read exactly like an armed miner whose card died.
        assert "attempts_per_second" not in st
        assert_equal(st["active"], False)
        assert_equal(st["template_height"], 0)
        assert_equal(st["template_transactions"], 0)
        assert_equal(st["blocks_found"], 0)
        assert_equal(st["coins_minted_session"], Decimal(0))
        assert_equal(st["graphs_attempted"], 0)

        self.log.info("No payout address -> error")
        assert_raises_rpc_error(-8, "No payout address", node.startmining)

        # Regression guard for the bootstrap launch blocker: a fresh height-0 chain
        # whose genesis is timestamped in the past reports initialblockdownload:true,
        # yet the service must still mine — it gates on chain-frontier, not tip-age.
        assert_equal(node.getblockchaininfo()["initialblockdownload"], True)
        assert_equal(node.getblockcount(), 0)
        height_before = node.getblockcount()  # == 0; service mines straight from genesis

        self.log.info("Start mining and watch blocks appear")
        res = node.startmining(addr)
        assert_equal(res["active"], True)
        assert_equal(res["address"], addr)

        # The worker publishes the block it is about to grind before its first
        # sweep, so an armed miner always names a height.
        self.wait_until(lambda: node.getminingstatus()["template_height"] > 0, timeout=30)

        self.wait_until(lambda: node.getminingstatus()["blocks_found"] >= 3, timeout=30)
        node.stopmining()
        st = node.getminingstatus()
        assert_equal(st["active"], False)
        assert st["blocks_found"] >= 3

        self.log.info("The block being ground is reported apart from anyone else's template")
        # getmininginfo's currentblocktx is a process-wide static stamped by
        # whichever caller assembled last, so an RPC getblocktemplate moves it
        # while the miner's own template is untouched -- which is how a node
        # came to report 0 transactions for a block whose template held one.
        # getminingstatus answers for the miner alone, and a stopped miner is
        # grinding nothing at all.
        assert_equal(st["template_height"], 0)
        assert_equal(st["template_transactions"], 0)
        node.getblocktemplate({"rules": ["segwit"]})
        assert "currentblocktx" in node.getmininginfo()
        st = node.getminingstatus()
        assert_equal(st["template_height"], 0)
        assert_equal(st["template_transactions"], 0)

        self.log.info("Sandbox solves no graphs, and that is not a solver fault")
        # fBlockPowNoCycle short-circuits SolveBlockPoW before the solver is
        # reached, so no attempt is ever observed. The rate is therefore absent
        # rather than 0.0: on this network there is nothing to measure, which is
        # not the same claim as "the solver is doing nothing".
        st = node.getminingstatus()
        assert_equal(st["solver_ok"], True)
        assert_equal(st["last_solver_error"], "")
        assert_equal(st["graphs_attempted"], 0)
        assert "attempts_per_second" not in st

        self.log.info("coins_minted_session matches the coinbase value of service-mined blocks")
        # Only blocks mined by the service this run count; the bootstrap block does not.
        assert_equal(node.getblockcount(), height_before + st["blocks_found"])
        minted = Decimal(0)
        for h in range(height_before + 1, node.getblockcount() + 1):
            block = node.getblock(node.getblockhash(h), 2)
            # empty blocks: coinbase is the only tx; sum its outputs
            minted += sum(Decimal(str(o["value"])) for o in block["tx"][0]["vout"])
        assert_equal(Decimal(str(st["coins_minted_session"])), minted)

        self.log.info("Stop halts progress")
        stopped_at = node.getminingstatus()["blocks_found"]
        time.sleep(1)
        assert_equal(node.getminingstatus()["blocks_found"], stopped_at)

        self.log.info("Session counters reset on restart")
        self.restart_node(0)
        st = node.getminingstatus()
        assert_equal(st["active"], False)
        assert_equal(st["blocks_found"], 0)
        assert_equal(st["coins_minted_session"], Decimal(0))

        self.log.info("-mine requires -mineaddress (fail fast)")
        self.stop_node(0)
        node.assert_start_raises_init_error(["-mine"], "requires -mineaddress", match=ErrorMatch.PARTIAL_REGEX)

        self.log.info("-mine with -mineaddress auto-starts")
        # Chain already has blocks (recent tips), so the node is past IBD and the
        # auto-started role mines immediately.
        self.start_node(0, ["-mine", f"-mineaddress={addr}"])
        self.wait_until(lambda: node.getminingstatus()["blocks_found"] >= 1, timeout=30)
        assert_equal(node.getminingstatus()["active"], True)
        node.stopmining()


if __name__ == '__main__':
    MiningServiceTest(__file__).main()
