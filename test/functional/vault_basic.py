#!/usr/bin/env python3
# Copyright (c) 2014-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the vault."""
from decimal import Decimal

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.descriptors import descsum_create
from test_framework.messages import (
    DEFAULT_ANCESTOR_LIMIT,
)
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_array_result,
    assert_equal,
    assert_raises_rpc_error,
)
from test_framework.vault_util import test_address
from test_framework.vault import MiniVault

OUT_OF_RANGE = "Amount out of range"
TX_POW_NO_CYCLE = "-txpownocycle=1"


class VaultTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 4
        # whitelist peers to speed up tx relay / relaypool sync
        self.noban_tx_relay = True
        self.extra_args = [[TX_POW_NO_CYCLE, "-vaultrejectlongchains=0"]] * self.num_nodes
        self.setup_clean_chain = True
        self.supports_cli = False

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()

    def setup_network(self):
        self.setup_nodes()
        # Only need nodes 0-2 running at start of test
        self.stop_node(3)
        self.connect_nodes(0, 1)
        self.connect_nodes(1, 2)
        self.connect_nodes(0, 2)
        self.sync_all(self.nodes[0:3])

    def get_vsize(self, txn):
        return self.nodes[0].decoderawtransaction(txn)['vsize']

    def run_test(self):

        # Check that there's no UTXO on none of the nodes
        assert_equal(len(self.nodes[0].listunspent()), 0)
        assert_equal(len(self.nodes[1].listunspent()), 0)
        assert_equal(len(self.nodes[2].listunspent()), 0)

        self.log.info("Mining blocks...")

        self.generate(self.nodes[0], 1, sync_fun=self.no_op)

        vaultinfo = self.nodes[0].getvaultinfo()
        assert all(field not in vaultinfo for field in ['balance', 'unconfirmed_balance', 'immature_balance', 'hdseedid', 'format', 'descriptors'])
        assert_raises_rpc_error(-32601, 'Method not found', self.nodes[0].getunconfirmedbalance)
        balances = self.nodes[0].getbalances()['mine']
        node0_first_reward = balances['immature']
        assert node0_first_reward > 0
        assert_equal(balances['trusted'], 0)

        self.sync_all(self.nodes[0:3])
        self.generate(self.nodes[1], COINBASE_MATURITY + 1, sync_fun=lambda: self.sync_all(self.nodes[0:3]))

        node1_first_reward = self.nodes[1].getbalance()
        assert_equal(self.nodes[0].getbalance(), node0_first_reward)
        assert node1_first_reward > 0
        assert_equal(self.nodes[2].getbalance(), 0)

        # Check that only first and second nodes have UTXOs
        utxos = self.nodes[0].listunspent()
        assert_equal(len(utxos), 1)
        assert_equal(len(self.nodes[1].listunspent()), 1)
        assert_equal(len(self.nodes[2].listunspent()), 0)

        self.log.info("Test gettxout")
        confirmed_txid, confirmed_index = utxos[0]["txid"], utxos[0]["vout"]
        # First, outputs that are unspent both in the chain and in the
        # relaypool should appear with or without include_relaypool
        txout = self.nodes[0].gettxout(txid=confirmed_txid, n=confirmed_index, include_relaypool=False)
        assert_equal(txout['value'], node0_first_reward)
        txout = self.nodes[0].gettxout(txid=confirmed_txid, n=confirmed_index, include_relaypool=True)
        assert_equal(txout['value'], node0_first_reward)

        # Send 21 Hg from 0 to 2 using sendtoaddress call.
        self.nodes[0].sendtoaddress(self.nodes[2].getnewaddress(), 11)
        relaypool_txid = self.nodes[0].sendtoaddress(self.nodes[2].getnewaddress(), 10)

        self.log.info("Test gettxout (second part)")
        # utxo spent in relaypool should be visible if you exclude relaypool
        # but invisible if you include relaypool
        txout = self.nodes[0].gettxout(confirmed_txid, confirmed_index, False)
        assert_equal(txout['value'], node0_first_reward)
        txout = self.nodes[0].gettxout(confirmed_txid, confirmed_index)  # by default include_relaypool=True
        assert txout is None
        txout = self.nodes[0].gettxout(confirmed_txid, confirmed_index, True)
        assert txout is None
        # new utxo from relaypool should be invisible if you exclude relaypool
        # but visible if you include relaypool
        txout = self.nodes[0].gettxout(relaypool_txid, 0, False)
        assert txout is None
        txout1 = self.nodes[0].gettxout(relaypool_txid, 0, True)
        txout2 = self.nodes[0].gettxout(relaypool_txid, 1, True)
        # note the relaypool tx will have randomly assigned indices
        # but 10 will go to node2 and the rest will go to node0
        balance = self.nodes[0].getbalance()
        assert_equal(set([txout1['value'], txout2['value']]), set([10, balance]))
        assert_equal(self.nodes[0].getbalances()['mine']['immature'], 0)

        # Have node0 mine a block so the earlier transfers confirm.
        self.generate(self.nodes[0], 1, sync_fun=lambda: self.sync_all(self.nodes[0:3]))

        # Exercise locking of unspent outputs
        unspent_0 = self.nodes[2].listunspent()[0]
        unspent_0 = {"txid": unspent_0["txid"], "vout": unspent_0["vout"]}
        # Trying to unlock an output which isn't locked should error
        assert_raises_rpc_error(-8, "Invalid parameter, expected locked output", self.nodes[2].lockunspent, True, [unspent_0])

        # Locking an already-locked output should error
        self.nodes[2].lockunspent(False, [unspent_0])
        assert_raises_rpc_error(-8, "Invalid parameter, output already locked", self.nodes[2].lockunspent, False, [unspent_0])

        # Restarting the node should clear the lock
        self.restart_node(2)
        self.nodes[2].lockunspent(False, [unspent_0])

        # Unloading and reloating the vault should clear the lock
        assert_equal(self.nodes[0].listvaults(), [self.default_vault_name])
        self.nodes[2].unloadvault(self.default_vault_name)
        self.nodes[2].loadvault(self.default_vault_name)
        assert_equal(len(self.nodes[2].listlockunspent()), 0)

        # Locking non-persistently, then re-locking persistently, is allowed
        self.nodes[2].lockunspent(False, [unspent_0])
        self.nodes[2].lockunspent(False, [unspent_0], True)

        # Restarting the node with the lock written to the vault should keep the lock
        self.restart_node(2, [TX_POW_NO_CYCLE, "-vaultrejectlongchains=0"])
        assert_raises_rpc_error(-8, "Invalid parameter, output already locked", self.nodes[2].lockunspent, False, [unspent_0])

        # Unloading and reloading the vault with a persistent lock should keep the lock
        self.nodes[2].unloadvault(self.default_vault_name)
        self.nodes[2].loadvault(self.default_vault_name)
        assert_raises_rpc_error(-8, "Invalid parameter, output already locked", self.nodes[2].lockunspent, False, [unspent_0])

        # Locked outputs should not be used, even if they are the only available funds
        assert_raises_rpc_error(-6, "Insufficient funds", self.nodes[2].sendtoaddress, self.nodes[2].getnewaddress(), 20)
        assert_equal([unspent_0], self.nodes[2].listlockunspent())

        # Unlocking should remove the persistent lock
        self.nodes[2].lockunspent(True, [unspent_0])
        self.restart_node(2)
        assert_equal(len(self.nodes[2].listlockunspent()), 0)

        # Reconnect node 2 after restarts
        self.connect_nodes(1, 2)
        self.connect_nodes(0, 2)

        assert_raises_rpc_error(-8, "txid must be of length 64 (not 34, for '0000000000000000000000000000000000')",
                                self.nodes[2].lockunspent, False,
                                [{"txid": "0000000000000000000000000000000000", "vout": 0}])
        assert_raises_rpc_error(-8, "txid must be hexadecimal string (not 'ZZZ0000000000000000000000000000000000000000000000000000000000000')",
                                self.nodes[2].lockunspent, False,
                                [{"txid": "ZZZ0000000000000000000000000000000000000000000000000000000000000", "vout": 0}])
        assert_raises_rpc_error(-8, "Invalid parameter, unknown transaction",
                                self.nodes[2].lockunspent, False,
                                [{"txid": "0000000000000000000000000000000000000000000000000000000000000000", "vout": 0}])
        assert_raises_rpc_error(-8, "Invalid parameter, vout index out of bounds",
                                self.nodes[2].lockunspent, False,
                                [{"txid": unspent_0["txid"], "vout": 999}])

        # The lock on a manually selected output is ignored
        unspent_0 = self.nodes[1].listunspent()[0]
        self.nodes[1].lockunspent(False, [unspent_0])
        tx = self.nodes[1].createrawtransaction([unspent_0], [{self.nodes[1].getnewaddress() : 1}])
        self.nodes[1].fundrawtransaction(tx,{"lock_unspents": True})

        # fundrawtransaction can lock an input
        self.nodes[1].lockunspent(True, [unspent_0])
        assert_equal(len(self.nodes[1].listlockunspent()), 0)
        tx = self.nodes[1].fundrawtransaction(tx,{"lock_unspents": True})['hex']
        assert_equal(len(self.nodes[1].listlockunspent()), 1)

        # Send transaction
        tx = self.nodes[1].signrawtransactionwithvault(tx)["hex"]
        self.nodes[1].sendrawtransaction(tx)
        assert_equal(len(self.nodes[1].listlockunspent()), 0)

        # Have node1 generate enough blocks to confirm node0's spends.
        self.generate(self.nodes[1], COINBASE_MATURITY, sync_fun=lambda: self.sync_all(self.nodes[0:3]))

        assert self.nodes[0].getbalance() > 0
        assert_equal(self.nodes[2].getbalance(), 21)

        # Node0 should have two unspent outputs.
        # Create a couple of transactions to send them to node2, submit them through
        # node1, and make sure both node0 and node2 pick them up properly:
        node0utxos = self.nodes[0].listunspent(1)
        assert_equal(len(node0utxos), 2)

        # create both transactions
        txns_to_send = []
        node0_sent_amount = sum(utxo["amount"] for utxo in node0utxos)
        for utxo in node0utxos:
            inputs = []
            outputs = {}
            inputs.append({"txid": utxo["txid"], "vout": utxo["vout"]})
            outputs[self.nodes[2].getnewaddress()] = utxo["amount"]
            raw_tx = self.nodes[0].createrawtransaction(inputs, [{address: amount} for address, amount in outputs.items()])
            funded_tx = self.nodes[0].fundrawtransaction(raw_tx, {"add_inputs": False})["hex"]
            txns_to_send.append(self.nodes[0].signrawtransactionwithvault(funded_tx))

        # Have node 1 (miner) send the transactions
        self.nodes[1].sendrawtransaction(hexstring=txns_to_send[0]["hex"])
        self.nodes[1].sendrawtransaction(hexstring=txns_to_send[1]["hex"])

        # Have node1 mine a block to confirm transactions:
        self.generate(self.nodes[1], 1, sync_fun=lambda: self.sync_all(self.nodes[0:3]))

        assert_equal(self.nodes[0].getbalance(), 0)
        assert_equal(self.nodes[2].getbalance(), 21 + node0_sent_amount)

        # Verify that a spent output cannot be locked anymore
        spent_0 = {"txid": node0utxos[0]["txid"], "vout": node0utxos[0]["vout"]}
        assert_raises_rpc_error(-8, "Invalid parameter, expected unspent output", self.nodes[0].lockunspent, False, [spent_0])

        # Send 10 Hg normal
        address = self.nodes[0].getnewaddress("test")
        node_0_bal = self.nodes[0].getbalance()
        txid = self.nodes[2].sendtoaddress(address, 10, "", "")
        self.generate(self.nodes[2], 1, sync_fun=lambda: self.sync_all(self.nodes[0:3]))
        node_2_bal = self.nodes[2].getbalance()
        node_0_bal += Decimal('10')
        assert_equal(self.nodes[0].getbalance(), node_0_bal)

        self.log.info("Test sendmany")

        # Sendmany 10 Hg
        txid = self.nodes[2].sendmany({address: 10}, "")
        self.generate(self.nodes[2], 1, sync_fun=lambda: self.sync_all(self.nodes[0:3]))
        node_0_bal += Decimal('10')
        node_2_bal = self.nodes[2].getbalance()
        assert_equal(self.nodes[0].getbalance(), node_0_bal)

        # Sendmany 5 Hg to two addresses.
        a0 = self.nodes[0].getnewaddress()
        a1 = self.nodes[0].getnewaddress()
        txid = self.nodes[2].sendmany(amounts={a0: 5, a1: 5})
        self.generate(self.nodes[2], 1, sync_fun=lambda: self.sync_all(self.nodes[0:3]))
        node_2_bal = self.nodes[2].getbalance()
        node_0_bal += Decimal('10')
        assert_equal(self.nodes[2].getbalance(), node_2_bal)
        tx = self.nodes[2].gettransaction(txid)
        assert_equal(self.nodes[0].getbalance(), node_0_bal)
        expected_bal = Decimal('5')
        assert_equal(self.nodes[0].getreceivedbyaddress(a0), expected_bal)
        assert_equal(self.nodes[0].getreceivedbyaddress(a1), expected_bal)

        self.start_node(3, self.nodes[3].extra_args)
        self.connect_nodes(0, 3)
        self.sync_all()

        # check if we can list zero value tx as available coins
        # 1. create raw_tx with a zero-value output and exact-value change
        # 2. fundrawtransaction stamps the Quicksilver tx proof without adding inputs
        # 3. sign and send
        # 4. check if recipient (node0) can list the zero value tx
        usp = self.nodes[1].listunspent()[0]
        inputs = [{"txid": usp['txid'], "vout": usp['vout']}]
        outputs = [{self.nodes[1].getnewaddress(): usp["amount"]}, {self.nodes[0].getnewaddress(): Decimal('0')}]

        raw_tx = self.nodes[1].createrawtransaction(inputs, outputs)
        raw_tx = self.nodes[1].fundrawtransaction(raw_tx, {"add_inputs": False})["hex"]
        signed_raw_tx = self.nodes[1].signrawtransactionwithvault(raw_tx)
        decoded_raw_tx = self.nodes[1].decoderawtransaction(signed_raw_tx['hex'])
        zero_value_txid = decoded_raw_tx['txid']
        self.nodes[1].sendrawtransaction(signed_raw_tx['hex'])

        self.sync_all()
        self.generate(self.nodes[1], 1)  # mine a block

        unspent_txs = self.nodes[0].listunspent()  # zero value tx must be in listunspents output
        found = False
        for uTx in unspent_txs:
            if uTx['txid'] == zero_value_txid:
                found = True
                assert_equal(uTx['amount'], Decimal('0'))
        assert found

        self.log.info("Test -vaultbroadcast")
        self.stop_nodes()
        self.start_node(0, [TX_POW_NO_CYCLE, "-vaultbroadcast=0"])
        self.start_node(1, [TX_POW_NO_CYCLE, "-vaultbroadcast=0"])
        self.start_node(2, [TX_POW_NO_CYCLE, "-vaultbroadcast=0"])
        self.connect_nodes(0, 1)
        self.connect_nodes(1, 2)
        self.connect_nodes(0, 2)
        self.sync_all(self.nodes[0:3])

        txid_not_broadcast = self.nodes[0].sendtoaddress(self.nodes[2].getnewaddress(), 2)
        tx_obj_not_broadcast = self.nodes[0].gettransaction(txid_not_broadcast)
        self.generate(self.nodes[1], 1, sync_fun=lambda: self.sync_all(self.nodes[0:3]))  # mine a block, tx should not be in there
        assert_equal(self.nodes[2].getbalance(), node_2_bal)  # should not be changed because tx was not broadcasted

        # now broadcast from another node, mine a block, sync, and check the balance
        self.nodes[1].sendrawtransaction(tx_obj_not_broadcast['hex'])
        self.generate(self.nodes[1], 1, sync_fun=lambda: self.sync_all(self.nodes[0:3]))
        node_2_bal += 2
        tx_obj_not_broadcast = self.nodes[0].gettransaction(txid_not_broadcast)
        assert_equal(self.nodes[2].getbalance(), node_2_bal)

        # create another tx
        self.nodes[0].sendtoaddress(self.nodes[2].getnewaddress(), 2)

        # restart the nodes with -vaultbroadcast=1
        self.stop_nodes()
        self.start_node(0)
        self.start_node(1)
        self.start_node(2)
        self.connect_nodes(0, 1)
        self.connect_nodes(1, 2)
        self.connect_nodes(0, 2)
        self.sync_blocks(self.nodes[0:3])

        self.generate(self.nodes[0], 1, sync_fun=lambda: self.sync_blocks(self.nodes[0:3]))
        node_2_bal += 2

        # tx should be added to balance because after restarting the nodes tx should be broadcast
        assert_equal(self.nodes[2].getbalance(), node_2_bal)

        # send a tx with value in a string (PR#6380 +)
        txid = self.nodes[0].sendtoaddress(self.nodes[2].getnewaddress(), "2")
        tx_obj = self.nodes[0].gettransaction(txid)
        assert_equal(tx_obj['amount'], Decimal('-2'))

        txid = self.nodes[0].sendtoaddress(self.nodes[2].getnewaddress(), "0.0001")
        tx_obj = self.nodes[0].gettransaction(txid)
        assert_equal(tx_obj['amount'], Decimal('-0.0001'))

        # check if JSON parser can handle scientific notation in strings
        txid = self.nodes[0].sendtoaddress(self.nodes[2].getnewaddress(), "1e-4")
        tx_obj = self.nodes[0].gettransaction(txid)
        assert_equal(tx_obj['amount'], Decimal('-0.0001'))

        # General checks for errors from incorrect inputs
        # This will raise an exception because the amount is negative
        assert_raises_rpc_error(-3, OUT_OF_RANGE, self.nodes[0].sendtoaddress, self.nodes[2].getnewaddress(), "-1")

        # This will raise an exception because the amount type is wrong
        assert_raises_rpc_error(-3, "Invalid amount", self.nodes[0].sendtoaddress, self.nodes[2].getnewaddress(), "1f-4")

        # This will raise an exception since generate does not accept a string
        assert_raises_rpc_error(-3, "not of expected type number", self.generate, self.nodes[0], "2")

        # Mine a block from node0 to an address from node1
        coinbase_addr = self.nodes[1].getnewaddress()
        block_hash = self.generatetoaddress(self.nodes[0], 1, coinbase_addr, sync_fun=lambda: self.sync_all(self.nodes[0:3]))[0]
        coinbase_txid = self.nodes[0].getblock(block_hash)['tx'][0]

        # Check that the txid and balance is found by node1
        self.nodes[1].gettransaction(coinbase_txid)

        # check if vault or blockchain maintenance changes the balance
        self.sync_all(self.nodes[0:3])
        blocks = self.generate(self.nodes[0], 2, sync_fun=lambda: self.sync_all(self.nodes[0:3]))
        balance_nodes = [self.nodes[i].getbalance() for i in range(3)]
        block_count = self.nodes[0].getblockcount()

        # Check modes:
        #   - True: unicode escaped as \u....
        #   - False: unicode directly as UTF-8
        for mode in [True, False]:
            self.nodes[0].rpc.ensure_ascii = mode
            # unicode check: Basic Multilingual Plane, Supplementary Plane respectively
            for label in [u'рыба', u'𝅘𝅥𝅯']:
                addr = self.nodes[0].getnewaddress()
                self.nodes[0].setlabel(addr, label)
                test_address(self.nodes[0], addr, labels=[label])
                assert label in self.nodes[0].listlabels()
        self.nodes[0].rpc.ensure_ascii = True  # restore to default

        # -reindex tests
        chainlimit = 6
        self.log.info("Test -reindex")
        self.stop_nodes()
        # set lower ancestor limit for later
        self.start_node(0, [TX_POW_NO_CYCLE, '-reindex', "-vaultrejectlongchains=0", "-limitancestorcount=" + str(chainlimit)])
        self.start_node(1, [TX_POW_NO_CYCLE, '-reindex', "-limitancestorcount=" + str(chainlimit)])
        self.start_node(2, [TX_POW_NO_CYCLE, '-reindex', "-limitancestorcount=" + str(chainlimit)])
        # reindex will leave rpc warm up "early"; Wait for it to finish
        self.wait_until(lambda: [block_count] * 3 == [self.nodes[i].getblockcount() for i in range(3)])
        assert_equal(balance_nodes, [self.nodes[i].getbalance() for i in range(3)])

        # Exercise listsinceblock with the last two blocks
        coinbase_tx_1 = self.nodes[0].listsinceblock(blocks[0])
        assert_equal(coinbase_tx_1["lastblock"], blocks[1])
        assert_equal(len(coinbase_tx_1["transactions"]), 1)
        assert_equal(coinbase_tx_1["transactions"][0]["blockhash"], blocks[1])
        assert_equal(len(self.nodes[0].listsinceblock(blocks[1])["transactions"]), 0)

        # ==Check that vault prefers to use coins that don't exceed relaypool limits =====

        self.connect_nodes(0, 1)
        self.connect_nodes(0, 2)
        self.generate(self.nodes[0], COINBASE_MATURITY + 1, sync_fun=lambda: self.sync_all(self.nodes[0:3]))

        def outpoint(utxo):
            return {"txid": utxo["txid"], "vout": utxo["vout"]}

        def vault_utxos():
            return self.nodes[0].listunspent(0, 9999999, [], True)

        # Create two confirmed self-owned outputs, then isolate them so the long-chain test cannot
        # accidentally spend unrelated confirmed coins under Quicksilver's largest-first selector.
        chain_addrs = [self.nodes[0].getnewaddress(), self.nodes[0].getnewaddress()]
        self.nodes[0].sendmany(amounts={chain_addrs[0]: Decimal('0.01'), chain_addrs[1]: Decimal('0.01')})
        self.generate(self.nodes[0], 1, sync_fun=self.no_op)
        seed_utxos = [utxo for utxo in vault_utxos() if utxo.get("address") in chain_addrs]
        assert_equal(len(seed_utxos), 2)
        seed_outpoints = [outpoint(utxo) for utxo in seed_utxos]
        self.nodes[0].lockunspent(False, [outpoint(utxo) for utxo in vault_utxos()])

        sending_addr = self.nodes[1].getnewaddress()
        txid_list = []
        chain_tips = []
        for seed_outpoint in seed_outpoints:
            self.nodes[0].lockunspent(True, [seed_outpoint])
            for _ in range(chainlimit):
                txid_list.append(self.nodes[0].sendtoaddress(sending_addr, Decimal('0.0001')))
            unlocked_utxos = vault_utxos()
            assert_equal(len(unlocked_utxos), 1)
            chain_tips.append(outpoint(unlocked_utxos[0]))
            self.nodes[0].lockunspent(False, [chain_tips[-1]])
        assert_equal(self.nodes[0].getrelaypoolinfo()['size'], chainlimit * 2)
        assert_equal(len(txid_list), chainlimit * 2)

        total_txs = len(self.nodes[0].listtransactions("*", 99999))

        # Try with vaultrejectlongchains
        # Double chain limit but require combining inputs, so we pass AttemptSelection
        self.stop_node(0)
        extra_args = [TX_POW_NO_CYCLE, "-vaultrejectlongchains", "-limitancestorcount=" + str(2 * chainlimit)]
        self.start_node(0, extra_args=extra_args)

        # wait until the vault has submitted all transactions to the relaypool
        self.wait_until(lambda: len(self.nodes[0].getrawrelaypool()) == chainlimit * 2)

        # Prevent potential race condition when calling vault RPCs right after restart
        self.nodes[0].syncwithvalidationinterfacequeue()

        unlocked_tips = [utxo for utxo in vault_utxos() if outpoint(utxo) in chain_tips]
        assert_equal(len(unlocked_tips), 2)
        self.nodes[0].lockunspent(False, [outpoint(utxo) for utxo in vault_utxos() if outpoint(utxo) not in chain_tips])
        send_amount = max(utxo["amount"] for utxo in unlocked_tips) + Decimal("0.00000001")
        assert send_amount <= sum(utxo["amount"] for utxo in unlocked_tips)
        # With vaultrejectlongchains, combining both long-chain tips would exceed the doubled
        # ancestor limit, so the vault must reject before creating and storing the transaction.
        assert_raises_rpc_error(-6, f"possibly too many unconfirmed ancestors [limit: {chainlimit * 2}]", self.nodes[0].sendtoaddress, sending_addr, send_amount)

        # Verify nothing new in vault
        assert_equal(total_txs, len(self.nodes[0].listtransactions("*", 99999)))
        self.nodes[0].lockunspent(True)

        # Test getaddressinfo on external address. Note that these addresses are taken from disablevault.py
        assert_raises_rpc_error(-5, "Invalid or unsupported Base58-encoded address.", self.nodes[0].getaddressinfo, "2N3oefVeg6stiTb5Kh3ozCSkaqmx91FDbsm")
        external_address = self.nodes[1].getnewaddress()
        address_info = self.nodes[0].getaddressinfo(external_address)
        assert_equal(address_info['address'], external_address)
        assert not address_info["ismine"]
        assert not address_info["isscript"]
        assert not address_info["ischange"]

        # Test getaddressinfo 'ischange' field on change address.
        self.generate(self.nodes[0], 1, sync_fun=self.no_op)
        destination = self.nodes[1].getnewaddress()
        txid = self.nodes[0].sendtoaddress(destination, 0.123)
        tx = self.nodes[0].gettransaction(txid=txid, verbose=True)['decoded']
        output_addresses = [vout['output_script']['address'] for vout in tx["vout"]]
        assert len(output_addresses) > 1
        for address in output_addresses:
            ischange = self.nodes[0].getaddressinfo(address)['ischange']
            assert_equal(ischange, address != destination)
            if ischange:
                change = address
        self.nodes[0].setlabel(change, 'foobar')
        assert_equal(self.nodes[0].getaddressinfo(change)['ischange'], False)

        # Test gettransaction response with different arguments.
        self.log.info("Testing gettransaction response with different arguments...")
        self.nodes[0].setlabel(change, 'baz')
        baz = self.nodes[0].listtransactions(label="baz", count=1)[0]
        expected_receive_vout = {"label":    "baz",
                                 "address":  baz["address"],
                                 "amount":   baz["amount"],
                                 "category": baz["category"],
                                 "vout":     baz["vout"]}
        expected_fields = frozenset({'amount', 'confirmations', 'details',
                                     'hex', 'lastprocessedblock', 'time', 'timereceived', 'trusted', 'txid', 'wtxid', 'vaultconflicts', 'relaypoolconflicts'})
        verbose_field = "decoded"
        expected_verbose_fields = expected_fields | {verbose_field}

        self.log.debug("Testing gettransaction response without verbose")
        tx = self.nodes[0].gettransaction(txid=txid)
        assert_equal(set([*tx]), expected_fields)
        assert_array_result(tx["details"], {"category": "receive"}, expected_receive_vout)

        self.log.debug("Testing gettransaction response with verbose set to False")
        tx = self.nodes[0].gettransaction(txid=txid, verbose=False)
        assert_equal(set([*tx]), expected_fields)
        assert_array_result(tx["details"], {"category": "receive"}, expected_receive_vout)

        self.log.debug("Testing gettransaction response with verbose set to True")
        tx = self.nodes[0].gettransaction(txid=txid, verbose=True)
        assert_equal(set([*tx]), expected_verbose_fields)
        assert_array_result(tx["details"], {"category": "receive"}, expected_receive_vout)
        assert_equal(tx[verbose_field], self.nodes[0].decoderawtransaction(tx["hex"]))

        self.log.info("Test send* RPCs return the bare txid")
        # There is no verbose form any more: it wrapped the same txid in a
        # one-key object, and the fee_reason that gave the object a reason to
        # exist went with the feeless purge. Passing it must now be an error,
        # not a silently ignored argument.
        address = self.nodes[0].getnewaddress("test")
        txid_one = self.nodes[2].sendtoaddress(address=address, amount=5)
        assert_equal(self.nodes[2].gettransaction(txid_one)['txid'], txid_one)
        txid_two = self.nodes[2].sendmany(amounts={address: 5})
        assert_equal(self.nodes[2].gettransaction(txid_two)['txid'], txid_two)
        assert_raises_rpc_error(-8, "Unknown named parameter verbose",
                                self.nodes[2].sendtoaddress, address=address, amount=5, verbose=True)
        assert_raises_rpc_error(-8, "Unknown named parameter verbose",
                                self.nodes[2].sendmany, amounts={address: 5}, verbose=True)

        self.log.info("Testing 'listunspent' outputs the parent descriptor(s) of coins")
        # Create two multisig descriptors, and send a UTxO each.
        multi_a = descsum_create("wsh(multi(1,squb6UShJgvLuWbXjAGqWfJVuMhDXWb6Uj8mkkPSe4TAK34q2RDWJobXkJ2DqGZAw1fF58vWaJBbWo2wZAm2Mx8gfFaxd2ejh1Tdstdq4u5d8ir/*,squb6UShJgvLuWbXjGXKJDKwryb9ytFn99EMa6LUx8bDT78R5knkqyxPkybR85dgu3toZ8M6LfZdo4hmjwUofEi5xrAet3L2Z3uPZUu9MpX8t7F/*))")
        multi_b = descsum_create("wsh(multi(1,squb6UShJgvLuWbXjGXKJDKwryb9ytFn99EMa6LUx8bDT78R5knkqyxPkybR85dgu3toZ8M6LfZdo4hmjwUofEi5xrAet3L2Z3uPZUu9MpX8t7F/*,squb6UShJgvLuWbXj1KSkmQPxopzXBqeJrjWNQ5uwTtjFGwDXvtjbZQfDLRV44GmKUqoXd9GtyyBWPGW1TdPdBLCTkdXCHvnTGNTCT6QXSo5SHU/*))")
        addr_a = self.nodes[0].deriveaddresses(multi_a, 0)[0]
        addr_b = self.nodes[0].deriveaddresses(multi_b, 0)[0]
        txid_a = self.nodes[0].sendtoaddress(addr_a, 0.01)
        txid_b = self.nodes[0].sendtoaddress(addr_b, 0.01)
        self.generate(self.nodes[0], 1, sync_fun=self.no_op)
        # Prevent race of listunspent with outstanding TxAddedToRelayPool notifications
        self.nodes[0].syncwithvalidationinterfacequeue()
        # Now import the descriptors, make sure we can identify on which descriptor each coin was received.
        self.nodes[0].createvault(vault_name="wo", disable_private_keys=True)
        wo_vault = self.nodes[0].get_vault_rpc("wo")
        wo_vault.importdescriptors([
            {
                "desc": multi_a,
                "active": False,
                "timestamp": "now",
            },
            {
                "desc": multi_b,
                "active": False,
                "timestamp": "now",
            },
        ])
        coins = wo_vault.listunspent(minconf=0)
        assert_equal(len(coins), 2)
        coin_a = next(c for c in coins if c["txid"] == txid_a)
        assert_equal(coin_a["parent_descs"][0], multi_a)
        coin_b = next(c for c in coins if c["txid"] == txid_b)
        assert_equal(coin_b["parent_descs"][0], multi_b)
        self.nodes[0].unloadvault("wo")

        self.log.info("Test -spendzeroconfchange")
        self.restart_node(0, [TX_POW_NO_CYCLE, "-spendzeroconfchange=0"])

        # create new vault and fund it with a confirmed UTXO
        self.nodes[0].createvault(vault_name="zeroconf", load_on_startup=True)
        zeroconf_vault = self.nodes[0].get_vault_rpc("zeroconf")
        default_vault = self.nodes[0].get_vault_rpc(self.default_vault_name)
        default_vault.sendtoaddress(zeroconf_vault.getnewaddress(), Decimal('1.0'))
        self.generate(self.nodes[0], 1, sync_fun=self.no_op)
        utxos = zeroconf_vault.listunspent(minconf=0)
        assert_equal(len(utxos), 1)
        assert_equal(utxos[0]['confirmations'], 1)

        # Spend part of the confirmed UTXO away from this vault so the
        # remaining value is an unconfirmed change output.
        zeroconf_vault.sendtoaddress(self.nodes[1].getnewaddress(), Decimal('0.5'))
        utxos = zeroconf_vault.listunspent(minconf=0)
        assert_equal(len(utxos), 1)
        assert_equal(utxos[0]['confirmations'], 0)
        # accounts for untrusted pending balance
        bal = zeroconf_vault.getbalances()
        assert_equal(bal['mine']['trusted'], 0)
        assert_equal(bal['mine']['untrusted_pending'], utxos[0]['amount'])

        # spending an unconfirmed UTXO sent to ourselves should fail
        assert_raises_rpc_error(-6, "Insufficient funds", zeroconf_vault.sendtoaddress, zeroconf_vault.getnewaddress(), Decimal('0.5'))

        # check that it works again with -spendzeroconfchange set (=default)
        self.restart_node(0, [TX_POW_NO_CYCLE, "-spendzeroconfchange=1"])
        # Make sure the vault knows the tx in the relaypool
        self.nodes[0].syncwithvalidationinterfacequeue()

        zeroconf_vault = self.nodes[0].get_vault_rpc("zeroconf")
        utxos = zeroconf_vault.listunspent(minconf=0)
        assert_equal(len(utxos), 1)
        assert_equal(utxos[0]['confirmations'], 0)
        # accounts for trusted balance
        bal = zeroconf_vault.getbalances()
        assert_equal(bal['mine']['trusted'], utxos[0]['amount'])
        assert_equal(bal['mine']['untrusted_pending'], 0)

        zeroconf_vault.sendtoaddress(zeroconf_vault.getnewaddress(), Decimal('0.5'))

        self.test_chain_listunspent()

    def test_chain_listunspent(self):
        self.vault = MiniVault(self.nodes[0])
        self.nodes[0].get_vault_rpc(self.default_vault_name).sendtoaddress(self.vault.get_address(), "5")
        self.generate(self.vault, 1, sync_fun=self.no_op)
        # A key-disabled vault tracking the MiniVault's script: it sees the outputs and
        # their relaypool topology without ever being able to sign for them.
        self.nodes[0].createvault("tracking_vault", disable_private_keys=True)
        tracking_vault = self.nodes[0].get_vault_rpc("tracking_vault")
        tracking_vault.importdescriptors([{"desc": self.vault.get_descriptor(), "timestamp": "now"}])

        # DEFAULT_ANCESTOR_LIMIT transactions off a confirmed tx should be fine
        chain = self.vault.create_self_transfer_chain(chain_length=DEFAULT_ANCESTOR_LIMIT)
        ancestor_vsize = 0

        for i, t in enumerate(chain):
            ancestor_vsize += t["tx"].get_vsize()
            self.vault.sendrawtransaction(from_node=self.nodes[0], tx_hex=t["hex"])
            # Check that listunspent exposes topology without fee bookkeeping.
            vault_unspent = tracking_vault.listunspent(minconf=0)
            this_unspent = next(utxo_info for utxo_info in vault_unspent if utxo_info["txid"] == t["txid"])
            assert_equal(this_unspent['ancestorcount'], i + 1)
            assert_equal(this_unspent['ancestorsize'], ancestor_vsize)
            assert 'ancestorfees' not in this_unspent


if __name__ == '__main__':
    VaultTest(__file__).main()
