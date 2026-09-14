# M9 — the E28 cycle rate on the Windows fleet card

- Date: 2026-08-28
- Git commit: `b232adfd` (deployed tree `95f50c63`; the four solver sources hash-match master)
- GPU: NVIDIA GeForce GTX 960, 2 GB, compute capability 5.2, driver 582.66
- `-GpuArch`: `sm_52`; CUDA 12.9.41, MSVC 14.44.35207
- Harness: `crypto/cuckatoo/gpu/qsgpucalibrate.cu`
- Raw data: `gtx960-e28.csv` (1300 rows); host detail in `meta.txt`

[M8](../m8-launch-card-rate/README.md) closed the launch card and ended by naming
the one input still missing: "the remaining open input is the GTX 960 on
`windowsqs2`, which is unmeasured." M9 measures it. It is the last card in the
arming schedule and the only one that mines under Windows, so it is also the only
measurement where the display-driver watchdog is a live hazard rather than a
footnote.

## Method

`qsgpucalibrate --edgebits=28 --graphs=1300 --device=0`, one process, timing by
CUDA events around `run_solver` — the same harness, sample count and pre-image as
M2, M7 and M8, so the rows are directly comparable.

Sole GPU in the box, no node running. Unlike the P104 in M8, **this card also
drives the display**, which is the configuration the box actually mines in.

Two Windows-specific things had to be true before the run was worth starting:

- **`TdrDelay = 60` was already present.** At the measured 4.21 s per graph, the
  Windows default of 2 s would reset the driver inside *every single graph*. This
  card is the reason that registry value is load-bearing rather than advisory.
- **The run had to survive its own launcher.** A process started over ssh is
  killed when the ssh session ends, which is not obvious because the redirection
  targets are still created before it dies. The sweep runs as a scheduled task
  (`LogonType Interactive`, `RunLevel Highest`) — the pattern the box's existing
  `QsBuild`/`QsCtest` tasks already use.

## Results

| | value |
| --- | ---: |
| graphs | 1300 |
| mean s/graph | **4.207628** |
| SD | 0.008832 (**0.210%** of mean) |
| min / max | 4.180422 / 4.261821 |
| cycles found | 30 |
| graphs/cycle, this run | 43.33 |

Quartile means are 4.207113 / 4.207542 / 4.207617 / 4.208241 — a 0.027% drift
across 91 minutes, with no row more than 1.3% above the mean. Clocks held
1354 MHz core / 3004 MHz memory throughout at 53–57 °C and ~58 W against a 130 W
limit, and `clocks_throttle_reasons.active` read `0x0` with every individual
reason "Not Active". Nothing throttled.

Wall time was 5475 s against 5469.9 s of summed graph time: **0.09% of the run
was outside a timed graph**, so the per-graph figure is the whole cost.

### Verification gate: PASSED

```
qscalibrate --verify-gpu-csv=gtx960-e28.csv
  rows=1300 found=30 verified=30 consensus_checked=0 failed=0
```

Every GPU-found cycle re-checked on the **CPU** (`lean.cpp`), a different
implementation family from the finder (`lean.cu`). `consensus_checked=0` is
expected at E28 for the reason M8 gives.

### The exit code was not captured — what stands in for it

`qsgpucalibrate` returns 5 on a mid-run device fault, and that exit code is
normally what certifies the file, because a faulted card still lets the event
timers run and still prints a plausible seconds value for every row. **This run's
exit code was lost to a bug in the launcher**, not in the harness: the batch file
ended with

```bat
echo calibrate_rc=%ERRORLEVEL%> C:\qs\m9\DONE
```

`%ERRORLEVEL%` expands to a digit, and `cmd` parses the resulting `0>` as a
redirect of file descriptor 0 rather than as `echo` followed by `>`. The marker
file is created empty, every time, for every possible exit code. Putting the
redirect first (`> file echo calibrate_rc=%ERRORLEVEL%`) fixes it; that form was
confirmed on the box to capture both `0` and a deliberately provoked `3`.

Since the certifying signal was missing, four independent ones were checked in
its place, and a device fault is excluded by each of them separately:

1. **The file is complete — 1300 of 1300 rows.** The fault path returns
   immediately after writing a partial-count line to stderr, so a fault cannot
   produce a full-length file.
2. **stderr is empty.** The fault path writes the CUDA error, the platform hint
   and the row count to stderr before returning.
3. **No display-driver reset in the run window.** `doc/gpu-solver.md` prescribes
   the System event log as the check that does not depend on the exit code. The
   log holds exactly one `nvlddmkm` error for the whole afternoon, at 15:33:07 —
   90 seconds *before* the sweep started, and caused by the ssh-killed launch
   attempt described above. The measured window 15:34:33 → 17:05:48 is clean.
4. **Cycles were still being found at the end.** A card that faults searches
   stale host memory and finds nothing afterwards. Solutions land at nonces 1273
   and 1280, and both verify on the CPU, so the card was still solving correctly
   at graph 1280 of 1300.

A re-run would not add much: the sweep is deterministic in content — same
pre-image, same nonce range, same 30 cycles at the same nonces — and only the
timing column would differ.

## This card runs at P2, and that is the state a miner occupies

The GTX 960's P0 ceilings are 1455 MHz core and 3505 MHz memory. Under a CUDA
context it sits in **P2**: 1354 MHz core, 3004 MHz memory, or 96.1 GB/s against
112.2 GB/s at P0. That is the ordinary GeForce compute behaviour, not a fault —
nothing was throttling — and it is not adjustable, because the application-clock
controls that would raise it are not exposed on GeForce parts.

It is also the correct state to measure. A miner holds a CUDA context
continuously, so P2 is where it lives. Note the contrast with M8, where the P104
held **P0** (1860 → 1847 MHz) for its whole run: each card was measured in the
state it actually mines in, which is why the two are comparable as *mining* rates
even though their P-states differ.

**An unreproduced faster reading is not used here.** A 3-graph smoke test taken
before the sweep read 3.19 s/graph. Nothing since has reproduced it: the
1300-graph sweep, a 6-graph interactive re-run and a 3-graph re-run using the
smoke's exact invocation all read 4.19–4.21, for 1309 timed graphs against 3.
The 3-graph reading was taken before the `nvlddmkm` engine reset at 15:33:07 and
no clock, thermal or power state observed since is capable of producing it. It is
recorded here rather than dropped, but the reproducible number is the one used,
and it is also the *slower* of the two — this does not flatter the schedule.

## Deriving the rate

As in M8, per-graph time comes from this run and the cycle probability comes from
M2's pooled constant (**0.02337 cycles/graph**, 451 cycles over 19,300 graphs),
because 30 events carry a ±18.3% 1σ and would be read as signal if quoted alone.

```
rate = 0.02337 / 4.207628 = 0.0055542 cycles/s
```

This run's own 43.33 graphs/cycle is 1.3% from the pooled 42.8 and sits inside its
95% CI [39.2, 47.1] — the corroboration a 30-event sample can legitimately give,
and, coincidentally, the same 43.33 M8 saw.

## The bandwidth ratio fails a third time — in the opposite direction

| pair | bandwidth ratio | measured s/graph ratio | error |
| --- | ---: | ---: | ---: |
| P104-100 vs GTX 1050 Ti (M8) | 2.857× | 2.153× | datasheet **33% high** |
| P104-100 vs GTX 960 | 3.333× | 2.858× | datasheet **17% high** |
| GTX 960 vs GTX 1050 Ti | 1.166× | 1.327× | datasheet **12% low** |

The first two overpredict and the third underpredicts, so this is not even a
consistent bias that could be corrected with a fudge factor. Peak memory
bandwidth is not a usable proxy for Cuckatoo solve time across these parts.
**Measure the card.**

One caveat on that third row, stated because it would otherwise be invisible: the
GTX 960 figure is a P2 bandwidth and the GTX 1050 Ti's P-state during its
2026-08-24 measurement was not recorded. The comparison is sound as a *rate*
comparison either way — both cards were measured mining — but the bandwidth
column for the 1050 Ti may be the wrong number for that card's actual state.

## The fleet, complete

Every card that can mine at launch is now measured. Rates are
`0.02337 / measured s-per-graph`.

| card | host | s/graph | cycles/s | 4-cycle block, alone |
| --- | --- | ---: | ---: | ---: |
| P104-100 | `linuxqs1` (GPU0) | 1.472141 | 0.0158748 | 252.0 s |
| GTX 950 | `linuxqs2` | 3.4262 | 0.0068210 | 586.4 s |
| **GTX 960** | **`windowsqs2`** | **4.207628** | **0.0055542** | **720.2 s** |
| GTX 1050 Ti | `linuxqs1` (GPU1) | 3.1697 | 0.0073729 | 542.5 s |

The GTX 1050 Ti is listed for completeness but is **not** an arming card: it
drives Xorg on `linuxqs1` while the P104 mines beside it. The GTX 960 in
`windowsqs1` is likewise excluded — that box is a node and ctest host, not a
miner.

## Consequence for the arming schedule

| armed cards | combined cycles/s | block time | vs 300 s target |
| --- | ---: | ---: | ---: |
| P104 alone | 0.0158748 | 252.0 s | −16.0% |
| P104 + GTX 950 | 0.0226958 | 176.2 s | −41.3% |
| P104 + GTX 950 + GTX 960 | 0.0282500 | 141.6 s | −52.8% |

M8 estimated the three-card case at "near 135 s if the GTX 960 matches the 950".
It does not match the 950 — it is 23% slower per graph — so the measured figure
is **141.6 s**, 5% above that estimate. Small, and in the safe direction, but it
makes four for four: every card-speed figure in this program that was estimated
rather than measured has been contradicted by the measurement.

Nothing here changes the recommendation. The floor exists so that one commodity
GPU can start and restart the chain; the full fleet opening at 141.6 s is
difficulty rising off the floor within the first retarget window, which is the
mechanism working. **Leave the floor at 4 cycles.** Adding the GTX 960 last
keeps the opening spacing closest to target for longest, if a staggered arming
order is wanted.
