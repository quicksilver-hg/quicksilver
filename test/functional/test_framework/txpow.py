#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Helpers for Quicksilver per-transaction proof-of-work in functional tests."""

from test_framework.cuckatoo import solve_trivial
from test_framework.messages import tx_from_hex


# The cheap sandbox target for a small transaction. solve_trivial() grinds a cycle
# that does not depend on the transaction body, so two small txs proved at this
# target get comparable actual work.
DEFAULT_TXPOW_TARGET = (1 << 248) - 1

# A tighter target for a replacement candidate. Actual work is 2**256/(proof_hash+1),
# so a lower proof hash is strictly more work and therefore strictly more surplus
# (surplus = actual - floor, and two txs anchored at the same tip share a floor).
# That is how a feeless replacement out-ranks its conflict under #6
# replace-by-surplus-work -- see feature_quicksilver_replace.py.
REPLACEMENT_TXPOW_TARGET = (1 << 244) - 1

# Sandbox consensus constants, mirroring kernel/chainparams.cpp (ChainType::SANDBOX).
# If those move, these must move with them, or every functional test silently
# under-proves.
TX_WORK_REF_BYTES = 4739   # R_b
TX_UTXO_REF_COUNT = 50     # U

# BaseTxWork(anchor) on sandbox. nTxWorkCouplingK is 0xFFFFFFFF there, so the moving
# average of block proof (~2 at the sandbox powLimit) divided by K floors to zero in
# integer arithmetic, and pow.cpp's "if (required == 0) required = 1" clamp makes the
# result exactly 1 -- for every anchor and every congestion multiplier. That is what
# lets this mirror be a constant rather than a chain query. It is ONLY valid for
# sandbox; main and publictest use K = 106 against a real moving average.
SANDBOX_BASE_TX_WORK = 1


def required_tx_work(nbytes, nout, nin):
    """Mirror RequiredTxWorkForBytes() from pow.cpp, for the sandbox network.

    Required work scales with serialized WITH-witness bytes and with net UTXO
    creation. A transaction proved before its body is finished -- one whose witness
    is inflated afterwards, say -- owes more work than it carries, and the node
    rejects it as tx-pow-invalid.
    """
    utxo_delta = nout - nin if nout > nin else 0
    denom = TX_WORK_REF_BYTES * TX_UTXO_REF_COUNT
    factor = nbytes * TX_UTXO_REF_COUNT + utxo_delta * TX_WORK_REF_BYTES
    # Round UP, exactly as pow.cpp does. Flooring here would make the mirror weaker
    # than consensus instead of equal to it.
    required = -(-(SANDBOX_BASE_TX_WORK * factor) // denom)
    return max(required, 1)


def target_for_tx(tx):
    """The proof-hash target this transaction's own size demands.

    Mirrors GetTxPowTarget(): floor((2**256 - 1) / required). The PoW tail is a
    fixed 176 bytes whatever the cycle contains, so measuring size before grinding
    gives the same answer as measuring it after.
    """
    nbytes = len(tx.serialize_with_witness())
    return ((1 << 256) - 1) // required_tx_work(nbytes, len(tx.vout), len(tx.vin))


def prove_tx_pow(tx, *, anchor_height, target_int=DEFAULT_TXPOW_TARGET):
    """Attach a valid sandbox tx-PoW tail to a transaction object.

    `target_int` is an upper bound, not the target: the transaction's own size may
    demand a tighter one, and the tighter of the two always wins. Over-proving is
    always valid, so a caller asking for a specific target -- a replacement wanting
    surplus work, say -- keeps its intent and simply grinds harder when the body
    requires it.
    """
    tx.nAnchorHeight = anchor_height
    tx.nPowNonce = 0
    tx.nCycle = [0] * 42  # placeholder; the tail is fixed-size, so this fixes the size
    effective_target = min(target_int, target_for_tx(tx))
    tx.nCycle = solve_trivial(effective_target)
    tx.rehash()
    return tx


def prove_raw_tx_pow(tx_hex, node, *, target_int=DEFAULT_TXPOW_TARGET):
    """Return tx hex with a fresh tx-PoW tail anchored to node's current tip."""
    tx = tx_from_hex(tx_hex)
    prove_tx_pow(tx, anchor_height=node.getblockcount(), target_int=target_int)
    return tx.serialize().hex()
