# SP1b — Rig E29 GPU Parity & Watchdog Calibration

**Date:** 2026-07-13
**Rig:** P104-100 calibration rig
**Purpose:** Prove a genuine GPU-produced edgebits-29 Cuckatoo cycle verifies under
the node's consensus `CuckatooVerify`, and calibrate the no-progress watchdog
default (`no_progress_timeout_sec` in `src/crypto/cuckatoo/gpu_solver.cpp`)
from the measured `qsgpusolve` progress cadence.

## Hardware / toolchain

| Item | Value |
|---|---|
| GPU | NVIDIA P104-100 (Pascal, sm_61), 8192 MiB, ×7 (single card used: `CUDA_VISIBLE_DEVICES=0`) |
| Driver | 580.142 |
| CUDA / nvcc | 12.0 (V12.0.140) |
| Solver build | `make` in `cuckatoo/gpu` → `nvcc -std=c++11 -DEDGEBITS=29 -arch sm_61` |
| Node test binary | `test_quicksilver` built on the dev host (glibc 2.39, Ubuntu 24.04 ABI), run on the rig with `libevent*.so.7` shipped in `LD_LIBRARY_PATH` (rig has no cmake) |

The `qsgpusolve` built here carries the SP1b Task 2 heartbeat (`progress=<nonce>`).

## Parity gate (the consensus proof)

Command (rig):

```
LD_LIBRARY_PATH=/home/worker/qs_libs \
CUCKATOO_GPU_SOLVER=/home/worker/qs_calib/cuckatoo/gpu/qsgpusolve \
test_quicksilver --run_test=gpu_parity_tests --log_level=test_suite
```

Result: **PASS** — `gpu_parity_tests/verifies` ran the real solver through the
SP1b watchdog bridge (`cuckatoo::GpuSolveBytes`), reconstructed SipHash keys from
the winning nonce, and:

- `CuckatooVerify(cyc, keys, 29)` → **true** (genuine GPU E29 cycle accepted by consensus)
- tampered cycle (`bad[PROOFSIZE-1] += 2`) → **false** (rejected)

Testing time ≈ 9.6 s (solution found near the start of the sweep). This exercises
the full untrusted-GPU contract end to end: fork/exec + `poll()` watchdog →
`progress=`-tolerant parse → key reconstruction → consensus verify.

Raw solve line (`raw/parity-preimage-solve.log`, preimage `p[i]=0xA0+(i&0x0f)`, 96 B):
solution at `nonce=2`, full 42-index `cycle=…` printed and verified.

## Throughput & progress cadence

Measured by running `qsgpusolve 29 <ff*96> 0 100` and timestamping each stdout
line (`raw/heartbeat-cadence.log`, 13 consecutive `progress=` intervals):

| Metric | Value |
|---|---|
| Per-attempt E29 solve time (= inter-heartbeat gap) | mean **3.395 s**, min 3.369 s, **max 3.435 s** |
| Throughput (single P104-100) | ≈ **0.295 attempts/s/card** |

Key observation: `qsgpusolve` checks the `now != last_beat` heartbeat condition at
the **top of each grind iteration**, and a single E29 `run_solver` call takes
~3.4 s, so exactly one heartbeat is emitted per attempt. The **max no-progress gap
under healthy operation is therefore ~3.44 s** — this is the true silence window
the watchdog must tolerate, since a genuine hang means one `run_solver` call never
returns (no heartbeat during that call).

## Chosen watchdog default

`no_progress_timeout_sec()` default set to **30 s** (was placeholder 60).

Rationale: ~8.7× the measured 3.44 s worst-case cadence. Generous enough to absorb
thermal throttling, 7-card contention, and slower single attempts on larger graphs
(E31 block PoW, not measured here — re-measure if GPU block mining is deployed on
bigger cards), while still bounding a genuinely wedged solver to 30 s — negligible
against 5-minute block spacing. Errs deliberately against false-killing a solver
that is about to find a block. Runtime override: `-cuckatoosolvertimeout=<sec>`
(sets `CUCKATOO_GPU_TIMEOUT`).

## Reproduction notes

- Rig is internet-isolated; source synced via `rsync`, `test_quicksilver` binary
  copied from the dev host (identical glibc/ABI), `libevent*.so.7` shipped
  alongside because the rig lacks them and has no cmake to build the node.
- Health check first: `dmesg | grep NVRM` (this rig had a recurring
  `RmInitAdapter` fault); clean on this run.
