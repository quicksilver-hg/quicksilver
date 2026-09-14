# M1 — CPU time per graph, edge bits 22-29

- Date: 2026-08-01
- Git commit: `6171d69`
- CPU: Intel(R) Xeon(R) CPU E5-1620 v2 @ 3.70GHz — 4 cores / 8 threads
- L3 cache: 10 MiB
- RAM: 46 GiB total (`dmidecode` not run; needs root)
- Machine otherwise idle: **no** — a desktop session (cinnamon, Xorg) and two
  agent processes were running throughout. Load average 2.4–4.2.
- Command: `tools/calibration/run-m1.sh build/bin/qscalibrate 8`
- Produced by: `src/crypto/cuckatoo/bench/qscalibrate.cpp`

## Scope: 8 threads only

The plan called for passes at 4 and 8 threads. Only the **8-thread** pass was
run, because it is the configuration that reproduces the recorded anchor. See
the anchor investigation below.

## Anchor check

`doc/design/chain-growth.md` records, for E29 on an eight-thread desktop with the
machine otherwise idle:

| Threads | Recorded | Measured here | Delta |
| --- | ---: | ---: | ---: |
| 4 | 58.83 s (100-graph mean; table says 61.5 s) | 74.1 s | **+26%** |
| 8 | 52.4 s | 49.1 s | −6% |

**The 8-thread anchor reproduces. The 4-thread anchor does not, and the cause is
unresolved.**

### What was ruled out

- **Stray processes.** No orphaned `quicksilverd` or test runners were alive.
- **Contention.** Contention would inflate both configurations. The 8-thread
  figure is 6% *faster* than recorded, so the machine and build are healthy.
- **Hyperthread scheduling.** With 4 threads on 8 logical CPUs the scheduler can
  stack two threads on one physical core's siblings. Pinning with
  `taskset -c 0,1,2,3` (CPUs 0-3 are distinct physical cores on this part)
  changed nothing: 74.5 s pinned against 74.1 s unpinned.
- **Thread starvation.** A 2-graph 4-thread run consumed 604 s of user time in
  153 s of wall time — 3.95 CPUs. The threads run flat out.

### What remains unexplained

The 4-thread configuration is reproducibly ~26% slower than recorded while the
8-thread configuration is slightly faster. Candidates not investigated: memory
channel population on the machine used for the original figure, turbo behaviour
under partial load, or an error in the recorded value. Note the source document
is internally inconsistent about this number — its table says 61.5 s and its text
says a 58.83 s mean over 100 graphs.

### The 4→8 thread gain is much larger than the spec claims

`doc/design/chain-growth.md` states that "doubling threads bought 17%" and
concludes from that the solver is memory-bandwidth-bound, so more cores will not
help. Measured here, the gain is far larger and consistent across graph sizes:

| Edge bits | 4 threads | 8 threads | 4→8 gain |
| --- | ---: | ---: | ---: |
| 22 | 0.2856 s | 0.2059 s | 1.39x |
| 25 | 2.1562 s | 1.6013 s | 1.35x |
| 29 | 74.1 s | 49.1 s | 1.51x |

**This weakens the "more cores will not fix it" inference**, which rests on the
same 4-thread baseline that failed to reproduce. It does not overturn the
conclusion — 1.5x is still not the order of magnitude that would change
viability — but the 17% figure should not be cited as measured.

Rewriting that document is Stage 2 work; this record exists so the discrepancy is
not lost.

## Consequences for the sweep

The 4→8 ratio is uniform across sizes (1.35–1.51x), so **relative scaling across
edge bits is preserved** regardless of which configuration is used. The sweep is
internally valid. Only the absolute level is in question, and criterion 5 —
honest sender grind cost — is the conclusion that depends on it.

## Results

Run 2026-07-31T22:19 to 2026-08-01T04:12 (5 h 53 m). Per-graph rows are in
`e{22..29}-t8.csv` with the schema `edgebits,nonce,seconds,found,threads`.

| Edge bits | Graphs | Mean s | Median s | Min s | Max s | SD | x prev |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 22 | 2000 | 0.2090 | 0.1932 | 0.1777 | 0.3764 | 0.0350 | — |
| 23 | 2000 | 0.3732 | 0.3715 | 0.3502 | 0.4408 | 0.0100 | 1.785 |
| 24 | 2000 | 0.7387 | 0.7381 | 0.7022 | 0.8341 | 0.0140 | 1.980 |
| 25 | 500 | 1.4596 | 1.4581 | 1.4047 | 1.5780 | 0.0153 | 1.976 |
| 26 | 500 | 3.2192 | 3.1783 | 3.0853 | 5.7175 | 0.2560 | 2.206 |
| 27 | 200 | 9.0369 | 9.0272 | 8.9459 | 10.0677 | 0.0852 | 2.807 |
| 28 | 200 | 22.1299 | 22.1308 | 21.9677 | 22.3163 | 0.0672 | 2.449 |
| 29 | 200 | 49.6925 | 49.1642 | 48.8536 | 69.1754 | 2.3257 | 2.245 |

Distributions are tight; the maxima at E26 and E29 are desktop contention, not
solver behaviour. Trimming 10% from each tail moves E29 from 49.6925 s to
49.1641 s and no other size by more than 0.4%, so the means below are used as-is.

### Anchor check: PASSED

E29 at 8 threads measured **49.69 s** against the recorded 52.4 s — **5.2% faster**.
This confirms the pre-sweep spot check (49.1 s over 2 graphs) at n=200. The plan's
gate for Task 6 is met for the configuration that was run. The 4-thread anchor
remains unreproduced and unexplained, as recorded above.

### The cycle rate does not depend on graph size

174 cycles found in 7600 graphs — pooled rate **0.02289**, 95% CI [0.01953,
0.02626]. Per-size rates range 0.0140 to 0.0300, but a chi-square test against
the pooled rate gives **4.03 on 7 dof** (critical value 14.07 at p=0.05): the
variation is entirely consistent with sampling noise.

This matters for the flag day. It means the expected number of graphs per 42-cycle
is a constant **43.7** [38.1, 51.2] regardless of `nTxEdgeBits`, so grind cost
scales with time-per-graph alone and the size choice can be made on the timing
table without re-measuring the cycle rate at each candidate.

### Expected honest grind cost per transaction

Seconds per graph divided by the pooled cycle rate, at 8 CPU threads:

| Edge bits | Grind s | Grind min |
| --- | ---: | ---: |
| 22 | 9.1 | 0.2 |
| 23 | 16.3 | 0.3 |
| 24 | 32.3 | 0.5 |
| 25 | 63.8 | 1.1 |
| 26 | 140.6 | 2.3 |
| 27 | 394.7 | 6.6 |
| 28 | 966.6 | 16.1 |
| 29 | 2170.5 | 36.2 |

E29 at 36.2 min sits inside the spec's recorded 33-41 min band, which is a second
independent confirmation that this machine reproduces the reference conditions.

Note these are *CPU* costs. Criterion 5 is about the honest sender, who is
expected to have a GPU; converting this table into the criterion-5 verdict needs
the M3 GPU timings, which are deferred.

### Scaling is superlinear, and the knee is where the working set leaves L3

Each solver instance holds two bitmaps of 2^EDGEBITS bits — `shrinkingset::bmap`
over `NEDGES` (lean.hpp:74) and `cuckoo_ctx::nonleaf` over `NNODES1 >> PART_BITS`
with `PART_BITS 0` (lean.hpp:42,117) — and `NEDGES == NNODES1 == 1ULL << EDGEBITS`
(cuckatoo.h:59,65). So the working set is 2^EDGEBITS / 4 bytes:

| Edge bits | Working set | Fits 10 MiB L3 | x prev |
| --- | ---: | :---: | ---: |
| 23 | 2 MB | yes | 1.785 |
| 24 | 4 MB | yes | 1.980 |
| 25 | 8 MB | yes | 1.976 |
| 26 | 16 MB | **no** | 2.206 |
| 27 | 32 MB | no | 2.807 |
| 28 | 64 MB | no | 2.449 |
| 29 | 128 MB | no | 2.245 |

Doubling the graph doubles the work, so a compute-bound solver would show a ratio
of 2.0 throughout. Below the cache boundary it does (1.98, 1.98). Above it the
ratio rises to 2.21-2.81, averaging 2.43 — the extra 20% per edge bit is DRAM
traffic that the cache used to absorb.

**This independently confirms the spec's memory-bandwidth conclusion** on evidence
that does not depend on the 4-thread baseline that failed to reproduce. The
"doubling threads bought 17%" figure should still be dropped (see above), but the
inference it was used to support survives, and now rests on a boundary that can be
predicted from the allocation sizes rather than on a single timing pair.

Practical consequence for the flag day: the cost curve is steeper than 2x per edge
bit in exactly the region the candidates live in, so an `nTxEdgeBits` decision made
by interpolating a 2x rule would understate cost. Use the measured table.
