#!/usr/bin/env python3
# Copyright (c) 2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

import base64

from .messages import (
    CTransaction,
    deser_string,
    from_binary,
    ser_compact_size,
)


# global types
PSQT_GLOBAL_UNSIGNED_TX = 0x00
PSQT_GLOBAL_XPUB = 0x01
PSQT_GLOBAL_TX_VERSION = 0x02
PSQT_GLOBAL_FALLBACK_LOCKTIME = 0x03
PSQT_GLOBAL_INPUT_COUNT = 0x04
PSQT_GLOBAL_OUTPUT_COUNT = 0x05
PSQT_GLOBAL_TX_MODIFIABLE = 0x06
PSQT_GLOBAL_VERSION = 0xfb
PSQT_GLOBAL_PROPRIETARY = 0xfc

# per-input types
PSQT_IN_NON_WITNESS_UTXO = 0x00
PSQT_IN_WITNESS_UTXO = 0x01
PSQT_IN_PARTIAL_SIG = 0x02
PSQT_IN_SIGHASH_TYPE = 0x03
PSQT_IN_REDEEM_SCRIPT = 0x04
PSQT_IN_WITNESS_SCRIPT = 0x05
PSQT_IN_BIP32_DERIVATION = 0x06
PSQT_IN_FINAL_SCRIPTSIG = 0x07
PSQT_IN_FINAL_SCRIPTWITNESS = 0x08
PSQT_IN_POR_COMMITMENT = 0x09
PSQT_IN_RIPEMD160 = 0x0a
PSQT_IN_SHA256 = 0x0b
PSQT_IN_HASH160 = 0x0c
PSQT_IN_HASH256 = 0x0d
PSQT_IN_PREVIOUS_TXID = 0x0e
PSQT_IN_OUTPUT_INDEX = 0x0f
PSQT_IN_SEQUENCE = 0x10
PSQT_IN_REQUIRED_TIME_LOCKTIME = 0x11
PSQT_IN_REQUIRED_HEIGHT_LOCKTIME = 0x12
PSQT_IN_TAP_KEY_SIG = 0x13
PSQT_IN_TAP_SCRIPT_SIG = 0x14
PSQT_IN_TAP_LEAF_SCRIPT = 0x15
PSQT_IN_TAP_BIP32_DERIVATION = 0x16
PSQT_IN_TAP_INTERNAL_KEY = 0x17
PSQT_IN_TAP_MERKLE_ROOT = 0x18
PSQT_IN_PROPRIETARY = 0xfc

# per-output types
PSQT_OUT_REDEEM_SCRIPT = 0x00
PSQT_OUT_WITNESS_SCRIPT = 0x01
PSQT_OUT_BIP32_DERIVATION = 0x02
PSQT_OUT_AMOUNT = 0x03
PSQT_OUT_SCRIPT = 0x04
PSQT_OUT_TAP_INTERNAL_KEY = 0x05
PSQT_OUT_TAP_TREE = 0x06
PSQT_OUT_TAP_BIP32_DERIVATION = 0x07
PSQT_OUT_PROPRIETARY = 0xfc


class PSQTMap:
    """Class for serializing and deserializing PSQT maps"""

    def __init__(self, map=None):
        self.map = map if map is not None else {}

    def deserialize(self, f):
        m = {}
        while True:
            k = deser_string(f)
            if len(k) == 0:
                break
            v = deser_string(f)
            if len(k) == 1:
                k = k[0]
            assert k not in m
            m[k] = v
        self.map = m

    def serialize(self):
        m = b""
        for k,v in self.map.items():
            if isinstance(k, int) and 0 <= k and k <= 255:
                k = bytes([k])
            m += ser_compact_size(len(k)) + k
            m += ser_compact_size(len(v)) + v
        m += b"\x00"
        return m

class PSQT:
    """Class for serializing and deserializing PSQTs"""

    def __init__(self, *, g=None, i=None, o=None):
        self.g = g if g is not None else PSQTMap()
        self.i = i if i is not None else []
        self.o = o if o is not None else []
        self.tx = None

    def deserialize(self, f):
        assert f.read(5) == b"psqt\xff"
        self.g = from_binary(PSQTMap, f)
        assert PSQT_GLOBAL_UNSIGNED_TX in self.g.map
        self.tx = from_binary(CTransaction, self.g.map[PSQT_GLOBAL_UNSIGNED_TX])
        self.i = [from_binary(PSQTMap, f) for _ in self.tx.vin]
        self.o = [from_binary(PSQTMap, f) for _ in self.tx.vout]
        return self

    def serialize(self):
        assert isinstance(self.g, PSQTMap)
        assert isinstance(self.i, list) and all(isinstance(x, PSQTMap) for x in self.i)
        assert isinstance(self.o, list) and all(isinstance(x, PSQTMap) for x in self.o)
        assert PSQT_GLOBAL_UNSIGNED_TX in self.g.map
        tx = from_binary(CTransaction, self.g.map[PSQT_GLOBAL_UNSIGNED_TX])
        assert len(tx.vin) == len(self.i)
        assert len(tx.vout) == len(self.o)

        psqt = [x.serialize() for x in [self.g] + self.i + self.o]
        return b"psqt\xff" + b"".join(psqt)

    def make_blank(self):
        """
        Remove all fields except for PSQT_GLOBAL_UNSIGNED_TX
        """
        for m in self.i + self.o:
            m.map.clear()

        self.g = PSQTMap(map={PSQT_GLOBAL_UNSIGNED_TX: self.g.map[PSQT_GLOBAL_UNSIGNED_TX]})

    def to_base64(self):
        return base64.b64encode(self.serialize()).decode("utf8")

    @classmethod
    def from_base64(cls, b64psqt):
        return from_binary(cls, base64.b64decode(b64psqt))
