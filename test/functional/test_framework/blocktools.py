#!/usr/bin/env python3
# Copyright (c) 2015-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Utilities for manipulating blocks and transactions."""

import struct
import time
import unittest
from decimal import Decimal

from .messages import (
    CBlock,
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxInWitness,
    CTxOut,
    SEQUENCE_FINAL,
    hash256,
    ser_uint256,
    MAX_BLOCK_WEIGHT,
    tx_from_hex,
    uint256_from_compact,
    uint256_from_str,
    WITNESS_SCALE_FACTOR,
)
from .script import (
    CScript,
    CScriptNum,
    CScriptOp,
    OP_0,
    OP_RETURN,
    OP_TRUE,
)
from .script_util import (
    key_to_p2pk_script,
    key_to_p2wpkh_script,
    keys_to_multisig_script,
    script_to_p2wsh_script,
)
from .util import assert_equal

MAX_BLOCK_SIGOPS = 20000

# Quicksilver (#5c-1 Phase 2): the EIP-1559 congestion multiplier, mirrored from
# src/pow.h and NextCongestionMultiplier() in src/pow.cpp. It is a header field
# (CBlockHeader.nCongestion), so a block built here must carry the exact value
# consensus recomputes at ConnectBlock or it is rejected as "bad-congestion".
CONGESTION_ONE = 65536  # fixed-point scale: this value is multiplier 1.0
# Sandbox (regtest) values from src/kernel/chainparams.cpp.
SANDBOX_CONGESTION_TARGET_PERMILLE = 5
SANDBOX_CONGESTION_STEP_DENOM = 8
SANDBOX_CONGESTION_MAX_MULTIPLIER = 64


def next_congestion_multiplier(prev_m, block_weight,
                               target_permille=SANDBOX_CONGESTION_TARGET_PERMILLE,
                               step_denom=SANDBOX_CONGESTION_STEP_DENOM,
                               max_multiplier=SANDBOX_CONGESTION_MAX_MULTIPLIER):
    """One step of the congestion recurrence. Mirrors NextCongestionMultiplier().

    Integer arithmetic throughout, matching the C++ exactly — floating point here
    would round differently from consensus and produce blocks rejected as
    'bad-congestion' only sometimes, which is the worst possible failure mode.
    """
    floor_m = CONGESTION_ONE
    cap_m = max_multiplier * CONGESTION_ONE
    target_weight = MAX_BLOCK_WEIGHT * target_permille // 1000
    if target_weight <= 0:
        return min(max(prev_m, floor_m), cap_m)
    diff = abs(block_weight - target_weight)
    delta = prev_m * diff // target_weight // step_denom
    if block_weight > target_weight:
        next_m = prev_m + delta
    else:
        next_m = prev_m - min(prev_m, delta)
    return min(max(next_m, floor_m), cap_m)
MAX_BLOCK_SIGOPS_WEIGHT = MAX_BLOCK_SIGOPS * WITNESS_SCALE_FACTOR
MAX_STANDARD_TX_WEIGHT = 400000

# Genesis block time (sandbox)
TIME_GENESIS_BLOCK = 1750000000

MAX_FUTURE_BLOCK_TIME = 2 * 60 * 60

# Coinbase transaction outputs can only be spent after this number of new blocks (network rule)
COINBASE_MATURITY = 100
SANDBOX_INITIAL_SUBSIDY = 50 * COIN
SANDBOX_TAIL_SUBSIDY = 1 * COIN
SANDBOX_BOOTSTRAP_BLOCKS = 150


def quicksilver_sandbox_subsidy_sats(height):
    if height < 0:
        return 0
    if height >= SANDBOX_BOOTSTRAP_BLOCKS:
        return SANDBOX_TAIL_SUBSIDY
    return SANDBOX_INITIAL_SUBSIDY - ((SANDBOX_INITIAL_SUBSIDY - SANDBOX_TAIL_SUBSIDY) * height) // SANDBOX_BOOTSTRAP_BLOCKS


def quicksilver_sandbox_subsidy(height):
    return Decimal(quicksilver_sandbox_subsidy_sats(height)) / COIN

# From BIP141
WITNESS_COMMITMENT_HEADER = b"\xaa\x21\xa9\xed"

NORMAL_GBT_REQUEST_PARAMS = {"rules": ["segwit"]}
VERSIONBITS_LAST_OLD_BLOCK_VERSION = 4
MIN_BLOCKS_TO_KEEP = 288

SANDBOX_RETARGET_PERIOD = 150

SANDBOX_N_BITS = 0x207fffff  # difficulty retargeting is disabled in SANDBOX chainparams"
SANDBOX_TARGET = 0x7fffff0000000000000000000000000000000000000000000000000000000000
assert_equal(uint256_from_compact(SANDBOX_N_BITS), SANDBOX_TARGET)

def nbits_str(nbits):
    return f"{nbits:08x}"

def target_str(target):
    return f"{target:064x}"

def rederive_block_congestion(block, node):
    """Re-derive block.nCongestion after the body changed, reading the parent's exact
    multiplier from the node.

    Any test that mutates a block after create_block() -- adds transactions, inflates a
    witness, swaps the coinbase -- has changed its weight and must call this before
    solve(). nCongestion lives inside the pre-pow, so a stale value is both a
    'bad-congestion' rejection and an invalidated proof.

    Reads the exact integer 'congestion' field, never 'congestion_multiplier': the
    recurrence is integer math and the ratio form is lossy.
    """
    prev = node.getblockheader(f"{block.hashPrevBlock:064x}")["congestion"]
    block.nCongestion = next_congestion_multiplier(prev, block.get_weight())
    return block.nCongestion


def create_block(hashprev=None, coinbase=None, ntime=None, *, version=None, tmpl=None, txlist=None,
                 prev_congestion=CONGESTION_ONE):
    """Create a block (with sandbox difficulty).

    prev_congestion is the PARENT block's nCongestion; this block's is derived from
    it and from this block's own weight, exactly as consensus does at ConnectBlock.
    The default is the floor (CONGESTION_ONE), which is the steady state of any
    sandbox chain whose blocks stay under the fullness target — that covers almost
    every functional test. A test that deliberately drives the multiplier up must
    pass the real parent value, or its block will be rejected as 'bad-congestion'.
    """
    block = CBlock()
    if tmpl is None:
        tmpl = {}
    block.nVersion = version or tmpl.get('version') or VERSIONBITS_LAST_OLD_BLOCK_VERSION
    block.nTime = ntime or tmpl.get('curtime') or int(time.time() + 600)
    block.hashPrevBlock = hashprev or int(tmpl['previousblockhash'], 0x10)
    if tmpl and tmpl.get('bits') is not None:
        block.nBits = struct.unpack('>I', bytes.fromhex(tmpl['bits']))[0]
    else:
        block.nBits = SANDBOX_N_BITS
    if coinbase is None:
        coinbase = create_coinbase(height=tmpl['height'])
    block.vtx.append(coinbase)
    if txlist:
        for tx in txlist:
            if not hasattr(tx, 'calc_sha256'):
                tx = tx_from_hex(tx)
            block.vtx.append(tx)
    block.hashMerkleRoot = block.calc_merkle_root()
    # After the body is final: the multiplier folds in this block's own weight.
    # nCongestion is a fixed 4 bytes whatever its value, so weight does not depend
    # on it and there is no fixpoint to solve here.
    block.nCongestion = next_congestion_multiplier(prev_congestion, block.get_weight())
    block.calc_sha256()
    return block

def get_witness_script(witness_root, witness_nonce):
    witness_commitment = uint256_from_str(hash256(ser_uint256(witness_root) + ser_uint256(witness_nonce)))
    output_data = WITNESS_COMMITMENT_HEADER + ser_uint256(witness_commitment)
    return CScript([OP_RETURN, output_data])

def add_witness_commitment(block, nonce=0):
    """Add a witness commitment to the block's coinbase transaction.

    According to BIP141, blocks with witness rules active must commit to the
    hash of all in-block transactions including witness."""
    # First calculate the merkle root of the block's
    # transactions, with witnesses.
    witness_nonce = nonce
    witness_root = block.calc_witness_merkle_root()
    # witness_nonce should go to coinbase witness.
    block.vtx[0].wit.vtxinwit = [CTxInWitness()]
    block.vtx[0].wit.vtxinwit[0].scriptWitness.stack = [ser_uint256(witness_nonce)]

    # witness commitment is the last OP_RETURN output in coinbase
    block.vtx[0].vout.append(CTxOut(0, get_witness_script(witness_root, witness_nonce)))
    block.vtx[0].rehash()
    block.hashMerkleRoot = block.calc_merkle_root()
    block.rehash()


def script_BIP34_coinbase_height(height):
    if height <= 16:
        res = CScriptOp.encode_op_n(height)
        # Append dummy to increase scriptSig size to 2 (see bad-cb-length consensus rule)
        return CScript([res, OP_0])
    return CScript([CScriptNum(height)])


def create_coinbase(height, pubkey=None, *, script_pubkey=None, extra_output_script=None, nValue=None):
    """Create a coinbase transaction.

    If pubkey is passed in, the coinbase output will be a P2PK output;
    otherwise an anyone-can-spend output.

    If extra_output_script is given, make a 0-value output to that
    script. This is useful to pad block weight/sigops as needed. """
    coinbase = CTransaction()
    coinbase.vin.append(CTxIn(COutPoint(0, 0xffffffff), script_BIP34_coinbase_height(height), SEQUENCE_FINAL))
    coinbaseoutput = CTxOut()
    if nValue is None:
        coinbaseoutput.nValue = quicksilver_sandbox_subsidy_sats(height)
    else:
        coinbaseoutput.nValue = nValue * COIN
    if pubkey is not None:
        coinbaseoutput.scriptPubKey = key_to_p2pk_script(pubkey)
    elif script_pubkey is not None:
        coinbaseoutput.scriptPubKey = script_pubkey
    else:
        coinbaseoutput.scriptPubKey = CScript([OP_TRUE])
    coinbase.vout = [coinbaseoutput]
    if extra_output_script is not None:
        coinbaseoutput2 = CTxOut()
        coinbaseoutput2.nValue = 0
        coinbaseoutput2.scriptPubKey = extra_output_script
        coinbase.vout.append(coinbaseoutput2)
    coinbase.calc_sha256()
    return coinbase

def create_tx_with_script(prevtx, n, script_sig=b"", *, amount, output_script=None):
    """Return one-input, one-output transaction object
       spending the prevtx's n-th output with the given amount.

       Can optionally pass scriptPubKey and scriptSig, default is anyone-can-spend output.
    """
    if output_script is None:
        output_script = CScript()
    tx = CTransaction()
    assert n < len(prevtx.vout)
    tx.vin.append(CTxIn(COutPoint(prevtx.sha256, n), script_sig, SEQUENCE_FINAL))
    tx.vout.append(CTxOut(amount, output_script))
    tx.calc_sha256()
    return tx

def get_legacy_sigopcount_block(block, accurate=True):
    count = 0
    for tx in block.vtx:
        count += get_legacy_sigopcount_tx(tx, accurate)
    return count

def get_legacy_sigopcount_tx(tx, accurate=True):
    count = 0
    for i in tx.vout:
        count += i.scriptPubKey.GetSigOpCount(accurate)
    for j in tx.vin:
        # scriptSig might be of type bytes, so convert to CScript for the moment
        count += CScript(j.scriptSig).GetSigOpCount(accurate)
    return count

def witness_script(use_p2wsh, pubkey):
    """Create a scriptPubKey for a pay-to-witness TxOut.

    This is either a P2WPKH output for the given pubkey, or a P2WSH output of a
    1-of-1 multisig for the given pubkey. Returns the hex encoding of the
    scriptPubKey."""
    if not use_p2wsh:
        # P2WPKH instead
        pkscript = key_to_p2wpkh_script(pubkey)
    else:
        # 1-of-1 multisig
        witness_script = keys_to_multisig_script([pubkey])
        pkscript = script_to_p2wsh_script(witness_script)
    return pkscript.hex()

class TestFrameworkBlockTools(unittest.TestCase):
    def test_create_coinbase(self):
        height = 20
        coinbase_tx = create_coinbase(height=height)
        assert_equal(CScriptNum.decode(coinbase_tx.vin[0].scriptSig), height)
        assert_equal(coinbase_tx.vout[0].nValue, quicksilver_sandbox_subsidy_sats(height))
