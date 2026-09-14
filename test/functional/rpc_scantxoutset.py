#!/usr/bin/env python3
# Copyright (c) 2018-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the scantxoutset rpc call."""
from test_framework.address import address_to_scriptpubkey
from test_framework.blocktools import COINBASE_MATURITY
from test_framework.messages import COIN
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.vault import (
    MiniVault,
    getnewdestination,
)

from decimal import Decimal


def descriptors(out):
    return sorted(u['desc'] for u in out['unspents'])


class ScantxoutsetTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-txpownocycle=1"]]

    def sendtodestination(self, destination, amount):
        # interpret strings as addresses, assume scriptPubKey otherwise
        if isinstance(destination, str):
            destination = address_to_scriptpubkey(destination)
        self.vault.send_to(from_node=self.nodes[0], scriptPubKey=destination, amount=int(COIN * amount))

    def run_test(self):
        self.vault = MiniVault(self.nodes[0])
        self.generate(self.vault, COINBASE_MATURITY + 1)

        self.log.info("Test if we find coinbase outputs.")
        coinbase_unspents = self.nodes[0].scantxoutset("start", [self.vault.get_descriptor()])["unspents"]
        assert all(u["coinbase"] for u in coinbase_unspents)
        assert len(coinbase_unspents) >= COINBASE_MATURITY + 1

        self.log.info("Create UTXOs...")
        pubk1, spk_P2PKH, addr_P2PKH = getnewdestination("base58")
        pubk2, spk_BECH32, addr_BECH32 = getnewdestination("bech32")
        pubk3, _, _ = getnewdestination("bech32")
        self.sendtodestination(spk_P2PKH, 0.002)
        self.sendtodestination(spk_BECH32, 0.004)

        #send to child keys of sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7
        self.sendtodestination("SS4XjynUG3QHYfkHPQkpg2BWsgRg3J9876", 0.000008)  # (m/0'/0'/0')
        self.sendtodestination("SQbXBDTU64nGbvkmhFRbWN5DaGSVvzRMBz", 0.000016)  # (m/0'/0'/1')
        self.sendtodestination("Sitfu4LGHAmE1YTitT7YyDB714Tq8kZ1m9", 0.000032)  # (m/0'/0'/1500')
        self.sendtodestination("SXDCAcNJhi67k9Z5njgx4BjE4yWXKe71aB", 0.000064)  # (m/0'/0'/0)
        self.sendtodestination("SUEipUBgmPcYSUtAZAvoxDAqttCZ4qVs2d", 0.000128)  # (m/0'/0'/1)
        self.sendtodestination("SSRvwQnM4ouRPqKZ2jqWwHER5TZS1eDLyA", 0.000256)  # (m/0'/0'/1500)
        self.sendtodestination("SQv2znYm6xPXFj7n2G2W7Y15WapDtuJQ4Z", 0.000512)  # (m/1/1/0')
        self.sendtodestination("SMZNZ76QbEYfKzXrYPv45dKeFq2CkyfA64", 0.001024)  # (m/1/1/1')
        self.sendtodestination("SVg9LxhvVimj7VerhizCpXNBaQhbbBdi1C", 0.002048)  # (m/1/1/1500')
        self.sendtodestination("SaSXYGH84fpqjiUGeo2WBFtkmbSQgzgsxB", 0.004096)  # (m/1/1/0)
        self.sendtodestination("SebAfuRJMLvmqXSHiJ36ZUs8fquzojhZK8", 0.008192)  # (m/1/1/1)
        self.sendtodestination("SWBBbbSLd9mPv5cFExFoBZR1Wx9U5oKP7D", 0.016384)  # (m/1/1/1500)

        self.generate(self.nodes[0], 1)

        scan = self.nodes[0].scantxoutset("start", [])
        info = self.nodes[0].gettxoutsetinfo()
        assert_equal(scan['success'], True)
        assert_equal(scan['height'], info['height'])
        assert_equal(scan['txouts'], info['txouts'])
        assert_equal(scan['bestblock'], info['bestblock'])

        self.log.info("Test if we have found the non HD unspent outputs.")
        assert_equal(self.nodes[0].scantxoutset("start", ["pkh(" + pubk1.hex() + ")", "pkh(" + pubk2.hex() + ")", "pkh(" + pubk3.hex() + ")"])['total_amount'], Decimal("0.002"))
        assert_equal(self.nodes[0].scantxoutset("start", ["wpkh(" + pubk1.hex() + ")", "wpkh(" + pubk2.hex() + ")", "wpkh(" + pubk3.hex() + ")"])['total_amount'], Decimal("0.004"))
        assert_raises_rpc_error(-5, "Can only have wpkh() at top level", self.nodes[0].scantxoutset, "start", ["sh(wpkh(" + pubk1.hex() + "))"])
        assert_equal(self.nodes[0].scantxoutset("start", ["combo(" + pubk1.hex() + ")", "combo(" + pubk2.hex() + ")", "combo(" + pubk3.hex() + ")"])['total_amount'], Decimal("0.006"))
        assert_equal(self.nodes[0].scantxoutset("start", ["addr(" + addr_P2PKH + ")", "addr(" + addr_BECH32 + ")"])['total_amount'], Decimal("0.006"))
        assert_equal(self.nodes[0].scantxoutset("start", ["addr(" + addr_P2PKH + ")", "combo(" + pubk2.hex() + ")"])['total_amount'], Decimal("0.006"))

        self.log.info("Test range validation.")
        assert_raises_rpc_error(-8, "End of range is too high", self.nodes[0].scantxoutset, "start", [{"desc": "desc", "range": -1}])
        assert_raises_rpc_error(-8, "Range should be greater or equal than 0", self.nodes[0].scantxoutset, "start", [{"desc": "desc", "range": [-1, 10]}])
        assert_raises_rpc_error(-8, "End of range is too high", self.nodes[0].scantxoutset, "start", [{"desc": "desc", "range": [(2 << 31 + 1) - 1000000, (2 << 31 + 1)]}])
        assert_raises_rpc_error(-8, "Range specified as [begin,end] must not have begin after end", self.nodes[0].scantxoutset, "start", [{"desc": "desc", "range": [2, 1]}])
        assert_raises_rpc_error(-8, "Range is too large", self.nodes[0].scantxoutset, "start", [{"desc": "desc", "range": [0, 1000001]}])

        self.log.info("Test extended key derivation.")
        # Run various scans, and verify that the sum of the amounts of the matches corresponds to the expected subset.
        # Note that all amounts in the UTXO set are powers of 2 multiplied by 0.000001 Hg, so each amounts uniquely identifies a subset.
        assert_equal(self.nodes[0].scantxoutset("start", ["combo(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/0'/0h/0h)"])['total_amount'], Decimal("0.000008"))
        assert_equal(self.nodes[0].scantxoutset("start", ["combo(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/0'/0'/1h)"])['total_amount'], Decimal("0.000016"))
        assert_equal(self.nodes[0].scantxoutset("start", ["combo(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/0h/0'/1500')"])['total_amount'], Decimal("0.000032"))
        assert_equal(self.nodes[0].scantxoutset("start", ["combo(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/0h/0h/0)"])['total_amount'], Decimal("0.000064"))
        assert_equal(self.nodes[0].scantxoutset("start", ["combo(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/0'/0h/1)"])['total_amount'], Decimal("0.000128"))
        assert_equal(self.nodes[0].scantxoutset("start", ["combo(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/0h/0'/1500)"])['total_amount'], Decimal("0.000256"))
        assert_equal(self.nodes[0].scantxoutset("start", [{"desc": "combo(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/0'/0h/*h)", "range": 1499}])['total_amount'], Decimal("0.000024"))
        assert_equal(self.nodes[0].scantxoutset("start", [{"desc": "combo(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/0'/0'/*h)", "range": 1500}])['total_amount'], Decimal("0.000056"))
        assert_equal(self.nodes[0].scantxoutset("start", [{"desc": "combo(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/0h/0'/*)", "range": 1499}])['total_amount'], Decimal("0.000192"))
        assert_equal(self.nodes[0].scantxoutset("start", [{"desc": "combo(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/0'/0h/*)", "range": 1500}])['total_amount'], Decimal("0.000448"))
        assert_equal(self.nodes[0].scantxoutset("start", ["combo(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/1/1/0')"])['total_amount'], Decimal("0.000512"))
        assert_equal(self.nodes[0].scantxoutset("start", ["combo(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/1/1/1')"])['total_amount'], Decimal("0.001024"))
        assert_equal(self.nodes[0].scantxoutset("start", ["combo(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/1/1/1500h)"])['total_amount'], Decimal("0.002048"))
        assert_equal(self.nodes[0].scantxoutset("start", ["combo(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/1/1/0)"])['total_amount'], Decimal("0.004096"))
        assert_equal(self.nodes[0].scantxoutset("start", ["combo(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/1/1/1)"])['total_amount'], Decimal("0.008192"))
        assert_equal(self.nodes[0].scantxoutset("start", ["combo(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/1/1/1500)"])['total_amount'], Decimal("0.016384"))
        assert_equal(self.nodes[0].scantxoutset("start", ["combo(squb6UShJgvLuWbXhZQZ1fML1Lg1yCsWwqoXY4vG1H3hFXrcUJ3kMeYf22JbXHi8iWoZQ8d1QbA5SBPxQw68h2mUNLdXNVQ4F5kgSzf8F3ox7o6/1/1/0)"])['total_amount'], Decimal("0.004096"))
        assert_equal(self.nodes[0].scantxoutset("start", ["combo([abcdef88/1/2'/3/4h]squb6UShJgvLuWbXhZQZ1fML1Lg1yCsWwqoXY4vG1H3hFXrcUJ3kMeYf22JbXHi8iWoZQ8d1QbA5SBPxQw68h2mUNLdXNVQ4F5kgSzf8F3ox7o6/1/1/1)"])['total_amount'], Decimal("0.008192"))
        assert_equal(self.nodes[0].scantxoutset("start", ["combo(squb6UShJgvLuWbXhZQZ1fML1Lg1yCsWwqoXY4vG1H3hFXrcUJ3kMeYf22JbXHi8iWoZQ8d1QbA5SBPxQw68h2mUNLdXNVQ4F5kgSzf8F3ox7o6/1/1/1500)"])['total_amount'], Decimal("0.016384"))
        assert_equal(self.nodes[0].scantxoutset("start", [{"desc": "combo(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/1/1/*')", "range": 1499}])['total_amount'], Decimal("0.001536"))
        assert_equal(self.nodes[0].scantxoutset("start", [{"desc": "combo(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/1/1/*')", "range": 1500}])['total_amount'], Decimal("0.003584"))
        assert_equal(self.nodes[0].scantxoutset("start", [{"desc": "combo(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/1/1/*)", "range": 1499}])['total_amount'], Decimal("0.012288"))
        assert_equal(self.nodes[0].scantxoutset("start", [{"desc": "combo(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/1/1/*)", "range": 1500}])['total_amount'], Decimal("0.028672"))
        assert_equal(self.nodes[0].scantxoutset("start", [{"desc": "combo(squb6UShJgvLuWbXhZQZ1fML1Lg1yCsWwqoXY4vG1H3hFXrcUJ3kMeYf22JbXHi8iWoZQ8d1QbA5SBPxQw68h2mUNLdXNVQ4F5kgSzf8F3ox7o6/1/1/*)", "range": 1499}])['total_amount'], Decimal("0.012288"))
        assert_equal(self.nodes[0].scantxoutset("start", [{"desc": "combo(squb6UShJgvLuWbXhZQZ1fML1Lg1yCsWwqoXY4vG1H3hFXrcUJ3kMeYf22JbXHi8iWoZQ8d1QbA5SBPxQw68h2mUNLdXNVQ4F5kgSzf8F3ox7o6/1/1/*)", "range": 1500}])['total_amount'], Decimal("0.028672"))
        assert_equal(self.nodes[0].scantxoutset("start", [{"desc": "combo(squb6UShJgvLuWbXhZQZ1fML1Lg1yCsWwqoXY4vG1H3hFXrcUJ3kMeYf22JbXHi8iWoZQ8d1QbA5SBPxQw68h2mUNLdXNVQ4F5kgSzf8F3ox7o6/1/1/*)", "range": [1500, 1500]}])['total_amount'], Decimal("0.016384"))
        assert_equal(self.nodes[0].scantxoutset("start", [ {"desc": "pkh(squb6UShJgvLuWbXhZQZ1fML1Lg1yCsWwqoXY4vG1H3hFXrcUJ3kMeYf22JbXHi8iWoZQ8d1QbA5SBPxQw68h2mUNLdXNVQ4F5kgSzf8F3ox7o6/1/1/<0;1>)"}])["total_amount"], Decimal("0.012288"))

        # Test the reported descriptors for a few matches
        assert_equal(descriptors(self.nodes[0].scantxoutset("start", [{"desc": "combo(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/0h/0h/*)", "range": 1499}])), ["pkh([0c5f9a1e/0h/0h/0]026dbd8b2315f296d36e6b6920b1579ca75569464875c7ebe869b536a7d9503c8c)#rthll0rg", "pkh([0c5f9a1e/0h/0h/1]033e6f25d76c00bedb3a8993c7d5739ee806397f0529b1b31dda31ef890f19a60c)#mcjajulr"])
        assert_equal(descriptors(self.nodes[0].scantxoutset("start", ["combo(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/1/1/0)"])), ["pkh([0c5f9a1e/1/1/0]03e1c5b6e650966971d7e71ef2674f80222752740fc1dfd63bbbd220d2da9bd0fb)#cxmct4w8"])
        assert_equal(descriptors(self.nodes[0].scantxoutset("start", [{"desc": "combo(squb6UShJgvLuWbXhZQZ1fML1Lg1yCsWwqoXY4vG1H3hFXrcUJ3kMeYf22JbXHi8iWoZQ8d1QbA5SBPxQw68h2mUNLdXNVQ4F5kgSzf8F3ox7o6/1/1/*)", "range": 1500}])), ['pkh([0c5f9a1e/1/1/0]03e1c5b6e650966971d7e71ef2674f80222752740fc1dfd63bbbd220d2da9bd0fb)#cxmct4w8', 'pkh([0c5f9a1e/1/1/1500]03832901c250025da2aebae2bfb38d5c703a57ab66ad477f9c578bfbcd78abca6f)#vchwd07g', 'pkh([0c5f9a1e/1/1/1]030d820fc9e8211c4169be8530efbc632775d8286167afd178caaf1089b77daba7)#z2t3ypsa'])

        # Check that status and abort don't need second arg
        assert_equal(self.nodes[0].scantxoutset("status"), None)
        assert_equal(self.nodes[0].scantxoutset("abort"), False)

        # Check that the blockhash and confirmations fields are correct
        self.generate(self.nodes[0], 2)
        unspent = self.nodes[0].scantxoutset("start", ["addr(SWBBbbSLd9mPv5cFExFoBZR1Wx9U5oKP7D)"])["unspents"][0]
        blockhash = self.nodes[0].getblockhash(info["height"])
        assert_equal(unspent["height"], info["height"])
        assert_equal(unspent["blockhash"], blockhash)
        assert_equal(unspent["confirmations"], 3)

        # Check that first arg is needed
        assert_raises_rpc_error(-1, "scantxoutset \"action\" ( [scanobjects,...] )", self.nodes[0].scantxoutset)

        # Check that second arg is needed for start
        assert_raises_rpc_error(-1, "scanobjects argument is required for the start action", self.nodes[0].scantxoutset, "start")

        # Check that invalid command give error
        assert_raises_rpc_error(-8, "Invalid action 'invalid_command'", self.nodes[0].scantxoutset, "invalid_command")


if __name__ == "__main__":
    ScantxoutsetTest(__file__).main()
