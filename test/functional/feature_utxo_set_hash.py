#!/usr/bin/env python3
# Copyright (c) 2020-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test UTXO set hash value calculation in gettxoutsetinfo."""

from test_framework.messages import (
    CBlock,
    COutPoint,
    from_hex,
)
from test_framework.crypto.muhash import MuHash3072
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal
from test_framework.vault import MiniVault

class UTXOSetHashTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-txpownocycle=1"]]

    def test_muhash_implementation(self):
        self.log.info("Test MuHash implementation consistency")

        node = self.nodes[0]
        vault = MiniVault(node)
        mocktime = node.getblockheader(node.getblockhash(0))['time'] + 1
        node.setmocktime(mocktime)

        # Generate 100 blocks and remove the first since we plan to spend its
        # coinbase
        block_hashes = self.generate(vault, 1) + self.generate(node, 99)
        blocks = list(map(lambda block: from_hex(CBlock(), node.getblock(block, 0)), block_hashes))
        blocks.pop(0)

        # Create a spending transaction and mine a block which includes it
        txid = vault.send_self_transfer(from_node=node)['txid']
        tx_block = self.generateblock(node, output=vault.get_address(), transactions=[txid])
        blocks.append(from_hex(CBlock(), node.getblock(tx_block['hash'], 0)))

        # Serialize the outputs that should be in the UTXO set and add them to
        # a MuHash object
        muhash = MuHash3072()

        for height, block in enumerate(blocks):
            # The Genesis block coinbase is not part of the UTXO set and we
            # spent the first mined block
            height += 2

            for tx in block.vtx:
                for n, tx_out in enumerate(tx.vout):
                    coinbase = 1 if not tx.vin[0].prevout.hash else 0

                    # Skip witness commitment
                    if (coinbase and n > 0):
                        continue

                    data = COutPoint(int(tx.rehash(), 16), n).serialize()
                    data += (height * 2 + coinbase).to_bytes(4, "little")
                    data += tx_out.serialize()

                    muhash.insert(data)

        finalized = muhash.digest()
        node_muhash = node.gettxoutsetinfo("muhash")['muhash']

        assert_equal(finalized[::-1].hex(), node_muhash)

        self.log.info("Test deterministic UTXO set hash results")
        assert_equal(node.gettxoutsetinfo()['muhash'], "f50af2bb33b6d99515d1cd13bb4ed8cefeeaafa4fa10c56589c390aa9141c792")
        assert_equal(node.gettxoutsetinfo("muhash")['muhash'], "f50af2bb33b6d99515d1cd13bb4ed8cefeeaafa4fa10c56589c390aa9141c792")

    def run_test(self):
        self.test_muhash_implementation()


if __name__ == '__main__':
    UTXOSetHashTest(__file__).main()
