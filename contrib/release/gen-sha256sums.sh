#!/usr/bin/env bash
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Write SHA256SUMS for one directory of release artifacts, in the form
# `sha256sum -c` reads. Checksums are how a published binary is identified
# once it has left the machine that built it. This script does not sign
# anything and does not claim the build can be reproduced elsewhere.
export LC_ALL=C
set -euo pipefail

if [ "$#" -ne 1 ]; then
  echo "Usage: gen-sha256sums.sh DIRECTORY" >&2
  exit 2
fi

dir=$1

if [ ! -d "$dir" ]; then
  echo "ERROR: not a directory: $dir" >&2
  exit 1
fi

sums=$dir/SHA256SUMS
if [ -e "$sums" ]; then
  echo "ERROR: $sums already exists; refusing to overwrite a directory that already has checksums" >&2
  exit 1
fi

names=()
while IFS= read -r -d '' name; do
  names+=("$name")
done < <(find "$dir" -maxdepth 1 -type f -printf '%f\0' | sort -z)

if [ "${#names[@]}" -eq 0 ]; then
  echo "ERROR: no release artifacts in $dir" >&2
  exit 1
fi

tmp=$(mktemp)
cleanup() {
  rm -f "$tmp"
}
trap cleanup EXIT

(
  cd "$dir" || exit 1
  sha256sum -- "${names[@]}"
) >"$tmp"

mv "$tmp" "$sums"
trap - EXIT
