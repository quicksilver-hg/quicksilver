#!/usr/bin/env python3
# Copyright (c) 2024-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

from decimal import Decimal

from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.messages import COIN
from test_framework.vault import MiniVault, MiniVaultMode, getnewdestination


class GetBlocksActivityTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # MiniVault attaches a cheap trivial-cycle per-tx PoW that clears the
        # proof-hash target but is not a real Cuckatoo cycle; enable the sandbox
        # seam that skips full-cycle verification (anchor recency + target still
        # enforced), matching the other MiniVault-based functional tests.
        self.extra_args = [["-txpownocycle=1"]] * self.num_nodes

    def run_test(self):
        node = self.nodes[0]
        vault = MiniVault(node)
        node.setmocktime(node.getblockheader(node.getbestblockhash())['time'])
        self.generate(vault, 200)

        self.test_no_activity(node)
        self.test_activity_in_block(node, vault)
        self.test_no_relaypool_inclusion(node, vault)
        self.test_multiple_addresses(node, vault)
        self.test_invalid_blockhash(node, vault)
        self.test_invalid_descriptor(node, vault)
        self.test_confirmed_and_unconfirmed(node, vault)
        self.test_receive_then_spend(node, vault)
        self.test_no_address(node, vault)

    def test_no_activity(self, node):
        _, _, addr_1 = getnewdestination()
        result = node.getdescriptoractivity([], [f"addr({addr_1})"], True)
        assert_equal(len(result['activity']), 0)

    def test_activity_in_block(self, node, vault):
        _, spk_1, addr_1 = getnewdestination(address_type='bech32m')
        txid = vault.send_to(from_node=node, scriptPubKey=spk_1, amount=1 * COIN)['txid']
        blockhash = self.generate(node, 1)[0]

        # Test getdescriptoractivity with the specific blockhash
        result = node.getdescriptoractivity([blockhash], [f"addr({addr_1})"], True)
        assert_equal(list(result.keys()), ['activity'])
        [activity] = result['activity']

        for k, v in {
                'amount': Decimal('1.00000000'),
                'blockhash': blockhash,
                'height': 201,
                'txid': txid,
                'type': 'receive',
                'vout': 1,
        }.items():
            assert_equal(activity[k], v)

        outspk = activity['output_spk']

        assert_equal(outspk['asm'][:2], '1 ')
        assert_equal(outspk['desc'].split('(')[0], 'rawtr')
        assert_equal(outspk['hex'], spk_1.hex())
        assert_equal(outspk['address'], addr_1)
        assert_equal(outspk['type'], 'witness_v1_taproot')


    def test_no_relaypool_inclusion(self, node, vault):
        _, spk_1, addr_1 = getnewdestination()
        vault.send_to(from_node=node, scriptPubKey=spk_1, amount=1 * COIN)

        _, spk_2, addr_2 = getnewdestination()
        vault.send_to(
            from_node=node, scriptPubKey=spk_2, amount=1 * COIN)

        # Do not generate a block to keep the transaction in the relaypool

        result = node.getdescriptoractivity([], [f"addr({addr_1})", f"addr({addr_2})"], False)

        assert_equal(len(result['activity']), 0)

    def test_multiple_addresses(self, node, vault):
        _, spk_1, addr_1 = getnewdestination()
        _, spk_2, addr_2 = getnewdestination()
        vault.send_to(from_node=node, scriptPubKey=spk_1, amount=1 * COIN)
        vault.send_to(from_node=node, scriptPubKey=spk_2, amount=2 * COIN)

        blockhash = self.generate(node, 1)[0]

        result = node.getdescriptoractivity([blockhash], [f"addr({addr_1})", f"addr({addr_2})"], True)

        assert_equal(len(result['activity']), 2)

        # Duplicate address specification is fine.
        assert_equal(
            result,
            node.getdescriptoractivity([blockhash], [
                f"addr({addr_1})", f"addr({addr_1})", f"addr({addr_2})"], True))

        # Flipping descriptor order doesn't affect results.
        result_flipped = node.getdescriptoractivity(
            [blockhash], [f"addr({addr_2})", f"addr({addr_1})"], True)
        assert_equal(result, result_flipped)

        [a1] = [a for a in result['activity'] if a['output_spk']['address'] == addr_1]
        [a2] = [a for a in result['activity'] if a['output_spk']['address'] == addr_2]

        assert a1['blockhash'] == blockhash
        assert a1['amount'] == 1.0

        assert a2['blockhash'] == blockhash
        assert a2['amount'] == 2.0

    def test_invalid_blockhash(self, node, vault):
        self.generate(node, 20) # Generate more mature coinbase

        _, spk_1, addr_1 = getnewdestination()
        vault.send_to(from_node=node, scriptPubKey=spk_1, amount=1 * COIN)

        invalid_blockhash = "0000000000000000000000000000000000000000000000000000000000000000"

        assert_raises_rpc_error(
            -5, "Block not found",
            node.getdescriptoractivity, [invalid_blockhash], [f"addr({addr_1})"], True)

    def test_invalid_descriptor(self, node, vault):
        blockhash = self.generate(node, 1)[0]
        _, _, addr_1 = getnewdestination()

        assert_raises_rpc_error(
            -5, "is not a valid descriptor",
            node.getdescriptoractivity, [blockhash], [f"addrx({addr_1})"], True)

    def test_confirmed_and_unconfirmed(self, node, vault):
        self.generate(node, 20) # Generate more mature coinbase

        _, spk_1, addr_1 = getnewdestination()
        txid_1 = vault.send_to(
            from_node=node, scriptPubKey=spk_1, amount=1 * COIN)['txid']
        blockhash = self.generate(node, 1)[0]

        _, spk_2, to_addr = getnewdestination()
        txid_2 = vault.send_to(
            from_node=node, scriptPubKey=spk_2, amount=1 * COIN)['txid']

        result = node.getdescriptoractivity(
            [blockhash], [f"addr({addr_1})", f"addr({to_addr})"], True)

        activity = result['activity']
        assert_equal(len(activity), 2)

        [confirmed] = [a for a in activity if a.get('blockhash') == blockhash]
        assert confirmed['txid'] == txid_1
        assert confirmed['height'] == node.getblockchaininfo()['blocks']

        [unconfirmed] = [a for a in activity if not a.get('blockhash')]
        assert 'blockhash' not in unconfirmed
        assert 'height' not in unconfirmed

        assert any(a['txid'] == txid_2 for a in activity if not a.get('blockhash'))

    def test_receive_then_spend(self, node, vault):
        """Also important because this tests multiple blockhashes."""
        self.generate(node, 20) # Generate more mature coinbase

        sent1 = vault.send_self_transfer(from_node=node)
        utxo = sent1['new_utxo']
        blockhash_1 = self.generate(node, 1)[0]

        sent2 = vault.send_self_transfer(from_node=node, utxo_to_spend=utxo)
        blockhash_2 = self.generate(node, 1)[0]

        result = node.getdescriptoractivity(
            [blockhash_1, blockhash_2], [vault.get_descriptor()], True)

        assert_equal(len(result['activity']), 4)

        assert result['activity'][1]['type'] == 'receive'
        assert result['activity'][1]['txid'] == sent1['txid']
        assert result['activity'][1]['blockhash'] == blockhash_1

        assert result['activity'][2]['type'] == 'spend'
        assert result['activity'][2]['spend_txid'] == sent2['txid']
        assert result['activity'][2]['spend_vin'] == 0
        assert result['activity'][2]['prevout_txid'] == sent1['txid']
        assert result['activity'][2]['blockhash'] == blockhash_2

        # Test that reversing the blockorder yields the same result.
        assert_equal(result, node.getdescriptoractivity(
            [blockhash_1, blockhash_2], [vault.get_descriptor()], True))

        # Test that duplicating a blockhash yields the same result.
        assert_equal(result, node.getdescriptoractivity(
            [blockhash_1, blockhash_2, blockhash_2], [vault.get_descriptor()], True))

    def test_no_address(self, node, vault):
        raw_vault = MiniVault(self.nodes[0], mode=MiniVaultMode.RAW_P2PK)
        self.generate(raw_vault, 100)

        no_addr_tx = raw_vault.send_self_transfer(from_node=node)
        raw_desc = raw_vault.get_descriptor()

        blockhash = self.generate(node, 1)[0]

        result = node.getdescriptoractivity([blockhash], [raw_desc], False)

        assert_equal(len(result['activity']), 2)

        a1 = result['activity'][0]
        a2 = result['activity'][1]

        assert a1['type'] == "spend"
        assert a1['blockhash'] == blockhash
        # sPK lacks address.
        assert_equal(list(a1['prevout_spk'].keys()), ['asm', 'desc', 'hex', 'type'])
        assert "fee" not in no_addr_tx
        assert a1['amount'] == Decimal(no_addr_tx["tx"].vout[0].nValue) / COIN

        assert a2['type'] == "receive"
        assert a2['blockhash'] == blockhash
        # sPK lacks address.
        assert_equal(list(a2['output_spk'].keys()), ['asm', 'desc', 'hex', 'type'])
        assert a2['amount'] == Decimal(no_addr_tx["tx"].vout[0].nValue) / COIN


if __name__ == '__main__':
    GetBlocksActivityTest(__file__).main()
