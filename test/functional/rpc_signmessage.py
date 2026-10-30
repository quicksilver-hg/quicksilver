#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Exercise the node-only message signing and verification RPCs."""

from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_raises_rpc_error
from test_framework.vault_util import get_generate_key, get_multisig


class SignMessageTest(QuicksilverTestFramework):
    def add_options(self, parser):
        # Only the G1 7d case below needs a vault; the node-only cases run without one too.
        self.add_vault_options(parser)

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
        # G1 7c: a Bech32 or Base58 script-hash address is not a key-hash address; the
        # error names the letter key-hash addresses start with (sandbox: S).
        not_key_hash = "Message signatures can only be checked against Base58 key-hash addresses, which start with S."
        assert signer.p2pkh_addr.startswith("S")
        script_hash_addr = get_multisig(node).p2sh_addr
        assert not script_hash_addr.startswith("S")
        for address in [signer.p2wpkh_addr, script_hash_addr]:
            assert_raises_rpc_error(-3, not_key_hash, node.verifymessage, address, "AAAA", "message")
        assert_raises_rpc_error(-3, "Malformed base64 encoding", node.verifymessage, signer.p2pkh_addr, "not-base64!", "message")

        if self.options.enable_vault:
            # G1 7d: the vault's signmessage names the letter and how to make such an
            # address, and following that advice works.
            not_key_hash = 'Message signing works only with Base58 key-hash addresses, which start with S. Create one with getnewaddress "" base58.'
            node.createvault("signer")
            assert_raises_rpc_error(-3, not_key_hash, node.signmessage, node.getnewaddress(), "message")
            assert_raises_rpc_error(-3, not_key_hash, node.signmessage, script_hash_addr, "message")
            base58 = node.getnewaddress("", "base58")
            assert base58.startswith("S")
            assert node.verifymessage(base58, node.signmessage(base58, "message"), "message")


if __name__ == "__main__":
    SignMessageTest(__file__).main()
