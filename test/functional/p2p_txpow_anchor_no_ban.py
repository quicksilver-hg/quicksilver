#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""F-137: a per-tx PoW anchor miss must not get the relaying peer discouraged.

CheckTxAnchor fails for three reasons and none of them means "these bytes are
invalid forever": the anchor is ahead of OUR tip (we are behind), it has aged out
of the recency window, or it resolves onto a branch we do not have. In every case
the same transaction is perfectly valid to a peer at a different height.

Classifying that TX_CONSENSUS made MaybePunishNodeForTx discourage whoever relayed
it, so a node a few blocks behind the tip banned honest peers for relaying valid
payments -- worst precisely when heights are most ragged, which is at launch.

The shape here is the common one, not a contrived one: node0 is 8 blocks ahead and
sends an ordinary payment. The vault anchors VAULT_ANCHOR_DEPTH back from node0's
tip, which is still ahead of node1's tip, so node1 cannot resolve the anchor and
rejects a payment that is in no way malformed.

NOTE ON THE POSITIVE CONTROL. Misbehaving() logs under HgLog::MESH at debug level,
so asserting the ABSENCE of "Misbehaving" proves nothing unless that category is
enabled and known to be reachable in this run. The control below sends a tx that
genuinely should be punished and asserts the line DOES appear. Without it this
test would pass just as happily against the unfixed code.
"""
from test_framework.messages import msg_tx, tx_from_hex
from test_framework.p2p import P2PInterface
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal, assert_greater_than

ANCHOR_DEPTH = 6      # VAULT_ANCHOR_DEPTH (src/vault/spend.h)
LAG = 8               # blocks node1 falls behind: > ANCHOR_DEPTH, so the anchor is unresolvable


class TxPowAnchorNoBanTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        # Both nodes need the mesh category or the Misbehaving assertions are vacuous.
        self.extra_args = [["-debug=mesh", "-debug=txpow"]] * 2
        # Real Cuckatoo cycle-finding is seconds/tx even at sandbox EDGEBITS-19.
        self.rpc_timeout = 1200

    def run_test(self):
        node0, node1 = self.nodes
        node0.createvault(vault_name="anchor")
        addr = node0.getnewaddress(address_type="bech32")

        # Chain deep enough to mature a coinbase (COINBASE_MATURITY=100).
        self.generatetoaddress(node0, 110, addr)
        self.sync_all()
        assert_greater_than(node0.getbalance(), 0)

        # Split the two, then let node0 run ahead. node1 is now a perfectly honest
        # node that is simply behind -- the everyday condition this test is about.
        self.disconnect_nodes(0, 1)
        behind_height = node1.getblockcount()
        self.generatetoaddress(node0, LAG, addr, sync_fun=self.no_op)
        assert_equal(node1.getblockcount(), behind_height)

        # An ordinary payment. Nothing about it is malformed.
        txid = node0.sendtoaddress(node0.getnewaddress(address_type="bech32"), 1)
        raw = node0.getrawtransaction(txid)
        decoded = node0.decoderawtransaction(raw)
        assert any(e != 0 for e in decoded["cuckatoo_cycle"]), "payment carries no proof"
        # The anchor node0 chose is ahead of node1's tip, so node1 cannot resolve it.
        assert_greater_than(decoded["anchor_height"], node1.getblockcount())

        # --- Positive control: prove this run can actually observe a discouragement. ---
        # Same payment with a corrupted cycle. On node0 the anchor DOES resolve, so it
        # gets past CheckTxAnchor and fails the proof itself -> "tx-pow-invalid" ->
        # TX_CONSENSUS -> discouraged. (The PoW gate runs well before conflict
        # detection, so sharing inputs with the real tx does not shortcut it.)
        bad = tx_from_hex(raw)
        bad.nCycle[0] ^= 0xffff
        bad.rehash()
        # send_message, not send_and_ping: being punished DISCONNECTS us
        # (MaybeDiscourageAndDisconnect), so there is no live connection left to carry
        # the pong back. The disconnect is itself part of what the control demonstrates.
        peer0 = node0.add_p2p_connection(P2PInterface())
        with node0.assert_debug_log(expected_msgs=["tx-pow-invalid", "Misbehaving"]):
            peer0.send_message(msg_tx(bad))
            peer0.wait_for_disconnect()
        self.log.info("positive control: an invalid cycle IS punished, detector works")

        # --- The actual case: the honest payment, relayed to the node that is behind. ---
        peer1 = node1.add_p2p_connection(P2PInterface())
        # send_message + the log wait, NOT send_and_ping. Under the unfixed code the peer
        # is disconnected, and a pong-based sync then dies on an internal "assert
        # self.is_connected" that says nothing about why. Waiting on the rejection line
        # instead needs no live connection, so the assertions below are what report the
        # failure -- and they name it.
        with node1.assert_debug_log(expected_msgs=["bad-txns-pow-anchor"],
                                    unexpected_msgs=["Misbehaving"]):
            peer1.send_message(msg_tx(tx_from_hex(raw)))
        # The consequence that actually matters, and the mirror of the control above:
        # the honest relayer is still here.
        assert peer1.is_connected, "honest peer was disconnected over a tip-relative anchor miss"

        # The rejection itself is correct and must stand: node1 genuinely cannot
        # validate this yet. What must not happen is the punishment.
        assert txid not in node1.getrawrelaypool()

        self.log.info("Quicksilver F-137 (stale/future anchor is not bannable): PASS")


if __name__ == "__main__":
    TxPowAnchorNoBanTest(__file__).main()
