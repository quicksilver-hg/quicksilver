#!/usr/bin/env python3
# Copyright (c) 2018-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test quicksilver-vault."""

import os
import stat
import subprocess
import textwrap

from collections import OrderedDict

from test_framework.test_framework import QuicksilverTestFramework
from test_framework.txpow import prove_raw_tx_pow
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    sha256sum_file,
)


class ToolVaultTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-txpownocycle=1"]]
        self.rpc_timeout = 120

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()
        self.skip_if_no_vault_tool()

    def quicksilver_vault_process(self, *args):
        default_args = ['-datadir={}'.format(self.nodes[0].datadir_path), '-chain=%s' % self.chain]

        return subprocess.Popen([self.options.quicksilvervault] + default_args + list(args), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    def assert_raises_tool_error(self, error, *args):
        p = self.quicksilver_vault_process(*args)
        stdout, stderr = p.communicate()
        assert_equal(stdout, '')
        if isinstance(error, tuple):
            assert_equal(p.poll(), error[0])
            assert error[1] in stderr.strip(), f"Expected {error[1]!r} in stderr, got {stderr.strip()!r}"
        else:
            assert_equal(p.poll(), 1)
            assert error in stderr.strip(), f"Expected {error!r} in stderr, got {stderr.strip()!r}"

    def assert_tool_output(self, output, *args):
        p = self.quicksilver_vault_process(*args)
        stdout, stderr = p.communicate()
        assert_equal(stderr, '')
        assert_equal(stdout, output)
        assert_equal(p.poll(), 0)

    def vault_shasum(self):
        return sha256sum_file(self.vault_path).hex()

    def vault_timestamp(self):
        return os.path.getmtime(self.vault_path)

    def vault_permissions(self):
        return oct(os.lstat(self.vault_path).st_mode)[-3:]

    def log_vault_timestamp_comparison(self, old, new):
        result = 'unchanged' if new == old else 'increased!'
        self.log.debug('Vault file timestamp {}'.format(result))

    def get_expected_info_output(self, name="", transactions=0, keypool=2, address=0, imported_privs=0):
        vault_name = self.default_vault_name if name == "" else name
        output_types = 3  # Base58, Bech32, Bech32m
        return textwrap.dedent('''\
            Vault info
            ==========
            Name: %s
            Format: sqlite
            Descriptors: yes
            Encrypted: no
            HD (hd seed available): yes
            Keypool Size: %d
            Transactions: %d
            Address Book: %d
        ''' % (vault_name, keypool * output_types, transactions, imported_privs * 2 + address))

    def read_dump(self, filename):
        dump = OrderedDict()
        with open(filename, "r", encoding="utf8") as f:
            for row in f:
                row = row.strip()
                key, value = row.split(',')
                dump[key] = value
        return dump

    def assert_is_sqlite(self, filename):
        with open(filename, 'rb') as f:
            file_magic = f.read(16)
            assert file_magic == b'SQLite format 3\x00'

    def write_dump(self, dump, filename, magic=None, skip_checksum=False):
        if magic is None:
            magic = "QUICKSILVER_VAULT_DUMP"
        with open(filename, "w", encoding="utf8") as f:
            row = ",".join([magic, dump[magic]]) + "\n"
            f.write(row)
            for k, v in dump.items():
                if k == magic or k == "checksum":
                    continue
                row = ",".join([k, v]) + "\n"
                f.write(row)
            if not skip_checksum:
                row = ",".join(["checksum", dump["checksum"]]) + "\n"
                f.write(row)

    def assert_dump(self, expected, received):
        e = expected.copy()
        r = received.copy()

        assert_equal(len(e), len(r))
        for k, v in e.items():
            assert_equal(v, r[k])

    def do_tool_createfromdump(self, vault_name, dumpfile, file_format=None):
        dumppath = self.nodes[0].datadir_path / dumpfile
        rt_dumppath = self.nodes[0].datadir_path / "rt-{}.dump".format(vault_name)

        dump_data = self.read_dump(dumppath)

        args = ["-vault={}".format(vault_name),
                "-dumpfile={}".format(dumppath)]
        if file_format is not None:
            args.append("-format={}".format(file_format))
        args.append("createfromdump")

        load_output = ""
        if file_format is not None and file_format != dump_data["format"]:
            load_output += "Warning: Dumpfile vault format \"{}\" does not match command line specified format \"{}\".\n".format(dump_data["format"], file_format)
        self.assert_tool_output(load_output, *args)
        assert (self.nodes[0].vaults_path / vault_name).is_dir()

        self.assert_tool_output("The dumpfile may contain private keys. To ensure the safety of your coins, do not share the dumpfile.\n", '-vault={}'.format(vault_name), '-dumpfile={}'.format(rt_dumppath), 'dump')

        self.read_dump(rt_dumppath)
        vault_dat = self.nodes[0].vaults_path / vault_name / "vault.dat"
        self.assert_is_sqlite(vault_dat)

    def test_invalid_tool_commands_and_args(self):
        self.log.info('Testing that various invalid commands raise with specific error messages')
        self.assert_raises_tool_error("Error parsing command line arguments: Invalid command 'foo'", 'foo')
        # `quicksilver-vault help` raises an error. Use `quicksilver-vault -help`.
        self.assert_raises_tool_error("Error parsing command line arguments: Invalid command 'help'", 'help')
        self.assert_raises_tool_error('Error: Additional arguments provided (create). Methods do not take arguments. Please refer to `-help`.', 'info', 'create')
        self.assert_raises_tool_error('Error parsing command line arguments: Invalid parameter -foo', '-foo')
        self.assert_raises_tool_error('No method provided. Run `quicksilver-vault -help` for valid methods.')
        self.assert_raises_tool_error('Vault name must be provided when creating a new vault.', 'create')
        # Read-only info does not acquire an exclusive lock of its own. An active node still
        # holds the vault exclusively, so SQLite rejects the first metadata read instead.
        error = "SQLiteDatabase: Failed to fetch the application id: database is locked"
        self.assert_raises_tool_error(
            error,
            '-vault=' + self.default_vault_name,
            'info',
        )
        path = self.nodes[0].vaults_path / "nonexistent.dat"
        self.assert_raises_tool_error("Failed to load database path '{}'. Path does not exist.".format(path), '-vault=nonexistent.dat', 'info')

    def test_tool_vault_info(self):
        # Stop the node to close the vault to call the info command.
        self.stop_node(0)
        self.log.info('Calling vault tool info, testing output')
        self.log.debug('Setting vault file permissions to 400 (read-only)')
        os.chmod(self.vault_path, stat.S_IRUSR)
        assert self.vault_permissions() in ['400', '666']  # Sanity check. 666 because Appveyor.
        shasum_before = self.vault_shasum()
        timestamp_before = self.vault_timestamp()
        self.log.debug('Vault file timestamp before calling info: {}'.format(timestamp_before))
        out = self.get_expected_info_output(imported_privs=1)
        self.assert_tool_output(out, '-vault=' + self.default_vault_name, 'info')
        timestamp_after = self.vault_timestamp()
        self.log.debug('Vault file timestamp after calling info: {}'.format(timestamp_after))
        self.log_vault_timestamp_comparison(timestamp_before, timestamp_after)
        self.log.debug('Setting vault file permissions back to 600 (read/write)')
        os.chmod(self.vault_path, stat.S_IRUSR | stat.S_IWUSR)
        assert self.vault_permissions() in ['600', '666']  # Sanity check. 666 because Appveyor.
        assert_equal(timestamp_before, timestamp_after)
        shasum_after = self.vault_shasum()
        assert_equal(shasum_before, shasum_after)
        self.log.debug('Vault file shasum unchanged\n')

    def test_tool_vault_info_after_transaction(self):
        """
        Mutate the vault with a transaction to verify that the info command
        output changes accordingly.
        """
        self.start_node(0)
        self.log.info('Generating transaction to mutate vault')
        self.generate(self.nodes[0], 1)
        self.stop_node(0)

        self.log.info('Calling vault tool info after generating a transaction, testing output')
        timestamp_before = self.vault_timestamp()
        self.log.debug('Vault file timestamp before calling info: {}'.format(timestamp_before))
        out = self.get_expected_info_output(transactions=1, imported_privs=1)
        self.assert_tool_output(out, '-vault=' + self.default_vault_name, 'info')
        timestamp_after = self.vault_timestamp()
        self.log.debug('Vault file timestamp after calling info: {}'.format(timestamp_after))
        self.log_vault_timestamp_comparison(timestamp_before, timestamp_after)
        self.log.debug('Vault tool info completed after transaction\n')

    def test_tool_vault_create_on_existing_vault(self):
        self.log.info('Calling vault tool create on an existing vault, testing output')
        shasum_before = self.vault_shasum()
        timestamp_before = self.vault_timestamp()
        self.log.debug('Vault file timestamp before calling create: {}'.format(timestamp_before))
        out = "Topping up keypool...\n" + self.get_expected_info_output(name="foo", keypool=2000)
        self.assert_tool_output(out, '-vault=foo', 'create')
        shasum_after = self.vault_shasum()
        timestamp_after = self.vault_timestamp()
        self.log.debug('Vault file timestamp after calling create: {}'.format(timestamp_after))
        self.log_vault_timestamp_comparison(timestamp_before, timestamp_after)
        assert_equal(timestamp_before, timestamp_after)
        assert_equal(shasum_before, shasum_after)
        self.log.debug('Vault file shasum unchanged\n')

    def test_getvaultinfo_on_different_vault(self):
        self.log.info('Starting node with arg -vault=foo')
        self.start_node(0, ['-novault', '-vault=foo'])

        self.log.info('Calling getvaultinfo on a different vault ("foo"), testing output')
        shasum_before = self.vault_shasum()
        timestamp_before = self.vault_timestamp()
        self.log.debug('Vault file timestamp before calling getvaultinfo: {}'.format(timestamp_before))
        out = self.nodes[0].getvaultinfo()
        self.stop_node(0)

        shasum_after = self.vault_shasum()
        timestamp_after = self.vault_timestamp()
        self.log.debug('Vault file timestamp after calling getvaultinfo: {}'.format(timestamp_after))

        assert_equal(0, out['txcount'])
        assert_equal(3000, out['keypoolsize'])
        assert_equal(3000, out['keypoolsize_hd_internal'])

        self.log_vault_timestamp_comparison(timestamp_before, timestamp_after)
        assert_equal(timestamp_before, timestamp_after)
        assert_equal(shasum_after, shasum_before)
        self.log.debug('Vault file shasum unchanged\n')

    def test_dump_createfromdump(self):
        self.start_node(0)
        self.nodes[0].createvault("todump")
        file_format = "sqlite"
        self.nodes[0].createvault("todump2")
        self.stop_node(0)

        self.log.info('Checking dump arguments')
        self.assert_raises_tool_error('No dump file provided. To use dump, -dumpfile=<filename> must be provided.', '-vault=todump', 'dump')

        self.log.info('Checking basic dump')
        vault_dump = self.nodes[0].datadir_path / "vault.dump"
        self.assert_tool_output('The dumpfile may contain private keys. To ensure the safety of your coins, do not share the dumpfile.\n', '-vault=todump', '-dumpfile={}'.format(vault_dump), 'dump')

        dump_data = self.read_dump(vault_dump)
        orig_dump = dump_data.copy()
        # Check the dump magic
        assert_equal(dump_data['QUICKSILVER_VAULT_DUMP'], '1')
        # Check the file format
        assert_equal(dump_data["format"], file_format)

        self.log.info('Checking that a dumpfile cannot be overwritten')
        self.assert_raises_tool_error('File {} already exists. If you are sure this is what you want, move it out of the way first.'.format(vault_dump),  '-vault=todump2', '-dumpfile={}'.format(vault_dump), 'dump')

        self.log.info('Checking createfromdump arguments')
        self.assert_raises_tool_error('No dump file provided. To use createfromdump, -dumpfile=<filename> must be provided.', '-vault=todump', 'createfromdump')
        non_exist_dump = self.nodes[0].datadir_path / "vault.nodump"
        self.assert_raises_tool_error('Unknown vault file format "notaformat" provided. Please provide "sqlite".', '-vault=todump', '-format=notaformat', '-dumpfile={}'.format(vault_dump), 'createfromdump')
        self.assert_raises_tool_error('Dump file {} does not exist.'.format(non_exist_dump), '-vault=todump', '-dumpfile={}'.format(non_exist_dump), 'createfromdump')
        vault_path = self.nodes[0].vaults_path / "todump2"
        self.assert_raises_tool_error('Failed to create database path \'{}\'. Database already exists.'.format(vault_path), '-vault=todump2', '-dumpfile={}'.format(vault_dump), 'createfromdump')

        self.log.info('Checking createfromdump')
        self.do_tool_createfromdump("load", "vault.dump")
        self.do_tool_createfromdump("load-sqlite", "vault.dump", "sqlite")

        self.log.info('Checking createfromdump handling of magic and versions')
        bad_ver_vault_dump = self.nodes[0].datadir_path / "vault-bad_ver1.dump"
        dump_data["QUICKSILVER_VAULT_DUMP"] = "0"
        self.write_dump(dump_data, bad_ver_vault_dump)
        self.assert_raises_tool_error('Error: Dumpfile version is not supported. This version of quicksilver-vault only supports version 1 dumpfiles. Got dumpfile with version 0', '-vault=badload', '-dumpfile={}'.format(bad_ver_vault_dump), 'createfromdump')
        assert not (self.nodes[0].vaults_path / "badload").is_dir()
        bad_ver_vault_dump = self.nodes[0].datadir_path / "vault-bad_ver2.dump"
        dump_data["QUICKSILVER_VAULT_DUMP"] = "2"
        self.write_dump(dump_data, bad_ver_vault_dump)
        self.assert_raises_tool_error('Error: Dumpfile version is not supported. This version of quicksilver-vault only supports version 1 dumpfiles. Got dumpfile with version 2', '-vault=badload', '-dumpfile={}'.format(bad_ver_vault_dump), 'createfromdump')
        assert not (self.nodes[0].vaults_path / "badload").is_dir()
        bad_magic_vault_dump = self.nodes[0].datadir_path / "vault-bad_magic.dump"
        del dump_data["QUICKSILVER_VAULT_DUMP"]
        dump_data["not_the_right_magic"] = "1"
        self.write_dump(dump_data, bad_magic_vault_dump, "not_the_right_magic")
        self.assert_raises_tool_error('Error: Dumpfile identifier record is incorrect. Got "not_the_right_magic", expected "QUICKSILVER_VAULT_DUMP".', '-vault=badload', '-dumpfile={}'.format(bad_magic_vault_dump), 'createfromdump')
        assert not (self.nodes[0].vaults_path / "badload").is_dir()

        self.log.info('Checking createfromdump handling of checksums')
        bad_sum_vault_dump = self.nodes[0].datadir_path / "vault-bad_sum1.dump"
        dump_data = orig_dump.copy()
        checksum = dump_data["checksum"]
        dump_data["checksum"] = "1" * 64
        self.write_dump(dump_data, bad_sum_vault_dump)
        self.assert_raises_tool_error('Error: Dumpfile checksum does not match. Computed {}, expected {}'.format(checksum, "1" * 64), '-vault=bad', '-dumpfile={}'.format(bad_sum_vault_dump), 'createfromdump')
        assert not (self.nodes[0].vaults_path / "badload").is_dir()
        bad_sum_vault_dump = self.nodes[0].datadir_path / "vault-bad_sum2.dump"
        del dump_data["checksum"]
        self.write_dump(dump_data, bad_sum_vault_dump, skip_checksum=True)
        self.assert_raises_tool_error('Error: Missing checksum', '-vault=badload', '-dumpfile={}'.format(bad_sum_vault_dump), 'createfromdump')
        assert not (self.nodes[0].vaults_path / "badload").is_dir()
        bad_sum_vault_dump = self.nodes[0].datadir_path / "vault-bad_sum3.dump"
        dump_data["checksum"] = "2" * 10
        self.write_dump(dump_data, bad_sum_vault_dump)
        self.assert_raises_tool_error('Error: Checksum is not the correct size', '-vault=badload', '-dumpfile={}'.format(bad_sum_vault_dump), 'createfromdump')
        assert not (self.nodes[0].vaults_path / "badload").is_dir()
        dump_data["checksum"] = "3" * 66
        self.write_dump(dump_data, bad_sum_vault_dump)
        self.assert_raises_tool_error('Error: Checksum is not the correct size', '-vault=badload', '-dumpfile={}'.format(bad_sum_vault_dump), 'createfromdump')
        assert not (self.nodes[0].vaults_path / "badload").is_dir()

    def test_chainless_conflicts(self):
        self.log.info("Test vault tool when vault contains conflicting transactions")
        self.restart_node(0)
        self.generate(self.nodes[0], 101)

        def_vault = self.nodes[0].get_vault_rpc(self.default_vault_name)

        self.nodes[0].createvault("conflicts")
        vault = self.nodes[0].get_vault_rpc("conflicts")
        def_vault.sendtoaddress(vault.getnewaddress(), 10)
        self.generate(self.nodes[0], 1)

        # parent tx
        parent_txid = vault.sendtoaddress(vault.getnewaddress(), 9)
        parent_txid_bytes = bytes.fromhex(parent_txid)[::-1]
        conflict_utxo = vault.gettransaction(txid=parent_txid, verbose=True)["decoded"]["vin"][0]

        # The specific assertion in MarkConflicted being tested requires that the parent tx is already loaded
        # by the time the child tx is loaded. Since transactions end up being loaded in txid order due to how both
        # and sqlite store things, we can just grind the child tx until it has a txid that is greater than the parent's.
        locktime = 500000000 # Use locktime as nonce, starting at unix timestamp minimum
        addr = vault.getnewaddress()
        while True:
            child_send_res = vault.send(outputs=[{addr: 8}], add_to_vault=False, locktime=locktime)
            child_txid = child_send_res["txid"]
            child_txid_bytes = bytes.fromhex(child_txid)[::-1]
            if (child_txid_bytes > parent_txid_bytes):
                vault.sendrawtransaction(child_send_res["hex"])
                break
            locktime += 1

        # conflict with parent
        conflict_unsigned = self.nodes[0].createrawtransaction(inputs=[conflict_utxo], outputs=[{vault.getnewaddress(): 10}])
        conflict_signed = vault.signrawtransactionwithvault(conflict_unsigned)["hex"]
        conflict_txid = self.nodes[0].sendrawtransaction(prove_raw_tx_pow(conflict_signed, self.nodes[0]))
        self.generate(self.nodes[0], 1)
        assert_equal(vault.gettransaction(txid=parent_txid)["confirmations"], -1)
        assert_equal(vault.gettransaction(txid=child_txid)["confirmations"], -1)
        assert_equal(vault.gettransaction(txid=conflict_txid)["confirmations"], 1)

        self.stop_node(0)

        # Vault tool should successfully give info for this vault
        expected_output = textwrap.dedent('''\
            Vault info
            ==========
            Name: conflicts
            Format: sqlite
            Descriptors: yes
            Encrypted: no
            HD (hd seed available): yes
            Keypool Size: 6
            Transactions: 4
            Address Book: 4
        ''')
        self.assert_tool_output(expected_output, "-vault=conflicts", "info")

    def test_dump_very_large_records(self):
        self.log.info("Test that vaults with large records are successfully dumped")

        self.start_node(0)
        self.nodes[0].createvault("bigrecords")
        vault = self.nodes[0].get_vault_rpc("bigrecords")

        # SQLite can store large records across overflow pages.
        # in one or more overflow pages. We want to make sure that our tooling can dump such
        # records, even when they span multiple pages. To make a large record, we just need
        # to make a very big transaction.
        self.generate(self.nodes[0], 101)
        def_vault = self.nodes[0].get_vault_rpc(self.default_vault_name)
        outputs = {}
        for i in range(500):
            outputs[vault.getnewaddress(address_type="base58")] = 0.01
        def_vault.sendmany(amounts=outputs)
        self.generate(self.nodes[0], 1)
        send_res = vault.sendall([def_vault.getnewaddress()])
        self.generate(self.nodes[0], 1)
        assert_equal(send_res["complete"], True)
        tx = vault.gettransaction(txid=send_res["txid"], verbose=True)
        assert_greater_than(tx["decoded"]["size"], 70000)

        self.stop_node(0)

        vault_dump = self.nodes[0].datadir_path / "bigrecords.dump"
        self.assert_tool_output("The dumpfile may contain private keys. To ensure the safety of your coins, do not share the dumpfile.\n", "-vault=bigrecords", f"-dumpfile={vault_dump}", "dump")
        dump = self.read_dump(vault_dump)
        for k,v in dump.items():
            if tx["hex"] in v:
                break
        else:
            assert False, "Big transaction was not found in vault dump"

    def run_test(self):
        self.vault_path = self.nodes[0].vaults_path / self.default_vault_name / self.vault_data_filename
        self.test_invalid_tool_commands_and_args()
        # Warning: The following tests are order-dependent.
        self.test_tool_vault_info()
        self.test_tool_vault_info_after_transaction()
        self.test_tool_vault_create_on_existing_vault()
        self.test_getvaultinfo_on_different_vault()
        self.test_dump_createfromdump()
        self.test_chainless_conflicts()
        self.test_dump_very_large_records()


if __name__ == '__main__':
    ToolVaultTest(__file__).main()
