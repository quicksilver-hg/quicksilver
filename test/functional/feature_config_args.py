#!/usr/bin/env python3
# Copyright (c) 2017-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test various command line arguments and configuration file parameters."""

import json
import os
from pathlib import Path
import platform
import re
import tempfile
import time

from test_framework.netutil import UNREACHABLE_PROXY_ARG
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.test_node import ErrorMatch
from test_framework import util


class ConfArgsTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        # Prune to prevent disk space warning on CI systems with limited space,
        # when using networks other than sandbox.
        self.extra_args = [["-prune=550"]]
        self.supports_cli = False
        self.vault_names = []
        self.disable_autoconnect = False

    # Overridden to avoid attempt to sync not yet started nodes.
    def setup_network(self):
        self.setup_nodes()

    # Overridden to not start nodes automatically - doing so is the
    # responsibility of each test function.
    def setup_nodes(self):
        self.add_nodes(self.num_nodes, self.extra_args)
        self.ensure_debug_log()

    def ensure_debug_log(self):
        # Ensure a log file exists as TestNode.assert_debug_log() expects it.
        self.nodes[0].debug_log_path.parent.mkdir(parents=True, exist_ok=True)
        self.nodes[0].debug_log_path.touch()

    def test_dir_config(self):
        self.log.info('Error should be emitted if config file is a directory')
        conf_path = self.nodes[0].datadir_path / 'quicksilver.conf'
        os.rename(conf_path, conf_path.with_suffix('.confbkp'))
        conf_path.mkdir()
        self.stop_node(0)
        self.nodes[0].assert_start_raises_init_error(
            extra_args=['-sandbox'],
            expected_msg=f'Error: Error reading configuration file: Config file "{conf_path}" is a directory.',
        )
        conf_path.rmdir()
        os.rename(conf_path.with_suffix('.confbkp'), conf_path)

        self.log.debug('Verifying includeconf directive pointing to directory is caught')
        with open(conf_path, 'a', encoding='utf-8') as conf:
            conf.write(f'includeconf={self.nodes[0].datadir_path}\n')
        self.nodes[0].assert_start_raises_init_error(
            extra_args=['-sandbox'],
            expected_msg=f'Error: Error reading configuration file: Included config file "{self.nodes[0].datadir_path}" is a directory.',
        )

        self.nodes[0].replace_in_config([(f'includeconf={self.nodes[0].datadir_path}', '')])

    def test_negated_config(self):
        self.log.info('Disabling configuration via -noconf')

        conf_path = self.nodes[0].datadir_path / 'quicksilver.conf'
        with open(conf_path, encoding='utf-8') as conf:
            settings = [f'-{line.rstrip()}' for line in conf if len(line) > 1 and line[0] != '[']
        os.rename(conf_path, conf_path.with_suffix('.confbkp'))

        self.log.debug('Verifying garbage in config can be detected')
        with open(conf_path, 'a', encoding='utf-8') as conf:
            conf.write('garbage\n')
        self.nodes[0].assert_start_raises_init_error(
            extra_args=['-sandbox'],
            expected_msg='Error: Error reading configuration file: parse error on line 1: garbage',
        )

        self.log.debug('Verifying that disabling of the config file means garbage inside of it does ' \
            'not prevent the node from starting, and message about existing config file is logged')
        ignored_file_message = [f'Data directory "{self.nodes[0].datadir_path}" contains a "quicksilver.conf" file which is explicitly ignored using -noconf.']
        with self.nodes[0].assert_debug_log(timeout=60, expected_msgs=ignored_file_message):
            self.start_node(0, extra_args=settings + ['-noconf'])
        self.stop_node(0)

        self.log.debug('Verifying no message appears when removing config file')
        os.remove(conf_path)
        with self.nodes[0].assert_debug_log(timeout=60, expected_msgs=[], unexpected_msgs=ignored_file_message):
            self.start_node(0, extra_args=settings + ['-noconf'])
        self.stop_node(0)

        os.rename(conf_path.with_suffix('.confbkp'), conf_path)

    def test_config_file_parser(self):
        self.log.info('Test config file parser')

        # Check that startup fails if conf= is set in quicksilver.conf or in an included conf file
        bad_conf_file_path = self.nodes[0].datadir_path / "quicksilver_bad.conf"
        util.write_config(bad_conf_file_path, n=0, chain='', extra_config='conf=some.conf\n')
        conf_in_config_file_err = 'Error: Error reading configuration file: conf cannot be set in the configuration file; use includeconf= if you want to include additional config files'
        self.nodes[0].assert_start_raises_init_error(
            extra_args=[f'-conf={bad_conf_file_path}'],
            expected_msg=conf_in_config_file_err,
        )
        inc_conf_file_path = self.nodes[0].datadir_path / 'include.conf'
        with open(self.nodes[0].datadir_path / 'quicksilver.conf', 'a', encoding='utf-8') as conf:
            conf.write(f'includeconf={inc_conf_file_path}\n')
        with open(inc_conf_file_path, 'w', encoding='utf-8') as conf:
            conf.write('conf=some.conf\n')
        self.nodes[0].assert_start_raises_init_error(
            expected_msg=conf_in_config_file_err,
        )

        self.nodes[0].assert_start_raises_init_error(
            expected_msg='Error: Error parsing command line arguments: Invalid parameter -dash_cli=1',
            extra_args=['-dash_cli=1'],
        )
        with open(inc_conf_file_path, 'w', encoding='utf-8') as conf:
            conf.write('dash_conf=1\n')

        with self.nodes[0].assert_debug_log(expected_msgs=['Ignoring unknown configuration value dash_conf']):
            self.start_node(0)
        self.stop_node(0)

        with open(inc_conf_file_path, 'w', encoding='utf-8') as conf:
            conf.write('reindex=1\n')

        with self.nodes[0].assert_debug_log(expected_msgs=['Warning: reindex=1 is set in the configuration file, which will significantly slow down startup. Consider removing or commenting out this option for better performance, unless there is currently a condition which makes rebuilding the indexes necessary']):
            self.start_node(0)
        self.stop_node(0)

        with open(inc_conf_file_path, 'w', encoding='utf-8') as conf:
            conf.write('-dash=1\n')
        self.nodes[0].assert_start_raises_init_error(expected_msg='Error: Error reading configuration file: parse error on line 1: -dash=1, options in configuration file must be specified without leading -')

        if self.is_vault_compiled():
            with open(inc_conf_file_path, 'w', encoding='utf8') as conf:
                conf.write("vault=foo\n")
            self.nodes[0].assert_start_raises_init_error(expected_msg=f'Error: Config setting for -vault only applied on {self.chain} network when in [{self.chain}] section.')

        main_conf_file_path = self.nodes[0].datadir_path / "quicksilver_main.conf"
        util.write_config(main_conf_file_path, n=0, chain='', extra_config=f'includeconf={inc_conf_file_path}\n')
        with open(inc_conf_file_path, 'w', encoding='utf-8') as conf:
            conf.write('acceptnonstdtxn=1\n')
        default_conf = self.nodes[0].datadir_path / "quicksilver.conf"
        unused_conf = default_conf.with_suffix(".conf.unused")
        default_conf.rename(unused_conf)
        try:
            self.nodes[0].assert_start_raises_init_error(extra_args=[f"-conf={main_conf_file_path}"], expected_msg='Error: acceptnonstdtxn is not currently supported for main chain')
        finally:
            unused_conf.rename(default_conf)

        with open(inc_conf_file_path, 'w', encoding='utf-8') as conf:
            conf.write('nono\n')
        self.nodes[0].assert_start_raises_init_error(expected_msg='Error: Error reading configuration file: parse error on line 1: nono, if you intended to specify a negated option, use nono=1 instead')

        with open(inc_conf_file_path, 'w', encoding='utf-8') as conf:
            conf.write('server=1\nrpcuser=someuser\nrpcpassword=some#pass')
        self.nodes[0].assert_start_raises_init_error(expected_msg='Error: Error reading configuration file: parse error on line 3, using # in rpcpassword can be ambiguous and should be avoided')

        with open(inc_conf_file_path, 'w', encoding='utf-8') as conf:
            conf.write('server=1\nrpcuser=someuser\nmain.rpcpassword=some#pass')
        self.nodes[0].assert_start_raises_init_error(expected_msg='Error: Error reading configuration file: parse error on line 3, using # in rpcpassword can be ambiguous and should be avoided')

        with open(inc_conf_file_path, 'w', encoding='utf-8') as conf:
            conf.write('server=1\nrpcuser=someuser\n[main]\nrpcpassword=some#pass')
        self.nodes[0].assert_start_raises_init_error(expected_msg='Error: Error reading configuration file: parse error on line 4, using # in rpcpassword can be ambiguous and should be avoided')

        inc_conf_file2_path = self.nodes[0].datadir_path / 'include2.conf'
        with open(self.nodes[0].datadir_path / 'quicksilver.conf', 'a', encoding='utf-8') as conf:
            conf.write(f'includeconf={inc_conf_file2_path}\n')

        with open(inc_conf_file_path, 'w', encoding='utf-8') as conf:
            conf.write('testnot.datadir=1\n')
        with open(inc_conf_file2_path, 'w', encoding='utf-8') as conf:
            conf.write('[testnet]\n')
        self.restart_node(0)
        self.nodes[0].stop_node(expected_stderr=f'Warning: {inc_conf_file_path}:1 Section [testnot] is not recognized.{os.linesep}{inc_conf_file2_path}:1 Section [testnet] is not recognized.')

        with open(inc_conf_file_path, 'w', encoding='utf-8') as conf:
            conf.write('')  # clear
        with open(inc_conf_file2_path, 'w', encoding='utf-8') as conf:
            conf.write('')  # clear

    def test_config_file_log(self):
        # Disable this test for windows currently because trying to override
        # the default datadir through the environment does not seem to work.
        if platform.system() == "Windows":
            return

        self.log.info('Test that correct configuration path is changed when configuration file changes the datadir')

        # Create a temporary directory that will be treated as the default data
        # directory by quicksilverd.
        env, default_datadir = util.get_temp_default_datadir(Path(self.options.tmpdir, "test_config_file_log"))
        default_datadir.mkdir(parents=True)

        # Write a quicksilver.conf file in the default data directory containing a
        # datadir= line pointing at the node datadir.
        node = self.nodes[0]
        conf_text = node.quicksilverconf.read_text()
        conf_path = default_datadir / "quicksilver.conf"
        conf_path.write_text(f"datadir={node.datadir_path}\n{conf_text}")

        # Drop the node -datadir= argument during this test, because if it is
        # specified it would take precedence over the datadir setting in the
        # config file.
        node_args = node.args
        node.args = [arg for arg in node.args if not arg.startswith("-datadir=")]

        # Check that correct configuration file path is actually logged
        # (conf_path, not node.quicksilverconf)
        unused_node_conf = node.quicksilverconf.with_suffix(".conf.unused")
        node.quicksilverconf.rename(unused_node_conf)
        try:
            with self.nodes[0].assert_debug_log(expected_msgs=[f"loaded config={conf_path}"]):
                self.start_node(0, extra_args=[], env=env)
                self.stop_node(0)
        finally:
            unused_node_conf.rename(node.quicksilverconf)

        # Restore node arguments after the test
        node.args = node_args

    def test_invalid_command_line_options(self):
        self.nodes[0].assert_start_raises_init_error(
            expected_msg='Error: Error parsing command line arguments: Invalid parameter -deprecatedrpc=warnings',
            extra_args=['-deprecatedrpc=warnings'],
        )
        self.nodes[0].assert_start_raises_init_error(
            expected_msg='Error: Error parsing command line arguments: Invalid parameter -checkpoints=0',
            extra_args=['-checkpoints=0'],
        )
        self.nodes[0].assert_start_raises_init_error(
            expected_msg='Error: Error parsing command line arguments: Invalid parameter -forcednsseed=1',
            extra_args=['-forcednsseed=1'],
        )
        self.nodes[0].assert_start_raises_init_error(
            expected_msg='Error: Error parsing command line arguments: Can not set -proxy with no value. Please specify value with -proxy=value.',
            extra_args=['-proxy'],
        )
        # Provide a value different from 1 to the -vault negated option
        if self.is_vault_compiled():
            for value in [0, 'not_a_boolean']:
                self.nodes[0].assert_start_raises_init_error(
                    expected_msg="Error: Invalid value detected for '-vault' or '-novault'. '-vault' requires a string value, while '-novault' accepts only '1' to disable all vaults",
                    extra_args=[f'-novault={value}'],
                )

    def test_log_buffer(self):
        with self.nodes[0].assert_debug_log(expected_msgs=['Warning: parsed potentially confusing double-negative -listen=0\n']):
            self.start_node(0, extra_args=['-nolisten=0'])
        self.stop_node(0)

    def test_args_log(self):
        self.log.info('Test config args logging')
        with self.nodes[0].assert_debug_log(
                expected_msgs=[
                    'arg source=command-line addnode="some.node"',
                    'arg source=command-line rpcauth=****',
                    'arg source=command-line rpcpassword=****',
                    'arg source=command-line rpcuser=****',
                    'arg source=command-line torpassword=****',
                    f'arg source=config-file {self.chain}="1"',
                    f'arg source=config-file section={self.chain} server="1"',
                ],
                unexpected_msgs=[
                    'alice:f7efda5c189b999524f151318c0c86$d5b51b3beffbc0',
                    'secret-rpcuser',
                    'secret-torpassword',
                    'arg source=command-line rpcbind=****',
                    'arg source=command-line rpcallowip=****',
                ]):
            self.start_node(0, extra_args=[
                '-addnode=some.node',
                '-rpcauth=alice:f7efda5c189b999524f151318c0c86$d5b51b3beffbc0',
                '-rpcbind=127.1.1.1',
                '-rpcbind=127.0.0.1',
                "-rpcallowip=127.0.0.1",
                '-rpcpassword=',
                '-rpcuser=secret-rpcuser',
                '-torpassword=secret-torpassword',
                UNREACHABLE_PROXY_ARG,
            ])
        self.stop_node(0)

    def test_networkactive(self):
        self.log.info('Test -networkactive option')
        with self.nodes[0].assert_debug_log(expected_msgs=['ready network_active=1']):
            self.start_node(0)

        self.stop_node(0)
        with self.nodes[0].assert_debug_log(expected_msgs=['ready network_active=1']):
            self.start_node(0, extra_args=['-networkactive'])

        self.stop_node(0)
        with self.nodes[0].assert_debug_log(expected_msgs=['ready network_active=1']):
            self.start_node(0, extra_args=['-networkactive=1'])

        self.stop_node(0)
        with self.nodes[0].assert_debug_log(expected_msgs=['ready network_active=0']):
            self.start_node(0, extra_args=['-networkactive=0'])

        self.stop_node(0)
        with self.nodes[0].assert_debug_log(expected_msgs=['ready network_active=0']):
            self.start_node(0, extra_args=['-nonetworkactive'])

        self.stop_node(0)
        with self.nodes[0].assert_debug_log(expected_msgs=['ready network_active=0']):
            self.start_node(0, extra_args=['-nonetworkactive=1'])
        self.stop_node(0)

    def test_seed_peers(self):
        self.log.info('Test seed peers')
        default_data_dir = self.nodes[0].datadir_path
        peer_dat = default_data_dir / 'peers.dat'

        # No peers.dat exists and -dnsseed=1
        # We expect the node will use DNS Seeds, but Sandbox mode does not have
        # any valid DNS seeds. So after 60 seconds, the node should fallback to
        # fixed seeds
        assert not peer_dat.exists()
        start = int(time.time())
        with self.nodes[0].assert_debug_log(
                expected_msgs=[
                    "Loaded 0 addresses from peers.dat",
                    "loaded source=dnsseed addresses=0",
                    "opencon thread start",  # Ensure ThreadOpenConnections::start time is properly set
                ],
                timeout=10,
        ):
            self.start_node(0, extra_args=['-dnsseed=1', '-fixedseeds=1', f'-mocktime={start}', UNREACHABLE_PROXY_ARG])

        # Only sandbox has no fixed seeds. To avoid connections to random
        # nodes, sandbox is the only network where it is safe to enable
        # -fixedseeds in tests
        util.assert_equal(self.nodes[0].getblockchaininfo()['chain'],'sandbox')

        with self.nodes[0].assert_debug_log(expected_msgs=[
                "bootstrapping source=fixedseeds reason=addrman-empty",
        ]):
            self.nodes[0].setmocktime(start + 65)
        self.stop_node(0)

        # No peers.dat exists and -dnsseed=0
        # We expect the node will fallback immediately to fixed seeds
        assert not peer_dat.exists()
        with self.nodes[0].assert_debug_log(expected_msgs=[
                "Loaded 0 addresses from peers.dat",
                "ready dnsseed=disabled",
                "bootstrapping source=fixedseeds reason=dnsseed-disabled\n",
        ]):
            self.start_node(0, extra_args=['-dnsseed=0', '-fixedseeds=1'])
        self.stop_node(0)
        self.nodes[0].assert_start_raises_init_error(['-dnsseed=1', '-onlynet=i2p', '-i2psam=127.0.0.1:7656'], "Error: Incompatible options: -dnsseed=1 was explicitly specified, but -onlynet forbids connections to IPv4/IPv6")

        # No peers.dat exists and dns seeds are disabled.
        # We expect the node will not add fixed seeds when explicitly disabled.
        assert not peer_dat.exists()
        with self.nodes[0].assert_debug_log(expected_msgs=[
                "Loaded 0 addresses from peers.dat",
                "ready dnsseed=disabled",
                "ready fixedseeds=disabled",
        ]):
            self.start_node(0, extra_args=['-dnsseed=0', '-fixedseeds=0'])
        self.stop_node(0)

        # No peers.dat exists and -dnsseed=0, but a -addnode is provided
        # We expect the node will allow 60 seconds prior to using fixed seeds
        assert not peer_dat.exists()
        start = int(time.time())
        with self.nodes[0].assert_debug_log(
                expected_msgs=[
                    "Loaded 0 addresses from peers.dat",
                    "ready dnsseed=disabled",
                    "opencon thread start",  # Ensure ThreadOpenConnections::start time is properly set
                ],
                timeout=10,
        ):
            self.start_node(0, extra_args=['-dnsseed=0', '-fixedseeds=1', '-addnode=fakenodeaddr', f'-mocktime={start}', UNREACHABLE_PROXY_ARG])
        with self.nodes[0].assert_debug_log(expected_msgs=[
                "bootstrapping source=fixedseeds reason=addrman-empty",
        ]):
            self.nodes[0].setmocktime(start + 65)
        self.stop_node(0)

    def test_connect_with_seednode(self):
        self.log.info('Test -connect with -seednode')
        seednode_ignored = ['ignored option=-seednode reason=connect-set\n']
        dnsseed_ignored = ['ignored option=-dnsseed reason=connect-set-and-proxy-set\n']
        addcon_thread_started = ['addcon thread start\n']
        dnsseed_disabled = "overrode option=-dnsseed value=0 reason=connect-or-maxconnections-zero"
        listen_disabled = "overrode option=-listen value=0 reason=connect-or-maxconnections-zero"

        # When -connect is supplied, expanding addrman via getaddr calls to ADDR_FETCH(-seednode)
        # nodes is irrelevant and -seednode is ignored.
        with self.nodes[0].assert_debug_log(expected_msgs=seednode_ignored):
            self.start_node(0, extra_args=['-connect=fakeaddress1', '-seednode=fakeaddress2', UNREACHABLE_PROXY_ARG])

        # With -proxy, an ADDR_FETCH connection is made to a peer that the dns seed resolves to.
        # ADDR_FETCH connections are not used when -connect is used.
        with self.nodes[0].assert_debug_log(expected_msgs=dnsseed_ignored):
            self.restart_node(0, extra_args=['-connect=fakeaddress1', '-dnsseed=1', UNREACHABLE_PROXY_ARG])

        # If the user did not disable -dnsseed, but it was soft-disabled because they provided -connect,
        # they shouldn't see a warning about -dnsseed being ignored.
        with self.nodes[0].assert_debug_log(expected_msgs=addcon_thread_started,
                unexpected_msgs=dnsseed_ignored):
            self.restart_node(0, extra_args=['-connect=fakeaddress1', UNREACHABLE_PROXY_ARG])

        # We have to supply expected_msgs as it's a required argument
        # The expected_msg must be something we are confident will be logged after the unexpected_msg
        # These cases test for -connect being supplied but only to disable it
        for connect_arg in ['-connect=0', '-noconnect']:
            with self.nodes[0].assert_debug_log(expected_msgs=addcon_thread_started,
                    unexpected_msgs=seednode_ignored):
                self.restart_node(0, extra_args=[connect_arg, '-seednode=fakeaddress2'])

            # Make sure -noconnect soft-disables -listen and -dnsseed.
            # Need to temporarily remove these settings from the config file in
            # order for the two log messages to appear
            self.nodes[0].replace_in_config([("bind=", "#bind="), ("dnsseed=", "#dnsseed=")])
            with self.nodes[0].assert_debug_log(expected_msgs=[dnsseed_disabled, listen_disabled]):
                self.restart_node(0, extra_args=[connect_arg])
            self.nodes[0].replace_in_config([("#bind=", "bind="), ("#dnsseed=", "dnsseed=")])

            # Make sure -proxy and -noconnect warn about -dnsseed setting being
            # ignored, just like -proxy and -connect do.
            with self.nodes[0].assert_debug_log(expected_msgs=dnsseed_ignored):
                self.restart_node(0, extra_args=[connect_arg, '-dnsseed', '-proxy=localhost:1080'])
        self.stop_node(0)

    def test_ignored_conf(self):
        self.log.info('Test error is triggered when the datadir in use contains a quicksilver.conf file that would be ignored '
                      'because a conflicting -conf file argument is passed.')
        node = self.nodes[0]
        with tempfile.NamedTemporaryFile(dir=self.options.tmpdir, mode="wt", delete=False) as temp_conf:
            temp_conf.write(f"datadir={node.datadir_path}\n")
        node.assert_start_raises_init_error([f"-conf={temp_conf.name}"], re.escape(
            f'Error: Data directory "{node.datadir_path}" contains a "quicksilver.conf" file which is ignored, because a '
            f'different configuration file "{temp_conf.name}" from command line argument "-conf={temp_conf.name}" '
            f'is being used instead.') + r"[\s\S]*", match=ErrorMatch.FULL_REGEX)

        # Test that passing a redundant -conf command line argument pointing to
        # the same quicksilver.conf that would be loaded anyway does not trigger an
        # error.
        self.start_node(0, [f'-conf={node.datadir_path}/quicksilver.conf'])
        self.stop_node(0)

    def test_ignored_default_conf(self):
        # Disable this test for windows currently because trying to override
        # the default datadir through the environment does not seem to work.
        if platform.system() == "Windows":
            return

        self.log.info('Test error is triggered when quicksilver.conf in the default data directory sets another datadir '
                      'and it contains a different quicksilver.conf file that would be ignored')

        # Create a temporary directory that will be treated as the default data
        # directory by quicksilverd.
        env, default_datadir = util.get_temp_default_datadir(Path(self.options.tmpdir, "home"))
        default_datadir.mkdir(parents=True)

        # Write a quicksilver.conf file in the default data directory containing a
        # datadir= line pointing at the node datadir. This will trigger a
        # startup error because the node datadir contains a different
        # quicksilver.conf that would be ignored.
        node = self.nodes[0]
        (default_datadir / "quicksilver.conf").write_text(f"datadir={node.datadir_path}\n")

        # Drop the node -datadir= argument during this test, because if it is
        # specified it would take precedence over the datadir setting in the
        # config file.
        node_args = node.args
        node.args = [arg for arg in node.args if not arg.startswith("-datadir=")]
        node.assert_start_raises_init_error([], re.escape(
            f'Error: Data directory "{node.datadir_path}" contains a "quicksilver.conf" file which is ignored, because a '
            f'different configuration file "{default_datadir}/quicksilver.conf" from data directory "{default_datadir}" '
            f'is being used instead.') + r"[\s\S]*", env=env, match=ErrorMatch.FULL_REGEX)
        node.args = node_args

    def test_upnp_is_not_an_option(self):
        self.log.info("Test -upnp is rejected as unknown, not silently migrated")
        self.nodes[0].assert_start_raises_init_error(
            ['-upnp=1'],
            'Error: Error parsing command line arguments: Invalid parameter -upnp=1')

        self.log.debug("A settings.json upnp entry is not migrated to natpmp either")
        settings_file = self.nodes[0].chain_path / "settings.json"
        with open(settings_file, "w", encoding="utf8") as f:
            f.write('{"upnp": true}\n')
        self.start_node(0)
        self.stop_node(0)
        with open(settings_file, encoding="utf8") as f:
            settings = json.load(f)
        assert "natpmp" not in settings, "a removed option must not seed a live one"

    def test_removed_legacy_network_options(self):
        self.log.info("Test removed legacy network startup options")
        self.nodes[0].assert_start_raises_init_error(
            ['-testnet'],
            'Error: Error parsing command line arguments: Invalid parameter -testnet')
        self.nodes[0].assert_start_raises_init_error(
            ['-testnet4'],
            'Error: Error parsing command line arguments: Invalid parameter -testnet4')
        self.nodes[0].assert_start_raises_init_error(
            ['-regtest'],
            'Error: Error parsing command line arguments: Invalid parameter -regtest')
        self.nodes[0].assert_start_raises_init_error(
            ['-whitelistrelay'],
            'Error: Error parsing command line arguments: Invalid parameter -whitelistrelay')
        self.nodes[0].assert_start_raises_init_error(
            ['-whitelistforcerelay'],
            'Error: Error parsing command line arguments: Invalid parameter -whitelistforcerelay')

        self.log.debug("Public test network starts without a removed-network warning")
        removed_network_warning = "deprecated and will be removed"
        self.nodes[0].chain = 'publictest'
        self.nodes[0].replace_in_config([('sandbox=', 'publictest='), ('[sandbox]', '[publictest]')])
        self.ensure_debug_log()
        with self.nodes[0].assert_debug_log([], unexpected_msgs=[removed_network_warning]):
            self.start_node(0)
        self.stop_node(0)

        self.log.debug("Reset to sandbox")
        self.nodes[0].chain = 'sandbox'
        self.nodes[0].replace_in_config([('publictest=', 'sandbox='), ('[publictest]', '[sandbox]')])

    def run_test(self):
        self.test_log_buffer()
        self.test_args_log()
        self.test_seed_peers()
        self.test_networkactive()
        self.test_connect_with_seednode()

        self.test_dir_config()
        self.test_negated_config()
        self.test_config_file_parser()
        self.test_config_file_log()
        self.test_invalid_command_line_options()
        self.test_ignored_conf()
        self.test_ignored_default_conf()
        self.test_removed_legacy_network_options()
        self.test_upnp_is_not_an_option()

        # Remove the -datadir argument so it doesn't override the config file
        self.nodes[0].args = [arg for arg in self.nodes[0].args if not arg.startswith("-datadir")]

        default_data_dir = self.nodes[0].datadir_path
        new_data_dir = default_data_dir / 'newdatadir'
        new_data_dir_2 = default_data_dir / 'newdatadir2'

        # Check that using -datadir argument on non-existent directory fails
        self.nodes[0].datadir_path = new_data_dir
        self.nodes[0].assert_start_raises_init_error([f'-datadir={new_data_dir}'], f'Error: Specified data directory "{new_data_dir}" does not exist.')

        # Check that using non-existent datadir in conf file fails
        conf_file = default_data_dir / "quicksilver.conf"

        # datadir needs to be set before [chain] section
        with open(conf_file, encoding='utf8') as f:
            conf_file_contents = f.read()
        with open(conf_file, 'w', encoding='utf8') as f:
            f.write(f"datadir={new_data_dir}\n")
            f.write(conf_file_contents)

        self.nodes[0].assert_start_raises_init_error([f'-conf={conf_file}'], f'Error: Error reading configuration file: specified data directory "{new_data_dir}" does not exist.')

        # Check that an explicitly specified config file that cannot be opened fails
        none_existent_conf_file = default_data_dir / "none_existent_quicksilver.conf"
        self.nodes[0].assert_start_raises_init_error(['-conf=' + f'{none_existent_conf_file}'], 'Error: Error reading configuration file: specified config file "' + f'{none_existent_conf_file}' + '" could not be opened.')

        # Create the directory and ensure the config file now works
        new_data_dir.mkdir()
        self.start_node(0, [f'-conf={conf_file}'])
        self.stop_node(0)
        assert (new_data_dir / self.chain / 'blocks').exists()

        # Ensure command line argument overrides datadir in conf
        new_data_dir_2.mkdir()
        self.nodes[0].datadir_path = new_data_dir_2
        self.start_node(0, [f'-datadir={new_data_dir_2}', f'-conf={conf_file}'])
        assert (new_data_dir_2 / self.chain / 'blocks').exists()


if __name__ == '__main__':
    ConfArgsTest(__file__).main()
