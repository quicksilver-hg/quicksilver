#!/usr/bin/env python3
# Copyright (c) 2023 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test sigop limit relaypool policy (`-bytespersigop` parameter)"""
from decimal import Decimal
from math import ceil

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.messages import (
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxInWitness,
    CTxOut,
    WITNESS_SCALE_FACTOR,
    tx_from_hex,
)
from test_framework.script import (
    CScript,
    OP_CHECKMULTISIG,
    OP_CHECKSIG,
    OP_ENDIF,
    OP_FALSE,
    OP_IF,
    OP_RETURN,
    OP_TRUE,
)
from test_framework.script_util import (
    keys_to_multisig_script,
    script_to_p2wsh_script,
)
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.txpow import prove_tx_pow
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_greater_than_or_equal,
)
from test_framework.vault import MiniVault
from test_framework.vault_util import generate_keypair

DEFAULT_BYTES_PER_SIGOP = 20  # default setting
MAX_PUBKEYS_PER_MULTISIG = 20

class BytesPerSigOpTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        # allow large datacarrier output to pad transactions
        self.extra_args = [['-datacarriersize=100000', '-txpownocycle=1']]

    def create_p2wsh_spending_tx(self, witness_script, output_script):
        """Create a 1-input-1-output P2WSH spending transaction with only the
           witness script in the witness stack and the given output script."""
        # create P2WSH address and fund it via MiniVault first
        fund = self.vault.send_to(
            from_node=self.nodes[0],
            scriptPubKey=script_to_p2wsh_script(witness_script),
            amount=1000000,
        )

        # create spending transaction
        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(int(fund["txid"], 16), fund["sent_vout"]))]
        tx.wit.vtxinwit = [CTxInWitness()]
        tx.wit.vtxinwit[0].scriptWitness.stack = [bytes(witness_script)]
        tx.vout = [
            CTxOut(0, output_script),
            CTxOut(1000000, self.vault.get_output_script()),
        ]
        prove_tx_pow(tx, anchor_height=self.nodes[0].getblockcount(), target_int=(1 << 240) - 1)
        return tx

    def test_sigops_limit(self, bytes_per_sigop, num_sigops):
        sigop_equivalent_vsize = ceil(num_sigops * bytes_per_sigop / WITNESS_SCALE_FACTOR)
        self.log.info(f"- {num_sigops} sigops (equivalent size of {sigop_equivalent_vsize} vbytes)")

        # create a template tx with the specified sigop cost in the witness script
        # (note that the sigops count even though being in a branch that's not executed)
        num_multisigops = num_sigops // 20
        num_singlesigops = num_sigops % 20
        witness_script = CScript(
            [OP_FALSE, OP_IF] +
            [OP_CHECKMULTISIG]*num_multisigops +
            [OP_CHECKSIG]*num_singlesigops +
            [OP_ENDIF, OP_TRUE]
        )
        # use a 256-byte data-push as lower bound in the output script, in order
        # to avoid having to compensate for tx size changes caused by varying
        # length serialization sizes (both for scriptPubKey and data-push lengths)
        tx = self.create_p2wsh_spending_tx(witness_script, CScript([OP_RETURN, b'X'*256]))

        # bump the tx to reach the sigop-limit equivalent size by padding the datacarrier output
        assert_greater_than_or_equal(sigop_equivalent_vsize, tx.get_vsize())
        vsize_to_pad = sigop_equivalent_vsize - tx.get_vsize()
        tx.vout[0].scriptPubKey = CScript([OP_RETURN, b'X'*(256+vsize_to_pad)])
        assert_equal(sigop_equivalent_vsize, tx.get_vsize())

        res = self.nodes[0].testrelaypoolaccept([tx.serialize().hex()])[0]
        assert res['allowed'], res
        assert_equal(res['vsize'], sigop_equivalent_vsize)

        # increase the tx's vsize to be right above the sigop-limit equivalent size
        # => tx's vsize in relaypool should also grow accordingly
        tx.vout[0].scriptPubKey = CScript([OP_RETURN, b'X'*(256+vsize_to_pad+1)])
        res = self.nodes[0].testrelaypoolaccept([tx.serialize().hex()])[0]
        assert res['allowed'], res
        assert_equal(res['vsize'], sigop_equivalent_vsize+1)

        # decrease the tx's vsize to be right below the sigop-limit equivalent size
        # => tx's vsize in relaypool should stick at the sigop-limit equivalent
        # bytes level, as it is higher than the tx's serialized vsize
        # (the maximum of both is taken)
        tx.vout[0].scriptPubKey = CScript([OP_RETURN, b'X'*(256+vsize_to_pad-1)])
        res = self.nodes[0].testrelaypoolaccept([tx.serialize().hex()])[0]
        assert res['allowed'], res
        assert_equal(res['vsize'], sigop_equivalent_vsize)

        # check that the ancestor and descendant size calculations in the relaypool
        # also use the same max(sigop_equivalent_vsize, serialized_vsize) logic
        # (to keep it simple, we only test the case here where the sigop vsize
        # is much larger than the serialized vsize, i.e. we create a small child
        # tx by getting rid of the large padding output)
        tx.vout[0].scriptPubKey = CScript([OP_RETURN, b'test123'])
        assert_greater_than(sigop_equivalent_vsize, tx.get_vsize())
        self.nodes[0].sendrawtransaction(hexstring=tx.serialize().hex(), maxburnamount='1.0')

        # fetch parent tx, which doesn't contain any sigops
        parent_txid = tx.vin[0].prevout.hash.to_bytes(32, 'big').hex()
        parent_tx = tx_from_hex(self.nodes[0].getrawtransaction(txid=parent_txid))

        entry_child = self.nodes[0].getrelaypoolentry(tx.rehash())
        assert_equal(entry_child['descendantcount'], 1)
        assert_equal(entry_child['descendantsize'], sigop_equivalent_vsize)

        entry_parent = self.nodes[0].getrelaypoolentry(parent_tx.rehash())
        assert_equal(entry_child['ancestorcount'], entry_parent['ancestorcount'] + 1)
        assert_equal(entry_child['ancestorsize'], entry_parent['ancestorsize'] + sigop_equivalent_vsize)
        assert_equal(entry_parent['descendantcount'], entry_child['descendantcount'] + 1)
        assert_equal(entry_parent['descendantsize'], parent_tx.get_vsize() + sigop_equivalent_vsize)

    def test_sigops_package(self):
        self.log.info("Test a overly-large sigops-vbyte hits package limits")
        # Make a 2-transaction package which fails vbyte checks even though
        # separately they would work.
        self.restart_node(0, extra_args=["-bytespersigop=5000","-permitbaremultisig=1"] + self.extra_args[0])

        def create_bare_multisig_tx(utxo_to_spend=None):
            _, pubkey = generate_keypair()
            amount_for_bare = 50000
            tx_dict = self.vault.create_self_transfer(utxo_to_spend=utxo_to_spend)
            tx_utxo = tx_dict["new_utxo"]
            tx = tx_dict["tx"]
            tx.vout.append(CTxOut(amount_for_bare, keys_to_multisig_script([pubkey], k=1)))
            tx.vout[0].nValue -= amount_for_bare
            tx_utxo["txid"] = tx.rehash()
            tx_utxo["value"] -= Decimal(amount_for_bare) / COIN
            return (tx_utxo, tx)

        tx_parent_utxo, tx_parent = create_bare_multisig_tx()
        _tx_child_utxo, tx_child = create_bare_multisig_tx(tx_parent_utxo)

        # Separately, the parent tx is ok
        parent_individual_testres = self.nodes[0].testrelaypoolaccept([tx_parent.serialize().hex()])[0]
        assert parent_individual_testres["allowed"]
        max_multisig_vsize = MAX_PUBKEYS_PER_MULTISIG * 5000
        assert_equal(parent_individual_testres["vsize"], max_multisig_vsize)

        # But together, it's exceeding limits in the *package* context. If sigops adjusted vsize wasn't being checked
        # here, it would get further in validation and give too-long-relaypool-chain error instead.
        packet_test = self.nodes[0].testrelaypoolaccept([tx_parent.serialize().hex(), tx_child.serialize().hex()])
        expected_package_error = f"package-relaypool-limits, package size {2*max_multisig_vsize} exceeds ancestor size limit [limit: 101000]"
        assert_equal([x["package-error"] for x in packet_test], [expected_package_error] * 2)

        # When we actually try to submit, the parent makes it into the relaypool, but the child would exceed ancestor vsize limits
        res = self.nodes[0].submitpackage([tx_parent.serialize().hex(), tx_child.serialize().hex()])
        assert "too-long-relaypool-chain" in res["tx-results"][tx_child.getwtxid()]["error"]
        assert tx_parent.rehash() in self.nodes[0].getrawrelaypool()

        # Transactions are tiny in weight
        assert_greater_than(3000, tx_parent.get_weight() + tx_child.get_weight())

    def run_test(self):
        self.vault = MiniVault(self.nodes[0])
        self.generate(self.vault, COINBASE_MATURITY + 100)

        for bytes_per_sigop in (DEFAULT_BYTES_PER_SIGOP, 43, 81, 165, 327, 649, 1072):
            if bytes_per_sigop == DEFAULT_BYTES_PER_SIGOP:
                self.log.info(f"Test default sigops limit setting ({bytes_per_sigop} bytes per sigop)...")
            else:
                bytespersigop_parameter = f"-bytespersigop={bytes_per_sigop}"
                self.log.info(f"Test sigops limit setting {bytespersigop_parameter}...")
                self.restart_node(0, extra_args=[bytespersigop_parameter] + self.extra_args[0])

            for num_sigops in (142, 183, 222):
                self.test_sigops_limit(bytes_per_sigop, num_sigops)
                self.generate(self.vault, 1)

        self.test_sigops_package()


if __name__ == '__main__':
    BytesPerSigOpTest(__file__).main()
