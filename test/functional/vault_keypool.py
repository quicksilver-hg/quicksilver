#!/usr/bin/env python3
# Copyright (c) 2014-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the vault keypool and interaction with vault encryption/locking."""

import time

from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.vault_util import VaultUnlock

class KeyPoolTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()

    def run_test(self):
        nodes = self.nodes
        addr_before_encrypting = nodes[0].getnewaddress()
        addr_before_encrypting_data = nodes[0].getaddressinfo(addr_before_encrypting)

        # Encrypt vault and wait to terminate
        nodes[0].encryptvault('test')
        # Import hardened derivation only descriptors
        nodes[0].vaultpassphrase('test', 10)
        nodes[0].importdescriptors([
            {
                "desc": "wpkh(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/0h/*h)#ytnex03w",
                "timestamp": "now",
                "range": [0,0],
                "active": True
            },
            {
                "desc": "pkh(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/1h/*h)#gvejwz68",
                "timestamp": "now",
                "range": [0,0],
                "active": True
            },
            {
                "desc": "wpkh(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/3h/*h)#jgp2888x",
                "timestamp": "now",
                "range": [0,0],
                "active": True,
                "internal": True
            },
            {
                "desc": "pkh(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/4h/*h)#2jj4vqfp",
                "timestamp": "now",
                "range": [0,0],
                "active": True,
                "internal": True
            }
        ])
        nodes[0].vaultlock()
        # Keep creating keys
        addr = nodes[0].getnewaddress()
        addr_data = nodes[0].getaddressinfo(addr)
        assert addr_before_encrypting_data['hdmasterfingerprint'] != addr_data['hdmasterfingerprint']
        assert_raises_rpc_error(-12, "Error: Keypool ran out, please call keypoolrefill first", nodes[0].getnewaddress)

        # put six (plus 2) new keys in the keypool (100% external-, +100% internal-keys, 1 in min)
        with VaultUnlock(nodes[0], 'test'):
            nodes[0].keypoolrefill(6)
        wi = nodes[0].getvaultinfo()
        assert_equal(wi['keypoolsize_hd_internal'], 18)
        assert_equal(wi['keypoolsize'], 18)

        # drain the internal keys
        nodes[0].getrawchangeaddress()
        nodes[0].getrawchangeaddress()
        nodes[0].getrawchangeaddress()
        nodes[0].getrawchangeaddress()
        nodes[0].getrawchangeaddress()
        nodes[0].getrawchangeaddress()
        # remember keypool sizes
        wi = nodes[0].getvaultinfo()
        kp_size_before = [wi['keypoolsize_hd_internal'], wi['keypoolsize']]
        # the next one should fail
        assert_raises_rpc_error(-12, "Keypool ran out", nodes[0].getrawchangeaddress)
        # check that keypool sizes did not change
        wi = nodes[0].getvaultinfo()
        kp_size_after = [wi['keypoolsize_hd_internal'], wi['keypoolsize']]
        assert_equal(kp_size_before, kp_size_after)

        # drain the external keys
        addr = set()
        addr.add(nodes[0].getnewaddress(address_type="bech32"))
        addr.add(nodes[0].getnewaddress(address_type="bech32"))
        addr.add(nodes[0].getnewaddress(address_type="bech32"))
        addr.add(nodes[0].getnewaddress(address_type="bech32"))
        addr.add(nodes[0].getnewaddress(address_type="bech32"))
        addr.add(nodes[0].getnewaddress(address_type="bech32"))
        assert len(addr) == 6
        # remember keypool sizes
        wi = nodes[0].getvaultinfo()
        kp_size_before = [wi['keypoolsize_hd_internal'], wi['keypoolsize']]
        # the next one should fail
        assert_raises_rpc_error(-12, "Error: Keypool ran out, please call keypoolrefill first", nodes[0].getnewaddress)
        # check that keypool sizes did not change
        wi = nodes[0].getvaultinfo()
        kp_size_after = [wi['keypoolsize_hd_internal'], wi['keypoolsize']]
        assert_equal(kp_size_before, kp_size_after)

        # refill keypool with three new addresses
        nodes[0].vaultpassphrase('test', 1)
        nodes[0].keypoolrefill(3)

        # test vaultpassphrase timeout
        time.sleep(1.1)
        assert_equal(nodes[0].getvaultinfo()["unlocked_until"], 0)

        # drain the keypool
        for _ in range(3):
            nodes[0].getnewaddress()
        assert_raises_rpc_error(-12, "Keypool ran out", nodes[0].getnewaddress)

        with VaultUnlock(nodes[0], 'test'):
            nodes[0].keypoolrefill(100)
            wi = nodes[0].getvaultinfo()
            assert_equal(wi['keypoolsize_hd_internal'], 300)
            assert_equal(wi['keypoolsize'], 300)

        # create a blank vault
        nodes[0].createvault(vault_name='w2', blank=True, disable_private_keys=True)
        w2 = nodes[0].get_vault_rpc('w2')

        # refer to initial vault as w1
        w1 = nodes[0].get_vault_rpc(self.default_vault_name)

        # import private key and fund it
        address = addr.pop()
        desc = w1.getaddressinfo(address)['desc']
        res = w2.importdescriptors([{'desc': desc, 'timestamp': 'now'}])
        assert_equal(res[0]['success'], True)

        with VaultUnlock(w1, 'test'):
            res = w1.sendtoaddress(address=address, amount=0.00010000)
        self.generate(nodes[0], 1)
        destination = addr.pop()

        # Creating a 5,000 cinnabar transaction with change should not be possible.
        assert_raises_rpc_error(-4, "Transaction needs a change address, but we can't generate it.", w2.vaultcreatefundedpsqt, inputs=[], outputs=[{addr.pop(): 0.00005000}])

        # Creating a 10,000 cinnabar transaction without change, with a manual input, should still be possible.
        res = w2.vaultcreatefundedpsqt(inputs=w2.listunspent(), outputs=[{destination: 0.00010000}])
        assert_equal("psqt" in res, True)

        # Creating a 10,000 cinnabar transaction without change should still be possible.
        res = w2.vaultcreatefundedpsqt(inputs=[], outputs=[{destination: 0.00010000}])
        assert_equal("psqt" in res, True)

        # Exact-value funding creates no change and returns only PSQT metadata.
        res = w2.vaultcreatefundedpsqt(inputs=[], outputs=[{destination: 0.00010000}])
        assert_equal("psqt" in res, True)
        assert_equal(set(res), {"psqt", "changepos"})
        assert_equal(res["changepos"], -1)

        # creating a 10,000 cinnabar transaction with a manual change address should be possible
        res = w2.vaultcreatefundedpsqt(inputs=[], outputs=[{destination: 0.00010000}], change_address=addr.pop())
        assert_equal("psqt" in res, True)

if __name__ == '__main__':
    KeyPoolTest(__file__).main()
