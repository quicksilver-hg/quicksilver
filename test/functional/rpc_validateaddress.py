#!/usr/bin/env python3
# Copyright (c) 2023 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test validateaddress for main chain

The vectors below are Quicksilver's own: mainnet addresses use the hrp "hg" and
the other-network case uses publictest's "phg". They mirror the structure of the
BIP-173 / BIP-350 vector sets — one address per decoder failure mode — but every
address is re-encoded for this chain, because an address carrying a foreign hrp
never reaches the checksum, witness-version or program-size checks at all: it is
rejected up front as an unsupported encoding, so it can only ever exercise that
one path.
"""

from test_framework.test_framework import QuicksilverTestFramework

from test_framework.util import assert_equal

UNSUPPORTED = "Invalid or unsupported Segwit (Bech32) or Base58 encoding."
NEEDS_BECH32M = "Version 1+ witness address must use Bech32m checksum"

INVALID_DATA = [
    # An hrp belonging to no Quicksilver network.
    ("tc1qw508d6qejxtdg4y5r3zarvary0c5xw7kg3g4ty", UNSUPPORTED, []),
    ("tc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vq5zuyut", UNSUPPORTED, []),
    # publictest's hrp on a mainnet node: structurally valid, wrong network.
    ("phg1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3q3lh9m3", UNSUPPORTED, []),
    ("phg1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqt32v90", UNSUPPORTED, []),
    ("phg1z0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqrvnrty", UNSUPPORTED, []),
    # Mixed case, publictest hrp.
    ("THG1P0XLXVLHEMJA6C4DQV22UAPCTQUPFHLXM9H8Z3K2E72Q4K9HCZ7VQDXVqs4", UNSUPPORTED, []),
    # A valid mainnet v0 address with its final data character advanced by one.
    ("hg1qw508d6qejxtdg4y5r3zarvary0c5xw7kdl70uv", "Invalid Bech32 checksum", [41]),
    # The same address upper-cased with one character left lower-case.
    ("HG1QW508D6QEJXTDG4Y5R3ZARVARY0C5XW7KDL70uT", "Invalid character or mixed case", [40]),
    # Witness v1+ carrying a Bech32 checksum where Bech32m is required.
    ("HG1RW508D6QEJXTDG4Y5R3ZARVARY0C5XW7KS5WJTK", NEEDS_BECH32M, []),
    ("hg1rw50q9zjytx", NEEDS_BECH32M, []),
    ("hg10w508d6qejxtdg4y5r3zarvary0c5xw7kw508d6qejxtdg4y5r3zarvary0c5xw7kqqpfmsdm", NEEDS_BECH32M, []),
    ("hg1zw508d6qejxtdg4y5r3zarvaryv9xjgjf", NEEDS_BECH32M, []),
    ("hg1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqerwtyc", NEEDS_BECH32M, []),
    ("HG1S0XLXVLHEMJA6C4DQV22UAPCTQUPFHLXM9H8Z3K2E72Q4K9HCZ7VQ6UYVV2", NEEDS_BECH32M, []),
    # Witness v0 carrying a Bech32m checksum where Bech32 is required.
    ("hg1qw508d6qejxtdg4y5r3zarvary0c5xw7kcrwref", "Version 0 witness address must use Bech32 checksum", []),
    # v0 program length BIP141 does not allow.
    ("HG1QW508D6QEJXTDG4Y5R3ZARVARYVLVYVC7", "Invalid Bech32 v0 address program size (16 bytes), per BIP141", []),
    # Checksum only, no data.
    ("hg16uf3hn", "Empty Bech32 data section", []),
    # 'o' is not in the Bech32 charset.
    ("hg1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqvlo8p6", "Invalid Base 32 character", [58]),
    # Witness version 17.
    ("HG130XLXVLHEMJA6C4DQV22UAPCTQUPFHLXM9H8Z3K2E72Q4K9HCZ7VQSTY95K", "Invalid Bech32 address witness version", []),
    # v1+ program sizes outside 2..40 bytes.
    ("hg1pw56mu4vm", "Invalid Bech32 address program size (1 byte)", []),
    ("hg1pw508d6qejxtdg4y5r3zarvary0c5xw7kw508d6qejxtdg4y5r3zarvary0c5xw7kqqkpw8ac", "Invalid Bech32 address program size (41 bytes)", []),
    # More than 4 bits of zero padding: 51 data values carry 255 bits, i.e. a
    # 31-byte program plus 7 zero bits.
    ("hg1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hczuqyl36cm", "Invalid padding in Bech32 data section", []),
]
VALID_DATA = [
    (
        "HG1QW508D6QEJXTDG4Y5R3ZARVARY0C5XW7KDL70UT",
        "0014751e76e8199196d454941c45d1b3a323f1433bd6",
    ),
    (
        "hg1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3qk3rwly",
        "00201863143c14c5166804bd19203356da136c985678cd4d27a1b8c6329604903262",
    ),
    (
        "hg1pw508d6qejxtdg4y5r3zarvary0c5xw7kw508d6qejxtdg4y5r3zarvary0c5xw7kk88end",
        "5128751e76e8199196d454941c45d1b3a323f1433bd6751e76e8199196d454941c45d1b3a323f1433bd6",
    ),
    ("HG1SW50QWDNERH", "6002751e"),
    ("hg1zw508d6qejxtdg4y5r3zarvaryvs6zyht", "5210751e76e8199196d454941c45d1b3a323"),
    (
        "hg1qqqqqp399et2xygdj5xreqhjjvcmzhxw4aywxecjdzew6hylgvses68tq77",
        "0020000000c4a5cad46221b2a187905e5266362b99d5e91c6ce24d165dab93e86433",
    ),
    (
        "hg1pqqqqp399et2xygdj5xreqhjjvcmzhxw4aywxecjdzew6hylgvsessstfxz",
        "5120000000c4a5cad46221b2a187905e5266362b99d5e91c6ce24d165dab93e86433",
    ),
    (
        "hg1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqvl78p6",
        "512079be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798",
    ),
    # PayToAnchor(P2A)
    (
        "hg1pfeeskrvalv",
        "51024e73",
    ),
]


class ValidateAddressMainTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.chain = ""  # main
        self.num_nodes = 1
        self.extra_args = [["-prune=899"]] * self.num_nodes

    def check_valid(self, addr, spk):
        info = self.nodes[0].validateaddress(addr)
        assert_equal(info["isvalid"], True)
        assert_equal(info["output_script"], spk)
        assert "error" not in info
        assert "error_locations" not in info

    def check_invalid(self, addr, error_str, error_locations):
        res = self.nodes[0].validateaddress(addr)
        assert_equal(res["isvalid"], False)
        assert_equal(res["error"], error_str)
        assert_equal(res["error_locations"], error_locations)

    def test_validateaddress(self):
        for (addr, error, locs) in INVALID_DATA:
            self.check_invalid(addr, error, locs)
        for (addr, spk) in VALID_DATA:
            self.check_valid(addr, spk)

    def run_test(self):
        self.test_validateaddress()


if __name__ == "__main__":
    ValidateAddressMainTest(__file__).main()
