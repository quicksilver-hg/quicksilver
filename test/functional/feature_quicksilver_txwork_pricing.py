#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Stage 2 flag day: transaction work is priced by bytes and UTXO creation.

Three rules, end to end on a sandbox node:

  1. A transaction must carry work proportional to its serialized bytes. Proved
     one cycle short of what its bytes demand it is rejected; proved at exactly
     that number it is accepted.
  2. A consolidating transaction pays the byte term ONLY. The UTXO delta clamps
     at zero, so many-in/one-out is never penalised and never rewarded -- and a
     same-size transaction that CREATES outputs is charged strictly more.
  3. A block's summed per-transaction mint is capped at nMaxBlockMint.

Sandbox drives base_coupled to zero through a huge nTxWorkCouplingK, and
BaseTxWork clamps that to 1 -- so required work here IS the transaction-local
factor, which is what makes these rules observable without a real difficulty.

See doc/design/chain-growth.md and section 13 of
tools/calibration/stage2-byte-pricing.md.
"""
from test_framework.cuckatoo import PROOFSIZE, proof_hash_int
from test_framework.messages import COIN
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error
from test_framework.vault import MiniVault

R_B = 4739              # consensus.nTxWorkRefBytes
U = 50                  # consensus.nTxUtxoRefCount
MAX_BLOCK_MINT = 2 * COIN
SANDBOX_TX_POW_MINT = 1 * COIN


def required_work(tx_bytes, nout, nin):
    """The consensus rule at base == 1: one ceiling division, delta clamped at zero.

    Restated here rather than read back from the node, so the test states the rule
    instead of echoing whatever the implementation happens to do.
    """
    delta = max(0, nout - nin)
    numerator = tx_bytes * U + delta * R_B
    denom = R_B * U
    return max(1, -(-numerator // denom))


def target_for(work):
    """The tx-PoW target a transaction owing `work` cycles must meet."""
    return ((1 << 256) - 1) // work


def solve_for_exact_work(work):
    """A 42-edge cycle whose proof clears `work` cycles but NOT work + 1.

    solve_trivial only guarantees the proof clears a target, and its overshoot is
    random: a proof aimed at 10 cycles clears 11 about 10/11 of the time, which
    would make a rejection test flaky rather than false. Bounding the proof hash on
    both sides removes the randomness entirely -- and pinning it is cheap, needing
    about work^2 hashes.
    """
    hi = target_for(work)          # hash <= hi  =>  clears `work`
    lo = target_for(work + 1)      # hash >  lo  =>  does NOT clear work + 1
    cycle = list(range(1, PROOFSIZE + 1))
    while True:
        h = proof_hash_int(cycle)
        if lo < h <= hi:
            return cycle
        cycle[0] = (cycle[0] + PROOFSIZE) & 0xffffffff


class TxWorkPricingTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [[
            '-txpownocycle=1',          # target check stays live; skip the cycle solve
            '-datacarriersize=200000',  # needed to pad transactions with MiniVault
        ]]

    def run_test(self):
        self.node = self.nodes[0]
        self.vault = MiniVault(self.node)
        self.generate(self.vault, 110)

        self.test_bytes_are_priced()
        self.test_consolidation_pays_the_byte_term_only()
        self.test_block_mint_is_capped()

    def prove(self, tx, work):
        """Re-prove `tx` at exactly `work` cycles, anchored at the current tip."""
        tx.nAnchorHeight = self.node.getblockcount()
        tx.nPowNonce = 0
        tx.nCycle = solve_for_exact_work(work)
        tx.rehash()
        return tx

    def test_bytes_are_priced(self):
        self.log.info("A transaction must carry work proportional to its bytes")
        built = self.vault.create_self_transfer(target_vsize=50_000)
        tx = built["tx"]
        size = len(tx.serialize())
        need = required_work(size, len(tx.vout), len(tx.vin))
        self.log.info(f"  {size} bytes, {len(tx.vin)} in / {len(tx.vout)} out -> {need} cycles")
        # If this ever floors to one the rest of the case proves nothing.
        assert_greater_than(need, 1)

        # One cycle short of what its bytes demand: rejected.
        self.prove(tx, need - 1)
        assert_raises_rpc_error(-26, "tx-pow-invalid",
                                self.node.sendrawtransaction, tx.serialize().hex())

        # Exactly what its bytes demand: accepted. Same transaction, same bytes --
        # the only thing that changed is the work behind it.
        self.prove(tx, need)
        txid = self.node.sendrawtransaction(tx.serialize().hex())
        assert txid in self.node.getrawrelaypool()
        self.generate(self.vault, 1)

    def test_consolidation_pays_the_byte_term_only(self):
        self.log.info("A consolidating transaction pays the byte term only")
        # Build a pool of spendable outputs to consolidate.
        self.vault.send_self_transfer_multi(from_node=self.node, num_outputs=20)
        self.generate(self.vault, 1)
        utxos = [self.vault.get_utxo(confirmed_only=True) for _ in range(20)]

        built = self.vault.create_self_transfer_multi(
            utxos_to_spend=utxos, num_outputs=1, target_vsize=20_000)
        tx = built["tx"]
        size = len(tx.serialize())
        nin, nout = len(tx.vin), len(tx.vout)
        assert_greater_than(nin, nout)  # genuinely consolidating

        # Raw delta is negative. Clamped, so the charge is the byte term alone.
        need = required_work(size, nout, nin)
        assert_equal(need, required_work(size, 0, 0))
        assert_greater_than(need, 1)
        self.log.info(f"  {nin} in / {nout} out, {size} bytes -> {need} cycles (byte term only)")

        self.prove(tx, need - 1)
        assert_raises_rpc_error(-26, "tx-pow-invalid",
                                self.node.sendrawtransaction, tx.serialize().hex())
        self.prove(tx, need)
        assert self.node.sendrawtransaction(tx.serialize().hex()) in self.node.getrawrelaypool()
        self.generate(self.vault, 1)

        # The same byte count spent CREATING outputs costs strictly more. That gap is
        # the UTXO term, and it is the reason a clamp rather than a credit is correct:
        # were the delta credited, the consolidating shape above would have been
        # charged LESS than its bytes, not the same.
        creating = required_work(size, 400, 1)
        assert_greater_than(creating, need)
        self.log.info(f"  same bytes creating 400 outputs -> {creating} cycles")

    def test_block_mint_is_capped(self):
        self.log.info("A block's summed per-transaction mint is capped")
        self.generate(self.vault, 1)  # start from an empty relaypool

        n_txs = 5
        for _ in range(n_txs):
            built = self.vault.create_self_transfer()
            tx = built["tx"]
            need = required_work(len(tx.serialize()), len(tx.vout), len(tx.vin))
            assert_equal(need, 1)  # a small payment still costs one cycle
            self.prove(tx, need)
            self.vault.sendrawtransaction(from_node=self.node, tx_hex=tx.serialize().hex())

        # The template must exceed the cap, or this case measures nothing.
        template = self.node.getblocktemplate({"rules": ["segwit"]})
        uncapped = sum(entry["mintvalue"] for entry in template["transactions"])
        assert_equal(uncapped, n_txs * SANDBOX_TX_POW_MINT)
        assert_greater_than(uncapped, MAX_BLOCK_MINT)

        blockhash = self.generate(self.vault, 1)[0]
        block = self.node.getblock(blockhash, 2)
        assert_equal(len(block["tx"]) - 1, n_txs)  # all five were mined
        coinbase_out = sum(int(round(o["value"] * COIN)) for o in block["tx"][0]["vout"])
        subsidy = self.node.getblockstats(blockhash)["subsidy"]
        self.log.info(f"  {n_txs} txs would mint {uncapped}, coinbase minted "
                      f"{coinbase_out - subsidy}, cap {MAX_BLOCK_MINT}")
        assert_equal(coinbase_out - subsidy, MAX_BLOCK_MINT)


if __name__ == "__main__":
    TxWorkPricingTest(__file__).main()
