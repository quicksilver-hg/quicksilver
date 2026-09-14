#!/usr/bin/env python3
# Copyright (c) 2018-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test transaction time during old block rescanning
"""

from decimal import Decimal
import time

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
    set_node_times,
)


class TransactionTimeRescanTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.setup_clean_chain = False
        self.num_nodes = 3
        self.extra_args = [["-keypool=400"],
                           ["-keypool=400"],
                           []
                          ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()

    def run_test(self):
        self.log.info('Prepare nodes and vault')

        minernode = self.nodes[0]  # node used to mine Hg and create transactions
        usernode = self.nodes[1]  # user node with correct time
        restorenode = self.nodes[2]  # node used to restore user vault and check time determination in ComputeSmartTime (vault.cpp)

        # time constant
        cur_time = int(time.time())
        ten_days = 10 * 24 * 60 * 60

        # synchronize nodes and time
        self.sync_all()
        set_node_times(self.nodes, cur_time)

        # prepare miner vault
        minernode.createvault(vault_name='default')
        miner_vault = minernode.get_vault_rpc('default')
        m1 = miner_vault.getnewaddress()

        # prepare the user vault with 3 tracked addresses
        wo1 = usernode.getnewaddress()
        wo2 = usernode.getnewaddress()
        wo3 = usernode.getnewaddress()

        usernode.createvault(vault_name='wo', disable_private_keys=True)
        wo_vault = usernode.get_vault_rpc('wo')

        wo_vault.importaddress(wo1)
        wo_vault.importaddress(wo2)
        wo_vault.importaddress(wo3)

        self.log.info('Start transactions')

        # check blockcount
        assert_equal(minernode.getblockcount(), 200)

        # generate some Hg to create transactions and check blockcount
        initial_mine = COINBASE_MATURITY + 1
        self.generatetoaddress(minernode, initial_mine, m1)
        assert_equal(minernode.getblockcount(), initial_mine + 200)

        # synchronize nodes and time
        self.sync_all()
        set_node_times(self.nodes, cur_time + ten_days)
        # send Hg to the user's first tracked address
        self.log.info('Send Hg to user')
        miner_vault.sendtoaddress(wo1, 0.4)

        # generate blocks and check blockcount
        self.generatetoaddress(minernode, COINBASE_MATURITY, m1)
        assert_equal(minernode.getblockcount(), initial_mine + 300)

        # synchronize nodes and time
        self.sync_all()
        set_node_times(self.nodes, cur_time + ten_days + ten_days)
        # send Hg to our second tracked address
        self.log.info('Send Hg to user')
        miner_vault.sendtoaddress(wo2, 0.3)

        # generate blocks and check blockcount
        self.generatetoaddress(minernode, COINBASE_MATURITY, m1)
        assert_equal(minernode.getblockcount(), initial_mine + 400)

        # synchronize nodes and time
        self.sync_all()
        set_node_times(self.nodes, cur_time + ten_days + ten_days + ten_days)
        # send Hg to our third tracked address
        self.log.info('Send Hg to user')
        miner_vault.sendtoaddress(wo3, 0.2)

        # generate more blocks and check blockcount
        self.generatetoaddress(minernode, COINBASE_MATURITY, m1)
        assert_equal(minernode.getblockcount(), initial_mine + 500)

        self.log.info('Check user\'s final balance and transaction count')
        assert_equal(wo_vault.getbalance(), Decimal("0.9"))
        assert_equal(len(wo_vault.listtransactions()), 3)

        self.log.info('Check transaction times')
        for tx in wo_vault.listtransactions():
            if tx['address'] == wo1:
                assert_equal(tx['blocktime'], cur_time + ten_days)
                assert_equal(tx['time'], cur_time + ten_days)
            elif tx['address'] == wo2:
                assert_equal(tx['blocktime'], cur_time + ten_days + ten_days)
                assert_equal(tx['time'], cur_time + ten_days + ten_days)
            elif tx['address'] == wo3:
                assert_equal(tx['blocktime'], cur_time + ten_days + ten_days + ten_days)
                assert_equal(tx['time'], cur_time + ten_days + ten_days + ten_days)

        # restore user vault without rescan
        self.log.info('Restore user vault on another node without rescan')
        restorenode.createvault(vault_name='wo', disable_private_keys=True)
        restorewo_vault = restorenode.get_vault_rpc('wo')

        # importaddress maps to importdescriptors (timestamp='now'), which always rescans
        # blocks of the past 2 hours based on the current MTP timestamp; in order to avoid
        # importing the last address (wo3), we advance the time further and generate 10 blocks
        set_node_times(self.nodes, cur_time + ten_days + ten_days + ten_days + ten_days)
        self.generatetoaddress(minernode, 10, m1)

        restorewo_vault.importaddress(wo1, rescan=False)
        restorewo_vault.importaddress(wo2, rescan=False)
        restorewo_vault.importaddress(wo3, rescan=False)

        # check user has 0 balance and no transactions
        assert_equal(restorewo_vault.getbalance(), 0)
        assert_equal(len(restorewo_vault.listtransactions()), 0)

        # proceed to rescan, first with an incomplete one, then with a full rescan
        self.log.info('Rescan last history part')
        restorewo_vault.rescanblockchain(initial_mine + 350)
        self.log.info('Rescan all history')
        restorewo_vault.rescanblockchain()

        self.log.info('Check user\'s final balance and transaction count after restoration')
        assert_equal(restorewo_vault.getbalance(), Decimal("0.9"))
        assert_equal(len(restorewo_vault.listtransactions()), 3)

        self.log.info('Check transaction times after restoration')
        for tx in restorewo_vault.listtransactions():
            if tx['address'] == wo1:
                assert_equal(tx['blocktime'], cur_time + ten_days)
                assert_equal(tx['time'], cur_time + ten_days)
            elif tx['address'] == wo2:
                assert_equal(tx['blocktime'], cur_time + ten_days + ten_days)
                assert_equal(tx['time'], cur_time + ten_days + ten_days)
            elif tx['address'] == wo3:
                assert_equal(tx['blocktime'], cur_time + ten_days + ten_days + ten_days)
                assert_equal(tx['time'], cur_time + ten_days + ten_days + ten_days)


        self.log.info('Test handling of invalid parameters for rescanblockchain')
        assert_raises_rpc_error(-8, "Invalid start_height", restorewo_vault.rescanblockchain, -1, 10)
        assert_raises_rpc_error(-8, "Invalid stop_height", restorewo_vault.rescanblockchain, 1, -1)
        assert_raises_rpc_error(-8, "stop_height must be greater than start_height", restorewo_vault.rescanblockchain, 20, 10)

        self.log.info("Test `rescanblockchain` fails when vault is encrypted and locked")
        usernode.createvault(vault_name="enc_vault", passphrase="passphrase")
        enc_vault = usernode.get_vault_rpc("enc_vault")
        assert_raises_rpc_error(-13, "Error: Please enter the vault passphrase with vaultpassphrase first.", enc_vault.rescanblockchain)


if __name__ == '__main__':
    TransactionTimeRescanTest(__file__).main()
