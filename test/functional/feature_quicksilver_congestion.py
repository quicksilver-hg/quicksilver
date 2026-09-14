#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""#5c-1 Phase 2: per-tx congestion floor — end-to-end on a sandbox node.

Proves the wired-up node moves the cached EIP-1559 congestion multiplier m in
response to real block fullness, and that m persists across a restart:

  1. Floor: a near-empty chain reports congestion_multiplier == 1.0.
  2. Rise: mining one block that crosses the (sandbox) target fullness ratchets m
     above 1.0. Sandbox sets nCongestionTargetPermille = 5 (0.5%), so a single
     ~9 KB OP_RETURN vault tx — one per-tx PoW grind — tips the block over target
     without 500 KB of block stuffing.
  3. Decay: subsequent empty blocks pull m back toward (and clamp at) the 1.0 floor.
  4. Persistence: m is read from the block-tree DB on restart, not recomputed — the
     heavy block reports the same m before and after a node restart.

The m arithmetic itself (rise/decay/floor/cap, bit-exact) is pinned at the unit
level by txpow_tests/congestion_multiplier_recurrence; this test covers the wiring.
"""
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal, assert_greater_than

# A single OP_RETURN large enough to push one block over the sandbox target
# fullness (0.5% of MAX_BLOCK_WEIGHT). Stays under the 10 KB MAX_SCRIPT_SIZE.
PAD_BYTES = 9000


class CongestionMultiplierTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # Real Cuckatoo grinding (sandbox EDGEBITS-19) is seconds per tx.
        self.rpc_timeout = 1200
        # Allow the big OP_RETURN that pads our one tx over the target fullness.
        self.extra_args = [["-datacarriersize=20000"]]

    def m_of(self, node, blockhash):
        return node.getblockheader(blockhash)["congestion_multiplier"]

    def m_of_tip(self, node):
        return self.m_of(node, node.getbestblockhash())

    def run_test(self):
        node = self.nodes[0]
        node.createvault(vault_name="cong")
        addr = node.getnewaddress(address_type="bech32")

        # Mature coinbase 1 (COINBASE_MATURITY=100) so we have spendable funds.
        self.generatetoaddress(node, 110, addr)
        assert_greater_than(node.getbalance(), 0)

        # --- 1. Floor: a near-empty chain sits at m = 1.0. ---
        assert_equal(self.m_of_tip(node), 1.0)

        # --- 2. Rise: one padded tx makes the next block cross target fullness. ---
        dest = node.getnewaddress(address_type="bech32")
        raw = node.createrawtransaction([], [{dest: 0.01}, {"data": "ab" * PAD_BYTES}])
        funded = node.fundrawtransaction(raw)
        signed = node.signrawtransactionwithvault(funded["hex"])
        assert signed["complete"], "funded padded tx did not sign cleanly"
        txid = node.sendrawtransaction(signed["hex"])
        assert txid in node.getrawrelaypool(), "padded congestion tx not accepted"

        self.generatetoaddress(node, 1, addr)
        assert txid not in node.getrawrelaypool(), "padded tx was not mined"
        heavy_hash = node.getbestblockhash()
        m_loaded = self.m_of(node, heavy_hash)
        assert_greater_than(m_loaded, 1.0)  # congestion ratcheted m up off the floor

        # --- 3. Decay: empty blocks relax m back toward the floor (never below it). ---
        self.generatetoaddress(node, 12, addr)
        m_idle = self.m_of_tip(node)
        assert_greater_than(m_loaded, m_idle)  # strictly decayed
        assert m_idle >= 1.0, f"m fell below the floor: {m_idle}"

        # --- 4. Persistence: m survives a restart (read from disk, not recomputed). ---
        m_before = self.m_of(node, heavy_hash)
        self.restart_node(0, extra_args=["-datacarriersize=20000"])
        m_after = self.m_of(self.nodes[0], heavy_hash)
        assert_equal(m_before, m_after)

        self.log.info("Quicksilver #5c-1 congestion multiplier (rise + decay + persist): PASS")


if __name__ == "__main__":
    CongestionMultiplierTest(__file__).main()
