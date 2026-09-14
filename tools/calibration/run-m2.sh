#!/usr/bin/env bash
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# M2/M3: GPU cycle rate and GPU time per graph, edge bits 22-29 (plus 19).
#
# Runs on the GPU box. Binaries are built elsewhere -- see the M2 README for why
# the compile host and the run host are allowed to differ.
#
# 1300 graphs per size gives ~30 cycle events at the measured rate of one per
# ~43.7 graphs, and pooled across sizes it is a far tighter rate estimate than
# M1 managed on CPU.
#
# E19 is not part of the sweep. It is run because it is one of only two sizes
# where the CONSENSUS verifier can check a GPU-produced cycle.
# Locale-independent numeric formatting in the recorded output.
export LC_ALL=C

set -euo pipefail

BINDIR="${1:-$HOME/qscal}"
OUT="${2:-$HOME/qscal/out}"
GRAPHS="${3:-1300}"
mkdir -p "$OUT"

for bits in 19 22 23 24 25 26 27 28 29; do
  bin="$BINDIR/qsgpucalibrate-e${bits}"
  if [ ! -x "$bin" ]; then
    echo "$(date -Iseconds) E${bits}: MISSING $bin -- skipping" >&2
    continue
  fi
  dest="$OUT/gpu-e${bits}.csv"
  echo "$(date -Iseconds) E${bits}: ${GRAPHS} graphs -> ${dest}" >&2
  "$bin" --edgebits="$bits" --graphs="$GRAPHS" --device=0 > "$dest"
done

echo "$(date -Iseconds) M2/M3 sweep complete" >&2
