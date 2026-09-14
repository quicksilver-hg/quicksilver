#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Exercise the node-only message signing and verification RPCs."""

from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_raises_rpc_error
from test_framework.vault_util import get_generate_key


class SignMessageTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def run_test(self):
        node = self.nodes[0]
        signer = get_generate_key()
        other = get_generate_key()

        for message in ["", "Quicksilver message", "proof of work \u26cf"]:
            signature = node.signmessagewithprivkey(signer.privkey, message)
            assert node.verifymessage(signer.p2pkh_addr, signature, message)
            assert not node.verifymessage(other.p2pkh_addr, signature, message)
            assert not node.verifymessage(signer.p2pkh_addr, signature, message + " altered")

        assert_raises_rpc_error(-5, "Invalid private key", node.signmessagewithprivkey, "not-a-key", "message")
        assert_raises_rpc_error(-5, "Invalid address", node.verifymessage, "not-an-address", "AAAA", "message")
        assert_raises_rpc_error(-3, "Address does not refer to key", node.verifymessage, signer.p2wpkh_addr, "AAAA", "message")
        assert_raises_rpc_error(-3, "Malformed base64 encoding", node.verifymessage, signer.p2pkh_addr, "not-base64!", "message")


if __name__ == "__main__":
    SignMessageTest(__file__).main()
