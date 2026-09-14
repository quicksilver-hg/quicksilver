#!/usr/bin/env bash
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# M1: CPU time per graph across edge bits 22-29.
#
# Sample counts fall with graph size because cost per graph rises ~2.4x per edge
# bit above E25; the small sizes get more samples for free.
#
# Thread count is a parameter, defaulting to 8. On the reference machine the
# 8-thread configuration is the one that reproduces the recorded E29 anchor
# (49.1 s against 52.4 s); the 4-thread configuration does not, and the reason is
# unresolved. See m1-cpu-sweep/README.md before adding a 4-thread pass.
# Locale-independent numeric formatting in the recorded output.
export LC_ALL=C

set -euo pipefail

BIN="${1:?usage: run-m1.sh /path/to/qscalibrate [threads]}"
THREADS="${2:-8}"
OUT="$(dirname "$0")/m1-cpu-sweep"
mkdir -p "$OUT"

for bits in 22 23 24 25 26 27 28 29; do
  case "$bits" in
    22|23|24) graphs=2000 ;;
    25|26)    graphs=500  ;;
    27|28|29) graphs=200  ;;
  esac
  dest="$OUT/e${bits}-t${THREADS}.csv"
  echo "$(date -Iseconds) E${bits} t${THREADS}: ${graphs} graphs -> ${dest}" >&2
  "$BIN" --edgebits="$bits" --graphs="$graphs" --threads="$THREADS" > "$dest"
done

echo "$(date -Iseconds) sweep complete" >&2
