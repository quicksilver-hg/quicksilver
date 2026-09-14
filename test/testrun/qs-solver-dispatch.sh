#!/bin/sh
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# CUCKATOO_GPU_SOLVER dispatcher. Block PoW and per-tx PoW share one graph size,
# so a single compiled binary (qsgpusolve) serves both. The node passes the
# requested edgebits as $1. Point -cuckatoosolver at this script.
#
# This script deliberately does NOT know the graph size. It used to whitelist
# `29)` and reject everything else, which meant the E28 flag day silently broke
# GPU solving on every deployed box: the node began asking for 28 and the script
# answered "unsupported edgebits". A copy of a consensus constant living on the
# mining boxes, rather than in the repo, is the worst place to keep one.
#
# qsgpusolve validates $1 against its own compiled EDGEBITS and exits non-zero
# with `FATAL: edgebits N != compiled EDGEBITS M`. That check is authoritative and
# is the only one that can be right, so pass through and let it speak. A mismatch
# still fails loudly; it just fails with the truth instead of a guess.
export LC_ALL=C
DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
exec "$DIR/qsgpusolve" "$@"
