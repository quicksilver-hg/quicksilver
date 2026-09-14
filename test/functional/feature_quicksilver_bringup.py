#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Quicksilver chain bring-up acceptance test.

Verifies the forked sandbox chain end to end: identity (sandbox, fresh genesis),
a Quicksilver (shg) bech32 address, mineable blocks, coinbase maturity under the
tail-emission ramp, and a spendable transaction.
"""
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal


class QuicksilverBringupTest(QuicksilverTestFramework):
    def add_options(self, parser):
        # Core 29.1 idiom: requesting vault options marks the test as needing a
        # vault; the framework creates the default vault and skips the test if
        # vault support is not compiled in.
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # Real Cuckatoo block PoW is ~seconds/block even at sandbox EDGEBITS-19
        # (finding a 42-cycle is intrinsically expensive), so mining 100+ blocks
        # for coinbase maturity needs a generous RPC timeout.
        self.rpc_timeout = 1200

    def run_test(self):
        node = self.nodes[0]

        # Core 29.1 no longer auto-creates a default vault; create one explicitly.
        node.createvault(vault_name="quicksilver")

        # 1. Identity: sandbox chain, height 0 at start (fresh Quicksilver genesis).
        info = node.getblockchaininfo()
        assert_equal(info["chain"], "sandbox")
        assert_equal(info["blocks"], 0)

        # 2. Address format: a freshly-created bech32 address uses the shg HRP.
        addr = node.getnewaddress(address_type="bech32")
        assert addr.startswith("shg1"), f"address {addr} does not use the shg HRP"

        # 3. Mine past coinbase maturity (COINBASE_MATURITY=100) so block 1's
        #    coinbase becomes spendable at height 101.
        self.generatetoaddress(node, 101, addr)
        assert_equal(node.getblockchaininfo()["blocks"], 101)

        # 3b. Each mined block carries a real Cuckatoo 42-cycle proof (the block
        #     would not have validated on connect otherwise). Confirm the tip's
        #     header exposes a non-zero cycle.
        header = node.getblockheader(node.getbestblockhash())
        assert "cuckatoo_cycle" in header, "block header missing cuckatoo_cycle"
        assert_equal(len(header["cuckatoo_cycle"]), 42)
        assert any(e != 0 for e in header["cuckatoo_cycle"]), "cuckatoo_cycle is all zeros"

        # 4. Tail-emission ramp pays out: a matured coinbase gives a balance.
        balance = node.getbalance()
        assert balance > 0, "no spendable balance after coinbase maturity"

        # 5. Spend: send to a second address and confirm it in a block.
        dest = node.getnewaddress(address_type="bech32")
        txid = node.sendtoaddress(dest, 1)
        self.generatetoaddress(node, 1, addr)
        assert_equal(node.gettransaction(txid)["confirmations"], 1)

        # 5b. The vault-produced spend carries a real per-tx Cuckatoo proof (it
        #     could not have entered the relaypool or a block otherwise). Use the
        #     vault's verbose gettransaction decode (the tx is confirmed, so it is
        #     no longer in the relaypool and getrawtransaction would need -txindex).
        decoded = node.gettransaction(txid, True)["decoded"]
        assert "cuckatoo_cycle" in decoded, "spend tx missing cuckatoo_cycle"
        assert_equal(len(decoded["cuckatoo_cycle"]), 42)
        assert any(e != 0 for e in decoded["cuckatoo_cycle"]), "spend tx cuckatoo_cycle is all zeros"

        # 6. Frame-B mint (#4): a block containing a user tx mints C to the miner.
        #    Compare the miner's coinbase payout in an EMPTY block vs a block that
        #    includes one user transaction. The difference is the mint C (= the 1 COIN
        #    sandbox fixture), so it must be exactly 1 whole coin.
        from decimal import Decimal

        # Under the #5a emission ramp the per-block subsidy DECREASES with height, so two
        # consecutive blocks have different subsidies and the subsidy term would not cancel.
        # Isolate the mint by comparing an empty block and a one-tx block mined at the SAME
        # height: mine the empty block, record its payout, invalidate it to roll back, then
        # mine the tx block at that same height. GetBlockSubsidy is purely a function of
        # height, so subsidy(H) is identical for both and cancels, leaving exactly the mint C.
        # This is robust to any emission curve, flat or ramped.
        empty_hash = self.generatetoaddress(node, 1, addr)[0]
        empty_height = node.getblock(empty_hash)["height"]
        empty_cb = node.getblock(empty_hash, 2)["tx"][0]
        empty_payout = sum(o["value"] for o in empty_cb["vout"])

        node.invalidateblock(empty_hash)  # roll back to empty_height - 1

        dest2 = node.getnewaddress()
        node.sendtoaddress(dest2, 1)
        tx_block_hash = self.generatetoaddress(node, 1, addr)[0]
        tx_block = node.getblock(tx_block_hash, 2)
        assert_equal(tx_block["height"], empty_height)  # SAME height -> subsidy cancels
        assert_equal(len(tx_block["tx"]), 2)  # coinbase + the one user tx
        tx_cb_payout = sum(o["value"] for o in tx_block["tx"][0]["vout"])

        minted = tx_cb_payout - empty_payout
        assert minted >= Decimal("1.0"), f"expected mint >= 1 COIN, got {minted}"
        assert minted < Decimal("2.0"), f"single-tx block over-minted: {minted}"
        self.log.info(f"Frame-B mint: one-tx block minted {minted} more than empty block")

        self.log.info("Quicksilver sandbox bring-up acceptance: PASS")


if __name__ == "__main__":
    QuicksilverBringupTest(__file__).main()
