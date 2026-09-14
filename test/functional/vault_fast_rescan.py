#!/usr/bin/env python3
# Copyright (c) 2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that fast rescan using block filters for vaults detects top-ups correctly
   and finds the same transactions as the slow variant.

The second half is the differential guard for block-filter correctness. Filters are
outside consensus -- nothing commits a filter header to a block and no peer validates
one -- so a wrong filter never forks the chain; it silently breaks each node on its own.
CVault::ScanForVaultTransactions skips a block entirely when the filter reports no match,
so a filter false negative leaves a transaction absent from the vault while the rescan
reports success.

The slow path answers the same question -- "does this block hold anything of mine?" --
by direct script matching, which makes it an independent second implementation and a
genuine oracle rather than a self-agreement check.

For that oracle to see anything, the chain has to exercise the branches of
BasicFilterElements (src/blockfilter.cpp) that decide set membership:

  * a spend whose only trace in the block is the prevout script in undo data,
  * an output script the filter deliberately skips because it is empty,
  * an output script the filter deliberately skips because it starts with OP_RETURN,
  * a bare multisig output, so the vault-owned element is not always P2WPKH.

MUTATION TEST -- this test is worthless unless it fails when the code is broken. Drop the
block_undo loop from BasicFilterElements and check_undo_data_differential() must fail. If
it still passes, the seeded chain no longer contains a spend and the oracle has gone
blind.
"""
from test_framework.address import address_to_scriptpubkey
from test_framework.descriptors import descsum_create
from test_framework.script import CScript, OP_RETURN
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.test_node import TestNode
from test_framework.util import assert_equal
from test_framework.vault import MiniVault, getnewdestination
from test_framework.vault_util import get_generate_key, get_multisig


KEYPOOL_SIZE = 100   # smaller than default size to speed-up test
NUM_DESCRIPTORS = 7  # number of descriptors (6 default ranged ones + 1 fixed non-ranged one)
NUM_BLOCKS = 6       # number of blocks to mine


class VaultFastRescanTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 1
        # -acceptnonstdtxn lets the chain carry the empty and OP_RETURN output scripts
        # that BasicFilterElements skips; without it they cannot be broadcast at all.
        self.extra_args = [[f'-keypool={KEYPOOL_SIZE}', '-blockfilterindex=1', '-txpownocycle=1',
                            '-acceptnonstdtxn=1', '-permitbaremultisig=1']]

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()
        self.skip_if_no_sqlite()

    def get_vault_txids(self, node: TestNode, vault_name: str) -> list[str]:
        w = node.get_vault_rpc(vault_name)
        txs = w.listtransactions('*', 1000000)
        return [tx['txid'] for tx in txs]

    def seed_undo_data_chain(self, node: TestNode, vault: MiniVault) -> dict:
        """Mine the two blocks the differential guard needs, and return what must be found.

        Block A carries a P2WPKH output and a bare multisig output the watch-only vault
        owns, next to the two output scripts BasicFilterElements refuses to add to the
        filter (empty, and OP_RETURN-prefixed).

        Block B carries a spend of the P2WPKH coin with no change and no output paying
        anything the vault owns. The only record of the vault in that block is the prevout
        script recovered from undo data, so the block matches the filter through the
        block_undo loop or not at all.
        """
        key = get_generate_key()
        multisig = get_multisig(node)
        wpkh_spk = bytes.fromhex(key.p2wpkh_script)
        # get_multisig's redeem_script is the bare multisig script itself; paying to it
        # directly (rather than to its P2SH or P2WSH wrapper) is the bare multisig case.
        bare_multisig_spk = bytes.fromhex(multisig.redeem_script)

        self.log.info("Block A: vault-owned outputs beside the scripts the filter skips")
        fund_txid = vault.send_to(from_node=node, scriptPubKey=wpkh_spk, amount=100000)['txid']
        multisig_txid = vault.send_to(from_node=node, scriptPubKey=bare_multisig_spk, amount=100000)['txid']
        # Zero-valued: both scripts are unspendable, and sendrawtransaction refuses to
        # burn value into them (maxburnamount). Value is irrelevant here anyway -- the
        # filter's skip branches key on the script, not the amount.
        vault.send_to(from_node=node, scriptPubKey=CScript(), amount=0)
        vault.send_to(from_node=node, scriptPubKey=CScript([OP_RETURN, b'quicksilver']), amount=0)
        self.generate(node, 1)

        self.log.info("Block B: spend the P2WPKH coin, leaving nothing but undo data behind")
        node.createvault(vault_name='undo_spender', blank=True)
        spender = node.get_vault_rpc('undo_spender')
        spender.importdescriptors([{"desc": descsum_create(f"wpkh({key.privkey})"), "timestamp": 0}])
        _, _, external_addr = getnewdestination(address_type='bech32')
        spend_txid = spender.sendall([external_addr])['txid']
        # sendall spends every confirmed UTXO into the one recipient, so on a feeless chain
        # there is no change output. Assert that rather than trust it: the moment this block
        # gains an output paying something the vault owns, the block matches on the output
        # loop alone, the undo data stops being load-bearing, and the guard below goes blind
        # while still passing.
        spend_tx = node.getrawtransaction(spend_txid, 1)
        assert_equal(len(spend_tx['vout']), 1)
        assert_equal(spend_tx['vout'][0]['output_script']['address'], external_addr)
        self.generate(node, 1)
        node.unloadvault('undo_spender')

        self.log.info(f"  fund={fund_txid} multisig={multisig_txid} spend={spend_txid}")
        return {
            'descriptors': [
                {"desc": descsum_create(f"wpkh({key.pubkey})"), "timestamp": 0},
                {"desc": descsum_create("multi(2,{},{},{})".format(*multisig.pubkeys)), "timestamp": 0},
            ],
            # The recorded expected value. Comparing the two variants to each other is not
            # enough on its own: a blind spot shared by both paths would compare equal.
            'expected_txids': {fund_txid, multisig_txid, spend_txid},
        }

    def import_undo_data_vault(self, node: TestNode, vault_name: str, seeded: dict, variant: str) -> set:
        """Rescan the seeded chain into a fresh watch-only vault and return what it found."""
        node.createvault(vault_name=vault_name, disable_private_keys=True, blank=True)
        with node.assert_debug_log([f'variant={variant}']):
            node.get_vault_rpc(vault_name).importdescriptors(seeded['descriptors'])
        return set(self.get_vault_txids(node, vault_name))

    def run_test(self):
        node = self.nodes[0]
        vault = MiniVault(node)

        self.log.info("Create vault with backup")
        VAULT_BACKUP_FILENAME = node.datadir_path / 'vault.bak'
        node.createvault(vault_name='topup_test')
        w = node.get_vault_rpc('topup_test')
        fixed_key = get_generate_key()
        print(w.importdescriptors([{"desc": descsum_create(f"wpkh({fixed_key.privkey})"), "timestamp": "now"}]))
        descriptors = w.listdescriptors()['descriptors']
        assert_equal(len(descriptors), NUM_DESCRIPTORS)
        w.backupvault(VAULT_BACKUP_FILENAME)

        self.log.info("Create txs sending to end range address of each descriptor, triggering top-ups")
        for i in range(NUM_BLOCKS):
            self.log.info(f"Block {i+1}/{NUM_BLOCKS}")
            for desc_info in w.listdescriptors()['descriptors']:
                if 'range' in desc_info:
                    start_range, end_range = desc_info['range']
                    addr = w.deriveaddresses(desc_info['desc'], [end_range, end_range])[0]
                    spk = address_to_scriptpubkey(addr)
                    self.log.info(f"-> range [{start_range},{end_range}], last address {addr}")
                else:
                    spk = bytes.fromhex(fixed_key.p2wpkh_script)
                    self.log.info(f"-> fixed non-range descriptor address {fixed_key.p2wpkh_addr}")
                vault.send_to(from_node=node, scriptPubKey=spk, amount=10000)
            self.generate(node, 1)

        self.log.info("Seed the chain the differential guard needs (see module docstring)")
        seeded = self.seed_undo_data_chain(node, vault)

        self.log.info("Import vault backup with block filter index")
        with node.assert_debug_log(['variant=fast']):
            node.restorevault('rescan_fast', VAULT_BACKUP_FILENAME)
        txids_fast = self.get_vault_txids(node, 'rescan_fast')

        self.log.info("Import non-active descriptors with block filter index")
        node.createvault(vault_name='rescan_fast_nonactive', disable_private_keys=True, blank=True)
        with node.assert_debug_log(['variant=fast']):
            w = node.get_vault_rpc('rescan_fast_nonactive')
            w.importdescriptors([{"desc": descriptor['desc'], "timestamp": 0} for descriptor in descriptors])
        txids_fast_nonactive = self.get_vault_txids(node, 'rescan_fast_nonactive')

        self.log.info("Rescan the undo-data chain with block filter index")
        undo_fast = self.import_undo_data_vault(node, 'undo_fast', seeded, 'fast')

        self.restart_node(0, [f'-keypool={KEYPOOL_SIZE}', '-blockfilterindex=0', '-txpownocycle=1',
                              '-acceptnonstdtxn=1', '-permitbaremultisig=1'])
        self.log.info("Import vault backup w/o block filter index")
        with node.assert_debug_log(['variant=slow']):
            node.restorevault("rescan_slow", VAULT_BACKUP_FILENAME)
        txids_slow = self.get_vault_txids(node, 'rescan_slow')

        self.log.info("Import non-active descriptors w/o block filter index")
        node.createvault(vault_name='rescan_slow_nonactive', disable_private_keys=True, blank=True)
        with node.assert_debug_log(['variant=slow']):
            w = node.get_vault_rpc('rescan_slow_nonactive')
            w.importdescriptors([{"desc": descriptor['desc'], "timestamp": 0} for descriptor in descriptors])
        txids_slow_nonactive = self.get_vault_txids(node, 'rescan_slow_nonactive')

        self.log.info("Rescan the undo-data chain w/o block filter index")
        undo_slow = self.import_undo_data_vault(node, 'undo_slow', seeded, 'slow')

        self.log.info("Verify that all rescans found the same txs in slow and fast variants")
        assert_equal(len(txids_slow), NUM_DESCRIPTORS * NUM_BLOCKS)
        assert_equal(len(txids_fast), NUM_DESCRIPTORS * NUM_BLOCKS)
        assert_equal(len(txids_slow_nonactive), NUM_DESCRIPTORS * NUM_BLOCKS)
        assert_equal(len(txids_fast_nonactive), NUM_DESCRIPTORS * NUM_BLOCKS)
        assert_equal(sorted(txids_slow), sorted(txids_fast))
        assert_equal(sorted(txids_slow_nonactive), sorted(txids_fast_nonactive))

        self.log.info("Verify the fast path did not skip a block it could only match through undo data")
        # Both halves matter. The equality is the differential oracle -- the slow path is an
        # independent implementation of the same question. The comparison against the
        # recorded set is what stops the oracle passing vacuously if both paths go blind.
        self.log.info(f"  slow found {sorted(undo_slow)}")
        self.log.info(f"  fast found {sorted(undo_fast)}")
        assert_equal(undo_slow, seeded['expected_txids'])
        assert_equal(undo_fast, undo_slow)


if __name__ == '__main__':
    VaultFastRescanTest(__file__).main()
