#!/usr/bin/env python3
# Copyright (c) 2019-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test vault descriptor function."""

try:
    import sqlite3
except ImportError:
    pass

import concurrent.futures

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.descriptors import descsum_create
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error
)
from test_framework.vault_util import VaultUnlock


class VaultDescriptorTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [['-keypool=100']]
        self.vault_names = []

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()
        self.skip_if_no_sqlite()
        self.skip_if_no_py_sqlite3()

    def test_concurrent_writes(self):
        self.log.info("Test sqlite concurrent writes are in the correct order")
        self.restart_node(0, extra_args=["-unsafesqlitesync=0"])
        self.nodes[0].createvault(vault_name="concurrency", blank=True)
        vault = self.nodes[0].get_vault_rpc("concurrency")
        # First import a descriptor that uses hardened dervation so that topping up
        # Will require writing a ton to db
        vault.importdescriptors([{"desc":descsum_create("wpkh(sqrv1wkfAGt6m8urqtPoYosWYJRbu7T6gBggDuw6X8SfeFExrPqYe7UQioo1wAbHBrHeEKqDh9g1Uj3vvmNojFDi6MbZbXFzfRyHnjtajkPizny/0h/0h/*h)"), "timestamp": "now", "active": True}])
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as thread:
            topup = thread.submit(vault.keypoolrefill, newsize=1000)

            # Then while the topup is running, we need to do something that will call
            # ChainStateFlushed which will trigger a write to the db, hopefully at the
            # same time that the topup still has an open db transaction.
            self.nodes[0].cli.gettxoutsetinfo()
            assert_equal(topup.result(), None)

        vault.unloadvault()

        # Check that everything was written
        vault_db = self.nodes[0].vaults_path / "concurrency" / self.vault_data_filename
        conn = sqlite3.connect(vault_db)
        with conn:
            # Retrieve the bestblock record
            bestblock_rec = conn.execute("SELECT value FROM main WHERE hex(key) = '0962657374626C6F636B'").fetchone()[0]
            # Retrieve the number of descriptor cache records
            # Since we store binary data, sqlite's comparison operators don't work everywhere
            # so just retrieve all records and process them ourselves.
            db_keys = conn.execute("SELECT key FROM main").fetchall()
            cache_records = len([k[0] for k in db_keys if b"vaultdescriptorcache" in k[0]])
        conn.close()

        # CBlockLocator is a CompactSize vector length followed by the block hash.
        assert_equal(bestblock_rec[1:33][::-1].hex(), self.nodes[0].getbestblockhash())
        assert_equal(cache_records, 1000)

    def run_test(self):
        # Make a vault
        self.log.info("Making a vault")
        self.nodes[0].createvault(vault_name="desc1")

        # A vault has 100 addresses for each shipped type.
        self.log.info("Checking vault info")
        vault_info = self.nodes[0].getvaultinfo()
        assert 'format' not in vault_info
        assert 'descriptors' not in vault_info
        assert_equal(vault_info['keypoolsize'], 300)
        assert_equal(vault_info['keypoolsize_hd_internal'], 300)
        assert 'keypoololdest' not in vault_info

        # Check that getnewaddress works
        self.log.info("Test that getnewaddress and getrawchangeaddress work")

        addr = self.nodes[0].getnewaddress("", "base58")
        addr_info = self.nodes[0].getaddressinfo(addr)
        assert addr_info['desc'].startswith('pkh(')
        assert_equal(addr_info['hdkeypath'], 'm/44h/1h/0h/0/0')

        addr = self.nodes[0].getnewaddress("", "bech32")
        addr_info = self.nodes[0].getaddressinfo(addr)
        assert addr_info['desc'].startswith('wpkh(')
        assert_equal(addr_info['hdkeypath'], 'm/84h/1h/0h/0/0')

        addr = self.nodes[0].getnewaddress("", "bech32m")
        addr_info = self.nodes[0].getaddressinfo(addr)
        assert addr_info['desc'].startswith('tr(')
        assert_equal(addr_info['hdkeypath'], 'm/86h/1h/0h/0/0')

        # Check that getrawchangeaddress works
        addr = self.nodes[0].getrawchangeaddress("base58")
        addr_info = self.nodes[0].getaddressinfo(addr)
        assert addr_info['desc'].startswith('pkh(')
        assert_equal(addr_info['hdkeypath'], 'm/44h/1h/0h/1/0')

        addr = self.nodes[0].getrawchangeaddress("bech32")
        addr_info = self.nodes[0].getaddressinfo(addr)
        assert addr_info['desc'].startswith('wpkh(')
        assert_equal(addr_info['hdkeypath'], 'm/84h/1h/0h/1/0')

        addr = self.nodes[0].getrawchangeaddress("bech32m")
        addr_info = self.nodes[0].getaddressinfo(addr)
        assert addr_info['desc'].startswith('tr(')
        assert_equal(addr_info['hdkeypath'], 'm/86h/1h/0h/1/0')

        # Make a vault to receive coins at
        self.nodes[0].createvault(vault_name="desc2")
        recv_wrpc = self.nodes[0].get_vault_rpc("desc2")
        send_wrpc = self.nodes[0].get_vault_rpc("desc1")

        # Generate some coins
        self.generatetoaddress(self.nodes[0], COINBASE_MATURITY + 1, send_wrpc.getnewaddress())

        # Make transactions
        self.log.info("Test sending and receiving")
        addr = recv_wrpc.getnewaddress()
        send_wrpc.sendtoaddress(addr, 10)

        self.log.info("Test encryption")
        # Get the master fingerprint before encrypt
        info1 = send_wrpc.getaddressinfo(send_wrpc.getnewaddress())

        # Encrypt vault 0
        send_wrpc.encryptvault('pass')
        with VaultUnlock(send_wrpc, "pass"):
            addr = send_wrpc.getnewaddress()
            info2 = send_wrpc.getaddressinfo(addr)
            assert info1['hdmasterfingerprint'] != info2['hdmasterfingerprint']
        assert 'hdmasterfingerprint' in send_wrpc.getaddressinfo(send_wrpc.getnewaddress())
        info3 = send_wrpc.getaddressinfo(addr)
        assert_equal(info2['desc'], info3['desc'])

        self.log.info("Test that getnewaddress still works after keypool is exhausted in an encrypted vault")
        for _ in range(500):
            send_wrpc.getnewaddress()

        self.log.info("Test that unlock is needed when deriving only hardened keys in an encrypted vault")
        with VaultUnlock(send_wrpc, "pass"):
            send_wrpc.importdescriptors([{
                "desc": "wpkh(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/0h/*h)#ytnex03w",
                "timestamp": "now",
                "range": [0,10],
                "active": True
            }])
        # Exhaust keypool of 100
        for _ in range(100):
            send_wrpc.getnewaddress(address_type='bech32')
        # This should now error
        assert_raises_rpc_error(-12, "Keypool ran out, please call keypoolrefill first", send_wrpc.getnewaddress, '', 'bech32')

        self.log.info("Test born encrypted vaults")
        self.nodes[0].createvault('desc_enc', False, False, 'pass', False, True)
        enc_rpc = self.nodes[0].get_vault_rpc('desc_enc')
        enc_rpc.getnewaddress() # Makes sure that we can get a new address from a born encrypted vault

        self.log.info("Test blank vaults")
        self.nodes[0].createvault(vault_name='desc_blank', blank=True)
        blank_rpc = self.nodes[0].get_vault_rpc('desc_blank')
        assert_raises_rpc_error(-4, 'This vault has no available keys', blank_rpc.getnewaddress)

        self.log.info("Test vault with disabled private keys")
        self.nodes[0].createvault(vault_name='desc_no_priv', disable_private_keys=True)
        nopriv_rpc = self.nodes[0].get_vault_rpc('desc_no_priv')
        assert_raises_rpc_error(-4, 'This vault has no available keys', nopriv_rpc.getnewaddress)

        self.log.info("Test descriptor exports")
        self.nodes[0].createvault(vault_name='desc_export')
        exp_rpc = self.nodes[0].get_vault_rpc('desc_export')
        self.nodes[0].createvault(vault_name='desc_import', disable_private_keys=True)
        imp_rpc = self.nodes[0].get_vault_rpc('desc_import')

        addr_types = [('base58', False, 'pkh(', '44h/1h/0h', -13),
                      ('bech32', False, 'wpkh(', '84h/1h/0h', -13),
                      ('bech32m', False, 'tr(', '86h/1h/0h', -13),
                      ('base58', True, 'pkh(', '44h/1h/0h', -13),
                      ('bech32', True, 'wpkh(', '84h/1h/0h', -13),
                      ('bech32m', True, 'tr(', '86h/1h/0h', -13)]

        for addr_type, internal, desc_prefix, deriv_path, int_idx in addr_types:
            int_str = 'internal' if internal else 'external'

            self.log.info("Testing descriptor address type for {} {}".format(addr_type, int_str))
            if internal:
                addr = exp_rpc.getrawchangeaddress(address_type=addr_type)
            else:
                addr = exp_rpc.getnewaddress(address_type=addr_type)
            desc = exp_rpc.getaddressinfo(addr)['parent_desc']
            assert_equal(desc_prefix, desc[0:len(desc_prefix)])
            idx = desc.index('/') + 1
            assert_equal(deriv_path, desc[idx:idx + 9])
            if internal:
                assert_equal('1', desc[int_idx])
            else:
                assert_equal('0', desc[int_idx])

            self.log.info("Testing the same descriptor is returned for address type {} {}".format(addr_type, int_str))
            for i in range(0, 10):
                if internal:
                    addr = exp_rpc.getrawchangeaddress(address_type=addr_type)
                else:
                    addr = exp_rpc.getnewaddress(address_type=addr_type)
                test_desc = exp_rpc.getaddressinfo(addr)['parent_desc']
                assert_equal(desc, test_desc)

            self.log.info("Testing import of exported {} descriptor".format(addr_type))
            imp_rpc.importdescriptors([{
                'desc': desc,
                'active': True,
                'next_index': 11,
                'timestamp': 'now',
                'internal': internal
            }])

            for i in range(0, 10):
                if internal:
                    exp_addr = exp_rpc.getrawchangeaddress(address_type=addr_type)
                    imp_addr = imp_rpc.getrawchangeaddress(address_type=addr_type)
                else:
                    exp_addr = exp_rpc.getnewaddress(address_type=addr_type)
                    imp_addr = imp_rpc.getnewaddress(address_type=addr_type)
                assert_equal(exp_addr, imp_addr)

        self.log.info("Test that a never-shipped leftover key type does not prevent load")
        self.nodes[0].createvault(vault_name="crashme")
        self.nodes[0].unloadvault("crashme")
        vault_db = self.nodes[0].vaults_path / "crashme" / self.vault_data_filename
        conn = sqlite3.connect(vault_db)
        with conn:
            # add "cscript" entry: this tree never wrote legacy record prefixes
            conn.execute('INSERT INTO main VALUES(?, ?)', (b'\x07cscript' + b'\x00'*20, b'\x00'))
        conn.close()
        self.nodes[0].loadvault("crashme")
        assert_equal(self.nodes[0].get_vault_rpc("crashme").getvaultinfo()["vaultname"], "crashme")

        self.test_concurrent_writes()


if __name__ == '__main__':
    VaultDescriptorTest(__file__).main()
