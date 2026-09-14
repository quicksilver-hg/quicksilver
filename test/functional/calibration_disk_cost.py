#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""M5: measure what a full block actually costs on disk — blocks, undo data,
block index, and chainstate — rather than counting block bytes alone.

The 420 GB/year headline counts block bytes only. An operator also carries undo
files, the block index, and the chainstate, so the real ceiling is higher.

Writes tools/calibration/m5-disk-cost/disk-cost.csv.
"""

import collections
import csv
import os

from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal, assert_greater_than
from test_framework.vault import MiniVault

# Capacity is bound by WEIGHT, not serialized size — consensus.h calls
# MAX_BLOCK_SERIALIZED_SIZE "only for buffer size limits" and the network rule is
# GetBlockWeight(block) > MAX_BLOCK_WEIGHT. See tools/calibration/m4-tx-sizes.
MAX_BLOCK_WEIGHT = 4_000_000

# Transactions are made heavy with many spendable OUTPUTS rather than with
# MiniVault's target_vsize padding. Two reasons:
#
#  1. target_vsize pads with one large bare OP_RETURN, which relay policy rejects
#     ("scriptpubkey", -26). It would need -acceptnonstdtxn=1 to broadcast.
#  2. More importantly, an OP_RETURN output is provably unspendable and never
#     enters the UTXO set. Filling blocks that way would report a chainstate cost
#     near zero — and chainstate is precisely one of the components this
#     measurement exists to capture.
#
# Many-output transactions are standard, spendable, and grow the UTXO set the way
# real traffic does, so the per-block cost measured here is one an operator would
# actually carry.
# Sized just under MAX_STANDARD_TX_WEIGHT (400,000): at ~172 weight per output a
# transaction of this many outputs is ~396,500 weight, so roughly 11 fill a
# block. Fewer, heavier transactions cost the same in total outputs but a lot
# less in per-transaction overhead (build, sign, tx-PoW, RPC round trip).
OUTPUTS_PER_TX = 2_300
MAX_STANDARD_TX_WEIGHT = 400_000  # src/policy/policy.h

# The shape is a parameter because the per-block cost is NOT a single number —
# it depends almost entirely on how many UTXOs the traffic leaves behind, and
# chainstate is 75% of the total at the adversarial shape. Running two shapes and
# checking that chainstate divided by net UTXOs created is the same constant is
# what licenses computing any other shape from one measurement.

# blk*.dat and rev*.dat are preallocated in 16 MB / 1 MB chunks and the space is
# really reserved, not sparse — neither st_size nor st_blocks moves while a block
# lands inside an existing chunk. Ten blocks therefore measured a per-block cost
# of exactly zero. Enough blocks must be written to claim several fresh chunks,
# so the delta spans real allocations; the amortised figure is also the honest
# one, because an operator pays for reserved space whether or not it is filled.
FILLED_BLOCKS = 64

# Every fill transaction spends one mature coinbase, so the pool must cover the
# whole run plus the 100 blocks of coinbase maturity.
SETUP_BLOCKS = 900

# A block below this fraction of the WEIGHT cap has not filled, and the per-block
# figures would understate what an operator really carries.
MIN_FILL_FRACTION = 0.8

FIELDNAMES = ["component", "bytes_per_block"]

# Block and undo storage is taken from the node's own size_on_disk accounting
# rather than from a filesystem walk. blk/rev files are preallocated in 16 MB and
# 1 MB chunks, so over a 64-block window an st_blocks walk lands mid-chunk and
# undercounts: it reported 786,432 bytes per block against 992,277 consensus
# bytes, which is impossible — a block cannot occupy less space than it is.
# size_on_disk tracks bytes actually used and agrees with the consensus size.
#
# LevelDB directories are not preallocated, so the walk is accurate for those.
LEVELDB_COMPONENTS = ("block_index", "chainstate")


def dir_bytes(path):
    """Actual disk consumption under `path`, via st_blocks rather than st_size.

    Used for the LevelDB directories only — see LEVELDB_COMPONENTS for why the
    block and undo files cannot be measured this way.
    """
    total = 0
    for root, _dirs, files in os.walk(path):
        for name in files:
            total += os.stat(os.path.join(root, name)).st_blocks * 512
    return total


class CalibrationDiskCost(QuicksilverTestFramework):
    def add_options(self, parser):
        parser.add_argument("--outputs-per-tx", type=int, default=OUTPUTS_PER_TX,
                            help="outputs per fill transaction; sets how "
                                 "UTXO-heavy the simulated traffic is")
        parser.add_argument("--setup-blocks", type=int, default=SETUP_BLOCKS,
                            help="blocks mined before filling; must supply one "
                                 "mature coinbase per fill transaction")
        parser.add_argument("--recycle", action="store_true",
                            help="spend outputs created by earlier fill "
                                 "transactions instead of one coinbase each")
        parser.add_argument("--tag", default="",
                            help="suffix for the output CSV, so a second shape "
                                 "does not overwrite the first")

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-txpownocycle=1"]]

    def measure(self, node, blocks_dir, datadir):
        return {
            # Authoritative for blk/rev: bytes used, not chunks reserved.
            "blocks_and_undo": node.getblockchaininfo()["size_on_disk"],
            "block_index": dir_bytes(os.path.join(blocks_dir, "index")),
            "chainstate": dir_bytes(os.path.join(datadir, "chainstate")),
        }

    def run_test(self):
        node = self.nodes[0]
        vault = MiniVault(node)
        self.generate(vault, self.options.setup_blocks)

        datadir = os.path.join(node.datadir_path, self.chain)
        blocks_dir = os.path.join(datadir, "blocks")

        # Drain the vault's tracked UTXOs into a plain list ONCE, and spend from
        # that list explicitly for the rest of the run.
        #
        # MiniVault.get_utxo re-sorts its entire UTXO list on every call and then
        # does a linear .index() on it. Each fill transaction creates thousands
        # of outputs, so leaving them tracked makes the loop quadratic: an
        # earlier version of this measurement grew the list to ~1.5M entries and
        # had not finished after 26 minutes. Spending from a pre-drained pool and
        # never scanning the results back in keeps it O(1) per transaction.
        pool = collections.deque(
            vault.get_utxos(include_immature_coinbase=False, mark_as_spent=True))

        # Learn the real weight of one fill transaction rather than assuming it,
        # then size the per-block batch from that. A hard-coded count would
        # quietly stop filling blocks if the transaction shape ever changed.
        outputs_per_tx = self.options.outputs_per_tx
        recycle = self.options.recycle
        sample = vault.create_self_transfer_multi(utxos_to_spend=[pool[0]],
                                                   num_outputs=outputs_per_tx)
        tx_weight = node.decoderawtransaction(sample["hex"])["weight"]
        txs_per_block = MAX_BLOCK_WEIGHT // tx_weight + 1
        self.log.info("fill transaction: %d outputs, %d weight -> %d per block",
                      outputs_per_tx, tx_weight, txs_per_block)

        # A fill transaction over the standard weight cap would be rejected by
        # relay, and the run would fail late and confusingly.
        assert tx_weight <= MAX_STANDARD_TX_WEIGHT, (
            f"fill transaction is {tx_weight} weight, over the "
            f"{MAX_STANDARD_TX_WEIGHT} standard cap; lower OUTPUTS_PER_TX")

        needed = txs_per_block if recycle else FILLED_BLOCKS * txs_per_block
        assert len(pool) >= needed, (
            f"only {len(pool)} mature coinbases for {needed} fill transactions; "
            f"raise --setup-blocks")

        node.syncwithvalidationinterfacequeue()
        node.gettxoutsetinfo()  # force the chainstate to be written out
        before = self.measure(node, blocks_dir, datadir)

        # Fill blocks as full as the harness will build them, then measure the
        # delta over enough blocks to span several preallocation chunks.
        observed = []
        for _ in range(FILLED_BLOCKS):
            for _ in range(txs_per_block):
                tx = vault.create_self_transfer_multi(
                    utxos_to_spend=[pool.popleft()], num_outputs=outputs_per_tx)
                # Deliberately NOT vault.sendrawtransaction: that scans the new
                # outputs back into the vault, which is the quadratic blowup.
                node.sendrawtransaction(hexstring=tx["hex"])
                # Feed the new outputs back so the pool sustains itself. Without
                # this every fill transaction needs its own mature coinbase, and
                # an output-light shape -- the one that models honest traffic --
                # would need tens of thousands of setup blocks to measure.
                # Appending is O(1); it is MiniVault's own tracking that is
                # quadratic, and that is still bypassed.
                #
                # The pool is FIFO. Spending from the back would make each
                # transaction spend the outputs the previous one just created,
                # building an unconfirmed chain thousands deep against the
                # 25-ancestor relaypool limit. Taking from the front means a
                # recycled output is not reached until a later block, by which
                # point it is confirmed and the ancestor count has reset -- so
                # the seed pool only has to cover ONE block, not the whole run.
                if recycle:
                    pool.extend(tx["new_utxos"])
            block = node.getblock(self.generate(node, 1)[0])
            observed.append((block["weight"], block["size"]))

        node.syncwithvalidationinterfacequeue()
        node.gettxoutsetinfo()
        after = self.measure(node, blocks_dir, datadir)

        mean_weight = sum(w for w, _ in observed) / len(observed)
        mean_size = sum(s for _, s in observed) / len(observed)
        fill = mean_weight / MAX_BLOCK_WEIGHT
        self.log.info("mean block weight %.0f = %.1f%% of the %d cap; "
                      "mean serialized size %.0f bytes (%.2f weight per byte)",
                      mean_weight, fill * 100, MAX_BLOCK_WEIGHT, mean_size,
                      mean_weight / mean_size)

        # Do NOT silently report per-block costs derived from half-empty blocks:
        # the whole figure would understate what an operator carries.
        assert fill >= MIN_FILL_FRACTION, (
            f"blocks only filled to {fill:.1%} of the weight cap; raise "
            f"--outputs-per-tx until they fill, and record the "
            f"values used")

        rows = []
        for key in ("blocks_and_undo",) + LEVELDB_COMPONENTS:
            per_block = (after[key] - before[key]) / FILLED_BLOCKS
            rows.append({"component": key, "bytes_per_block": round(per_block)})
            self.log.info("%s: %.0f bytes per block", key, per_block)

        total = sum(r["bytes_per_block"] for r in rows)
        rows.append({"component": "total", "bytes_per_block": total})
        assert_greater_than(total, 0)

        # The consensus block bytes, independent of any storage accounting.
        # Recorded so the overhead can be stated against the thing the
        # 420 GB/year headline actually counts.
        rows.append({"component": "block_bytes_consensus",
                     "bytes_per_block": round(mean_size)})

        # Each fill transaction spends exactly one input and creates
        # outputs_per_tx, so the net UTXO delta per block is exact rather than
        # inferred. Recorded so chainstate can be divided by it -- the marginal
        # cost of one UTXO is the quantity that lets any other traffic shape be
        # computed from this run.
        net_utxos = txs_per_block * (outputs_per_tx - 1)
        rows.append({"component": "net_utxos_per_block", "bytes_per_block": net_utxos})
        chainstate = next(r["bytes_per_block"] for r in rows
                          if r["component"] == "chainstate")
        rows.append({"component": "chainstate_bytes_per_utxo_x1000",
                     "bytes_per_block": round(1000 * chainstate / net_utxos)})
        self.log.info("MARGINAL: %.1f chainstate bytes per net UTXO (%d per block)",
                      chainstate / net_utxos, net_utxos)

        block_storage = next(r["bytes_per_block"] for r in rows
                             if r["component"] == "blocks_and_undo")
        # A block cannot be stored in less space than the block occupies. An
        # earlier st_blocks-based measurement reported 786,432 against 992,277
        # consensus bytes, which is physically impossible and revealed that the
        # walk was landing mid-preallocation-chunk. This is the assertion that
        # catches that class of error rather than reporting a plausible-looking
        # number that is simply too small.
        assert block_storage >= mean_size, (
            f"block storage {block_storage} is below the consensus block size "
            f"{mean_size:.0f} — the measurement is undercounting, not the node "
            f"compressing")

        self.log.info("OVERHEAD: total %d bytes/block on disk vs %.0f consensus "
                      "block bytes — %.2fx",
                      total, mean_size, total / mean_size)

        outdir = os.path.join(self.config["environment"]["SRCDIR"],
                              "tools", "calibration", "m5-disk-cost")
        os.makedirs(outdir, exist_ok=True)
        path = os.path.join(outdir, f"disk-cost{self.options.tag}.csv")
        with open(path, "w", newline="", encoding="utf-8") as fh:
            writer = csv.DictWriter(fh, fieldnames=FIELDNAMES)
            writer.writeheader()
            writer.writerows(rows)

        with open(path, newline="", encoding="utf-8") as fh:
            written = list(csv.DictReader(fh))
        assert_equal(len(written), len(LEVELDB_COMPONENTS) + 5)
        assert_equal(list(written[0].keys()), FIELDNAMES)
        self.log.info("wrote %s", path)


if __name__ == "__main__":
    CalibrationDiskCost(__file__).main()
