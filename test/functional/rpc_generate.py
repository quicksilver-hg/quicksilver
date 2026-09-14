#!/usr/bin/env python3
# Copyright (c) 2020-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test generate* RPCs."""

from concurrent.futures import ThreadPoolExecutor

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.vault import MiniVault
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)


class RPCGenerateTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-txpownocycle=1"]]

    def run_test(self):
        self.test_generatetoaddress()
        self.test_generate()
        self.test_generateblock()

    def test_generatetoaddress(self):
        self.generatetoaddress(self.nodes[0], 1, 'SURbDZCioFW4WAkZJHxuS26WkdoZPm1dKB')
        assert_raises_rpc_error(-5, "Invalid address", self.generatetoaddress, self.nodes[0], 1, '2N3oefVeg6stiTb5Kh3ozCSkaqmx91FDbsm')

    def test_generateblock(self):
        node = self.nodes[0]
        minivault = MiniVault(node)

        self.log.info('Mine an empty block to address and return the hex')
        address = minivault.get_address()
        generated_block = self.generateblock(node, output=address, transactions=[], submit=False)
        node.submitblock(hexdata=generated_block['hex'])
        assert_equal(generated_block['hash'], node.getbestblockhash())

        self.log.info('Generate an empty block to address')
        hash = self.generateblock(node, output=address, transactions=[])['hash']
        block = node.getblock(blockhash=hash, verbosity=2)
        assert_equal(len(block['tx']), 1)
        assert_equal(block['tx'][0]['vout'][0]['output_script']['address'], address)

        self.log.info('Generate an empty block to a descriptor')
        hash = self.generateblock(node, 'addr(' + address + ')', [])['hash']
        block = node.getblock(blockhash=hash, verbosity=2)
        assert_equal(len(block['tx']), 1)
        assert_equal(block['tx'][0]['vout'][0]['output_script']['address'], address)

        self.log.info('Generate an empty block to a combo descriptor with compressed pubkey')
        combo_key = '0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798'
        combo_address = 'shg1qw508d6qejxtdg4y5r3zarvary0c5xw7kzwl4dg'
        hash = self.generateblock(node, 'combo(' + combo_key + ')', [])['hash']
        block = node.getblock(hash, 2)
        assert_equal(len(block['tx']), 1)
        assert_equal(block['tx'][0]['vout'][0]['output_script']['address'], combo_address)

        self.log.info('Generate an empty block to a combo descriptor with uncompressed pubkey')
        combo_key = '0408ef68c46d20596cc3f6ddf7c8794f71913add807f1dc55949fa805d764d191c0b7ce6894c126fce0babc6663042f3dde9b0cf76467ea315514e5a6731149c67'
        combo_address = 'SSPCBFJpiYNgYiYNTxdTp5eYPZN4zhjLit'
        hash = self.generateblock(node, 'combo(' + combo_key + ')', [])['hash']
        block = node.getblock(hash, 2)
        assert_equal(len(block['tx']), 1)
        assert_equal(block['tx'][0]['vout'][0]['output_script']['address'], combo_address)

        self.generate(minivault, COINBASE_MATURITY + 1)

        # Generate some extra relaypool transactions to verify they don't get mined
        for _ in range(10):
            minivault.send_self_transfer(from_node=node)

        self.log.info('Generate block with txid')
        txid = minivault.send_self_transfer(from_node=node, confirmed_only=True)['txid']
        hash = self.generateblock(node, address, [txid])['hash']
        block = node.getblock(hash, 1)
        assert_equal(len(block['tx']), 2)
        assert_equal(block['tx'][1], txid)

        self.log.info('Generate block with raw tx')
        rawtx = minivault.create_self_transfer(confirmed_only=True)['hex']
        hash = self.generateblock(node, address, [rawtx])['hash']

        block = node.getblock(hash, 1)
        assert_equal(len(block['tx']), 2)
        txid = block['tx'][1]
        assert_equal(node.getrawtransaction(txid=txid, verbosity=0, blockhash=hash), rawtx)
        assert_raises_rpc_error(-3, "JSON value of type bool is not of expected type number", node.getrawtransaction, txid, True)

        # Ensure that generateblock can be called concurrently by many threads.
        self.log.info('Generate blocks in parallel')
        generate_50_blocks = lambda n: [n.generateblock(output=address, transactions=[]) for _ in range(50)]
        rpcs = [node.cli for _ in range(6)]
        with ThreadPoolExecutor(max_workers=len(rpcs)) as threads:
            list(threads.map(generate_50_blocks, rpcs))

        self.log.info('Fail to generate block with out of order txs')
        txid1 = minivault.send_self_transfer(from_node=node, confirmed_only=True)['txid']
        utxo1 = minivault.get_utxo(txid=txid1)
        rawtx2 = minivault.create_self_transfer(utxo_to_spend=utxo1)['hex']
        assert_raises_rpc_error(-25, 'TestBlockValidity failed: bad-txns-inputs-missingorspent', self.generateblock, node, address, [rawtx2, txid1])

        self.log.info('Fail to generate block with txid not in relaypool')
        missing_txid = '0000000000000000000000000000000000000000000000000000000000000000'
        assert_raises_rpc_error(-5, 'Transaction ' + missing_txid + ' not in the relay pool.', self.generateblock, node, address, [missing_txid])

        self.log.info('Fail to generate block with invalid raw tx')
        invalid_raw_tx = '0000'
        assert_raises_rpc_error(-22, 'Transaction decode failed for ' + invalid_raw_tx, self.generateblock, node, address, [invalid_raw_tx])

        self.log.info('Fail to generate block with invalid address/descriptor')
        assert_raises_rpc_error(-5, 'Invalid address or descriptor', self.generateblock, node, '1234', [])

        self.log.info('Fail to generate block with a ranged descriptor')
        ranged_descriptor = 'pkh(squb6UShJgvLuWbXifcdv9236UbQnqJyJC366H4WRQv13dM5EtcbGQV5ahXqyZc5qzCq5un1uN9CY3TbSwdukftSEkdcimAvyXU6X7wfmCNR8eu/0/*)'
        assert_raises_rpc_error(-8, 'Ranged descriptor not accepted. Maybe pass through deriveaddresses first?', self.generateblock, node, ranged_descriptor, [])

        self.log.info('Fail to generate block with a descriptor missing a private key')
        child_descriptor = 'pkh(squb6UShJgvLuWbXifcdv9236UbQnqJyJC366H4WRQv13dM5EtcbGQV5ahXqyZc5qzCq5un1uN9CY3TbSwdukftSEkdcimAvyXU6X7wfmCNR8eu/0\'/0)'
        assert_raises_rpc_error(-5, 'Cannot derive script without private keys', self.generateblock, node, child_descriptor, [])

    def test_generate(self):
        self.log.info("Test rpc generate is unregistered, not a hidden stub")
        assert_raises_rpc_error(-32601, "Method not found", self.nodes[0].rpc.generate)
        assert_equal(self.nodes[0].help("generate"), "help: unknown command: generate")
        assert "has been replaced by the -generate" not in self.nodes[0].help()


if __name__ == "__main__":
    RPCGenerateTest(__file__).main()
