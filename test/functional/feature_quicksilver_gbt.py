#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""External-miner getblocktemplate round-trip (sub-project B): GBT -> assemble a
block in Python -> solve the trivial sandbox block PoW (fill nCycle) -> submitblock
-> accepted. Also pins the pow descriptor, the Frame-B subsidy/mintvalue breakdown,
proposal mode (accept/reject), and longpoll advance."""
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal
from test_framework.messages import CBlock, tx_from_hex
from test_framework.blocktools import create_coinbase, add_witness_commitment
from test_framework.cuckatoo import solve_trivial, proof_hash_int


class QuicksilverGbtTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.rpc_timeout = 1200

    def _assemble_and_solve(self, tmpl, script):
        block = CBlock()
        block.nVersion = tmpl["version"]
        block.hashPrevBlock = int(tmpl["previousblockhash"], 16)
        block.nTime = tmpl["curtime"]
        block.nBits = int(tmpl["bits"], 16)
        # The template's congestion multiplier, valid for its own transaction set (which
        # this block uses unchanged). Inside the pre-pow, so it precedes the grind.
        block.nCongestion = tmpl["congestion"]
        block.nNonce = 0
        coinbase = create_coinbase(tmpl["height"], script_pubkey=script)
        coinbase.vout[0].nValue = tmpl["coinbasevalue"]   # subsidy + ΣC
        coinbase.rehash()
        block.vtx = [coinbase] + [tx_from_hex(t["data"]) for t in tmpl["transactions"]]
        add_witness_commitment(block)
        block.hashMerkleRoot = block.calc_merkle_root()
        # solve the trivial block PoW: fill nCycle so the proof hash clears target
        target_int = int(tmpl["pow"]["proofhashtarget"], 16)
        block.nCycle = solve_trivial(target_int)
        assert proof_hash_int(block.nCycle) <= target_int
        block.rehash()
        return block

    def run_test(self):
        node = self.nodes[0]
        node.createvault(vault_name="m")
        addr = node.getnewaddress(address_type="bech32")
        self.generatetoaddress(node, 110, addr)

        # a feeless mint-bearing tx (vault grinds the real per-tx proof) -> ΣC > 0
        txid = node.sendtoaddress(node.getnewaddress(), 1)
        entry = node.getrelaypoolentry(txid)
        assert "fees" not in entry

        tmpl = node.getblocktemplate({"rules": ["segwit"]})
        tx_entry = next(t for t in tmpl["transactions"] if t["txid"] == txid)
        assert "fee" not in tx_entry
        assert_equal(tx_entry["mintvalue"], 100000000)
        assert "txwork" in tx_entry
        assert "txwork_surplus" in tx_entry
        assert "txwork_rate" in tx_entry
        pow = tmpl["pow"]
        assert_equal(pow["algorithm"], "cuckatoo")
        assert_equal(pow["edgebits"], 19)
        assert_equal(pow["proofsize"], 42)
        assert_equal(pow["proofhash"], "blake2b")
        assert_equal(pow["proofhashtarget"], tmpl["target"])
        assert_equal(pow["trivialcycle"], True)
        assert_equal(tmpl["coinbasevalue"], tmpl["subsidy"] + tmpl["mintvalue"])
        assert tmpl["mintvalue"] > 0, "expected the per-tx mint sum > 0 with a mint-bearing tx"
        assert txid in [t["txid"] for t in tmpl["transactions"]]
        longpoll_before = tmpl["longpollid"]

        script = bytes.fromhex(node.getaddressinfo(addr)["output_script"])

        # proposal mode: a well-formed assembled block validates (PoW skipped per BIP23)
        block = self._assemble_and_solve(tmpl, script)
        prop = node.getblocktemplate({"rules": ["segwit"], "mode": "proposal",
                                      "data": block.serialize().hex()})
        assert prop is None, f"valid proposal rejected: {prop}"

        # proposal reject: corrupt the merkle root
        bad = self._assemble_and_solve(tmpl, script)
        bad.hashMerkleRoot ^= 1
        bad.rehash()
        rej = node.getblocktemplate({"rules": ["segwit"], "mode": "proposal",
                                     "data": bad.serialize().hex()})
        assert rej is not None, "tampered proposal was not rejected"

        # submit the solved block -> accepted, tip advances, mint tx confirms
        h0 = node.getblockcount()
        res = node.submitblock(block.serialize().hex())
        assert res is None, f"submitblock rejected: {res}"
        assert_equal(node.getblockcount(), h0 + 1)
        assert_equal(node.gettransaction(txid)["confirmations"], 1)

        # longpoll id advances after the new tip
        tmpl2 = node.getblocktemplate({"rules": ["segwit"]})
        assert tmpl2["longpollid"] != longpoll_before, "longpollid did not advance"
        self.log.info("Quicksilver GBT external-miner round-trip + proposal + longpoll: PASS")


if __name__ == "__main__":
    QuicksilverGbtTest(__file__).main()
