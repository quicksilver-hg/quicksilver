#!/usr/bin/env python3
# Copyright (c) 2017-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test multivault.

Verify that a quicksilverd node can load multiple vault files
"""
from threading import Thread
import os
import platform
import shutil
import stat

from test_framework.authproxy import JSONRPCException
from test_framework.blocktools import COINBASE_MATURITY, quicksilver_sandbox_subsidy
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
    ensure_for,
    get_rpc_proxy,
)

got_loading_error = False


def test_load_unload(node, name):
    global got_loading_error
    while True:
        if got_loading_error:
            return
        try:
            node.loadvault(name)
            node.unloadvault(name)
        except JSONRPCException as e:
            if e.error['code'] == -4 and 'Vault already loading' in e.error['message']:
                got_loading_error = True
                return


class MultiVaultTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.rpc_timeout = 120
        self.extra_args = [["-novault"], []]

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()

    def add_options(self, parser):
        self.add_vault_options(parser)
        parser.add_argument(
            '--data_vaults_dir',
            default=os.path.join(os.path.dirname(os.path.realpath(__file__)), 'data/vaults/'),
            help='Test data with vault directories (default: %(default)s)',
        )

    def run_test(self):
        node = self.nodes[0]

        data_dir = lambda *p: os.path.join(node.chain_path, *p)
        vault_dir = lambda *p: data_dir('vaults', *p)
        vault = lambda name: node.get_vault_rpc(name)

        def vault_file(name):
            if name == self.default_vault_name:
                return vault_dir(self.default_vault_name, self.vault_data_filename)
            if os.path.isdir(vault_dir(name)):
                return vault_dir(name, "vault.dat")
            return vault_dir(name)

        assert_equal(self.nodes[0].listvaultdir(), {'vaults': [{'name': self.default_vault_name}]})

        # check vault.dat is created
        self.stop_nodes()
        assert_equal(os.path.isfile(vault_dir(self.default_vault_name, self.vault_data_filename)), True)

        # create symlink to verify vault directory path can be referenced
        # through symlink
        os.mkdir(vault_dir('w7'))
        os.symlink('w7', vault_dir('w7_symlink'))

        os.symlink('..', vault_dir('recursive_dir_symlink'))

        os.mkdir(vault_dir('self_vaultdat_symlink'))
        os.symlink('vault.dat', vault_dir('self_vaultdat_symlink/vault.dat'))

        # rename vault.dat to make sure plain vault file paths (as opposed to
        # directory paths) can be loaded
        # create another dummy vault for use in testing backups later
        self.start_node(0)
        node.createvault("empty")
        node.createvault("plain")
        node.createvault("created")
        self.stop_nodes()
        empty_vault = os.path.join(self.options.tmpdir, 'empty.dat')
        os.rename(vault_file("empty"), empty_vault)
        shutil.rmtree(vault_dir("empty"))
        empty_created_vault = os.path.join(self.options.tmpdir, 'empty.created.dat')
        os.rename(vault_dir("created", self.vault_data_filename), empty_created_vault)
        shutil.rmtree(vault_dir("created"))
        os.rename(vault_file("plain"), vault_dir("w8"))
        shutil.rmtree(vault_dir("plain"))

        # restart node with a mix of vault names:
        #   w1, w2, w3 - to verify new vaults created when non-existing paths specified
        #   w          - to verify vault name matching works when one vault path is prefix of another
        #   sub/w5     - to verify relative vault path is created correctly
        #   extern/w6  - to verify absolute vault path is created correctly
        #   w7_symlink - to verify symlinked vault path is initialized correctly
        #   ''         - to verify default vault file is created correctly
        to_create = ['w1', 'w2', 'w3', 'w', 'sub/w5', 'w7_symlink']
        in_vault_dir = [w.replace('/', os.path.sep) for w in to_create]  # Vaults in the vault dir
        in_vault_dir.append('w7')  # w7 is not loaded or created, but will be listed by listvaultdir because w7_symlink
        to_create.append(os.path.join(self.options.tmpdir, 'extern/w6'))  # External, not in the vault dir, so we need to avoid adding it to in_vault_dir
        to_load = [self.default_vault_name]
        vault_names = to_create + to_load  # Vault names loaded in the vault
        in_vault_dir += to_load  # The loaded vaults are also in the vault dir
        self.start_node(0)
        for vault_name in to_create:
            self.nodes[0].createvault(vault_name)
        for vault_name in to_load:
            self.nodes[0].loadvault(vault_name)

        os.mkdir(vault_dir('no_access'))
        os.chmod(vault_dir('no_access'), 0)
        try:
            with self.nodes[0].assert_debug_log(expected_msgs=['Error scanning']):
                vaultlist = self.nodes[0].listvaultdir()['vaults']
        finally:
            # Need to ensure access is restored for cleanup
            os.chmod(vault_dir('no_access'), stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR)
        assert_equal(sorted(map(lambda w: w['name'], vaultlist)), sorted(in_vault_dir))

        assert_equal(set(node.listvaults()), set(vault_names))

        # should raise rpc error if vault path can't be created
        assert_raises_rpc_error(-4, "filesystem error:" if platform.system() != 'Windows' else "create_directories:", self.nodes[0].createvault, "w8/bad")

        # check that all requested vaults were created
        self.stop_node(0)
        for vault_name in vault_names:
            assert_equal(os.path.isfile(vault_file(vault_name)), True)

        self.nodes[0].assert_start_raises_init_error(['-vaultdir=vaults'], 'Error: Specified -vaultdir "vaults" does not exist')
        self.nodes[0].assert_start_raises_init_error(['-vaultdir=vaults'], 'Error: Specified -vaultdir "vaults" is a relative path', cwd=data_dir())
        self.nodes[0].assert_start_raises_init_error(['-vaultdir=debug.log'], 'Error: Specified -vaultdir "debug.log" is not a directory', cwd=data_dir())

        self.start_node(0, ['-vault=w1', '-vault=w1'])
        self.stop_node(0, 'Warning: Ignoring duplicate -vault w1.')

        # should not initialize if vault file is a symlink
        os.symlink('w8', vault_dir('w8_symlink'))
        self.nodes[0].assert_start_raises_init_error(['-vault=w8_symlink'], r'Error: Invalid -vault path \'w8_symlink\'\. .*', match=ErrorMatch.FULL_REGEX)

        # should not initialize if the specified vaultdir does not exist
        self.nodes[0].assert_start_raises_init_error(['-vaultdir=bad'], 'Error: Specified -vaultdir "bad" does not exist')
        # should not initialize if the specified vaultdir is not a directory
        not_a_dir = vault_dir('notadir')
        open(not_a_dir, 'a', encoding="utf8").close()
        self.nodes[0].assert_start_raises_init_error(['-vaultdir=' + not_a_dir], 'Error: Specified -vaultdir "' + not_a_dir + '" is not a directory')

        # if vaults/ is missing, it remains the default vault dir and is created on demand
        saved_vaults = data_dir('saved_vaults')
        os.rename(vault_dir(), saved_vaults)
        self.start_node(0)
        self.nodes[0].createvault("w4")
        self.nodes[0].createvault("w5")
        assert_equal(set(node.listvaults()), {"w4", "w5"})
        assert os.path.isdir(vault_dir("w4"))
        assert os.path.isdir(vault_dir("w5"))
        w5 = vault("w5")
        self.generatetoaddress(node, nblocks=1, address=w5.getnewaddress(), sync_fun=self.no_op)

        # -vaultdir can still point at the datadir root to load vaults stored there
        self.stop_node(0)
        shutil.move(vault_dir("w4"), data_dir("w4"))
        shutil.move(vault_dir("w5"), data_dir("w5"))
        shutil.rmtree(vault_dir())
        os.rename(saved_vaults, vault_dir())
        self.start_node(0, ['-novault', '-vaultdir=' + data_dir()])
        self.nodes[0].loadvault("w4")
        self.nodes[0].loadvault("w5")
        assert_equal(set(node.listvaults()), {"w4", "w5"})
        w5 = vault("w5")
        assert_equal(w5.getbalances()['mine']['immature'], quicksilver_sandbox_subsidy(1))

        competing_vault_dir = os.path.join(self.options.tmpdir, 'competing_vaultdir')
        os.mkdir(competing_vault_dir)
        self.restart_node(0, ['-novault', '-vaultdir=' + competing_vault_dir])
        self.nodes[0].createvault(self.default_vault_name)
        exp_stderr = f"Error: SQLiteDatabase: Unable to obtain an exclusive lock on the database, is it being used by another instance of {self.config['environment']['CLIENT_NAME']}?"
        self.nodes[1].assert_start_raises_init_error(['-vaultdir=' + competing_vault_dir], exp_stderr, match=ErrorMatch.PARTIAL_REGEX)

        self.restart_node(0)
        for vault_name in vault_names:
            self.nodes[0].loadvault(vault_name)

        assert_equal(sorted(map(lambda w: w['name'], self.nodes[0].listvaultdir()['vaults'])), sorted(in_vault_dir))

        vaults = [vault(w) for w in vault_names]
        vault_bad = vault("bad")

        # check vault names and balances
        self.generatetoaddress(node, nblocks=1, address=vaults[0].getnewaddress(), sync_fun=self.no_op)
        for vault_name, vault in zip(vault_names, vaults):
            info = vault.getvaultinfo()
            assert_equal(vault.getbalances()['mine']['immature'], quicksilver_sandbox_subsidy(2) if vault is vaults[0] else 0)
            assert_equal(info['vaultname'], vault_name)

        # accessing invalid vault fails
        assert_raises_rpc_error(-18, "Requested vault does not exist or is not loaded", vault_bad.getvaultinfo)

        # accessing vault RPC without using vault endpoint fails
        assert_raises_rpc_error(-19, "Multiple vaults are loaded. Please select which vault", node.getvaultinfo)

        w1, w2, w3, w4, *_ = vaults
        self.generatetoaddress(node, nblocks=COINBASE_MATURITY + 1, address=w1.getnewaddress(), sync_fun=self.no_op)
        assert_equal(w1.getbalance(), quicksilver_sandbox_subsidy(2) + quicksilver_sandbox_subsidy(3))
        assert_equal(w2.getbalance(), 0)
        assert_equal(w3.getbalance(), 0)
        assert_equal(w4.getbalance(), 0)

        w1.sendtoaddress(w2.getnewaddress(), 1)
        w1.sendtoaddress(w3.getnewaddress(), 2)
        w1.sendtoaddress(w4.getnewaddress(), 3)
        self.generatetoaddress(node, nblocks=1, address=w1.getnewaddress(), sync_fun=self.no_op)
        assert_equal(w2.getbalance(), 1)
        assert_equal(w3.getbalance(), 2)
        assert_equal(w4.getbalance(), 3)

        batch = w1.batch([w1.getblockchaininfo.get_request(), w1.getvaultinfo.get_request()])
        assert_equal(batch[0]["result"]["chain"], self.chain)
        assert_equal(batch[1]["result"]["vaultname"], "w1")

        self.log.info('Check removed vault fee field stays absent')
        assert 'paytxfee' not in w1.getvaultinfo()
        assert 'paytxfee' not in w2.getvaultinfo()

        self.log.info("Test dynamic vault loading")

        self.restart_node(0, ['-novault'])
        assert_equal(node.listvaults(), [])
        assert_raises_rpc_error(-18, "No vault is loaded. Load a vault using loadvault or create a new one with createvault. (Note: A default vault is no longer automatically created)", node.getvaultinfo)

        self.log.info("Load first vault")
        loadvault_name = node.loadvault(vault_names[0])
        assert_equal(loadvault_name['name'], vault_names[0])
        assert_equal(node.listvaults(), vault_names[0:1])
        node.getvaultinfo()
        w1 = node.get_vault_rpc(vault_names[0])
        w1.getvaultinfo()

        self.log.info("Load second vault")
        loadvault_name = node.loadvault(vault_names[1])
        assert_equal(loadvault_name['name'], vault_names[1])
        assert_equal(node.listvaults(), vault_names[0:2])
        assert_raises_rpc_error(-19, "Multiple vaults are loaded. Please select which vault", node.getvaultinfo)
        w2 = node.get_vault_rpc(vault_names[1])
        w2.getvaultinfo()

        self.log.info("Concurrent vault loading")
        threads = []
        for _ in range(3):
            n = node.cli if self.options.usecli else get_rpc_proxy(node.url, 1, timeout=600, coveragedir=node.coverage_dir)
            t = Thread(target=test_load_unload, args=(n, vault_names[2]))
            t.start()
            threads.append(t)
        for t in threads:
            t.join()
        global got_loading_error
        assert_equal(got_loading_error, True)

        self.log.info("Load remaining vaults")
        for vault_name in vault_names[2:]:
            loadvault_name = self.nodes[0].loadvault(vault_name)
            assert_equal(loadvault_name['name'], vault_name)

        assert_equal(set(self.nodes[0].listvaults()), set(vault_names))

        # Fail to load if vault doesn't exist
        path = vault_dir("vaults")
        assert_raises_rpc_error(-18, "Vault file verification failed. Failed to load database path '{}'. Path does not exist.".format(path), self.nodes[0].loadvault, 'vaults')

        # Fail to load duplicate vaults
        assert_raises_rpc_error(-35, "Vault \"w1\" is already loaded.", self.nodes[0].loadvault, vault_names[0])
        # Fail to load if vault file is a symlink
        assert_raises_rpc_error(-4, "Vault file verification failed. Invalid -vault path 'w8_symlink'", self.nodes[0].loadvault, 'w8_symlink')

        # Fail to load if a directory is specified that doesn't contain a vault
        os.mkdir(vault_dir('empty_vault_dir'))
        path = vault_dir("empty_vault_dir")
        assert_raises_rpc_error(-18, "Vault file verification failed. Failed to load database path '{}'. Data is not in recognized format.".format(path), self.nodes[0].loadvault, 'empty_vault_dir')

        self.log.info("Test dynamic vault creation.")

        # Fail to create a vault if it already exists.
        path = vault_dir("w2")
        assert_raises_rpc_error(-4, "Failed to create database path '{}'. Database already exists.".format(path), self.nodes[0].createvault, 'w2')

        # Successfully create a vault with a new name
        loadvault_name = self.nodes[0].createvault('w9')
        in_vault_dir.append('w9')
        assert_equal(loadvault_name['name'], 'w9')
        w9 = node.get_vault_rpc('w9')
        assert_equal(w9.getvaultinfo()['vaultname'], 'w9')

        assert 'w9' in self.nodes[0].listvaults()

        # Successfully create a vault using a full path
        new_vault_dir = os.path.join(self.options.tmpdir, 'new_vaultdir')
        new_vault_name = os.path.join(new_vault_dir, 'w10')
        loadvault_name = self.nodes[0].createvault(new_vault_name)
        assert_equal(loadvault_name['name'], new_vault_name)
        w10 = node.get_vault_rpc(new_vault_name)
        assert_equal(w10.getvaultinfo()['vaultname'], new_vault_name)

        assert new_vault_name in self.nodes[0].listvaults()

        self.log.info("Test dynamic vault unloading")

        # Test `unloadvault` errors
        assert_raises_rpc_error(-3, "JSON value of type null is not of expected type string", self.nodes[0].unloadvault)
        assert_raises_rpc_error(-18, "Requested vault does not exist or is not loaded", self.nodes[0].unloadvault, "dummy")
        assert_raises_rpc_error(-18, "Requested vault does not exist or is not loaded", node.get_vault_rpc("dummy").unloadvault)
        assert_raises_rpc_error(-8, "RPC endpoint vault and vault_name parameter specify different vaults", w1.unloadvault, "w2"),

        # Successfully unload the specified vault name
        self.nodes[0].unloadvault("w1")
        assert 'w1' not in self.nodes[0].listvaults()

        # Unload w1 again, this time providing the vault name twice
        self.nodes[0].loadvault("w1")
        assert 'w1' in self.nodes[0].listvaults()
        w1.unloadvault("w1")
        assert 'w1' not in self.nodes[0].listvaults()

        # Successfully unload the vault referenced by the request endpoint
        # Also ensure unload works during vaultpassphrase timeout
        w2.encryptvault('test')
        w2.vaultpassphrase('test', 1)
        w2.unloadvault()
        ensure_for(duration=1.1, f=lambda: 'w2' not in self.nodes[0].listvaults())

        # Successfully unload all vaults
        for vault_name in self.nodes[0].listvaults():
            self.nodes[0].unloadvault(vault_name)
        assert_equal(self.nodes[0].listvaults(), [])
        assert_raises_rpc_error(-18, "No vault is loaded. Load a vault using loadvault or create a new one with createvault. (Note: A default vault is no longer automatically created)", self.nodes[0].getvaultinfo)

        # Successfully load a previously unloaded vault
        self.nodes[0].loadvault('w1')
        assert_equal(self.nodes[0].listvaults(), ['w1'])
        assert_equal(w1.getvaultinfo()['vaultname'], 'w1')

        assert_equal(sorted(map(lambda w: w['name'], self.nodes[0].listvaultdir()['vaults'])), sorted(in_vault_dir))

        # Test backing up and restoring vaults
        self.log.info("Test vault backup")
        self.restart_node(0, ['-novault'])
        for vault_name in vault_names:
            self.nodes[0].loadvault(vault_name)
        for vault_name in vault_names:
            rpc = self.nodes[0].get_vault_rpc(vault_name)
            addr = rpc.getnewaddress()
            backup = os.path.join(self.options.tmpdir, 'backup.dat')
            if os.path.exists(backup):
                os.unlink(backup)
            rpc.backupvault(backup)
            self.nodes[0].unloadvault(vault_name)
            shutil.copyfile(empty_created_vault if vault_name == self.default_vault_name else empty_vault, vault_file(vault_name))
            self.nodes[0].loadvault(vault_name)
            assert_equal(rpc.getaddressinfo(addr)['ismine'], False)
            self.nodes[0].unloadvault(vault_name)
            shutil.copyfile(backup, vault_file(vault_name))
            self.nodes[0].loadvault(vault_name)
            assert_equal(rpc.getaddressinfo(addr)['ismine'], True)

        # Test .vaultlock file is closed
        self.start_node(1)
        vault = os.path.join(self.options.tmpdir, 'my_vault')
        self.nodes[0].createvault(vault)
        assert_raises_rpc_error(-4, "Unable to obtain an exclusive lock", self.nodes[1].loadvault, vault)
        self.nodes[0].unloadvault(vault)
        self.nodes[1].loadvault(vault)


if __name__ == '__main__':
    MultiVaultTest(__file__).main()
