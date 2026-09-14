# M8 — the E28 cycle rate on the card that will actually open the chain

- Date: 2026-08-28
- Git commit: `37de11b1`
- GPU: NVIDIA P104-100, 8 GB, compute capability 6.1, driver 580.178.04
- `GPU_ARCH`: `sm_61`; CUDA 12.0.140
- Harness: `crypto/cuckatoo/gpu/qsgpucalibrate.cu`
- Raw data: `p104-e28-launch.csv` (1300 rows); host detail in `meta.txt`

Every block-time figure in
[`/doc/audit/mainnet-difficulty-floor-model.md`](../../../doc/audit/mainnet-difficulty-floor-model.md)
descends from one anchor measured on a **borrowed** rig
(`0.014935` cycles/s, M7). The standing rule is that capacity present at launch
must be capacity that stays, so the launch fleet was always going to open the
chain on a *different* card from the one the floor was calibrated on. M8 measures
the card that will actually do it.

## What was owed

Two things, and only one of them was a real risk.

The lesser one: the launch card is a P104-100, the same model as the anchor card,
so the floor should transfer. "Should" was doing the work — `P104-100` is not one
part, it ships in 4 GB and 8 GB variants with different memory, and lean Cuckatoo
is memory-bound. The installed card reports a 256-bit bus at 5005 MHz —
**320.3 GB/s**, the 8 GB variant, the same spec class as the anchor.

The greater one: two separate attempts to predict this card's speed from
datasheet bandwidth were both wrong, and the schedule built on them was rewritten
twice. See "The bandwidth ratio does not hold" below.

## Method

`qsgpucalibrate --edgebits=28 --graphs=1300 --device=0`, one process, timing by
CUDA events around `run_solver`. 1300 graphs is M2's per-size sample count,
chosen there to yield ~30 cycle events.

The card was idle: no node, no `qsgpusolve`, nothing else holding a CUDA context.
`--device=0` is the P104; this host has a second GPU (a GTX 1050 Ti on `04:00.0`)
which drives Xorg. A per-minute `nvidia-smi` sampler ran alongside to make a
thermal or clock artefact visible rather than silently averaged into the result.

**The run exited 0.** `qsgpucalibrate` returns 5 on a mid-run device fault, and
that distinction is the point: a faulted card still lets the event timers run and
still prints a plausible seconds value for every row, so the exit code, not the
CSV's shape, is what says the file is usable.

## Results

| | value |
| --- | ---: |
| graphs | 1300 |
| mean s/graph | **1.472141** |
| SD | 0.006604 (**0.449%** of mean) |
| min / max | 1.453978 / 1.516698 |
| cycles found | 30 |
| graphs/cycle, this run | 43.33 |

Clocks held P0 at 1860 → 1847 MHz across the 32-minute run (a 0.7% settle end to
end, with no throttle step), 33 → 67 °C, 88–105 W. The 0.449% timing SD is itself the
evidence that nothing throttled.

### Verification gate: PASSED

```
qscalibrate --verify-gpu-csv=p104-e28-launch.csv
  rows=1300 found=30 verified=30 consensus_checked=0 failed=0
```

Every GPU-found cycle re-checked on the **CPU** (`lean.cpp`), a different
implementation family from the finder (`lean.cu`). `consensus_checked=0` is
expected at E28 and is not a skipped check: only E19 and one other size are
checkable by the *consensus* verifier, which is the entire reason `run-m2.sh`
sweeps E19 alongside the sizes it actually cares about.

## Deriving the rate — use the pooled constant, not this run's cycle count

This run's own cycle count is the **wrong** input to quote. 30 events is a Poisson
count with a ±18.3% 1σ; taking `30 / 1913.783 s = 0.015676` and reporting it as a
+5.0% improvement on the anchor would be reading noise as signal.

Each measurement contributes the quantity it measures well. Per-graph time is
measured tightly here (0.449% SD, n=1300). The cycle probability per graph is a
property of the *graph*, not the card, and M2 already pooled it across 451 cycles
over 19,300 graphs: **0.02337 cycles/graph** (42.8 graphs/cycle, 95% CI
[39.2, 47.1]). That is the constant the floor model itself uses — `0.02337 /
1.5648 = 0.014935` reproduces M7's anchor exactly.

```
rate = 0.02337 / 1.472141 = 0.015875 cycles/s
```

This run's own 43.33 graphs/cycle sits inside M2's CI and is 1.3% from the pooled
value, which is the corroboration a 30-event sample can legitimately provide.

## Verdict — the floor transfers to the launch card

| card | E28 s/graph | cycles/s | 4-cycle block time |
| --- | ---: | ---: | ---: |
| P104-100, M7 anchor (rig, borrowed) | 1.5648 | 0.014935 | 267.8 s |
| **P104-100, M8 launch card (`linuxqs1`)** | **1.472141** | **0.015875** | **252.0 s** |

Same card model, same harness, same method, one idle card each: the launch card
is **6.3% faster per graph** than the anchor card. That is ordinary part-to-part
variance, comfortably inside the ±4.7% Poisson uncertainty the anchor's own
constant carries, and it moves the opening block time from **10.7%** under target
(267.8 s) to **16.0%** under target (252.0 s).

Fast remains the safe direction, for the reason M7 gave: the floor exists so one
commodity GPU can start and restart the chain. **Recommendation: leave the floor
at 4 cycles.** No consensus change is indicated and no re-mint is implied.

## The bandwidth ratio does not hold — stop deriving this from datasheets

| | s/graph | ratio vs P104 |
| --- | ---: | ---: |
| P104-100 (M8) | 1.472141 | — |
| GTX 1050 Ti (2026-08-24) | 3.1697 | **2.153×** |

Memory bandwidth predicts **2.857×** (320.3 / 112.1 GB/s). The measured ratio is
**2.153×** — the datasheet estimate is 33% high. Lean Cuckatoo is memory-*bound*
on these parts but not memory-*limited* in proportion to peak bandwidth, so peak
bandwidth is not a usable proxy for solve time across them.

This is the third datasheet-derived speed estimate in this program to be
contradicted by measurement, and each one moved a schedule before it was checked.
Derived rates for the remaining fleet cards, for planning only — these are
`0.02337 / measured s-per-graph`, and the s/graph figures are measured:

| card | s/graph | cycles/s |
| --- | ---: | ---: |
| GTX 1050 Ti | 3.1697 | 0.007373 |
| GTX 950 | 3.4262 | 0.006821 |
| GTX 960 | 4.207628 | 0.005554 |

The GTX 960 was measured after this record was written; see
[M9](../m9-windows-card-rate/README.md). It came in **23% slower per graph than
the GTX 950**, so the planning row above is a measurement, not an estimate.

## Consequence for the arming schedule

The schedule has been re-derived twice, and this measurement moves it back toward
its original 2026-08-23 shape. The intervening "one card opens at ~13 min, so the
fleet needs ~3 cards to reach target spacing" concern was computed when the
fastest card in the fleet was a GTX 1050 Ti. It no longer holds: **one card now
opens 16% fast of the 300 s target, not slow.**

That is a mild condition, not a problem. 252 s against 300 s is well inside a
single retarget step, so it resolves within one 144-block window. Adding the
GTX 950 gives ~0.02270 cycles/s (~176 s/block), and the GTX 960 on `windowsqs2`
takes the full fleet to ~0.02825 cycles/s (~142 s/block); difficulty then rises
off the floor, which is the mechanism working, not a fault.

Every arming card is now measured. [M9](../m9-windows-card-rate/README.md) closed
the last one.
