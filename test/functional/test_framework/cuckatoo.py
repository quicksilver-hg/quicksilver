#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Quicksilver block-PoW helpers for functional tests.

Mirrors consensus crypto/cuckatoo/proofhash.cpp: the proof hash is
blake2b-256 over the 42 cycle edges packed little-endian uint32, compared
little-endian against the target. solve_trivial reproduces the sandbox
fBlockPowNoCycle grind (rpc/mining.cpp): no real 42-cycle is required, only
that the proof hash clears the target."""
import hashlib

PROOFSIZE = 42


def proof_hash_int(cycle):
    buf = b"".join(int(e & 0xffffffff).to_bytes(4, "little") for e in cycle)
    return int.from_bytes(hashlib.blake2b(buf, digest_size=32).digest(), "little")


def solve_trivial(target_int):
    """Return a 42-edge cycle whose proof hash <= target_int (sandbox trivial PoW)."""
    cycle = list(range(1, PROOFSIZE + 1))  # [1..42], matches mining.cpp init
    while proof_hash_int(cycle) > target_int:
        cycle[0] = (cycle[0] + PROOFSIZE) & 0xffffffff
    return cycle


if __name__ == "__main__":
    # self-check: solve against an easy target and confirm it clears
    t = (1 << 248) - 1
    c = solve_trivial(t)
    assert proof_hash_int(c) <= t and len(c) == 42
    print("cuckatoo.py self-check OK")
