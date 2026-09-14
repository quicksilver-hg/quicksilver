# M5 — real on-disk cost per block

- Date: 2026-08-01
- Git commit: `681c51e`
- Machine: Intel(R) Xeon(R) CPU E5-1620 v2 @ 3.70GHz (development desktop)
- Command: `cd build/test/functional && ./calibration_disk_cost.py`
- Network: sandbox, `-txpownocycle=1`
- Produced by: `test/functional/calibration_disk_cost.py`
- Fill: 64 blocks at 99.2% of the weight cap, 11 transactions per block, 2,300
  spendable outputs each (396,555 weight, just under the 400,000 standard cap)

## Result

| component | bytes/block |
| --- | ---: |
| blocks_and_undo | 992,732 |
| block_index | 320 |
| chainstate | 3,061,952 |
| **total** | **4,055,004** |
| block_bytes_consensus | 992,277 |

Total on-disk cost is **4.09x** the consensus block bytes.

## Finding 1 — the 420 GB/year headline is right by coincidence

At 300-second spacing (288 blocks/day):

- Measured total: 4,055,004 x 288 x 365 = **426 GB/year**
- The spec's headline: 4 MB blocks x 288 x 365 = 420 GB/year

These agree to within 1.5%, and **both figures are built from wrong parts**:

- The headline assumed 4 MB of block bytes. Real weight-full blocks of these
  transactions are ~0.99 MB — about 4x smaller (see M4 finding 3).
- The headline counted no chainstate at all. Chainstate is 3.06 MB/block here,
  which is almost exactly the shortfall.

So the number should not be treated as validated. It was arrived at by counting
the wrong thing and happening to land near the right total.

## Finding 2 — the binding storage cost is the UTXO set, not block bytes

Chainstate is **75% of total on-disk cost**; blocks and undo together are 24%,
and the block index is negligible at 320 bytes per block.

This matters for the flag day. The size term the spec proposes for
`RequiredTxWork` prices transaction *bytes*. It does not price UTXO creation at
all — and UTXO creation is three quarters of what an operator actually stores.
Two transactions of identical size can differ enormously in what they cost a node
to keep forever, depending on how many outputs they leave behind.

**Recommendation for Stage 2: consider an output-count or UTXO-delta term
alongside the size term.** A size term alone does not reach the dominant cost.

## Caveats — read before using the chainstate number

The chainstate figure is an **adversarial worst case, not typical traffic**:

- Every fill transaction spends 1 input and creates 2,300 outputs, so the UTXO
  set grows by ~25,300 entries per block and nothing is ever spent back.
- Real traffic roughly balances outputs created against outputs consumed, so
  steady-state chainstate growth is far lower.
- `undo` is correspondingly near zero: with one input spent per transaction there
  is almost nothing to record for a rollback. Under balanced traffic undo would
  be larger and chainstate smaller.

Honest-usage growth is therefore much closer to the blocks-and-undo figure alone:
992,732 x 288 x 365 = **104 GB/year**, comfortably under criterion 2's 200
GB/year ceiling. **The two bounds — 104 GB/year honest, 426 GB/year adversarial —
straddle that ceiling**, so which side of criterion 2 the design lands on depends
entirely on whether UTXO growth is priced.

## Measurement notes

Two instrument errors were found and fixed here; both are recorded because they
would recur in any similar measurement:

1. **`blk*.dat` and `rev*.dat` are preallocated in 16 MB / 1 MB chunks, with the
   space really reserved rather than sparse.** Neither `st_size` nor `st_blocks`
   moves while a block lands inside an existing chunk. A 10-block window reported
   a per-block cost of exactly zero; a 64-block window reported 786,432 bytes
   against 992,277 consensus bytes — still 21% low, because the window ends
   mid-chunk. Block storage is now taken from the node's own `size_on_disk`,
   which tracks bytes used, and the script asserts it is at least the consensus
   block size. That inequality is what catches this class of error; a `> 0` check
   passes on a number that is merely too small.

2. **`MiniVault.get_utxo` re-sorts its entire UTXO list on every call**
   (`vault.py:265`) and then does a linear `.index()`. Filling blocks with
   many-output transactions makes the harness quadratic: an earlier version grew
   the tracked list toward ~1.5M entries and had not finished after 26 minutes.
   The fill loop now drains the vault's UTXOs into a plain list once, spends
   from it explicitly, and calls `node.sendrawtransaction` directly rather than
   `vault.sendrawtransaction`, which would scan the new outputs back in. Runtime
   dropped to 105 seconds.

`target_vsize` padding was rejected as a fill mechanism: it pads with one large
bare `OP_RETURN`, which relay refuses (`scriptpubkey`, -26) and which — being
provably unspendable — never enters the UTXO set, so it would have reported a
chainstate cost near zero.

---

## Revision 2026-08-01: the per-block cost is a function of traffic shape

The original run used one shape — 2,300 spendable outputs per transaction — and
reported a single figure of 4,055,004 bytes per block. That figure is correct for
that shape and badly misleading as "the" cost of a full block.

Two further shapes were measured at the same 4 MB weight cap, all filling blocks
to 99%+ of the cap:

| Shape | tx/block | tx/s @300s | blocks+undo | chainstate | total | GB/year |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Honest, 2 outputs | 3,099 | 10.33 | 1,217,399 | 244,224 | 1,445,751 | **152.0** |
| Mid, 200 outputs | 114 | 0.38 | 998,125 | 2,960,064 | 3,922,349 | 412.2 |
| Adversarial, 2,300 outputs | 11 | 0.04 | 992,732 | 3,061,952 | 4,055,004 | 426.3 |

**Honest traffic at the current 4 MB cap and 300 s spacing costs 152 GB/year**,
which is inside the 200 GB/year ceiling. The 426 GB/year figure is entirely UTXO
bloat, not block size.

### The marginal cost of a UTXO is not a constant

Chainstate divided by net UTXOs created:

| Shape | net UTXOs/block | chainstate | bytes per UTXO |
| --- | ---: | ---: | ---: |
| 2 outputs | 3,099 | 244,224 | **78.8** |
| 200 outputs | 22,686 | 2,960,064 | **130.5** |
| 2,300 outputs | 25,289 | 3,061,952 | **121.1** |

A 1.65× spread. **UTXO cost is superlinear in UTXO density** — writing 23,000
entries per block costs more per entry than writing 3,000, presumably LevelDB
compaction depth under sustained write bursts. That strengthens the case for
pricing UTXO creation: bloat is worse than proportionally bad.

It also means a single measured shape cannot be extrapolated to another by
multiplying a per-UTXO constant. An earlier attempt to do exactly that predicted
149.9 GB/year for honest traffic against the 152.0 measured here — right to 1.4%,
but only by luck: it overstated the per-UTXO cost by 1.54× and understated block
bytes by 1.23×, and the two errors cancelled. **Measure the shape; do not scale
one.**

### Honest blocks hold MORE bytes than adversarial ones

blocks+undo is 1,217,399 for the honest shape against 992,732 for the
adversarial one, and serialized block size is 1,088,983 against 992,277 — despite
identical weight. Weight per byte is 3.67 for two-output transactions and 3.99
for 2,300-output ones, because outputs are non-witness data charged at 4× while
signatures are witness data charged at 1×.

**Weight is not bytes, and the ratio is attacker-controlled.** The measured range
is narrow (3.67–3.99) but the theoretical floor is 1.0: an all-witness
transaction would put 4 MB of serialized bytes into a 4 MB weight block, roughly
4× the honest byte cost, while paying the same weight-denominated work. A size
term in `RequiredTxWork` that prices *weight* does not close that. Stage 2 should
consider pricing serialized bytes, or bounding the weight-to-byte ratio.

### Method note

Output-light shapes need one mature coinbase per fill transaction unless outputs
are recycled, which would need ~57,000 setup blocks at two outputs each. The
`--recycle` flag feeds each transaction's outputs back into a **FIFO** pool.
FIFO is load-bearing: spending from the back makes each transaction spend what
the previous one just created, building an unconfirmed chain thousands deep
against the 25-ancestor relaypool limit. Taking from the front means a recycled
output is not reached until a later block, by which point it is confirmed.

`block_index` came back negative in both new runs (−15,872 and −35,840 bytes per
block): LevelDB compacted the index during the measurement window. It is
negligible either way against chainstate, but it is a reminder that these
LevelDB deltas carry compaction noise of a few tens of kilobytes per block.
