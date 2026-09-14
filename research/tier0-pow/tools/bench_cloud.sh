#!/usr/bin/env bash
# Cloud Volta+ mean-solver benchmark — closes the mainnet-launch "mean-r" gate.
#
# Runs on a rented sm_70+ GPU (V100/A100/A10/3090/4090), where Tromp's mean
# cuckatoo solver actually works (it produces corrupt graphs on the rig's Pascal
# P104 — memory `mean-solver-pascal-incompat`). Builds lean+mean solvers, gates
# each run against the Pascal-corruption signature, sweeps per-graph solve time
# at E29/E31, and analyzes the E29:E31 ratio r + the SAME-CARD lean->mean
# discount against the shipped mint-safety constant (K=106, C=57143).
#
# Usage (from the cuckatoo src dir on the instance):
#   cd research/tier0-pow/third_party/cuckoo/src/cuckatoo
#   ARCH=sm_89 GPU=0 bash /path/to/bench_cloud.sh
#
# Env knobs (with defaults):
#   ARCH   sm_89   card arch: sm_70 V100 | sm_80 A100 | sm_86 A10/3090 | sm_89 4090
#   GPU    0       device index
#   E31CFG part0   part0 (>=16GB, single-pass) | part1 (8-12GB, 2-pass, halved mem)
#   RANGE  600     nonces swept for mean E29/E31 (~RANGE/42 cycles each)
#   LRANGE 200     nonces swept for lean E29 (lean is ~tens of x slower/graph)
#   LR31   40      nonces swept for lean E31 (slowest solve; min is very tight)
#   OUT    ~/qs_meanr   output dir (binaries, logs, combined.csv)
#
# Four sweeps (mean+lean at E29+E31) so the analyzer gets mean_r directly AND the
# discount-uniformity check that explains any mean_r vs lean_r shift.
set -euo pipefail

ARCH="${ARCH:-sm_89}"
GPU="${GPU:-0}"
E31CFG="${E31CFG:-part0}"
RANGE="${RANGE:-600}"
LRANGE="${LRANGE:-200}"
LR31="${LR31:-40}"
OUT="${OUT:-$HOME/qs_meanr}"
SRC="${SRC:-$PWD}"                 # dir with mean.cu / lean.cu (…/src/cuckatoo)
BLAKE="../crypto/blake2b-ref.c"
NVCC="nvcc -std=c++11"
# parser lives next to this script; resolve regardless of CWD.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PARSE="python3 $HERE/parse_solver_timing.py"
CSV="$OUT/combined.csv"

cd "$SRC"
mkdir -p "$OUT"
rm -f "$CSV"

echo "== GPU identity (record this in the report) =="
nvidia-smi --query-gpu=name,memory.total,compute_cap --format=csv,noheader | tee "$OUT/gpu.txt"
echo "== arch=$ARCH e31cfg=$E31CFG range=$RANGE lrange=$LRANGE =="

# --- Build (arch override is mandatory: the Makefile targets hardcode sm_35,
#     which CUDA 12 removed). Standalone native-benchmark binaries.
#
# BUCKBITS=11 for E29 is MANDATORY, not tuning. mean.cu hardcodes `#define
# BUCKBITS 12`, correct only for E31: the Seed kernel packs tmp[NB][FLUSHA-1]
# then counters[NB] into ONE dynamic-shared block sized BITMAPBYTES, so it needs
# NB*FLUSHA*4 = 2^(BUCKBITS+4) <= BITMAPBYTES = 2^(EDGEBITS-BUCKBITS-PART_BITS-3),
# i.e. BUCKBITS <= (EDGEBITS-PART_BITS-7)/2  => 12 for E31 part0, but only 11 for
# E29 part0. BUCKBITS=12 at E29 overruns shared mem by 48KB (counters start at
# 0xc000). It's silent UB: benign on Pascal/CUDA12 (poisoned run2's mean r), but
# Ada/CUDA13 corrupts every graph -> 0 cycles. Verified: sanitizer clean + mean
# recovers the exact cycle lean finds. (Production per-tx PoW uses lean, not mean;
# this only affects calibration correctness.)
echo "== build =="
$NVCC -o "$OUT/cuda29"  -DBUCKBITS=11 -DEDGEBITS=29 -arch "$ARCH" mean.cu "$BLAKE"
if [ "$E31CFG" = part0 ]; then
  $NVCC -o "$OUT/cuda31" -DNEPS_A=133 -DNEPS_B=85 -DPART_BITS=0 -DEDGEBITS=31 -arch "$ARCH" mean.cu "$BLAKE"
else
  $NVCC -o "$OUT/cuda31" -DFLUSHA=2 -DNEPS_A=135 -DNEPS_B=88 -DPART_BITS=1 -DEDGEBITS=31 -arch "$ARCH" mean.cu "$BLAKE"
fi
$NVCC -o "$OUT/lcuda29" -DEDGEBITS=29 -arch "$ARCH" lean.cu "$BLAKE"
$NVCC -o "$OUT/lcuda31" -DEDGEBITS=31 -arch "$ARCH" lean.cu "$BLAKE"
echo "built: $(ls -1 "$OUT"/cuda29 "$OUT"/cuda31 "$OUT"/lcuda29 "$OUT"/lcuda31)"

# sweep <binary> <edgebits> <solver> <range> : run, gate (aborts on corruption
# via set -e), append healthy rows to the combined CSV.
sweep() {
  local bin="$1" eb="$2" solver="$3" range="$4"
  local log="$OUT/${solver}_e${eb}.log"
  echo "== sweep $solver E$eb : $range graphs on gpu $GPU =="
  "$OUT/$bin" -n 0 -r "$range" -d "$GPU" > "$log" 2>&1 || {
    echo "!! solver exited non-zero — tail:"; tail -5 "$log"; exit 1; }
  # --gate exits 1 on the Pascal-corruption signature: nothing is recorded and
  # the whole run stops before a degraded GPU can poison the constant.
  $PARSE "$log" --edgebits "$eb" --solver "$solver" --gpu "$GPU" --gate --to-csv "$CSV"
}

# E29 mean first: cheapest sweep AND the corruption tripwire — if this GPU is
# secretly Pascal-like, we abort here before spending time on E31/lean.
sweep cuda29 29 mean  "$RANGE"
sweep cuda31 31 mean  "$RANGE"
sweep lcuda29 29 lean "$LRANGE"
sweep lcuda31 31 lean "$LR31"

echo
echo "== analysis =="
python3 "$HERE/analyze_meanr.py" "$CSV"
echo
echo "combined CSV: $CSV   (rsync back to research/tier0-pow/tools/calibration/run3/ to commit)"
