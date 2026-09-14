#!/usr/bin/env python3
# Copyright (c) 2017-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test quicksilver-cli"""

from decimal import Decimal
import re

from test_framework.blocktools import COINBASE_MATURITY, quicksilver_sandbox_subsidy
from test_framework.netutil import test_ipv6_local
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than_or_equal,
    assert_raises_process_error,
    assert_raises_rpc_error,
    get_auth_cookie,
    rpc_port,
)
import time

# Quicksilver sandbox uses a height-based subsidy ramp. After mining 101 blocks,
# only the height-1 coinbase is mature.
BLOCKS = COINBASE_MATURITY + 1
BALANCE = quicksilver_sandbox_subsidy(1)

JSON_PARSING_ERROR = 'error: Error parsing JSON: foo'
BLOCKS_VALUE_OF_ZERO = 'error: the first argument (number of blocks to generate, default: 1) must be an integer value greater than zero'
TOO_MANY_ARGS = 'error: too many arguments (maximum 2 for nblocks and maxtries)'
VAULT_NOT_LOADED = 'Requested vault does not exist or is not loaded'
VAULT_NOT_SPECIFIED = (
    "Multiple vaults are loaded. Please select which vault to use by requesting the RPC "
    "through the /vault/<vaultname> URI path. Or for the CLI, specify the \"-rpcvault=<vaultname>\" "
    "option before the command (run \"quicksilver-cli -h\" for help or \"quicksilver-cli listvaults\" to see "
    "which vaults are currently loaded)."
)


def cli_get_info_string_to_dict(cli_get_info_string):
    """Helper method to convert human-readable -getinfo into a dictionary"""
    cli_get_info = {}
    lines = cli_get_info_string.splitlines()
    line_idx = 0
    ansi_escape = re.compile(r'(\x9B|\x1B\[)[0-?]*[ -\/]*[@-~]')
    while line_idx < len(lines):
        # Remove ansi colour code
        line = ansi_escape.sub('', lines[line_idx])
        if "Balances" in line:
            # When "Balances" appears in a line, all of the following lines contain "balance: vault" until an empty line
            cli_get_info["Balances"] = {}
            while line_idx < len(lines) and not (lines[line_idx + 1] == ''):
                line_idx += 1
                balance, vault = lines[line_idx].strip().split(" ")
                # Remove right justification padding
                vault = vault.strip()
                if vault == '""':
                    # Set default vault("") to empty string
                    vault = ''
                cli_get_info["Balances"][vault] = balance.strip()
        elif ": " in line:
            key, value = line.split(": ")
            if key == 'Vault' and value == '""':
                # Set default vault("") to empty string
                value = ''
            if key == "Proxies" and value == "n/a":
                # Set N/A to empty string to represent no proxy
                value = ''
            cli_get_info[key.strip()] = value.strip()
        line_idx += 1
    return cli_get_info


class TestQuicksilverCli(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_cli()

    def run_test(self):
        """Main test logic"""
        self.generate(self.nodes[0], BLOCKS)

        self.log.info("Compare responses from getblockchaininfo RPC and `quicksilver-cli getblockchaininfo`")
        cli_response = self.nodes[0].cli.getblockchaininfo()
        rpc_response = self.nodes[0].getblockchaininfo()
        assert_equal(cli_response, rpc_response)

        self.log.info("Test named arguments")
        assert_equal(self.nodes[0].cli.echo(0, 1, arg3=3, arg5=5), ['0', '1', None, '3', None, '5'])
        assert_raises_rpc_error(-8, "Parameter arg1 specified twice both as positional and named argument", self.nodes[0].cli.echo, 0, 1, arg1=1)
        assert_raises_rpc_error(-8, "Parameter arg1 specified twice both as positional and named argument", self.nodes[0].cli.echo, 0, None, 2, arg1=1)

        self.log.info("Test that later cli named arguments values silently overwrite earlier ones")
        assert_equal(self.nodes[0].cli("-named", "echo", "arg0=0", "arg1=1", "arg2=2", "arg1=3").send_cli(), ['0', '3', '2'])
        assert_raises_rpc_error(-8, "Parameter args specified multiple times", self.nodes[0].cli("-named", "echo", "args=[0,1,2,3]", "4", "5", "6", ).send_cli)

        user, password = get_auth_cookie(self.nodes[0].datadir_path, self.chain)

        self.log.info("Test -stdinrpcpass option")
        assert_equal(BLOCKS, self.nodes[0].cli(f'-rpcuser={user}', '-stdinrpcpass', input=password).getblockcount())
        assert_raises_process_error(1, 'Incorrect rpcuser or rpcpassword', self.nodes[0].cli(f'-rpcuser={user}', '-stdinrpcpass', input='foo').echo)

        self.log.info("Test -stdin and -stdinrpcpass")
        assert_equal(['foo', 'bar'], self.nodes[0].cli(f'-rpcuser={user}', '-stdin', '-stdinrpcpass', input=f'{password}\nfoo\nbar').echo())
        assert_raises_process_error(1, 'Incorrect rpcuser or rpcpassword', self.nodes[0].cli(f'-rpcuser={user}', '-stdin', '-stdinrpcpass', input='foo').echo)

        self.log.info("Test connecting to a non-existing server")
        assert_raises_process_error(1, "Could not connect to the server", self.nodes[0].cli('-rpcport=1').echo)

        self.log.info("Test handling of invalid ports in rpcconnect")
        assert_raises_process_error(1, "Invalid port provided in -rpcconnect: 127.0.0.1:notaport", self.nodes[0].cli("-rpcconnect=127.0.0.1:notaport").echo)
        assert_raises_process_error(1, "Invalid port provided in -rpcconnect: 127.0.0.1:-1", self.nodes[0].cli("-rpcconnect=127.0.0.1:-1").echo)
        assert_raises_process_error(1, "Invalid port provided in -rpcconnect: 127.0.0.1:0", self.nodes[0].cli("-rpcconnect=127.0.0.1:0").echo)
        assert_raises_process_error(1, "Invalid port provided in -rpcconnect: 127.0.0.1:65536", self.nodes[0].cli("-rpcconnect=127.0.0.1:65536").echo)

        self.log.info("Checking for IPv6")
        have_ipv6 = test_ipv6_local()
        if not have_ipv6:
            self.log.info("Skipping IPv6 tests")

        if have_ipv6:
            assert_raises_process_error(1, "Invalid port provided in -rpcconnect: [::1]:notaport", self.nodes[0].cli("-rpcconnect=[::1]:notaport").echo)
            assert_raises_process_error(1, "Invalid port provided in -rpcconnect: [::1]:-1", self.nodes[0].cli("-rpcconnect=[::1]:-1").echo)
            assert_raises_process_error(1, "Invalid port provided in -rpcconnect: [::1]:0", self.nodes[0].cli("-rpcconnect=[::1]:0").echo)
            assert_raises_process_error(1, "Invalid port provided in -rpcconnect: [::1]:65536", self.nodes[0].cli("-rpcconnect=[::1]:65536").echo)

        self.log.info("Test handling of invalid ports in rpcport")
        assert_raises_process_error(1, "Invalid port provided in -rpcport: notaport", self.nodes[0].cli("-rpcport=notaport").echo)
        assert_raises_process_error(1, "Invalid port provided in -rpcport: -1", self.nodes[0].cli("-rpcport=-1").echo)
        assert_raises_process_error(1, "Invalid port provided in -rpcport: 0", self.nodes[0].cli("-rpcport=0").echo)
        assert_raises_process_error(1, "Invalid port provided in -rpcport: 65536", self.nodes[0].cli("-rpcport=65536").echo)

        self.log.info("Test port usage preferences")
        node_rpc_port = rpc_port(self.nodes[0].index)
        # Prevent quicksilver-cli from using existing rpcport in conf
        conf_rpcport = "rpcport=" + str(node_rpc_port)
        self.nodes[0].replace_in_config([(conf_rpcport, "#" + conf_rpcport)])
        # prefer rpcport over rpcconnect
        assert_raises_process_error(1, "Could not connect to the server 127.0.0.1:1", self.nodes[0].cli(f"-rpcconnect=127.0.0.1:{node_rpc_port}", "-rpcport=1").echo)
        if have_ipv6:
            assert_raises_process_error(1, "Could not connect to the server ::1:1", self.nodes[0].cli(f"-rpcconnect=[::1]:{node_rpc_port}", "-rpcport=1").echo)

        assert_equal(BLOCKS, self.nodes[0].cli("-rpcconnect=127.0.0.1:18999", f'-rpcport={node_rpc_port}').getblockcount())
        if have_ipv6:
            assert_equal(BLOCKS, self.nodes[0].cli("-rpcconnect=[::1]:18999", f'-rpcport={node_rpc_port}').getblockcount())

        # prefer rpcconnect port over default
        assert_equal(BLOCKS, self.nodes[0].cli(f"-rpcconnect=127.0.0.1:{node_rpc_port}").getblockcount())
        if have_ipv6:
            assert_equal(BLOCKS, self.nodes[0].cli(f"-rpcconnect=[::1]:{node_rpc_port}").getblockcount())

        # prefer rpcport over default
        assert_equal(BLOCKS, self.nodes[0].cli(f'-rpcport={node_rpc_port}').getblockcount())
        # Re-enable rpcport in conf if present
        self.nodes[0].replace_in_config([("#" + conf_rpcport, conf_rpcport)])

        self.log.info("Test connecting with non-existing RPC cookie file")
        assert_raises_process_error(1, "Could not locate RPC credentials", self.nodes[0].cli('-rpccookiefile=does-not-exist', '-rpcpassword=').echo)

        self.log.info("Test connecting without RPC cookie file and with password arg")
        assert_equal(BLOCKS, self.nodes[0].cli('-norpccookiefile', f'-rpcuser={user}', f'-rpcpassword={password}').getblockcount())

        self.log.info("Test -getinfo with arguments fails")
        assert_raises_process_error(1, "-getinfo takes no arguments", self.nodes[0].cli('-getinfo').help)

        self.log.info("Test -getinfo with -color=never does not return ANSI escape codes")
        assert "\u001b[0m" not in self.nodes[0].cli('-getinfo', '-color=never').send_cli()

        self.log.info("Test -getinfo with -color=always returns ANSI escape codes")
        assert "\u001b[0m" in self.nodes[0].cli('-getinfo', '-color=always').send_cli()

        self.log.info("Test -getinfo with invalid value for -color option")
        assert_raises_process_error(1, "Invalid value for -color option. Valid values: always, auto, never.", self.nodes[0].cli('-getinfo', '-color=foo').send_cli)

        self.log.info("Test -getinfo returns expected network and blockchain info")
        if self.is_specified_vault_compiled():
            self.import_deterministic_coinbase_privkeys()
            self.nodes[0].encryptvault(password)
        cli_get_info_string = self.nodes[0].cli('-getinfo').send_cli()
        cli_get_info = cli_get_info_string_to_dict(cli_get_info_string)

        network_info = self.nodes[0].getnetworkinfo()
        blockchain_info = self.nodes[0].getblockchaininfo()
        assert_equal(int(cli_get_info['Version']), network_info['version'])
        assert_equal(cli_get_info['Verification progress'], "%.4f%%" % (blockchain_info['verificationprogress'] * 100))
        assert_equal(int(cli_get_info['Blocks']), blockchain_info['blocks'])
        assert_equal(int(cli_get_info['Headers']), blockchain_info['headers'])
        assert_equal(int(cli_get_info['Time offset (s)']), network_info['timeoffset'])
        expected_network_info = f"in {network_info['connections_in']}, out {network_info['connections_out']}, total {network_info['connections']}"
        assert_equal(cli_get_info["Network"], expected_network_info)
        assert_equal(cli_get_info['Proxies'], network_info['networks'][0]['proxy'])
        assert_equal(Decimal(cli_get_info['Difficulty']), blockchain_info['difficulty'])
        assert_equal(cli_get_info['Chain'], blockchain_info['chain'])

        self.log.info("Test -getinfo and quicksilver-cli return all proxies")
        self.restart_node(0, extra_args=["-proxy=127.0.0.1:9050", "-i2psam=127.0.0.1:7656"])
        network_info = self.nodes[0].getnetworkinfo()
        cli_get_info_string = self.nodes[0].cli('-getinfo').send_cli()
        cli_get_info = cli_get_info_string_to_dict(cli_get_info_string)
        assert_equal(cli_get_info["Proxies"], "127.0.0.1:9050 (ipv4, ipv6, onion, cjdns), 127.0.0.1:7656 (i2p)")

        if self.is_specified_vault_compiled():
            self.log.info("Test -getinfo and quicksilver-cli getvaultinfo return expected vault info")
            # Explicitly set the output type in order to have consistent tx vsize
            # for vaults (disables the change address type detection algorithm)
            self.restart_node(0, extra_args=["-addresstype=bech32", "-changetype=bech32"])
            assert_equal(Decimal(cli_get_info['Balance']), BALANCE)
            assert 'Balances' not in cli_get_info_string
            vault_info = self.nodes[0].getvaultinfo()
            assert_equal(int(cli_get_info['Keypool size']), vault_info['keypoolsize'])
            assert_equal(int(cli_get_info['Unlocked until']), vault_info['unlocked_until'])
            assert not any('paytxfee' in key.lower() for key in cli_get_info)
            assert 'paytxfee' not in vault_info
            assert_equal(self.nodes[0].cli.getvaultinfo(), vault_info)

            # Setup to test -getinfo, -generate, and -rpcvault= with multiple vaults.
            vaults = [self.default_vault_name, 'Encrypted', 'secret']
            amounts = [BALANCE - Decimal(40) + quicksilver_sandbox_subsidy(2), Decimal(9), Decimal(31)]
            self.nodes[0].createvault(vault_name=vaults[1])
            self.nodes[0].createvault(vault_name=vaults[2])
            w1 = self.nodes[0].get_vault_rpc(vaults[0])
            w2 = self.nodes[0].get_vault_rpc(vaults[1])
            w3 = self.nodes[0].get_vault_rpc(vaults[2])
            rpcvault2 = f'-rpcvault={vaults[1]}'
            rpcvault3 = f'-rpcvault={vaults[2]}'
            w1.vaultpassphrase(password, self.rpc_timeout)
            w2.encryptvault(password)
            w1.sendtoaddress(w2.getnewaddress(), amounts[1])
            w1.sendtoaddress(w3.getnewaddress(), amounts[2])

            # Mine a block to confirm; this also matures the height-2 coinbase.
            self.generate(self.nodes[0], 1)

            self.log.info("Test -getinfo with multiple vaults and -rpcvault returns specified vault balance")
            for i in range(len(vaults)):
                cli_get_info_string = self.nodes[0].cli('-getinfo', f'-rpcvault={vaults[i]}').send_cli()
                cli_get_info = cli_get_info_string_to_dict(cli_get_info_string)
                assert 'Balances' not in cli_get_info_string
                assert_equal(cli_get_info["Vault"], vaults[i])
                assert_equal(Decimal(cli_get_info['Balance']), amounts[i])

            self.log.info("Test -getinfo with multiple vaults and -rpcvault=non-existing-vault returns no balances")
            cli_get_info_string = self.nodes[0].cli('-getinfo', '-rpcvault=does-not-exist').send_cli()
            assert 'Balance' not in cli_get_info_string
            assert 'Balances' not in cli_get_info_string

            self.log.info("Test -getinfo with multiple vaults returns all loaded vault names and balances")
            assert_equal(set(self.nodes[0].listvaults()), set(vaults))
            cli_get_info_string = self.nodes[0].cli('-getinfo').send_cli()
            cli_get_info = cli_get_info_string_to_dict(cli_get_info_string)
            assert 'Balance' not in cli_get_info
            for k, v in zip(vaults, amounts):
                assert_equal(Decimal(cli_get_info['Balances'][k]), v)

            # Unload the default vault and re-verify.
            self.nodes[0].unloadvault(vaults[0])
            assert vaults[0] not in self.nodes[0].listvaults()
            cli_get_info_string = self.nodes[0].cli('-getinfo').send_cli()
            cli_get_info = cli_get_info_string_to_dict(cli_get_info_string)
            assert 'Balance' not in cli_get_info
            assert 'Balances' in cli_get_info_string
            for k, v in zip(vaults[1:], amounts[1:]):
                assert_equal(Decimal(cli_get_info['Balances'][k]), v)
            assert vaults[0] not in cli_get_info

            self.log.info("Test -getinfo after unloading all vaults except a non-default one returns its balance")
            self.nodes[0].unloadvault(vaults[2])
            assert_equal(self.nodes[0].listvaults(), [vaults[1]])
            cli_get_info_string = self.nodes[0].cli('-getinfo').send_cli()
            cli_get_info = cli_get_info_string_to_dict(cli_get_info_string)
            assert 'Balances' not in cli_get_info_string
            assert_equal(cli_get_info['Vault'], vaults[1])
            assert_equal(Decimal(cli_get_info['Balance']), amounts[1])

            self.log.info("Test -getinfo -norpcvault returns the same as -getinfo")
            # Previously there was a bug where -norpcvault was treated like -rpcvault=0
            assert_equal(self.nodes[0].cli('-getinfo', "-norpcvault").send_cli(), cli_get_info_string)

            self.log.info("Test -getinfo with -rpcvault=remaining-non-default-vault returns only its balance")
            cli_get_info_string = self.nodes[0].cli('-getinfo', rpcvault2).send_cli()
            cli_get_info = cli_get_info_string_to_dict(cli_get_info_string)
            assert 'Balances' not in cli_get_info_string
            assert_equal(cli_get_info['Vault'], vaults[1])
            assert_equal(Decimal(cli_get_info['Balance']), amounts[1])

            self.log.info("Test -getinfo with -rpcvault=unloaded vault returns no balances")
            cli_get_info_string = self.nodes[0].cli('-getinfo', rpcvault3).send_cli()
            cli_get_info_keys = cli_get_info_string_to_dict(cli_get_info_string)
            assert 'Balance' not in cli_get_info_keys
            assert 'Balances' not in cli_get_info_string

            # Test quicksilver-cli -generate.
            n1 = 3
            n2 = 4
            w2.vaultpassphrase(password, self.rpc_timeout)
            blocks = self.nodes[0].getblockcount()

            self.log.info('Test -generate with no args')
            generate = self.nodes[0].cli('-generate').send_cli()
            assert_equal(set(generate.keys()), {'address', 'blocks'})
            assert_equal(len(generate["blocks"]), 1)
            assert_equal(self.nodes[0].getblockcount(), blocks + 1)

            self.log.info('Test -generate with bad args')
            assert_raises_process_error(1, JSON_PARSING_ERROR, self.nodes[0].cli('-generate', 'foo').echo)
            assert_raises_process_error(1, BLOCKS_VALUE_OF_ZERO, self.nodes[0].cli('-generate', 0).echo)
            assert_raises_process_error(1, TOO_MANY_ARGS, self.nodes[0].cli('-generate', 1, 2, 3).echo)

            self.log.info('Test -generate with nblocks')
            generate = self.nodes[0].cli('-generate', n1).send_cli()
            assert_equal(set(generate.keys()), {'address', 'blocks'})
            assert_equal(len(generate["blocks"]), n1)
            assert_equal(self.nodes[0].getblockcount(), blocks + 1 + n1)

            self.log.info('Test -generate with nblocks and maxtries')
            generate = self.nodes[0].cli('-generate', n2, 1000000).send_cli()
            assert_equal(set(generate.keys()), {'address', 'blocks'})
            assert_equal(len(generate["blocks"]), n2)
            assert_equal(self.nodes[0].getblockcount(), blocks + 1 + n1 + n2)

            self.log.info('Test -generate -rpcvault in single-vault mode')
            generate = self.nodes[0].cli(rpcvault2, '-generate').send_cli()
            assert_equal(set(generate.keys()), {'address', 'blocks'})
            assert_equal(len(generate["blocks"]), 1)
            assert_equal(self.nodes[0].getblockcount(), blocks + 2 + n1 + n2)

            self.log.info('Test -generate -rpcvault=unloaded vault raises RPC error')
            assert_raises_rpc_error(-18, VAULT_NOT_LOADED, self.nodes[0].cli(rpcvault3, '-generate').echo)
            assert_raises_rpc_error(-18, VAULT_NOT_LOADED, self.nodes[0].cli(rpcvault3, '-generate', 'foo').echo)
            assert_raises_rpc_error(-18, VAULT_NOT_LOADED, self.nodes[0].cli(rpcvault3, '-generate', 0).echo)
            assert_raises_rpc_error(-18, VAULT_NOT_LOADED, self.nodes[0].cli(rpcvault3, '-generate', 1, 2, 3).echo)

            # Test quicksilver-cli -generate with -rpcvault in multivault mode.
            self.nodes[0].loadvault(vaults[2])
            n3 = 4
            n4 = 10
            blocks = self.nodes[0].getblockcount()

            self.log.info('Test -generate -rpcvault=<filename> raise RPC error')
            vault2_path = f'-rpcvault={self.nodes[0].vaults_path / vaults[2] / self.vault_data_filename}'
            assert_raises_rpc_error(-18, VAULT_NOT_LOADED, self.nodes[0].cli(vault2_path, '-generate').echo)

            self.log.info('Test -generate -rpcvault with no args')
            generate = self.nodes[0].cli(rpcvault2, '-generate').send_cli()
            assert_equal(set(generate.keys()), {'address', 'blocks'})
            assert_equal(len(generate["blocks"]), 1)
            assert_equal(self.nodes[0].getblockcount(), blocks + 1)

            self.log.info('Test -generate -rpcvault with bad args')
            assert_raises_process_error(1, JSON_PARSING_ERROR, self.nodes[0].cli(rpcvault2, '-generate', 'foo').echo)
            assert_raises_process_error(1, BLOCKS_VALUE_OF_ZERO, self.nodes[0].cli(rpcvault2, '-generate', 0).echo)
            assert_raises_process_error(1, TOO_MANY_ARGS, self.nodes[0].cli(rpcvault2, '-generate', 1, 2, 3).echo)

            self.log.info('Test -generate -rpcvault with nblocks')
            generate = self.nodes[0].cli(rpcvault2, '-generate', n3).send_cli()
            assert_equal(set(generate.keys()), {'address', 'blocks'})
            assert_equal(len(generate["blocks"]), n3)
            assert_equal(self.nodes[0].getblockcount(), blocks + 1 + n3)

            self.log.info('Test -generate -rpcvault with nblocks and maxtries')
            generate = self.nodes[0].cli(rpcvault2, '-generate', n4, 1000000).send_cli()
            assert_equal(set(generate.keys()), {'address', 'blocks'})
            assert_equal(len(generate["blocks"]), n4)
            assert_equal(self.nodes[0].getblockcount(), blocks + 1 + n3 + n4)

            self.log.info('Test -generate without -rpcvault in multivault mode raises RPC error')
            assert_raises_rpc_error(-19, VAULT_NOT_SPECIFIED, self.nodes[0].cli('-generate').echo)
            assert_raises_rpc_error(-19, VAULT_NOT_SPECIFIED, self.nodes[0].cli('-generate', 'foo').echo)
            assert_raises_rpc_error(-19, VAULT_NOT_SPECIFIED, self.nodes[0].cli('-generate', 0).echo)
            assert_raises_rpc_error(-19, VAULT_NOT_SPECIFIED, self.nodes[0].cli('-generate', 1, 2, 3).echo)
        else:
            self.log.info("*** Vault not compiled; cli getvaultinfo and -getinfo vault tests skipped")
            self.generate(self.nodes[0], 25)  # maintain block parity with the vault_compiled conditional branch

        self.log.info("Test -version with node stopped")
        self.stop_node(0)
        cli_response = self.nodes[0].cli('-version').send_cli()
        assert f"{self.config['environment']['CLIENT_NAME']} RPC client version" in cli_response

        self.log.info("Test -rpcwait option successfully waits for RPC connection")
        self.nodes[0].start()  # start node without RPC connection
        self.nodes[0].wait_for_cookie_credentials()  # ensure cookie file is available to avoid race condition
        blocks = self.nodes[0].cli('-rpcwait').send_cli('getblockcount')
        self.nodes[0].wait_for_rpc_connection()
        assert_equal(blocks, BLOCKS + 25)

        self.log.info("Test -rpcwait option waits at most -rpcwaittimeout seconds for startup")
        self.stop_node(0)  # stop the node so we time out
        start_time = time.time()
        assert_raises_process_error(1, "Could not connect to the server", self.nodes[0].cli('-rpcwait', '-rpcwaittimeout=5').echo)
        assert_greater_than_or_equal(time.time(), start_time + 5)

        self.log.info("Test that only one of -addrinfo, -generate, -getinfo, -netinfo may be specified at a time")
        assert_raises_process_error(1, "Only one of -getinfo, -netinfo may be specified", self.nodes[0].cli('-getinfo', '-netinfo').send_cli)


if __name__ == '__main__':
    TestQuicksilverCli(__file__).main()
