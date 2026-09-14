#!/usr/bin/env python3
# Copyright (c) 2024 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Helpful routines for relaypool testing."""

from .messages import CTransaction
from .util import (
    assert_equal,
)

ORPHAN_TX_EXPIRE_TIME = 1200

def assert_relaypool_contents(test_framework, node, expected=None, sync=True):
    """Assert that all transactions in expected are in the relaypool,
    and no additional ones exist. 'expected' is an array of
    CTransaction objects
    """
    if sync:
        test_framework.sync_relaypools()
    if not expected:
        expected = []
    assert_equal(len(expected), len(set(expected)))
    relaypool = node.getrawrelaypool(verbose=False)
    assert_equal(len(relaypool), len(expected))
    for tx in expected:
        assert tx.rehash() in relaypool


def tx_in_orphanage(node, tx: CTransaction) -> bool:
    """Returns true if the transaction is in the orphanage."""
    found = [o for o in node.getorphantxs(verbosity=1) if o["txid"] == tx.rehash() and o["wtxid"] == tx.getwtxid()]
    return len(found) == 1
