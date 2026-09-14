#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Replace-by-surplus-work (#6): replacement observed end-to-end.

Quicksilver is feeless (#5b), so a conflicting tx replaces an incumbent iff it
carries strictly more surplus per-tx PoW work (the 5c-2 ranking key). There is
no vault "rarer cycle" knob yet (that
is #7) and no Python Cuckatoo solver, so we cannot REQUEST a target surplus.
Instead we grind several conflicting candidates over the same input — each grind
finds a random cycle, hence random work — MEASURE each one's work via the #6
`txwork` RPC field, and order our submissions accordingly. Two txs ground at the
same tip share an anchor (same floor), so `txwork` order == surplus order.

Covers:
  1. A higher-surplus conflict REPLACES the incumbent (the feeless bump).
  2. A lower-surplus conflict is REJECTED ("insufficient surplus work").
  3. The winner mines.
"""
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error


class FeelessReplaceTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # Real Cuckatoo cycle-finding is seconds per grind even at sandbox EDGEBITS-19,
        # and we grind several candidates.
        self.rpc_timeout = 1200

    def grind_over(self, node, utxo):
        """Build a feeless (in==out) tx spending `utxo` to a FRESH address, grinding a
        fresh anchored per-tx proof. Returns (signed_hex, actual_txwork:int).

        The per-tx PoW grind is deterministic in the tx body + anchor, so each candidate
        must differ in body to get a different proof/work. We keep the same input (so all
        candidates conflict) but pay a distinct fresh address — that changes the proof
        preimage (which covers outputs but not scriptSig/witness), yielding distinct work
        while preserving both the conflict and the shared anchor."""
        dest = node.getnewaddress(address_type="bech32")
        raw = node.createrawtransaction(
            [{"txid": utxo["txid"], "vout": utxo["vout"]}], [{dest: utxo["amount"]}])
        # add_inputs=False: keep exactly the one pinned input. fundrawtransaction grinds
        # the proof + anchors to the tip; output already equals input (feeless in==out),
        # so no change is added.
        funded = node.fundrawtransaction(raw, {"add_inputs": False})
        signed = node.signrawtransactionwithvault(funded["hex"])
        assert signed["complete"], "funded feeless tx did not sign cleanly"
        work = int(node.decoderawtransaction(signed["hex"])["txwork"], 16)
        return signed["hex"], work

    def three_distinct_candidates(self, node, utxo):
        """Grind three conflicting candidates with distinct work. Distinct output addresses
        give distinct proofs; equal work across different preimages is astronomically
        unlikely, but the loop keeps the ordering deterministic if it ever happens."""
        while True:
            cands = sorted((self.grind_over(node, utxo) for _ in range(3)),
                           key=lambda c: c[1])
            works = [w for _, w in cands]
            if len(set(works)) == 3:
                return cands  # ascending by work == ascending by surplus (shared anchor)

    def run_test(self):
        node = self.nodes[0]
        node.createvault(vault_name="replace")
        addr = node.getnewaddress(address_type="bech32")
        self.generatetoaddress(node, 110, addr)  # mature a coinbase to spend

        utxo = node.listunspent(100)[0]
        low, mid, high = self.three_distinct_candidates(node, utxo)

        # 1. Submit the lowest-work candidate -> relaypool.
        low_txid = node.sendrawtransaction(low[0])
        assert low_txid in node.getrawrelaypool(), "floor tx not accepted"
        s_low = int(node.getrelaypoolentry(low_txid)["txwork_surplus"], 16)

        # 2. Submit the highest-work conflict -> it REPLACES the incumbent.
        high_txid = node.sendrawtransaction(high[0])
        mp = node.getrawrelaypool()
        assert high_txid in mp and low_txid not in mp, "higher-surplus tx did not replace incumbent"
        s_high = int(node.getrelaypoolentry(high_txid)["txwork_surplus"], 16)
        assert_greater_than(s_high, s_low)  # replacement carried strictly more surplus

        # 3. Submit the mid-work conflict: now lower-surplus than the incumbent -> REJECTED.
        assert_raises_rpc_error(-26, "insufficient surplus work",
                                node.sendrawtransaction, mid[0])
        assert_equal(node.getrawrelaypool(), [high_txid])  # incumbent untouched

        # 4. Mine; the winner confirms.
        self.generatetoaddress(node, 1, addr)
        assert high_txid not in node.getrawrelaypool(), "winner was not mined"
        assert_equal(node.gettransaction(high_txid)["confirmations"], 1)

        self.log.info("Quicksilver #6 replace-by-surplus-work (bump + lower-surplus reject): PASS")


if __name__ == "__main__":
    FeelessReplaceTest(__file__).main()
