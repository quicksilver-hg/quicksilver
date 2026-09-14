#!/usr/bin/env python3
# Copyright (c) 2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test Miniscript descriptors integration in the vault."""

from test_framework.descriptors import descsum_create
from test_framework.psqt import PSQT, PSQT_IN_SHA256
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.txpow import prove_raw_tx_pow
from test_framework.util import assert_equal


TPRVS = [
    "sqrv1wkfAGt6m8urqqJq8dCVPC8JDDj6Ng5WFojvGpJ4Z2zfn94G4baubULVxa2hKWThKFxxj5Rn6k4QjGGVvCDFwRXofy48rSkhwFnr6NC5zkr",
    "sqrv1wkfAGt6m8urp2WhtBtdffPWvnH1gg3s95VLsYbcfUG63yKvwUsQChiUrJwwFNhQhQ7i7nHjhmwDMWgrs5xARiuQJ8kA6pkndDic6rRrUxK",
    "sqrv26JpK8GiebXi5CkFG7pNr8UVsnADdJwwAoq8Wy7LhSvCw7XskoaLce1WgKWdtP9qujk62yhr1bWASE979mfgbHynka1Zx7txqshsdCh8Zn1",
]
TPUBS = [
    "squb6UShJgvLuWbXjN4i1XGKTMv3RDN25BjxZY1ouDV71vxEoNwYoNgJmwXMD4TWmPB2aDcHVpvRPZX7f3DF2XqPA5jnjdH4dfS2mxrCdwLbhDf",
    "squb6UShJgvLuWbXjLJJ2wTqhoh4Vd3SEtch8PzEKQUaDxD399coiq8uP6eii1Pbhvohz5iz7z11Uhffn6tmpB4QSFHCxiCKnLUygoPkJqdBnrZ",
    "squb6UShJgvLuWbXjT42Nt2UdsmQjXYkGdEvGsQZqbkfkDspuKiHGFH6DevuFKtE5SDknoW5vjZ8n3ZFz3vcYa3X9aA8BNVWHEAzxnSqnQxLc2x",
    "squb6UShJgvLuWbXiQFiP7WkrL4hWYoMBDEhiLFgnPeof7pZofa1h8jJbGDT7eqd9XjFExgxKXaSmBQaVBxGYYdrPh13hDiGqjTwoCEbJcVfARU",
    "squb6UShJgvLuWbXhrjY25w9vX6e94xpzMLT4xZaLLzaPieqZwHKtfU53wUUENb7UwrcJqAjZ9JGnmvGbqtyJ5S4FiwVon3ErPphMVks7HKDbhH",
    "squb6cK3TGuopaB3uXf5UrJ77Zbb8ETDdzhgFu1HACapzyKZb1uL7qKyNxYruxbfaW7VKXEmkaByytqxLGcWBXr1P5GpRpubcACLUCdAwS8MXtB",
    "squb6UShJgvLuWbXhQ3G1Yz7uKTYnkniYk5Jpv2X68dVDX9HK2UZqJ9Ce3bjoVbTBoH6cBcChwnxyQMfM5Ra9BLSEd15EGPxvCoSH5YtTQxvr8Q",
]
PUBKEYS = [
    "02aebf2d10b040eb936a6f02f44ee82f8b34f5c1ccb20ff3949c2b28206b7c1068",
    "030f64b922aee2fd597f104bc6cb3b670f1ca2c6c49b1071a1a6c010575d94fe5a",
    "02abe475b199ec3d62fa576faee16a334fdb86ffb26dce75becebaaedf328ac3fe",
    "0314f3dc33595b0d016bb522f6fe3a67680723d842c1b9b8ae6b59fdd8ab5cccb4",
    "025eba3305bd3c829e4e1551aac7358e4178832c739e4fc4729effe428de0398ab",
    "029ffbe722b147f3035c87cb1c60b9a5947dd49c774cc31e94773478711a929ac0",
    "0211c7b2e18b6fd330f322de087da62da92ae2ae3d0b7cec7e616479cce175f183",
]

P2WSH_MINISCRIPTS = [
    # One of two keys
    f"or_b(pk({TPUBS[0]}/*),s:pk({TPUBS[1]}/*))",
    # A script similar (same spending policy) to BOLT3's offered HTLC (with anchor outputs)
    f"or_d(pk({TPUBS[0]}/*),and_v(and_v(v:pk({TPUBS[1]}/*),or_c(pk({TPUBS[2]}/*),v:hash160(7f999c905d5e35cefd0a37673f746eb13fba3640))),older(1)))",
    # A Revault Unvault policy with the older() replaced by an after()
    f"andor(multi(2,{TPUBS[0]}/*,{TPUBS[1]}/*),and_v(v:multi(4,{PUBKEYS[0]},{PUBKEYS[1]},{PUBKEYS[2]},{PUBKEYS[3]}),after(424242)),thresh(4,pkh({TPUBS[2]}/*),a:pkh({TPUBS[3]}/*),a:pkh({TPUBS[4]}/*),a:pkh({TPUBS[5]}/*)))",
    # Liquid-like federated pegin with emergency recovery keys
    f"or_i(and_b(pk({PUBKEYS[0]}),a:and_b(pk({PUBKEYS[1]}),a:and_b(pk({PUBKEYS[2]}),a:and_b(pk({PUBKEYS[3]}),s:pk({PUBKEYS[4]}))))),and_v(v:thresh(2,pkh({TPUBS[0]}/*),a:pkh({PUBKEYS[5]}),a:pkh({PUBKEYS[6]})),older(4209713)))",
]

DESCS = [
    *[f"wsh({ms})" for ms in P2WSH_MINISCRIPTS],
    # A Taproot with one of the above scripts as the single script path.
    f"tr(4d54bb9928a0683b7e383de72943b214b0716f58aa54c7ba6bcea2328bc9c768,{P2WSH_MINISCRIPTS[0]})",
    # A Taproot with two script paths among the above scripts.
    f"tr(4d54bb9928a0683b7e383de72943b214b0716f58aa54c7ba6bcea2328bc9c768,{{{P2WSH_MINISCRIPTS[0]},{P2WSH_MINISCRIPTS[1]}}})",
    # A Taproot with three script paths among the above scripts.
    f"tr(4d54bb9928a0683b7e383de72943b214b0716f58aa54c7ba6bcea2328bc9c768,{{{{{P2WSH_MINISCRIPTS[0]},{P2WSH_MINISCRIPTS[1]}}},{P2WSH_MINISCRIPTS[2].replace('multi', 'multi_a')}}})",
    # A Taproot with all above scripts in its tree.
    f"tr(4d54bb9928a0683b7e383de72943b214b0716f58aa54c7ba6bcea2328bc9c768,{{{{{P2WSH_MINISCRIPTS[0]},{P2WSH_MINISCRIPTS[1]}}},{{{P2WSH_MINISCRIPTS[2].replace('multi', 'multi_a')},{P2WSH_MINISCRIPTS[3]}}}}})",
]

DESCS_PRIV = [
    # One of two keys, of which one private key is known
    {
        "desc": f"wsh(or_i(pk({TPRVS[0]}/*),pk({TPUBS[0]}/*)))",
        "sequence": None,
        "locktime": None,
        "sigs_count": 1,
        "stack_size": 3,
    },
    # A more complex policy, that can't be satisfied through the first branch (need for a preimage)
    {
        "desc": f"wsh(andor(ndv:older(2),and_v(v:pk({TPRVS[0]}),sha256(2a8ce30189b2ec3200b47aeb4feaac8fcad7c0ba170389729f4898b0b7933bcb)),and_v(v:pkh({TPRVS[1]}),pk({TPRVS[2]}/*))))",
        "sequence": 2,
        "locktime": None,
        "sigs_count": 3,
        "stack_size": 5,
    },
    # The same policy but we provide the preimage. This path will be chosen as it's a smaller witness.
    {
        "desc": f"wsh(andor(ndv:older(2),and_v(v:pk({TPRVS[0]}),sha256(61e33e9dbfefc45f6a194187684d278f789fd4d5e207a357e79971b6519a8b12)),and_v(v:pkh({TPRVS[1]}),pk({TPRVS[2]}/*))))",
        "sequence": 2,
        "locktime": None,
        "sigs_count": 3,
        "stack_size": 4,
        "sha256_preimages": {
            "61e33e9dbfefc45f6a194187684d278f789fd4d5e207a357e79971b6519a8b12": "e8774f330f5f330c23e8bbefc5595cb87009ddb7ac3b8deaaa8e9e41702d919c"
        },
    },
    # Signature with a relative timelock
    {
        "desc": f"wsh(and_v(v:older(2),pk({TPRVS[0]}/*)))",
        "sequence": 2,
        "locktime": None,
        "sigs_count": 1,
        "stack_size": 2,
    },
    # Signature with an absolute timelock
    {
        "desc": f"wsh(and_v(v:after(20),pk({TPRVS[0]}/*)))",
        "sequence": None,
        "locktime": 20,
        "sigs_count": 1,
        "stack_size": 2,
    },
    # Signature with both
    {
        "desc": f"wsh(and_v(v:older(4),and_v(v:after(30),pk({TPRVS[0]}/*))))",
        "sequence": 4,
        "locktime": 30,
        "sigs_count": 1,
        "stack_size": 2,
    },
    # We have one key on each branch; Core signs both (can't finalize)
    {
        "desc": f"wsh(c:andor(pk({TPRVS[0]}/*),pk_k({TPUBS[0]}),and_v(v:pk({TPRVS[1]}),pk_k({TPUBS[1]}))))",
        "sequence": None,
        "locktime": None,
        "sigs_count": 2,
        "stack_size": None,
    },
    # We have all the keys, vault selects the timeout path to sign since it's smaller and sequence is set
    {
        "desc": f"wsh(andor(pk({TPRVS[0]}/*),pk({TPRVS[2]}),and_v(v:pk({TPRVS[1]}),older(10))))",
        "sequence": 10,
        "locktime": None,
        "sigs_count": 3,
        "stack_size": 3,
    },
    # We have all the keys, vault selects the primary path to sign unconditionally since nsequence wasn't set to be valid for timeout path
    {
        "desc": f"wsh(andor(pk({TPRVS[0]}/*),pk({TPRVS[2]}),and_v(v:pkh({TPRVS[1]}),older(10))))",
        "sequence": None,
        "locktime": None,
        "sigs_count": 3,
        "stack_size": 3,
    },
    # Finalizes to the smallest valid witness, regardless of sequence
    {
        "desc": f"wsh(or_d(pk({TPRVS[0]}/*),and_v(v:pk({TPRVS[1]}),and_v(v:pk({TPRVS[2]}),older(10)))))",
        "sequence": 12,
        "locktime": None,
        "sigs_count": 3,
        "stack_size": 2,
    },
    # Liquid-like federated pegin with emergency recovery privkeys
    {
        "desc": f"wsh(or_i(and_b(pk({TPUBS[0]}/*),a:and_b(pk({TPUBS[1]}),a:and_b(pk({TPUBS[2]}),a:and_b(pk({TPUBS[3]}),s:pk({PUBKEYS[0]}))))),and_v(v:thresh(2,pkh({TPRVS[0]}),a:pkh({TPRVS[1]}),a:pkh({TPUBS[4]})),older(42))))",
        "sequence": 42,
        "locktime": None,
        "sigs_count": 2,
        "stack_size": 8,
    },
    # Each leaf needs two sigs. We've got one key on each. Will sign both but can't finalize.
    {
        "desc": f"tr({TPUBS[0]}/*,{{and_v(v:pk({TPRVS[0]}/*),pk({TPUBS[1]})),and_v(v:pk({TPRVS[1]}/*),pk({TPUBS[2]}))}})",
        "sequence": None,
        "locktime": None,
        "sigs_count": 2,
        "stack_size": None,
    },
    # The same but now the two leaves are identical. Will add a single sig that is valid for both. Can't finalize.
    {
        "desc": f"tr({TPUBS[0]}/*,{{and_v(v:pk({TPRVS[0]}/*),pk({TPUBS[1]})),and_v(v:pk({TPRVS[0]}/*),pk({TPUBS[1]}))}})",
        "sequence": None,
        "locktime": None,
        "sigs_count": 1,
        "stack_size": None,
    },
    # The same but we have the two necessary privkeys on one of the leaves. Also it uses a pubkey hash.
    {
        "desc": f"tr({TPUBS[0]}/*,{{and_v(v:pk({TPRVS[0]}/*),pk({TPUBS[1]})),and_v(v:pkh({TPRVS[1]}/*),pk({TPRVS[2]}))}})",
        "sequence": None,
        "locktime": None,
        "sigs_count": 3,
        "stack_size": 5,
    },
    # A key immediately or one of two keys after a timelock. If both paths are available it'll use the
    # non-timelocked path because it's a smaller witness.
    {
        "desc": f"tr({TPUBS[0]}/*,{{pk({TPRVS[0]}/*),and_v(v:older(42),multi_a(1,{TPRVS[1]},{TPRVS[2]}))}})",
        "sequence": 42,
        "locktime": None,
        "sigs_count": 3,
        "stack_size": 3,
    },
    # A key immediately or one of two keys after a timelock. If the "primary" key isn't available though it'll
    # use the timelocked path. Same remark for multi_a.
    {
        "desc": f"tr({TPUBS[0]}/*,{{pk({TPUBS[1]}/*),and_v(v:older(42),multi_a(1,{TPRVS[0]},{TPRVS[1]}))}})",
        "sequence": 42,
        "locktime": None,
        "sigs_count": 2,
        "stack_size": 4,
    },
    # Liquid-like federated pegin with emergency recovery privkeys, but in a Taproot.
    {
        "desc": f"tr({TPUBS[1]}/*,{{and_b(pk({TPUBS[2]}/*),a:and_b(pk({TPUBS[3]}),a:and_b(pk({TPUBS[4]}),a:and_b(pk({TPUBS[5]}),s:pk({PUBKEYS[0]}))))),and_v(v:thresh(2,pkh({TPRVS[0]}),a:pkh({TPRVS[1]}),a:pkh({TPUBS[6]})),older(42))}})",
        "sequence": 42,
        "locktime": None,
        "sigs_count": 2,
        "stack_size": 8,
    },
]


class VaultMiniscriptTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-txpownocycle=1"]]
        self.rpc_timeout = 180

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()
        self.skip_if_no_sqlite()

    def tracking_vault_test(self, desc):
        self.log.info(f"Importing descriptor '{desc}'")
        desc = descsum_create(f"{desc}")
        assert self.ms_wo_vault.importdescriptors(
            [
                {
                    "desc": desc,
                    "active": True,
                    "range": 2,
                    "next_index": 0,
                    "timestamp": "now",
                }
            ]
        )[0]["success"]

        self.log.info("Testing we derive new addresses for it")
        addr_type = "bech32m" if desc.startswith("tr(") else "bech32"
        assert_equal(
            self.ms_wo_vault.getnewaddress(address_type=addr_type),
            self.funder.deriveaddresses(desc, 0)[0],
        )
        assert_equal(
            self.ms_wo_vault.getnewaddress(address_type=addr_type),
            self.funder.deriveaddresses(desc, 1)[1],
        )

        self.log.info("Testing we detect funds sent to one of them")
        addr = self.ms_wo_vault.getnewaddress()
        txid = self.funder.sendtoaddress(addr, 0.01)
        self.wait_until(
            lambda: len(self.ms_wo_vault.listunspent(minconf=0, addresses=[addr])) == 1
        )
        utxo = self.ms_wo_vault.listunspent(minconf=0, addresses=[addr])[0]
        assert utxo["txid"] == txid and utxo["solvable"]

    def signing_test(
        self, desc, sequence, locktime, sigs_count, stack_size, sha256_preimages
    ):
        self.log.info(f"Importing private Miniscript descriptor '{desc}'")
        is_taproot = desc.startswith("tr(")
        desc = descsum_create(desc)
        res = self.ms_sig_vault.importdescriptors(
            [
                {
                    "desc": desc,
                    "active": True,
                    "range": 0,
                    "next_index": 0,
                    "timestamp": "now",
                }
            ]
        )
        assert res[0]["success"], res

        self.log.info("Generating an address for it and testing it detects funds")
        addr_type = "bech32m" if is_taproot else "bech32"
        addr = self.ms_sig_vault.getnewaddress(address_type=addr_type)
        txid = self.funder.sendtoaddress(addr, 0.01)
        self.wait_until(lambda: txid in self.funder.getrawrelaypool())
        self.funder.generatetoaddress(1, self.funder.getnewaddress())
        utxo = self.ms_sig_vault.listunspent(addresses=[addr])[0]
        assert txid == utxo["txid"] and utxo["solvable"]

        self.log.info("Creating a transaction spending these funds")
        dest_addr = self.funder.getnewaddress()
        seq = sequence if sequence is not None else 0xFFFFFFFF - 2
        lt = locktime if locktime is not None else 0
        psqt = self.ms_sig_vault.createpsqt(
            [
                {
                    "txid": txid,
                    "vout": utxo["vout"],
                    "sequence": seq,
                }
            ],
            [{dest_addr: 0.01}],
            lt,
        )

        self.log.info("Signing it and checking the satisfaction.")
        if sha256_preimages is not None:
            psqt = PSQT.from_base64(psqt)
            for (h, preimage) in sha256_preimages.items():
                k = PSQT_IN_SHA256.to_bytes(1, "big") + bytes.fromhex(h)
                psqt.i[0].map[k] = bytes.fromhex(preimage)
            psqt = psqt.to_base64()
        res = self.ms_sig_vault.vaultprocesspsqt(psqt=psqt, finalize=False)
        psqtin = self.nodes[0].rpc.decodepsqt(res["psqt"])["inputs"][0]
        sigs_field_name = "taproot_script_path_sigs" if is_taproot else "partial_signatures"
        assert len(psqtin[sigs_field_name]) == sigs_count
        res = self.ms_sig_vault.finalizepsqt(res["psqt"])
        assert res["complete"] == (stack_size is not None)

        if stack_size is not None:
            txin = self.nodes[0].rpc.decoderawtransaction(res["hex"])["vin"][0]
            assert len(txin["txinwitness"]) == stack_size, txin["txinwitness"]
            self.log.info("Broadcasting the transaction.")
            # If necessary, satisfy a relative timelock
            if sequence is not None:
                self.funder.generatetoaddress(sequence, self.funder.getnewaddress())
            # If necessary, satisfy an absolute timelock
            height = self.funder.getblockcount()
            if locktime is not None and height < locktime:
                self.funder.generatetoaddress(
                    locktime - height, self.funder.getnewaddress()
                )
            self.ms_sig_vault.sendrawtransaction(prove_raw_tx_pow(res["hex"], self.nodes[0]))

    def run_test(self):
        self.log.info("Making a vault")
        self.funder = self.nodes[0].get_vault_rpc(self.default_vault_name)
        self.nodes[0].createvault(
            vault_name="ms_wo", disable_private_keys=True
        )
        self.ms_wo_vault = self.nodes[0].get_vault_rpc("ms_wo")
        self.nodes[0].createvault(vault_name="ms_sig")
        self.ms_sig_vault = self.nodes[0].get_vault_rpc("ms_sig")

        # Sanity check we wouldn't let an insane Miniscript descriptor in
        res = self.ms_wo_vault.importdescriptors(
            [
                {
                    "desc": descsum_create(
                        "wsh(and_b(ripemd160(1fd9b55a054a2b3f658d97e6b84cf3ee00be429a),a:1))"
                    ),
                    "active": False,
                    "timestamp": "now",
                }
            ]
        )[0]
        assert not res["success"]
        assert "is not sane: witnesses without signature exist" in res["error"]["message"]

        # Sanity check we wouldn't let an unspendable Miniscript descriptor in
        res = self.ms_wo_vault.importdescriptors(
            [
                {
                    "desc": descsum_create("wsh(0)"),
                    "active": False,
                    "timestamp": "now",
                }
            ]
        )[0]
        assert not res["success"] and "is not satisfiable" in res["error"]["message"]

        # Test we can track any type of Miniscript
        for desc in DESCS:
            self.tracking_vault_test(desc)

        # Test we can sign for any Miniscript.
        for desc in DESCS_PRIV:
            self.signing_test(
                desc["desc"],
                desc["sequence"],
                desc["locktime"],
                desc["sigs_count"],
                desc["stack_size"],
                desc.get("sha256_preimages"),
            )

        # Test we can sign for a max-size TapMiniscript. Recompute the maximum accepted size
        # for a TapMiniscript (see cpp file for details). Then pad a simple pubkey check up
        # to the maximum size. Make sure we can import and spend this script.
        leeway_weight = (4 + 4 + 1 + 36 + 4 + 1 + 1 + 8 + 1 + 1 + 33) * 4 + 2
        max_tapmini_size = 400_000 - 3 - (1 + 65) * 1_000 - 3 - (33 + 32 * 128) - leeway_weight - 5
        padding = max_tapmini_size - 33 - 1
        ms = f"pk({TPRVS[0]}/*)"
        ms = "n" * padding + ":" + ms
        desc = f"tr({PUBKEYS[0]},{ms})"
        self.signing_test(desc, None, None, 1, 3, None)
        # This was really the maximum size, one more byte and we can't import it.
        ms = "n" + ms
        desc = f"tr({PUBKEYS[0]},{ms})"
        res = self.ms_wo_vault.importdescriptors(
            [
                {
                    "desc": descsum_create(desc),
                    "active": False,
                    "timestamp": "now",
                }
            ]
        )[0]
        assert not res["success"]
        assert "is not a valid descriptor function" in res["error"]["message"]


if __name__ == "__main__":
    VaultMiniscriptTest(__file__).main()
