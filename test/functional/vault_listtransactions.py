#!/usr/bin/env python3
# Copyright (c) 2014-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the listtransactions API."""

from decimal import Decimal
import shutil

from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_array_result,
    assert_equal,
    assert_raises_rpc_error,
)


class ListTransactionsTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 3
        # whitelist peers to speed up tx relay / relaypool sync
        self.noban_tx_relay = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()

    def run_test(self):
        self.log.info("Test simple send from node0 to node1")
        txid = self.nodes[0].sendtoaddress(self.nodes[1].getnewaddress(), 0.1)
        self.sync_all()
        assert_array_result(self.nodes[0].listtransactions(),
                            {"txid": txid},
                            {"category": "send", "amount": Decimal("-0.1"), "confirmations": 0, "trusted": True})
        assert_array_result(self.nodes[1].listtransactions(),
                            {"txid": txid},
                            {"category": "receive", "amount": Decimal("0.1"), "confirmations": 0, "trusted": False})
        self.log.info("Test confirmations change after mining a block")
        blockhash = self.generate(self.nodes[0], 1)[0]
        blockheight = self.nodes[0].getblockheader(blockhash)['height']
        assert_array_result(self.nodes[0].listtransactions(),
                            {"txid": txid},
                            {"category": "send", "amount": Decimal("-0.1"), "confirmations": 1, "blockhash": blockhash, "blockheight": blockheight})
        assert_array_result(self.nodes[1].listtransactions(),
                            {"txid": txid},
                            {"category": "receive", "amount": Decimal("0.1"), "confirmations": 1, "blockhash": blockhash, "blockheight": blockheight})

        self.log.info("Test send-to-self on node0")
        txid = self.nodes[0].sendtoaddress(self.nodes[0].getnewaddress(), 0.2)
        assert_array_result(self.nodes[0].listtransactions(),
                            {"txid": txid, "category": "send"},
                            {"amount": Decimal("-0.2")})
        assert_array_result(self.nodes[0].listtransactions(),
                            {"txid": txid, "category": "receive"},
                            {"amount": Decimal("0.2")})

        self.log.info("Test sendmany from node1: twice to self, twice to node0")
        send_to = {self.nodes[0].getnewaddress(): 0.11,
                   self.nodes[1].getnewaddress(): 0.22,
                   self.nodes[0].getnewaddress(): 0.33,
                   self.nodes[1].getnewaddress(): 0.44}
        txid = self.nodes[1].sendmany(send_to)
        self.sync_all()
        assert_array_result(self.nodes[1].listtransactions(),
                            {"category": "send", "amount": Decimal("-0.11")},
                            {"txid": txid})
        assert_array_result(self.nodes[0].listtransactions(),
                            {"category": "receive", "amount": Decimal("0.11")},
                            {"txid": txid})
        assert_array_result(self.nodes[1].listtransactions(),
                            {"category": "send", "amount": Decimal("-0.22")},
                            {"txid": txid})
        assert_array_result(self.nodes[1].listtransactions(),
                            {"category": "receive", "amount": Decimal("0.22")},
                            {"txid": txid})
        assert_array_result(self.nodes[1].listtransactions(),
                            {"category": "send", "amount": Decimal("-0.33")},
                            {"txid": txid})
        assert_array_result(self.nodes[0].listtransactions(),
                            {"category": "receive", "amount": Decimal("0.33")},
                            {"txid": txid})
        assert_array_result(self.nodes[1].listtransactions(),
                            {"category": "send", "amount": Decimal("-0.44")},
                            {"txid": txid})
        assert_array_result(self.nodes[1].listtransactions(),
                            {"category": "receive", "amount": Decimal("0.44")},
                            {"txid": txid})

        self.run_externally_generated_address_test()
        self.run_invalid_parameters_test()
        self.test_op_return()

    def run_externally_generated_address_test(self):
        """Test behavior when receiving address is not in the address book."""

        self.log.info("Setup the same vault on two nodes")
        # refill keypool otherwise the second node wouldn't recognize addresses generated on the first nodes
        self.nodes[0].keypoolrefill(1000)
        self.stop_nodes()
        vault0 = self.nodes[0].vaults_path / self.default_vault_name / self.vault_data_filename
        vault2 = self.nodes[2].vaults_path / self.default_vault_name / self.vault_data_filename
        shutil.copyfile(vault0, vault2)
        self.start_nodes()
        # reconnect nodes
        self.connect_nodes(0, 1)
        self.connect_nodes(1, 2)
        self.connect_nodes(2, 0)

        addr1 = self.nodes[0].getnewaddress("pizza1", 'bech32')
        addr2 = self.nodes[0].getnewaddress("pizza2", 'base58')
        addr3 = self.nodes[0].getnewaddress("pizza3", 'bech32')

        self.log.info("Send to externally generated addresses")
        # send to an address beyond the next to be generated to test the keypool gap
        self.nodes[1].sendtoaddress(addr3, "0.001")
        self.generate(self.nodes[1], 1)

        # send to an address that is already marked as used due to the keypool gap mechanics
        self.nodes[1].sendtoaddress(addr2, "0.001")
        self.generate(self.nodes[1], 1)

        # send to self transaction
        self.nodes[0].sendtoaddress(addr1, "0.001")
        self.generate(self.nodes[0], 1)

        self.log.info("Verify listtransactions is the same regardless of where the address was generated")
        transactions0 = self.nodes[0].listtransactions()
        transactions2 = self.nodes[2].listtransactions()

        # normalize results: remove fields that normally could differ and sort
        def normalize_list(txs):
            for tx in txs:
                tx.pop('label', None)
                tx.pop('time', None)
                tx.pop('timereceived', None)
            txs.sort(key=lambda x: x['txid'])

        normalize_list(transactions0)
        normalize_list(transactions2)
        assert_equal(transactions0, transactions2)

        self.log.info("Verify labels are persistent on the node that generated the addresses")
        assert_equal(['pizza1'], self.nodes[0].getaddressinfo(addr1)['labels'])
        assert_equal(['pizza2'], self.nodes[0].getaddressinfo(addr2)['labels'])
        assert_equal(['pizza3'], self.nodes[0].getaddressinfo(addr3)['labels'])

    def run_invalid_parameters_test(self):
        self.log.info("Test listtransactions RPC parameter validity")
        assert_raises_rpc_error(-8, 'Label argument must be a valid label name or "*".', self.nodes[0].listtransactions, label="")
        self.nodes[0].listtransactions(label="*")
        assert_raises_rpc_error(-8, "Negative count", self.nodes[0].listtransactions, count=-1)
        assert_raises_rpc_error(-8, "Negative from", self.nodes[0].listtransactions, skip=-1)

    def test_op_return(self):
        """Test if OP_RETURN outputs will be displayed correctly."""
        raw_tx = self.nodes[0].createrawtransaction([], [
            {self.nodes[0].getnewaddress(): Decimal("0.1")},
            {'data': 'aa'},
        ])
        funded_tx = self.nodes[0].fundrawtransaction(raw_tx)
        signed_tx = self.nodes[0].signrawtransactionwithvault(funded_tx['hex'])
        tx_id = self.nodes[0].sendrawtransaction(signed_tx['hex'])

        op_ret_tx = [tx for tx in self.nodes[0].listtransactions() if tx['txid'] == tx_id and 'address' not in tx][0]

        assert 'address' not in op_ret_tx


if __name__ == '__main__':
    ListTransactionsTest(__file__).main()
