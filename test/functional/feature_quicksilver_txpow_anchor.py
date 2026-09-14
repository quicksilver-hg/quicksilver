#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""#5c-1 Phase 1: per-tx PoW anchor — end-to-end on a sandbox node.

Covers the two anchor behaviours that are reachable without a Python Cuckatoo
solver (the vault grinds the proof in C++):

  1. A vault tx is anchored to the current tip and grinds its per-tx PoW over
     that block's hash; it enters the relaypool and is mined.
  2. A tx whose anchor falls outside the recency window (nMaxAnchorAge=20 on
     sandbox) is rejected as stale ("bad-txns-pow-anchor").

The precompute defence (a proof ground against a DIFFERENT anchor hash is
invalid) cannot be constructed here — it needs a Cuckatoo solver in Python to
mis-grind a proof — and is covered at the unit level by
txpow_tests/proof_is_bound_to_anchor_hash.

The stale case relies on the fact that the proof pre-image excludes
scriptSig/witness, so the vault grinds the proof BEFORE signing. That lets
fundrawtransaction return a fully-anchored, fully-proven, NON-broadcast tx: we
grind it at height H, mine past the window, then submit it and watch it bounce.
"""
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error

MAX_ANCHOR_AGE = 20   # consensus.nMaxAnchorAge on sandbox (kernel/chainparams.cpp)
ANCHOR_DEPTH = 6      # VAULT_ANCHOR_DEPTH (src/vault/spend.h)


class TxPowAnchorTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # Real Cuckatoo cycle-finding is seconds/block even at sandbox EDGEBITS-19.
        self.rpc_timeout = 1200

    def run_test(self):
        node = self.nodes[0]
        node.createvault(vault_name="anchor")
        addr = node.getnewaddress(address_type="bech32")

        # Build a chain so coinbase 1 matures (COINBASE_MATURITY=100) and we have
        # spendable funds to construct anchored txs.
        self.generatetoaddress(node, 110, addr)
        assert_greater_than(node.getbalance(), 0)

        # --- Case 1: a fresh vault tx is anchored to the tip and mineable. ---
        dest = node.getnewaddress(address_type="bech32")
        tip_height = node.getblockcount()
        txid = node.sendtoaddress(dest, 1)
        assert txid in node.getrawrelaypool(), "anchored vault tx not accepted to relaypool"

        # The tx commits a recent anchor and carries a real 42-cycle proof ground over
        # that anchor's block hash. The vault anchors VAULT_ANCHOR_DEPTH blocks BACK
        # from the tip, not at it: the proof commits to the anchor's hash, so an
        # orphaned anchor is a dead proof, and the grind is long enough that shallow
        # reorgs matter.
        entry = node.getrelaypoolentry(txid)  # noqa: F841  (presence already asserted)
        decoded = node.decoderawtransaction(node.getrawtransaction(txid))
        assert_equal(decoded["anchor_height"], tip_height - ANCHOR_DEPTH)
        assert any(e != 0 for e in decoded["cuckatoo_cycle"]), "spend tx cuckatoo_cycle is all zeros"

        self.generatetoaddress(node, 1, addr)
        assert txid not in node.getrawrelaypool(), "anchored tx was not mined"
        assert_equal(node.gettransaction(txid)["confirmations"], 1)

        # --- Case 2: a tx whose anchor has aged out of the window is rejected. ---
        # fundrawtransaction grinds the proof + sets the anchor (to the current tip)
        # but does NOT broadcast. We sign it (signing leaves the pre-image, hence the
        # proof, untouched), then mine past nMaxAnchorAge so the anchor is stale, then
        # submit it.
        stale_anchor_height = node.getblockcount()
        raw = node.createrawtransaction([], [{node.getnewaddress(): 1}])
        funded = node.fundrawtransaction(raw)
        signed = node.signrawtransactionwithvault(funded["hex"])
        assert signed["complete"], "funded anchored tx did not sign cleanly"

        # Sanity: the funded tx is anchored ANCHOR_DEPTH back from where it was built.
        decoded_stale = node.decoderawtransaction(signed["hex"])
        assert_equal(decoded_stale["anchor_height"], stale_anchor_height - ANCHOR_DEPTH)
        assert any(e != 0 for e in decoded_stale["cuckatoo_cycle"]), "stale tx has no proof"

        # Mine MAX_ANCHOR_AGE + 1 blocks; the tx was never broadcast, so it is not
        # included. Its anchor is now older than the window allows.
        self.generatetoaddress(node, MAX_ANCHOR_AGE + 1, addr)
        assert_greater_than(node.getblockcount() - (stale_anchor_height - ANCHOR_DEPTH),
                            MAX_ANCHOR_AGE)

        assert_raises_rpc_error(-26, "bad-txns-pow-anchor",
                                node.sendrawtransaction, signed["hex"])

        self.log.info("Quicksilver #5c-1 per-tx anchor (mine + stale-reject): PASS")


if __name__ == "__main__":
    TxPowAnchorTest(__file__).main()
