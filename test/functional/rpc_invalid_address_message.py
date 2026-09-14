#!/usr/bin/env python3
# Copyright (c) 2020-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test error messages for 'getaddressinfo' and 'validateaddress' RPC commands."""

from test_framework.test_framework import QuicksilverTestFramework

from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

BECH32_VALID = 'shg1qtmp74ayg7p24uslctssvjm06q5phz4yrq6vjet'
BECH32_VALID_UNKNOWN_WITNESS = 'shg1p424qdfhtc9'
BECH32_VALID_CAPITALS = 'SHG1QPLMTZKC2XHARPPZDLNPAQL78RSHJ68U3H9FD5Y'
BECH32_VALID_MULTISIG = 'shg1qdg3myrgvzw7ml9q0ejxhlkyxm7vl9r56yzkfgvzclrf4hkpx9yfqjnywl6'

# Wrong-HRP fixtures: sandbox decode rejects bcrt/bc prefixes as unsupported encoding.
BECH32_INVALID_BECH32 = 'bcrt1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqdmchcc'
BECH32_INVALID_BECH32M = 'bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7k35mrzd'
BECH32_INVALID_VERSION = 'bcrt130xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqynjegk'
BECH32_INVALID_SIZE = 'bcrt1s0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7v8n0nx0muaewav25430mtr'
BECH32_INVALID_V0_SIZE = 'bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kqqq5k3my'
BECH32_INVALID_PREFIX = 'bc1pw508d6qejxtdg4y5r3zarvary0c5xw7kw508d6qejxtdg4y5r3zarvary0c5xw7k7grplx'
BECH32_TOO_LONG = 'bcrt1q049edschfnwystcqnsvyfpj23mpsg3jcedq9xv049edschfnwystcqnsvyfpj23mpsg3jcedq9xv049edschfnwystcqnsvyfpj23m'
BECH32_ONE_ERROR = 'bcrt1q049edschfnwystcqnsvyfpj23mpsg3jcedq9xv'
BECH32_ONE_ERROR_CAPITALS = 'BCRT1QPLMTZKC2XHARPPZDLNPAQL78RSHJ68U32RAH7R'
BECH32_TWO_ERRORS = 'bcrt1qax9suht3qv95sw33xavx8crpxduefdrsvgsklu'
BECH32_NO_SEPARATOR = 'bcrtq049ldschfnwystcqnsvyfpj23mpsg3jcedq9xv'
BECH32_INVALID_CHAR = 'bcrt1q04oldschfnwystcqnsvyfpj23mpsg3jcedq9xv'
BECH32_MULTISIG_TWO_ERRORS = 'bcrt1qdg3myrgvzw7ml8q0ejxhlkyxn7vl9r56yzkfgvzclrf4hkpx9yfqhpsuks'
BECH32_WRONG_VERSION = 'bcrt1ptmp74ayg7p24uslctssvjm06q5phz4yrxucgnv'

BASE58_VALID = 'Sh7NPWrkYhreRDZh1PpAjJoBJrY2qNNEV2'
BASE58_INVALID_PREFIX = '2N3oefVeg6stiTb5Kh3ozCSkaqmx91FDbsm'
BASE58_INVALID_CHECKSUM = 'Sh7NPWrkYhreRDZh1PpAjJoBJrY2qNNEV3'
BASE58_INVALID_LENGTH = '2VKf7XKMrp4bVNVmuRbyCewkP8FhGLP2E54LHDPakr9Sq5mtU2'

INVALID_ADDRESS = 'asfah14i8fajz0123f'
INVALID_ADDRESS_2 = '1q049ldschfnwystcqnsvyfpj23mpsg3jcedq9xv'

class InvalidAddressErrorMessageTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def check_valid(self, addr):
        info = self.nodes[0].validateaddress(addr)
        assert info['isvalid']
        assert 'error' not in info
        assert 'error_locations' not in info

    def check_invalid(self, addr, error_str, error_locations=None):
        res = self.nodes[0].validateaddress(addr)
        assert not res['isvalid']
        assert_equal(res['error'], error_str)
        if error_locations:
            assert_equal(res['error_locations'], error_locations)
        else:
            assert_equal(res['error_locations'], [])

    def test_validateaddress(self):
        # Invalid Bech32
        self.check_invalid(BECH32_INVALID_SIZE, "Invalid or unsupported Segwit (Bech32) or Base58 encoding.")
        self.check_invalid(BECH32_INVALID_PREFIX, 'Invalid or unsupported Segwit (Bech32) or Base58 encoding.')
        self.check_invalid(BECH32_INVALID_BECH32, 'Invalid or unsupported Segwit (Bech32) or Base58 encoding.')
        self.check_invalid(BECH32_INVALID_BECH32M, 'Invalid or unsupported Segwit (Bech32) or Base58 encoding.')
        self.check_invalid(BECH32_INVALID_VERSION, 'Invalid or unsupported Segwit (Bech32) or Base58 encoding.')
        self.check_invalid(BECH32_INVALID_V0_SIZE, 'Invalid or unsupported Segwit (Bech32) or Base58 encoding.')
        self.check_invalid(BECH32_TOO_LONG, 'Invalid or unsupported Segwit (Bech32) or Base58 encoding.')
        self.check_invalid(BECH32_ONE_ERROR, 'Invalid or unsupported Segwit (Bech32) or Base58 encoding.')
        self.check_invalid(BECH32_TWO_ERRORS, 'Invalid or unsupported Segwit (Bech32) or Base58 encoding.')
        self.check_invalid(BECH32_ONE_ERROR_CAPITALS, 'Invalid checksum or length of Base58 address (P2PKH or P2SH)')
        self.check_invalid(BECH32_NO_SEPARATOR, 'Invalid or unsupported Segwit (Bech32) or Base58 encoding.')
        self.check_invalid(BECH32_INVALID_CHAR, 'Invalid or unsupported Segwit (Bech32) or Base58 encoding.')
        self.check_invalid(BECH32_MULTISIG_TWO_ERRORS, 'Invalid or unsupported Segwit (Bech32) or Base58 encoding.')
        self.check_invalid(BECH32_WRONG_VERSION, 'Invalid or unsupported Segwit (Bech32) or Base58 encoding.')

        # Valid Bech32
        self.check_valid(BECH32_VALID)
        self.check_valid(BECH32_VALID_UNKNOWN_WITNESS)
        self.check_valid(BECH32_VALID_CAPITALS)
        self.check_valid(BECH32_VALID_MULTISIG)

        # Invalid Base58
        self.check_invalid(BASE58_INVALID_PREFIX, 'Invalid or unsupported Base58-encoded address.')
        self.check_invalid(BASE58_INVALID_CHECKSUM, 'Invalid checksum or length of Base58 address (P2PKH or P2SH)')
        self.check_invalid(BASE58_INVALID_LENGTH, 'Invalid checksum or length of Base58 address (P2PKH or P2SH)')

        # Valid Base58
        self.check_valid(BASE58_VALID)

        # Invalid address format
        self.check_invalid(INVALID_ADDRESS, 'Invalid or unsupported Segwit (Bech32) or Base58 encoding.')
        self.check_invalid(INVALID_ADDRESS_2, 'Invalid or unsupported Segwit (Bech32) or Base58 encoding.')

        node = self.nodes[0]

        # Missing arg returns the help text
        assert_raises_rpc_error(-1, "Return information about the given Quicksilver address.", node.validateaddress)
        # Explicit None is not allowed for required parameters
        assert_raises_rpc_error(-3, "JSON value of type null is not of expected type string", node.validateaddress, None)

    def test_getaddressinfo(self):
        node = self.nodes[0]

        assert_raises_rpc_error(-5, "Invalid or unsupported Segwit (Bech32) or Base58 encoding.", node.getaddressinfo, BECH32_INVALID_SIZE)
        assert_raises_rpc_error(-5, "Invalid or unsupported Segwit (Bech32) or Base58 encoding.", node.getaddressinfo, BECH32_INVALID_PREFIX)
        assert_raises_rpc_error(-5, "Invalid or unsupported Base58-encoded address.", node.getaddressinfo, BASE58_INVALID_PREFIX)
        assert_raises_rpc_error(-5, "Invalid or unsupported Segwit (Bech32) or Base58 encoding.", node.getaddressinfo, INVALID_ADDRESS)
        assert "isscript" not in node.getaddressinfo(BECH32_VALID_UNKNOWN_WITNESS)

    def run_test(self):
        self.test_validateaddress()

        if self.is_vault_compiled():
            self.init_vault(node=0)
            self.test_getaddressinfo()


if __name__ == '__main__':
    InvalidAddressErrorMessageTest(__file__).main()
