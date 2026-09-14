#!/usr/bin/env python3
# Copyright (c) 2020-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test datacarrier functionality"""
from test_framework.blocktools import COINBASE_MATURITY
from test_framework.messages import (
    CTxOut,
    MAX_OP_RETURN_RELAY,
)
from test_framework.script import (
    CScript,
    OP_RETURN,
)
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.test_node import TestNode
from test_framework.util import assert_raises_rpc_error
from test_framework.vault import MiniVault

from random import randbytes


class DataCarrierTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 4
        self.extra_args = [
            ["-txpownocycle=1"],
            ["-datacarrier=0", "-txpownocycle=1"],
            ["-datacarrier=1", f"-datacarriersize={MAX_OP_RETURN_RELAY - 1}", "-txpownocycle=1"],
            ["-datacarrier=1", "-datacarriersize=2", "-txpownocycle=1"],
        ]

    def test_null_data_transaction(self, node: TestNode, data, success: bool) -> None:
        tx = self.vault.create_self_transfer(confirmed_only=True)["tx"]
        data = [] if data is None else [data]
        tx.vout.append(CTxOut(nValue=0, scriptPubKey=CScript([OP_RETURN] + data)))

        tx_hex = tx.serialize().hex()

        if success:
            self.vault.sendrawtransaction(from_node=node, tx_hex=tx_hex)
            assert tx.rehash() in node.getrawrelaypool(True), f'{tx_hex} not in relaypool'
        else:
            assert_raises_rpc_error(-26, "scriptpubkey", self.vault.sendrawtransaction, from_node=node, tx_hex=tx_hex)

    def run_test(self):
        self.vault = MiniVault(self.nodes[0])
        self.generate(self.vault, COINBASE_MATURITY + 20)

        # By default, only 80 bytes are used for data (+1 for OP_RETURN, +2 for the pushdata opcodes).
        default_size_data = randbytes(MAX_OP_RETURN_RELAY - 3)
        too_long_data = randbytes(MAX_OP_RETURN_RELAY - 2)
        small_data = randbytes(MAX_OP_RETURN_RELAY - 4)
        one_byte = randbytes(1)
        zero_bytes = randbytes(0)

        self.log.info("Testing null data transaction with default -datacarrier and -datacarriersize values.")
        self.test_null_data_transaction(node=self.nodes[0], data=default_size_data, success=True)

        self.log.info("Testing a null data transaction larger than allowed by the default -datacarriersize value.")
        self.test_null_data_transaction(node=self.nodes[0], data=too_long_data, success=False)

        self.log.info("Testing a null data transaction with -datacarrier=false.")
        self.test_null_data_transaction(node=self.nodes[1], data=default_size_data, success=False)

        self.log.info("Testing a null data transaction with a size larger than accepted by -datacarriersize.")
        self.test_null_data_transaction(node=self.nodes[2], data=default_size_data, success=False)

        self.log.info("Testing a null data transaction with a size smaller than accepted by -datacarriersize.")
        self.test_null_data_transaction(node=self.nodes[2], data=small_data, success=True)

        self.log.info("Testing a null data transaction with no data.")
        self.test_null_data_transaction(node=self.nodes[0], data=None, success=True)
        self.test_null_data_transaction(node=self.nodes[1], data=None, success=False)
        self.test_null_data_transaction(node=self.nodes[2], data=None, success=True)
        self.test_null_data_transaction(node=self.nodes[3], data=None, success=True)

        self.log.info("Testing a null data transaction with zero bytes of data.")
        self.test_null_data_transaction(node=self.nodes[0], data=zero_bytes, success=True)
        self.test_null_data_transaction(node=self.nodes[1], data=zero_bytes, success=False)
        self.test_null_data_transaction(node=self.nodes[2], data=zero_bytes, success=True)
        self.test_null_data_transaction(node=self.nodes[3], data=zero_bytes, success=True)

        self.log.info("Testing a null data transaction with one byte of data.")
        self.test_null_data_transaction(node=self.nodes[0], data=one_byte, success=True)
        self.test_null_data_transaction(node=self.nodes[1], data=one_byte, success=False)
        self.test_null_data_transaction(node=self.nodes[2], data=one_byte, success=True)
        self.test_null_data_transaction(node=self.nodes[3], data=one_byte, success=False)


if __name__ == '__main__':
    DataCarrierTest(__file__).main()
