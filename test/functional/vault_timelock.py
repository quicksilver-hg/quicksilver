#!/usr/bin/env python3
# Copyright (c) 2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

# Sandbox: a proof expires nMaxAnchorAge (20) blocks after its anchor, which is
# VAULT_ANCHOR_DEPTH (6) below the tip it was made at.
PROOF_SPAN = 20 - 6
TOO_FAR_AHEAD = (
    f"This locktime is too far ahead. A transfer's proof of work expires about {PROOF_SPAN} blocks after it is made, "
    f"so this transfer could never be accepted. Use a locktime at most {PROOF_SPAN} blocks ahead."
)


def assert_abandoned(tx):
    sent = [d for d in tx["details"] if d["category"] == "send"]
    assert sent
    assert all(d["abandoned"] for d in sent)


class VaultLocktimeTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()

    def run_test(self):
        node = self.nodes[0]

        mtp_tip = node.getblockheader(node.getbestblockhash())["mediantime"]

        self.log.info("Get new address with label")
        label = "timelock⌛🔓"
        address = node.getnewaddress(label=label)

        self.log.info("Send to new address with locktime")
        node.send(
            outputs=[{address: 5}],
            locktime=mtp_tip - 1,
        )
        self.generate(node, 1)

        self.log.info("Check that clock cannot change finality of confirmed txs")
        amount_before_ad = node.getreceivedbyaddress(address)
        amount_before_lb = node.getreceivedbylabel(label)
        list_before_ad = node.listreceivedbyaddress(address_filter=address)
        list_before_lb = node.listreceivedbylabel(include_empty=False)
        balance_before = node.getbalances()["mine"]["trusted"]
        coin_before = node.listunspent(maxconf=1)
        node.setmocktime(mtp_tip - 1)
        assert_equal(node.getreceivedbyaddress(address), amount_before_ad)
        assert_equal(node.getreceivedbylabel(label), amount_before_lb)
        assert_equal(node.listreceivedbyaddress(address_filter=address), list_before_ad)
        assert_equal(node.listreceivedbylabel(include_empty=False), list_before_lb)
        assert_equal(node.getbalances()["mine"]["trusted"], balance_before)
        assert_equal(node.listunspent(maxconf=1), coin_before)

        self.log.info("An accepted send carries no not-accepted warning")
        accepted = node.send(outputs=[{address: 1}])
        assert "warnings" not in accepted
        assert accepted["txid"] in node.getrawrelaypool()

        self.log.info("A send the node refuses as non-final is kept, and says so")
        mine_before = node.getbalances()["mine"]
        # Within the proof's lifetime (F-582). This was 100 blocks ahead, past the
        # point where the proof expires, so it pinned a transfer that could never be
        # accepted being kept and promised a resubmission: the F-582 defect itself.
        kept_height = node.getblockcount()
        kept = node.send(outputs=[{address: 1}], locktime=kept_height + 10)
        assert_equal(kept["warnings"], [
            "Not accepted by this node yet (non-final). "
            "The vault keeps the transaction and will resubmit it automatically."
        ])
        assert kept["txid"] not in node.getrawrelaypool()
        assert kept["txid"] in [tx["txid"] for tx in node.listtransactions(count=100)]

        self.log.info("The kept send's change counts as pending, so the balance does not drop (F-579)")
        # A feeless send to this vault's own address leaves what the vault owns unchanged.
        mine_after = node.getbalances()["mine"]
        assert mine_after["untrusted_pending"] > mine_before["untrusted_pending"]
        assert_equal(mine_after["trusted"] + mine_after["untrusted_pending"],
                     mine_before["trusted"] + mine_before["untrusted_pending"])

        self.log.info("gettransaction and listtransactions say the kept send is not accepted yet (F-576)")
        not_accepted = ("Not accepted by this node yet (non-final). "
                        "The vault keeps the transaction and will resubmit it automatically.")
        assert_equal(node.gettransaction(kept["txid"])["not_accepted"], not_accepted)
        listed = {tx["txid"]: tx for tx in node.listtransactions(count=100)}
        assert_equal(listed[kept["txid"]]["not_accepted"], not_accepted)
        assert "not_accepted" not in node.gettransaction(accepted["txid"])
        assert "not_accepted" not in listed[accepted["txid"]]

        self.log.info("It is not stored: the startup resubmission sets it again")
        self.restart_node(0)
        assert_equal(node.gettransaction(kept["txid"])["not_accepted"], not_accepted)

        self.log.info("Once the locktime has matured, the next resubmission clears it")
        # Close enough that the proof's anchor (tip - 6 when sent) is still within
        # sandbox's 20-block anchor age when the locktime matures.
        near_locktime = node.getblockcount() + 5
        near = node.send(outputs=[{address: 1}], locktime=near_locktime)
        assert_equal(node.gettransaction(near["txid"])["not_accepted"], not_accepted)
        self.generate(node, near_locktime - node.getblockcount())
        self.restart_node(0)
        assert near["txid"] in node.getrawrelaypool()
        assert "not_accepted" not in node.gettransaction(near["txid"])
        assert_equal(node.gettransaction(kept["txid"])["not_accepted"], not_accepted)

        self.log.info("A locktime the proof cannot outlive is refused before any proof is made (F-582)")
        assert_raises_rpc_error(-4, TOO_FAR_AHEAD, node.send,
                                outputs=[{address: 1}], locktime=node.getblockcount() + PROOF_SPAN + 1)
        assert_raises_rpc_error(-4, TOO_FAR_AHEAD, node.sendall,
                                recipients=[address], locktime=node.getblockcount() + PROOF_SPAN + 1)
        far_time = node.getblockheader(node.getbestblockhash())["mediantime"] + 10 * 365 * 24 * 60 * 60
        assert_raises_rpc_error(-4, TOO_FAR_AHEAD, node.send, outputs=[{address: 1}], locktime=far_time)

        self.log.info("The largest locktime the proof outlives is still built")
        edge_height = node.getblockcount()
        edge = node.send(outputs=[{address: 1}], locktime=edge_height + PROOF_SPAN)
        assert_equal(node.gettransaction(edge["txid"])["not_accepted"], not_accepted)

        self.log.info("A kept transfer whose proof expired is abandoned within a minute, and its coins return")
        self.generate(node, kept_height + PROOF_SPAN + 1 - node.getblockcount())
        node.syncwithvalidationinterfacequeue()
        kept_inputs = node.decoderawtransaction(node.gettransaction(kept["txid"])["hex"])["vin"]
        node.mockscheduler(60)
        node.syncwithvalidationinterfacequeue()
        expired = node.gettransaction(kept["txid"])
        assert_abandoned(expired)
        assert "not_accepted" not in expired
        assert "not_accepted" not in {tx["txid"]: tx for tx in node.listtransactions(count=100)}[kept["txid"]]
        unspent = {(u["txid"], u["vout"]) for u in node.listunspent(minconf=0)}
        for txin in kept_inputs:
            assert (txin["txid"], txin["vout"]) in unspent
        # The other kept transfer's proof has not expired: it is still kept.
        assert_equal(node.gettransaction(edge["txid"])["not_accepted"], not_accepted)

        self.log.info("After a restart, one that expired is abandoned, not offered again")
        self.generate(node, edge_height + PROOF_SPAN + 1 - node.getblockcount())
        self.restart_node(0)
        expired = node.gettransaction(edge["txid"])
        assert_abandoned(expired)
        assert "not_accepted" not in expired
        assert edge["txid"] not in node.getrawrelaypool()


if __name__ == "__main__":
    VaultLocktimeTest(__file__).main()
