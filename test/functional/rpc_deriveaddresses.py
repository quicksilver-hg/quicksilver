#!/usr/bin/env python3
# Copyright (c) 2018-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the deriveaddresses rpc call."""
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.descriptors import descsum_create
from test_framework.util import assert_equal, assert_raises_rpc_error

class DeriveaddressesTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def run_test(self):
        assert_raises_rpc_error(-5, "Missing checksum", self.nodes[0].deriveaddresses, "a")

        descriptor = "wpkh(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/1/1/0)#tyseyd4g"
        address = "shg1qjqmxmkpmxt80xz4y3746zgt0q3u3ferrhnfz8n"
        assert_equal(self.nodes[0].deriveaddresses(descriptor), [address])

        descriptor = descriptor[:-9]
        assert_raises_rpc_error(-5, "Missing checksum", self.nodes[0].deriveaddresses, descriptor)

        descriptor_pubkey = "wpkh(squb6UShJgvLuWbXhZQZ1fML1Lg1yCsWwqoXY4vG1H3hFXrcUJ3kMeYf22JbXHi8iWoZQ8d1QbA5SBPxQw68h2mUNLdXNVQ4F5kgSzf8F3ox7o6/1/1/0)#snxz06hu"
        address = "shg1qjqmxmkpmxt80xz4y3746zgt0q3u3ferrhnfz8n"
        assert_equal(self.nodes[0].deriveaddresses(descriptor_pubkey), [address])

        ranged_descriptor = "wpkh(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/1/1/*)#kh42ewne"
        assert_equal(self.nodes[0].deriveaddresses(ranged_descriptor, [1, 2]), ["shg1qhku5rq7jz8ulufe2y6fkcpnlvpsta7rqnnps8r", "shg1qpgptk2gvshyl0s9lqshsmx932l9ccsv2ujlkh8"])
        assert_equal(self.nodes[0].deriveaddresses(ranged_descriptor, 2), [address, "shg1qhku5rq7jz8ulufe2y6fkcpnlvpsta7rqnnps8r", "shg1qpgptk2gvshyl0s9lqshsmx932l9ccsv2ujlkh8"])

        ranged_descriptor = descsum_create("wpkh(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/1/<0;1>/*)")
        assert_equal(self.nodes[0].deriveaddresses(ranged_descriptor, [1, 2]), [["shg1q7c8mdmdktrzs8xgpjmqw90tjn65j5a3y5fppm5", "shg1qs6n37uzu0v0qfzf0r0csm0dwa7prc0v56mcjwg"], ["shg1qhku5rq7jz8ulufe2y6fkcpnlvpsta7rqnnps8r", "shg1qpgptk2gvshyl0s9lqshsmx932l9ccsv2ujlkh8"]])

        assert_raises_rpc_error(-8, "Range should not be specified for an un-ranged descriptor", self.nodes[0].deriveaddresses, descsum_create("wpkh(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/1/1/0)"), [0, 2])

        assert_raises_rpc_error(-8, "Range must be specified for a ranged descriptor", self.nodes[0].deriveaddresses, descsum_create("wpkh(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/1/1/*)"))

        assert_raises_rpc_error(-8, "End of range is too high", self.nodes[0].deriveaddresses, descsum_create("wpkh(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/1/1/*)"), 10000000000)

        assert_raises_rpc_error(-8, "Range is too large", self.nodes[0].deriveaddresses, descsum_create("wpkh(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/1/1/*)"), [1000000000, 2000000000])

        assert_raises_rpc_error(-8, "Range specified as [begin,end] must not have begin after end", self.nodes[0].deriveaddresses, descsum_create("wpkh(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/1/1/*)"), [2, 0])

        assert_raises_rpc_error(-8, "Range should be greater or equal than 0", self.nodes[0].deriveaddresses, descsum_create("wpkh(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/1/1/*)"), [-1, 0])

        combo_descriptor = descsum_create("combo(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/1/1/0)")
        assert_equal(self.nodes[0].deriveaddresses(combo_descriptor), ["SaSXYGH84fpqjiUGeo2WBFtkmbSQgzgsxB", address])

        # P2PK does not have a valid address
        assert_raises_rpc_error(-5, "Descriptor does not have a corresponding address", self.nodes[0].deriveaddresses, descsum_create("pk(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7)"))

        # Before #26275, quicksilverd would crash when deriveaddresses was
        # called with derivation index 2147483647, which is the maximum
        # positive value of a signed int32, and - currently - the
        # maximum value that the deriveaddresses RPC call
        # accepts as derivation index.
        assert_equal(self.nodes[0].deriveaddresses(descsum_create("wpkh(sqrv1wkfAGt6m8urp6Nm81gjbw1uQBManWccxmKUim1PqG4DdonyjFj4qXgjMAzPSjgBb6U6x947ZNrPtfBiKZ8NTfCaLUSX3GJCMqDjHqz7mN7/1/1/*)"), [2147483647, 2147483647]), ["shg1qtzs23vgzpreks5gtygwxf8tv5rldxvvsz04hu0"])

        hardened_without_privkey_descriptor = descsum_create("wpkh(squb6UShJgvLuWbXhZQZ1fML1Lg1yCsWwqoXY4vG1H3hFXrcUJ3kMeYf22JbXHi8iWoZQ8d1QbA5SBPxQw68h2mUNLdXNVQ4F5kgSzf8F3ox7o6/1'/1/0)")
        assert_raises_rpc_error(-5, "Cannot derive script without private keys", self.nodes[0].deriveaddresses, hardened_without_privkey_descriptor)

        bare_multisig_descriptor = descsum_create("multi(1,squb6UShJgvLuWbXhZQZ1fML1Lg1yCsWwqoXY4vG1H3hFXrcUJ3kMeYf22JbXHi8iWoZQ8d1QbA5SBPxQw68h2mUNLdXNVQ4F5kgSzf8F3ox7o6/1/1/0,squb6UShJgvLuWbXhZQZ1fML1Lg1yCsWwqoXY4vG1H3hFXrcUJ3kMeYf22JbXHi8iWoZQ8d1QbA5SBPxQw68h2mUNLdXNVQ4F5kgSzf8F3ox7o6/1/1/1)")
        assert_raises_rpc_error(-5, "Descriptor does not have a corresponding address", self.nodes[0].deriveaddresses, bare_multisig_descriptor)

if __name__ == '__main__':
    DeriveaddressesTest(__file__).main()
