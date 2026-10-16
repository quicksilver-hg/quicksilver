#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""-cuckatoodevice is validated at start-up, before any solve.

A typo or a contradictory choice must stop the node with the reason, not start
it and leave the user to discover hours later that nothing was solving."""
from test_framework.test_framework import QuicksilverTestFramework


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
        self.start_node(0, extra_args=["-cuckatoodevice=cpu"])


if __name__ == '__main__':
    GpuDeviceArgsTest(__file__).main()
