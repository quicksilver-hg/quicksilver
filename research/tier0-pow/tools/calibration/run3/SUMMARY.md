# mean-r Volta+ gate — calibration campaign run3 (2026-07-08)

## Root cause fixed (durable)
mean.cu hardcodes `#define BUCKBITS 12`, correct only for E31. The Seed kernel
packs tmp[NB][FLUSHA-1]+counters[NB] into one dynamic-shared block sized
BITMAPBYTES, requiring BUCKBITS <= (EDGEBITS - PART_BITS - 5 - log2(FLUSHA))/2.
=> E29 part0 (FLUSHA=4) needs BUCKBITS<=11; 12 overran shared mem by 48KB.
Silent UB: benign-ish on Pascal/CUDA12 (produced 0 cycles = "pascal-incompat"),
corrupts every graph on Ada/CUDA13. Fix: build E29 mean with -DBUCKBITS=11
(bench_cloud.sh). Verified: compute-sanitizer clean + mean recovers the exact
cycle lean finds, on BOTH Ada (L40S) and Pascal (P102). "mean-solver-pascal-
incompat" was this bug all along, NOT a hardware limitation.

Production per-tx PoW uses the LEAN solver, so consensus was never affected;
this bug only corrupted CALIBRATION. Run2's r=0.182 -> K=80 is VOID (corrupt).

## Valid results (post-fix)
L40S (Ada sm_89, CUDA13, E31 part0 single-pass), 3 runs:
  run1 (n0=0):        mean_r 0.366 med / 0.369 min
  run2 (n0=0):        mean_r 0.361 med / 0.367 min
  run3 (n0=5,000,000, independent graphs): mean_r 0.363 med / 0.369 min
  -> mean_r = 0.363 +/- 0.003, VERY stable across timing jitter AND graph set.
  lean_r on L40S = 0.101 (vs Pascal rig 0.243 -> lean ratio is hardware-variable).
  discount uniformity 0.27-0.28 => mean favors E31 (block PoW), NOT E29 (per-tx);
  the safe direction. No E29-specific attacker advantage.

P102 (Pascal sm_61, 10GB): E29 mean part0 healthy (median 181ms, valid cycles).
  E31 part0 needs ~16GB (OOM); E31 part1 requires NEPS below correctness floor
  (overflow, 0 cycles) -> 10GB cannot produce a valid E31 => no valid mean_r.
  Value: confirms the BUCKBITS fix works on Pascal.

## Verdict
mean_r 0.363 >> 0.25. Shipped K=106 carries ~6.0x mint-safety margin (design 4.0x).
VERDICT: MEASURED-AND-HELD. K to hit exactly 4x = ~159.

## Cross-architecture E29 (per-tx-PoW) record — 3 independent-nonce passes each
mean_r is L40S-ONLY (E31 needs ~10-20GB global buffers; 8GB & 10GB cards OOM/overflow
under ALL pass configs — empirically confirmed). Small cards give the E29 axis only.
BUCKBITS=11 fix verified correct (same cycle as lean) on ALL THREE architectures.

  Arch       Card       mean E29 (median, 3 passes)   lean E29    lean/mean discount
  Ada        L40S       99 / 99 / 99 ms               449 ms      4.5x
  Pascal     P102-100   183 / 185 / 185 ms            2330 ms     12.7x
  Blackwell  5060 Ti    262 / 267 / 267 ms            3284 ms     12.3x

Finding: mean E29 timing is per-arch very stable across independent nonce sets, but
the lean->mean discount varies ~3x across arches (4.5x Ada vs ~12-13x Pascal/Blackwell).
=> Solver behavior is demonstrably NOT arch-invariant; since mean_r could vary by arch
   the same way and we can only measure it on Ada, the ~6x margin at K=106 is the
   prudent hedge vs retuning to exactly 4x (K=159) on single-arch data.
