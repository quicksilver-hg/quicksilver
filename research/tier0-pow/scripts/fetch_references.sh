#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/third_party"
ZIPS="${ZIPS:-$ROOT}"   # reference zips are local inputs, not committed product files
mkdir -p "$TP"

# --- Cuckoo Cycle (tromp) — local snapshot a69ad1d ($ZIPS/cuckoo-master.zip) ---
CUCKOO_DIR="$TP/cuckoo"
if [ ! -d "$CUCKOO_DIR" ]; then
  unzip -q "$ZIPS/cuckoo-master.zip" -d "$TP"
  mv "$TP/cuckoo-master" "$CUCKOO_DIR"
fi
echo "Cuckoo reference at $CUCKOO_DIR (snapshot a69ad1d)"

# --- Equihash (tromp) — local snapshot fab686e ($ZIPS/equihash-master.zip) ---
EQUIHASH_DIR="$TP/equihash"
if [ ! -d "$EQUIHASH_DIR" ]; then
  unzip -q "$ZIPS/equihash-master.zip" -d "$TP"
  mv "$TP/equihash-master" "$EQUIHASH_DIR"
  # The 2018 snapshot's bundled blake2.h puts ALIGN(64) on structs inside a
  # #pragma pack(1) region, which GCC 13+ rejects ("size of array element is not
  # a multiple of its alignment"). Apply the committed fix so a fresh fetch
  # builds on modern toolchains. Applied once, right after unzip.
  patch -p1 -d "$EQUIHASH_DIR" < "$ROOT/patches/equihash-blake2-gcc13.patch"
  echo "Applied GCC13 blake2.h patch to Equihash reference"
fi
echo "Equihash reference at $EQUIHASH_DIR (snapshot fab686e)"
