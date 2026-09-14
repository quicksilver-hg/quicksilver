#!/usr/bin/env python3
# Copyright (c) 2014-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test logic for skipping signature validation on old blocks.

Test logic for skipping signature validation on blocks which we've assumed
valid.

We build a chain that includes and invalid signature for one of the
transactions:

    0:        genesis block
    1:        block 1 with coinbase transaction output.
    2-101:    bury that block with 100 blocks so the coinbase transaction
              output can be spent
    102:      a block containing a transaction spending the coinbase
              transaction output. The transaction has an invalid signature.
    103-4302: bury the bad block with just over two weeks' worth of blocks
              under Quicksilver's 5-minute target spacing (4200 blocks)

Start three nodes:

    - node0 has no -assumevalid parameter. Try to sync to block 4302. It will
      reject block 102 and only sync as far as block 101
    - node1 has -assumevalid set to the hash of block 102. Try to sync to
      block 4302. node1 will sync all the way to block 4302.
    - node2 has -assumevalid set to the hash of block 102. Try to sync to
      block 200. node2 will reject block 102 since it's assumed valid, but it
      isn't buried by at least two weeks' work.
"""

from test_framework.blocktools import (
    COINBASE_MATURITY,
    create_block,
    create_coinbase,
)
from test_framework.messages import (
    CBlockHeader,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
    msg_block,
    msg_headers,
)
from test_framework.p2p import P2PInterface
from test_framework.script import (
    CScript,
    OP_TRUE,
)
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.txpow import prove_tx_pow
from test_framework.util import assert_equal
from test_framework.vault_util import generate_keypair

ASSUMEVALID_BURY_BLOCKS = 4200
MAX_HEADERS_MESSAGE_SIZE = 2000


class BaseNode(P2PInterface):
    def send_header_for_blocks(self, new_blocks):
        headers_message = msg_headers()
        headers_message.headers = [CBlockHeader(b) for b in new_blocks]
        self.send_message(headers_message)


class AssumeValidTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        self.rpc_timeout = 120
        self.extra_args = [["-txpownocycle=1"] for _ in range(self.num_nodes)]

    def setup_network(self):
        self.add_nodes(3)
        # Start node0. We don't start the other nodes yet since
        # we need to pre-mine a block with an invalid transaction
        # signature so we can pass in the block hash as assumevalid.
        self.start_node(0)

    def send_blocks_until_disconnected(self, p2p_conn):
        """Keep sending blocks to the node until we're disconnected."""
        for i in range(len(self.blocks)):
            if not p2p_conn.is_connected:
                break
            try:
                p2p_conn.send_message(msg_block(self.blocks[i]))
            except IOError:
                assert not p2p_conn.is_connected
                break

    def send_all_headers(self, p2p_conn):
        for i in range(0, len(self.blocks), MAX_HEADERS_MESSAGE_SIZE):
            p2p_conn.send_header_for_blocks(self.blocks[i:i + MAX_HEADERS_MESSAGE_SIZE])

    def run_test(self):
        # Build the blockchain
        self.tip = int(self.nodes[0].getbestblockhash(), 16)
        self.block_time = self.nodes[0].getblock(self.nodes[0].getbestblockhash())['time'] + 1

        self.blocks = []

        # Get a pubkey for the coinbase TXO
        _, coinbase_pubkey = generate_keypair()

        # Create the first block with a coinbase output to our key
        height = 1
        block = create_block(self.tip, create_coinbase(height, coinbase_pubkey), self.block_time)
        self.blocks.append(block)
        self.block_time += 1
        block.solve()
        # Save the coinbase for later
        self.block1 = block
        self.tip = block.sha256
        height += 1

        # Bury the block 100 deep so the coinbase output is spendable
        for _ in range(100):
            block = create_block(self.tip, create_coinbase(height), self.block_time)
            block.solve()
            self.blocks.append(block)
            self.tip = block.sha256
            self.block_time += 1
            height += 1

        # Create a transaction spending the coinbase output with an invalid (null) signature
        tx = CTransaction()
        tx.vin.append(CTxIn(COutPoint(self.block1.vtx[0].sha256, 0), scriptSig=b""))
        tx.vout.append(CTxOut(self.block1.vtx[0].vout[0].nValue, CScript([OP_TRUE])))
        prove_tx_pow(tx, anchor_height=height - 1)

        block102 = create_block(self.tip, create_coinbase(height), self.block_time, txlist=[tx])
        self.block_time += 1
        block102.solve()
        self.blocks.append(block102)
        self.tip = block102.sha256
        self.block_time += 1
        height += 1

        # Bury the assumed valid block more than two weeks deep under
        # Quicksilver's 5-minute target spacing.
        for _ in range(ASSUMEVALID_BURY_BLOCKS):
            block = create_block(self.tip, create_coinbase(height), self.block_time)
            block.solve()
            self.blocks.append(block)
            self.tip = block.sha256
            self.block_time += 1
            height += 1

        # Start node1 and node2 with assumevalid so they accept a block with a bad signature.
        self.start_node(1, extra_args=self.extra_args[1] + ["-assumevalid=" + block102.hash])
        self.start_node(2, extra_args=self.extra_args[2] + ["-assumevalid=" + block102.hash])

        p2p0 = self.nodes[0].add_p2p_connection(BaseNode())
        self.send_all_headers(p2p0)

        # Send blocks to node0. Block 102 will be rejected.
        self.send_blocks_until_disconnected(p2p0)
        self.wait_until(lambda: self.nodes[0].getblockcount() >= COINBASE_MATURITY + 1)
        assert_equal(self.nodes[0].getblockcount(), COINBASE_MATURITY + 1)

        p2p1 = self.nodes[1].add_p2p_connection(BaseNode())
        self.send_all_headers(p2p1)

        # Send all blocks to node1. All blocks will be accepted.
        for block in self.blocks:
            p2p1.send_message(msg_block(block))
        # Syncing 4302 blocks can take a while on slow systems. Give it plenty of time to sync.
        p2p1.sync_with_ping(timeout=960)
        assert_equal(self.nodes[1].getblock(self.nodes[1].getbestblockhash())['height'], len(self.blocks))

        p2p2 = self.nodes[2].add_p2p_connection(BaseNode())
        p2p2.send_header_for_blocks(self.blocks[0:200])

        # Send blocks to node2. Block 102 will be rejected.
        self.send_blocks_until_disconnected(p2p2)
        self.wait_until(lambda: self.nodes[2].getblockcount() >= COINBASE_MATURITY + 1)
        assert_equal(self.nodes[2].getblockcount(), COINBASE_MATURITY + 1)


if __name__ == '__main__':
    AssumeValidTest(__file__).main()
