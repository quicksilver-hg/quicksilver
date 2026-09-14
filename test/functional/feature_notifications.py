#!/usr/bin/env python3
# Copyright (c) 2014-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the -alertnotify, -blocknotify and -vaultnotify options."""
import os
import platform

from test_framework.address import ADDRESS_SHG1_UNSPENDABLE
from test_framework.descriptors import descsum_create
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal,
)

# Linux allow all characters other than \x00
# Windows disallow control characters (0-31) and /\?%:|"<>
FILE_CHAR_START = 32 if platform.system() == 'Windows' else 1
FILE_CHAR_END = 128
FILE_CHARS_DISALLOWED = '/\\?%*:|"<>' if platform.system() == 'Windows' else '/'
UNCONFIRMED_HASH_STRING = 'unconfirmed'

def notify_outputname(vaultname, txid):
    return txid if platform.system() == 'Windows' else f'{vaultname}_{txid}'


class NotificationsTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True

    def setup_network(self):
        self.vault = ''.join(chr(i) for i in range(FILE_CHAR_START, FILE_CHAR_END) if chr(i) not in FILE_CHARS_DISALLOWED)
        self.alertnotify_dir = os.path.join(self.options.tmpdir, "alertnotify")
        self.blocknotify_dir = os.path.join(self.options.tmpdir, "blocknotify")
        self.vaultnotify_dir = os.path.join(self.options.tmpdir, "vaultnotify")
        self.shutdownnotify_dir = os.path.join(self.options.tmpdir, "shutdownnotify")
        self.shutdownnotify_file = os.path.join(self.shutdownnotify_dir, "shutdownnotify.txt")
        os.mkdir(self.alertnotify_dir)
        os.mkdir(self.blocknotify_dir)
        os.mkdir(self.vaultnotify_dir)
        os.mkdir(self.shutdownnotify_dir)

        # -alertnotify and -blocknotify on node0, vaultnotify on node1
        self.extra_args = [[
            f"-alertnotify=echo > {os.path.join(self.alertnotify_dir, '%s')}",
            f"-blocknotify=echo > {os.path.join(self.blocknotify_dir, '%s')}",
            f"-shutdownnotify=echo > {self.shutdownnotify_file}",
        ], [
            f"-vaultnotify=echo %h_%b > {os.path.join(self.vaultnotify_dir, notify_outputname('%w', '%s'))}",
        ]]
        self.vault_names = [self.default_vault_name, self.vault]
        super().setup_network()

    def run_test(self):
        if self.is_vault_compiled():
            # Setup the descriptors to be imported to the vault
            xpriv = "sqrv1wkfAGt6m8urrG6yVov7wQPehYVJZJcRzZecBjZBF7HkEzqkRXX5TNBkkJZGvKGG3dwBV1VQN2Y4CPNdx2ewwy3bs5MPtEUejkvaWzSh1wr"
            desc_imports = [{
                "desc": descsum_create(f"wpkh({xpriv}/0/*)"),
                "timestamp": 0,
                "active": True,
                "keypool": True,
            },{
                "desc": descsum_create(f"wpkh({xpriv}/1/*)"),
                "timestamp": 0,
                "active": True,
                "keypool": True,
                "internal": True,
            }]
            # Make the vaults and import the descriptors
            # Ensures that node 0 and node 1 share the same vault for the conflicting transaction tests below.
            for i, name in enumerate(self.vault_names):
                self.nodes[i].createvault(vault_name=name, blank=True, load_on_startup=True)
                self.nodes[i].importdescriptors(desc_imports)

        self.log.info("test -blocknotify")
        block_count = 10
        blocks = self.generatetoaddress(self.nodes[1], block_count, self.nodes[1].getnewaddress() if self.is_vault_compiled() else ADDRESS_SHG1_UNSPENDABLE)

        # wait at most 10 seconds for expected number of files before reading the content
        self.wait_until(lambda: len(os.listdir(self.blocknotify_dir)) == block_count, timeout=10)

        # directory content should equal the generated blocks hashes
        assert_equal(sorted(blocks), sorted(os.listdir(self.blocknotify_dir)))

        if self.is_vault_compiled():
            self.log.info("test -vaultnotify")
            # wait at most 10 seconds for expected number of files before reading the content
            self.wait_until(lambda: len(os.listdir(self.vaultnotify_dir)) == block_count, timeout=10)

            # directory content should equal the generated transaction hashes
            tx_details = list(map(lambda t: (t['txid'], t['blockheight'], t['blockhash']), self.nodes[1].listtransactions("*", block_count)))
            self.expect_vault_notify(tx_details)

            self.log.info("test -vaultnotify after rescan")
            # rescan to force vault notifications
            self.nodes[1].rescanblockchain()
            self.wait_until(lambda: len(os.listdir(self.vaultnotify_dir)) == block_count, timeout=10)

            self.connect_nodes(0, 1)

            # directory content should equal the generated transaction hashes
            tx_details = list(map(lambda t: (t['txid'], t['blockheight'], t['blockhash']), self.nodes[1].listtransactions("*", block_count)))
            self.expect_vault_notify(tx_details)

            # Conflicting transactions tests.
            # Generate spends from node 0, and check notifications
            # triggered by node 1
            self.log.info("test -vaultnotify with conflicting transactions")
            self.nodes[0].rescanblockchain()
            self.generatetoaddress(self.nodes[0], 100, ADDRESS_SHG1_UNSPENDABLE)

            # Generate transaction on node 0, sync relaypools, and check for
            # notification on node 1.
            tx1 = self.nodes[0].sendtoaddress(address=ADDRESS_SHG1_UNSPENDABLE, amount=1)
            assert_equal(tx1 in self.nodes[0].getrawrelaypool(), True)
            self.sync_relaypools()
            self.expect_vault_notify([(tx1, -1, UNCONFIRMED_HASH_STRING)])

            # Add tx1 transaction to new block, checking for a notification
            # and the correct number of confirmations.
            blockhash1 = self.generatetoaddress(self.nodes[0], 1, ADDRESS_SHG1_UNSPENDABLE)[0]
            blockheight1 = self.nodes[0].getblockcount()
            self.sync_blocks()
            self.expect_vault_notify([(tx1, blockheight1, blockhash1)])
            assert_equal(self.nodes[1].gettransaction(tx1)["confirmations"], 1)

        # TODO: add test for `-alertnotify` large fork notifications

        self.log.info("test -shutdownnotify")
        self.stop_nodes()
        self.wait_until(lambda: os.path.isfile(self.shutdownnotify_file), timeout=10)

    def expect_vault_notify(self, tx_details):
        self.wait_until(lambda: len(os.listdir(self.vaultnotify_dir)) >= len(tx_details), timeout=10)
        # Should have no more and no less files than expected
        assert_equal(sorted(notify_outputname(self.vault, tx_id) for tx_id, _, _ in tx_details), sorted(os.listdir(self.vaultnotify_dir)))
        # Should now verify contents of each file
        for tx_id, blockheight, blockhash in tx_details:
            fname = os.path.join(self.vaultnotify_dir, notify_outputname(self.vault, tx_id))
            # Wait for the cached writes to hit storage
            self.wait_until(lambda: os.path.getsize(fname) > 0, timeout=10)
            with open(fname, 'rt', encoding='utf-8') as f:
                text = f.read()
                # Universal newline ensures '\n' on 'nt'
                assert_equal(text[-1], '\n')
                text = text[:-1]
                if platform.system() == 'Windows':
                    # On Windows, echo as above will append a whitespace
                    assert_equal(text[-1], ' ')
                    text = text[:-1]
                expected = str(blockheight) + '_' + blockhash
                assert_equal(text, expected)

        for tx_file in os.listdir(self.vaultnotify_dir):
            os.remove(os.path.join(self.vaultnotify_dir, tx_file))


if __name__ == '__main__':
    NotificationsTest(__file__).main()
