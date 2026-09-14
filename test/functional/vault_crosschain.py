#!/usr/bin/env python3
# Copyright (c) 2020-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_raises_rpc_error

class VaultCrossChain(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()

    def setup_network(self):
        self.add_nodes(self.num_nodes)

        # Switch node 1 to publictest before starting it.
        self.nodes[1].chain = 'publictest'
        self.nodes[1].extra_args = ['-maxconnections=0', '-prune=550'] # disable publictest sync
        self.nodes[1].replace_in_config([('sandbox=', 'publictest='), ('[sandbox]', '[publictest]')])

        self.start_nodes()

    def run_test(self):
        self.log.info("Creating vaults")

        node0_vault = self.nodes[0].datadir_path / 'node0_vault'
        node0_vault_backup = self.nodes[0].datadir_path / 'node0_vault.bak'
        self.nodes[0].createvault(node0_vault)
        self.nodes[0].backupvault(node0_vault_backup)
        self.nodes[0].unloadvault(node0_vault)
        node1_vault = self.nodes[1].datadir_path / 'node1_vault'
        node1_vault_backup = self.nodes[0].datadir_path / 'node1_vault.bak'
        self.nodes[1].createvault(node1_vault)
        self.nodes[1].backupvault(node1_vault_backup)
        self.nodes[1].unloadvault(node1_vault)
        self.log.info("Loading/restoring vaults into nodes with a different genesis block")

        assert_raises_rpc_error(-18, 'Vault file verification failed.', self.nodes[0].loadvault, node1_vault)
        assert_raises_rpc_error(-18, 'Vault file verification failed.', self.nodes[1].loadvault, node0_vault)
        assert_raises_rpc_error(-18, 'Vault file verification failed.', self.nodes[0].restorevault, 'w', node1_vault_backup)
        assert_raises_rpc_error(-18, 'Vault file verification failed.', self.nodes[1].restorevault, 'w', node0_vault_backup)


if __name__ == '__main__':
    VaultCrossChain(__file__).main()
