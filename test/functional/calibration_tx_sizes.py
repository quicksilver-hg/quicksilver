#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""M4: measure real Quicksilver transaction sizes, including the 176-byte
proof-of-work tail, and derive transactions-per-block and mint-per-block.

Replaces an invented "~394 bytes per transaction". nTxPowMint = 57,143 was
derived assuming ~1,143 bytes, so the gap between the assumed size and the real
one is exactly the mint drift in finding 3.4 of the spec.

Writes tools/calibration/m4-tx-sizes/tx-sizes.csv. See
doc/design/chain-growth.md
"""

import csv
import os

from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal, assert_greater_than
from test_framework.vault import MiniVault

# Block capacity is bound by WEIGHT, not serialized size. consensus.h documents
# MAX_BLOCK_SERIALIZED_SIZE as "only for buffer size limits"; the network rule is
# GetBlockWeight(block) > MAX_BLOCK_WEIGHT (validation.cpp). The spec's own
# n_max = 4,000,000 / 400,000 = 10 is likewise a ratio of weights.
MAX_BLOCK_WEIGHT = 4_000_000           # src/consensus/consensus.h (network rule)
MAX_BLOCK_SERIALIZED_SIZE = 4_000_000  # src/consensus/consensus.h (buffers only)
MAX_STANDARD_TX_WEIGHT = 400_000       # src/policy/policy.h
TX_POW_TAIL_BYTES = 172                # 4 nAnchorHeight + 42 * 4 nCycle
NTXPOWMINT_CINNABAR = 57_143
DESIGN_INTENT_COIN_PER_BLOCK = 2.00

FIELDNAMES = ["label", "vsize", "weight", "serialized_bytes", "txs_per_block",
              "mint_per_block_cinnabar", "bytes_per_full_block"]


class CalibrationTxSizes(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # Sandbox tx-PoW is trivial, so many transactions can be built without
        # an infeasible grind. Sizes are unaffected: the 176-byte tail is
        # present either way.
        self.extra_args = [["-txpownocycle=1"]]

    def run_test(self):
        node = self.nodes[0]
        vault = MiniVault(node)
        self.generate(vault, 110)

        rows = []

        def record(label, tx_hex):
            serialized = len(tx_hex) // 2
            assert_greater_than(serialized, TX_POW_TAIL_BYTES)
            decoded = node.decoderawtransaction(tx_hex)
            weight = decoded["weight"]
            # Weight is the binding constraint, so it sets how many transactions
            # fit. Using serialized bytes here overstates capacity by ~3.6x and
            # inflates every mint figure derived from it.
            txs_per_block = MAX_BLOCK_WEIGHT // weight
            rows.append({
                "label": label,
                "vsize": decoded["vsize"],
                "weight": weight,
                "serialized_bytes": serialized,
                "txs_per_block": txs_per_block,
                "mint_per_block_cinnabar": txs_per_block * NTXPOWMINT_CINNABAR,
                # What a weight-full block of these actually costs on disk —
                # this, not the 4 MB cap, is the input to chain growth.
                "bytes_per_full_block": txs_per_block * serialized,
            })
            return serialized

        record("one_in_one_out", vault.create_self_transfer()["hex"])
        record("one_in_two_out",
               vault.create_self_transfer_multi(num_outputs=2)["hex"])

        # The bloat vector: as few transactions as possible per byte. target_vsize
        # pads to the standard weight ceiling directly, which is what an attacker
        # optimising for bytes-per-transaction would do.
        big_target = MAX_STANDARD_TX_WEIGHT // 4  # weight units -> vsize
        big = vault.create_self_transfer(target_vsize=big_target)["hex"]
        big_size = record("near_max_standard", big)

        # Proves the bloat row is actually large — without it the row could
        # silently be a small transaction and the mint-drift figure meaningless.
        assert_greater_than(big_size, 50_000)

        # Cross-check the capacity model against the spec: finding 3.3 uses
        # n_max = block_weight / max_tx_weight = 10. If this row does not give
        # exactly 10, then this script and the spec disagree about what limits a
        # block, and every mint figure below is computed on the wrong basis.
        assert_equal(rows[-1]["txs_per_block"],
                     MAX_BLOCK_WEIGHT // MAX_STANDARD_TX_WEIGHT)

        # Anchor on SRCDIR, not tmpdir: tmpdir lives under /tmp, so walking up
        # from it lands outside the repo entirely.
        outdir = os.path.join(self.config["environment"]["SRCDIR"],
                              "tools", "calibration", "m4-tx-sizes")
        os.makedirs(outdir, exist_ok=True)
        path = os.path.join(outdir, "tx-sizes.csv")
        with open(path, "w", newline="", encoding="utf-8") as fh:
            writer = csv.DictWriter(fh, fieldnames=FIELDNAMES)
            writer.writeheader()
            writer.writerows(rows)

        for row in rows:
            self.log.info("%s: %d bytes, %d vsize, %d weight, %d tx/block, "
                          "%d cinnabar minted, %d bytes per full block",
                          row["label"], row["serialized_bytes"], row["vsize"],
                          row["weight"], row["txs_per_block"],
                          row["mint_per_block_cinnabar"], row["bytes_per_full_block"])

        # The lightest transaction is what drives mint drift: it packs the most
        # transactions, and therefore the most mint, into one block. Record
        # whether the 2-COIN-per-block design intent still holds; do NOT assert
        # it, because discovering that it does not is the point of the
        # measurement.
        lightest = min(rows, key=lambda r: r["weight"])
        implied = lightest["mint_per_block_cinnabar"] / 1e8
        self.log.info("MINT DRIFT: lightest tx %r at %d weight gives %d tx/block, "
                      "implying %.3f COIN/block against the %.2f design intent (%.2fx)",
                      lightest["label"], lightest["weight"], lightest["txs_per_block"],
                      implied, DESIGN_INTENT_COIN_PER_BLOCK,
                      implied / DESIGN_INTENT_COIN_PER_BLOCK)

        heaviest = max(rows, key=lambda r: r["weight"])
        self.log.info("MINT SPAN: heaviest tx %r gives %d tx/block, %.3f COIN/block "
                      "— a %.0fx spread across ways of filling one block",
                      heaviest["label"], heaviest["txs_per_block"],
                      heaviest["mint_per_block_cinnabar"] / 1e8,
                      lightest["mint_per_block_cinnabar"]
                      / heaviest["mint_per_block_cinnabar"])

        # The CSV is the deliverable; a truncated or mis-keyed file would be
        # discovered only by the model script, long after this ran.
        with open(path, newline="", encoding="utf-8") as fh:
            written = list(csv.DictReader(fh))
        assert_equal(len(written), len(rows))
        assert_equal(list(written[0].keys()), FIELDNAMES)
        self.log.info("wrote %s", path)


if __name__ == "__main__":
    CalibrationTxSizes(__file__).main()
