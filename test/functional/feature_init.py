#!/usr/bin/env python3
# Copyright (c) 2021-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests related to node initialization."""
from pathlib import Path
import os
import platform
import shutil
import signal
import subprocess

from test_framework.test_framework import QuicksilverTestFramework
from test_framework.test_node import (
    QUICKSILVER_PID_FILENAME_DEFAULT,
    ErrorMatch,
)
from test_framework.util import assert_equal


class InitTest(QuicksilverTestFramework):
    """
    Ensure that initialization can be interrupted at a number of points and not impair
    subsequent starts.
    """

    def add_options(self, parser):
        self.add_vault_options(parser)

    def set_test_params(self):
        self.setup_clean_chain = False
        self.num_nodes = 1

    def init_stress_test(self):
        """
        - test terminating initialization after seeing a certain log line.
        - test removing certain essential files to test startup error paths.
        """
        self.stop_node(0)
        node = self.nodes[0]

        def sigterm_node():
            if platform.system() == 'Windows':
                # Don't call Python's terminate() since it calls
                # TerminateProcess(), which unlike SIGTERM doesn't allow
                # quicksilver-daemon to perform any shutdown logic.
                os.kill(node.process.pid, signal.CTRL_BREAK_EVENT)
            else:
                node.process.terminate()
            node.process.wait()

        def start_expecting_error(err_fragment):
            node.assert_start_raises_init_error(
                extra_args=['-txindex=1', '-blockfilterindex=1', '-coinstatsindex=1', '-checkblocks=200', '-checklevel=4'],
                expected_msg=err_fragment,
                match=ErrorMatch.PARTIAL_REGEX,
            )

        def check_clean_start():
            """Ensure that node restarts successfully after various interrupts."""
            node.start()
            node.wait_for_rpc_connection()
            assert_equal(200, node.getblockcount())

        lines_to_terminate_after = [
            b'ready assumevalid=none scope=all',
            b'scheduler thread start',
            b'Starting HTTP server',
            b'Loading peer signals',
            b'Loading banlist',
            b'Loading ledger index',
            b'checking blkfiles=all',
            b'loaded best_block=',
            b'init message: Verifying ledger blocks',
            b'init message: Starting signal threads',
            b'net thread start',
            b'addcon thread start',
            b'initload thread start',
            b'txindex thread start',
            b'block filter index thread start',
            b'coinstatsindex thread start',
            b'msghand thread start',
            b'net thread start',
            b'addcon thread start',
        ]
        if self.is_vault_compiled():
            lines_to_terminate_after.append(b'Verifying vault')

        args = ['-txindex=1', '-blockfilterindex=1', '-coinstatsindex=1']
        for terminate_line in lines_to_terminate_after:
            self.log.info(f"Starting node and will exit after line {terminate_line}")
            with node.busy_wait_for_debug_log([terminate_line]):
                if platform.system() == 'Windows':
                    # CREATE_NEW_PROCESS_GROUP is required in order to be able
                    # to terminate the child without terminating the test.
                    node.start(extra_args=args, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
                else:
                    node.start(extra_args=args)
            self.log.debug("Terminating node after terminate line was found")
            sigterm_node()

        check_clean_start()
        self.stop_node(0)

        self.log.info("Test startup errors after removing certain essential files")

        files_to_delete = {
            'blocks/index/*.ldb': 'Error opening block database.',
            'chainstate/*.ldb': 'Error opening coins database.',
            'blocks/blk*.dat': 'Error loading block database.',
        }

        files_to_perturb = {
            'blocks/index/*.ldb': 'Error loading block database.',
            'chainstate/*.ldb': 'Error (opening coins database|loading databases).',
        }

        for file_patt, err_fragment in files_to_delete.items():
            target_files = list(node.chain_path.glob(file_patt))

            for target_file in target_files:
                self.log.info(f"Deleting file to ensure failure {target_file}")
                bak_path = str(target_file) + ".bak"
                target_file.rename(bak_path)

            start_expecting_error(err_fragment)

            for target_file in target_files:
                bak_path = str(target_file) + ".bak"
                self.log.debug(f"Restoring file from {bak_path} and restarting")
                Path(bak_path).rename(target_file)

            check_clean_start()
            self.stop_node(0)

        self.log.info("Test startup errors after perturbing certain essential files")
        for file_patt, err_fragment in files_to_perturb.items():
            shutil.copytree(node.chain_path / "blocks", node.chain_path / "blocks_bak")
            shutil.copytree(node.chain_path / "chainstate", node.chain_path / "chainstate_bak")
            target_files = list(node.chain_path.glob(file_patt))

            for target_file in target_files:
                self.log.info(f"Perturbing file to ensure failure {target_file}")
                with open(target_file, "r+b") as tf:
                    # Choose a non-header byte range well into the file so the
                    # corruption affects a checked block rather than startup metadata.
                    tf.seek(max(150, target_file.stat().st_size // 2))
                    tf.write(b"1" * 200)

            start_expecting_error(err_fragment)

            shutil.rmtree(node.chain_path / "blocks")
            shutil.rmtree(node.chain_path / "chainstate")
            shutil.move(node.chain_path / "blocks_bak", node.chain_path / "blocks")
            shutil.move(node.chain_path / "chainstate_bak", node.chain_path / "chainstate")

    def init_pid_test(self):
        QUICKSILVER_PID_FILENAME_CUSTOM = "my_fancy_quicksilver_pid_file.foobar"

        self.log.info("Test specifying custom pid file via -pid command line option")
        custom_pidfile_relative = QUICKSILVER_PID_FILENAME_CUSTOM
        self.log.info(f"-> path relative to datadir ({custom_pidfile_relative})")
        self.restart_node(0, [f"-pid={custom_pidfile_relative}"])
        datadir = self.nodes[0].chain_path
        assert not (datadir / QUICKSILVER_PID_FILENAME_DEFAULT).exists()
        assert (datadir / custom_pidfile_relative).exists()
        self.stop_node(0)
        assert not (datadir / custom_pidfile_relative).exists()

        custom_pidfile_absolute = Path(self.options.tmpdir) / QUICKSILVER_PID_FILENAME_CUSTOM
        self.log.info(f"-> absolute path ({custom_pidfile_absolute})")
        self.restart_node(0, [f"-pid={custom_pidfile_absolute}"])
        assert not (datadir / QUICKSILVER_PID_FILENAME_DEFAULT).exists()
        assert custom_pidfile_absolute.exists()
        self.stop_node(0)
        assert not custom_pidfile_absolute.exists()

    def run_test(self):
        self.init_pid_test()
        self.init_stress_test()


if __name__ == '__main__':
    InitTest(__file__).main()
