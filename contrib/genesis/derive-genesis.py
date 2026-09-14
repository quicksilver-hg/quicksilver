#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Derive Quicksilver genesis merkle root and block hash offline.

Genesis is a zero-cycle trust anchor: CheckProofOfWorkImpl exempts exactly
consensus.hashGenesisBlock, and normal Cuckatoo block PoW begins at height 1.
There is nothing to grind -- the hashes follow directly from the header fields
and the coinbase scriptSig.

Run with no arguments to self-check against every genesis hash currently
asserted in src/kernel/chainparams.cpp. Pass a headline and an nTime to derive a
new pair; it refuses to derive anything new until it has reproduced all of the
known ones first.

The self-check covers SANDBOX as well as the two re-mintable networks, even
though SANDBOX is never re-minted. MAIN and PUBLICTEST share an nTime, an
nNonce of 0 and an nBits of 0x203fffff, so on those two alone a serializer that
dropped nNonce or nBits entirely would still reproduce all four hashes.
SANDBOX differs in all three, so it is what actually binds those fields.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "test" / "functional"))

from test_framework.messages import (  # noqa: E402
    COIN, CBlock, COutPoint, CTransaction, CTxIn, CTxOut,
)
from test_framework.script import CScript, OP_RETURN  # noqa: E402

CONGESTION_ONE = 1 << 16  # src/pow.h; genesis sits at the congestion floor

MARK = "Quicksilver Genesis - for the agents, raised by human, Claude, Codex, and Grok"


def build_genesis(pszTimestamp, mark, nTime, nBits, nNonce, nVersion, reward):
    tx = CTransaction()
    tx.version = 1
    tx.vin = [CTxIn(COutPoint(0, 0xffffffff), CScript([pszTimestamp.encode()]), 0xffffffff)]
    tx.vout = [CTxOut(reward, CScript([OP_RETURN, mark.encode()]))]
    tx.nLockTime = 0
    tx.nAnchorHeight = 0
    tx.nPowNonce = 0
    tx.nCycle = [0] * 42
    tx.rehash()

    block = CBlock()
    block.nVersion = nVersion
    block.hashPrevBlock = 0
    block.nTime = nTime
    block.nBits = nBits
    block.nCongestion = CONGESTION_ONE
    block.nNonce = nNonce
    block.vtx = [tx]
    block.hashMerkleRoot = block.calc_merkle_root()
    block.rehash()
    return f"{block.hashMerkleRoot:064x}", f"{block.sha256:064x}"


# name -> (pszTimestamp, nTime, nBits, nNonce, want_merkle_root, want_block_hash)
# Copied verbatim from the live asserts in src/kernel/chainparams.cpp.
KNOWN = {
    "main": (
        "Quicksilver mainnet genesis 2026-09-05 - zero-cycle trust anchor",
        1788566400, 0x203fffff, 0,
        "da9499c1476c92b69b7214ddb1a365548fff51159e523ca01c77686e022de966",
        "c9144acae20212e57e9e59f34a681bd25f55d81438001c424ebb95cd4869bcf3",
    ),
    "publictest": (
        "Quicksilver publictest genesis 2026-09-05 - zero-cycle trust anchor",
        1788566400, 0x203fffff, 0,
        "bdf0a510a4f1094ae464987ef01a0fe8409d7c61fee2ef4102d4d84159e78ad6",
        "917dde1f04c7470969bbdc344d39559e1af32b3b0e6caaed89db54637d6e46da",
    ),
    "sandbox": (
        "Quicksilver sandbox genesis 2026-08-24 - feeless per-tx PoW",
        1750000000, 0x207fffff, 4,
        "feacef8e2fca6169ea9726bbf4db05c5efa7c006c1f0d1f98c2cb056d7a06469",
        "bd806e80eec28f4b16ab48db377ad6af369fa3b1197cb6d9d77d07db6d5e91e7",
    ),
}


def self_check():
    ok = True
    checked = 0
    for name, (headline, ntime, nbits, nnonce, want_root, want_hash) in KNOWN.items():
        root, blockhash = build_genesis(headline, MARK, ntime, nbits, nnonce, 1, 50 * COIN)
        for label, got, want in (("merkle", root, want_root), ("hash  ", blockhash, want_hash)):
            checked += 1
            if got == want:
                print(f"OK  {name:<11} {label}: {got}")
            else:
                ok = False
                print(f"BAD {name:<11} {label}: {got}\n{'':<21}want: {want}")
    return ok, checked


if __name__ == "__main__":
    ok, checked = self_check()
    if not ok:
        sys.exit("self-check FAILED -- derivation is not trustworthy, do not use its output")
    print(f"\nself-check OK ({checked}/{checked}).")
    if len(sys.argv) == 1:
        sys.exit(0)
    if not 3 <= len(sys.argv) <= 5:
        sys.exit("usage: derive-genesis.py <headline> <nTime> [nBits-hex] [nNonce]")
    headline = sys.argv[1]
    ntime = int(sys.argv[2])
    nbits = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x203fffff
    nnonce = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    root, blockhash = build_genesis(headline, MARK, ntime, nbits, nnonce, 1, 50 * COIN)
    print(f"\n{headline!r}\n  nTime            = {ntime}\n  nBits            = 0x{nbits:08x}"
          f"\n  nNonce           = {nnonce}"
          f"\n  hashMerkleRoot   = {root}\n  hashGenesisBlock = {blockhash}")
