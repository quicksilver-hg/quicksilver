#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""-cuckatoodevice is validated at start-up, before any solve.

A typo or a contradictory choice must stop the node with the reason, not start
it and leave the user to discover hours later that nothing was solving.

Sandbox blocks need no solver (fBlockPowNoCycle), so the node logs once that
neither option is used for mining there (F-571), and says nothing when neither
is set."""
from pathlib import Path

from test_framework.test_framework import QuicksilverTestFramework

SANDBOX_INERT = "Sandbox blocks need no solver, so -cuckatoodevice and -cuckatoosolver are not used for mining."


class GpuDeviceArgsTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def run_test(self):
        node = self.nodes[0]
        self.stop_node(0)
        node.assert_start_raises_init_error(
            extra_args=["-cuckatoodevice=cpu", "-cuckatoodevice=pci:0000:04:00.0"],
            expected_msg="Error: -cuckatoodevice=cpu cannot be combined with a graphics card.",
        )
        node.assert_start_raises_init_error(
            extra_args=["-cuckatoodevice=04:00.0"],
            expected_msg="Error: -cuckatoodevice=04:00.0 is not a device id. Use an id from the helper's --list-devices output, such as pci:0000:04:00.0, or cpu.",
        )
        with node.assert_debug_log(expected_msgs=[SANDBOX_INERT]):
            self.start_node(0, extra_args=["-cuckatoodevice=cpu"])
        stub = Path(node.binary).with_name("test_solver_stub" + Path(node.binary).suffix)
        assert stub.exists(), stub
        with node.assert_debug_log(expected_msgs=[SANDBOX_INERT]):
            self.restart_node(0, extra_args=[f"-cuckatoosolver={stub}"])
        with node.assert_debug_log(expected_msgs=[], unexpected_msgs=[SANDBOX_INERT]):
            self.restart_node(0)


if __name__ == '__main__':
    GpuDeviceArgsTest(__file__).main()
