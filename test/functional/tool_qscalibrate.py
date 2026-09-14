#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Verify the qscalibrate CSV contract the model script depends on.

qscalibrate is calibration-only and EXCLUDE_FROM_ALL, so it is absent from an
ordinary build. This test skips in that case rather than failing: build it with
`cmake --build build --target qscalibrate`.
"""

import csv
import io
import os
import subprocess

from test_framework.test_framework import QuicksilverTestFramework, SkipTest
from test_framework.util import assert_equal

EXPECTED_HEADER = ["edgebits", "nonce", "seconds", "found", "threads"]


class QsCalibrateTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 0
        # qscalibrate is a standalone binary: no chain, no cache, no network.
        # Without setup_clean_chain the framework builds the block cache, which
        # generates against self.nodes[0] and has no node to generate against.
        self.setup_clean_chain = True

    def setup_network(self):
        pass

    def skip_test_if_missing_module(self):
        # qscalibrate is not one of set_binary_paths()' node binaries, but it is
        # emitted next to them by the global RUNTIME_OUTPUT_DIRECTORY.
        self.binary = os.path.join(
            self.config["environment"]["BUILDDIR"],
            "bin",
            "qscalibrate" + self.config["environment"]["EXEEXT"],
        )
        if not os.path.isfile(self.binary):
            raise SkipTest(
                "qscalibrate has not been built (it is EXCLUDE_FROM_ALL); "
                "build the 'qscalibrate' target to run this test")

    def run_test(self):
        out = subprocess.run(
            [self.binary, "--edgebits=22", "--graphs=5", "--threads=1"],
            capture_output=True, text=True, check=True).stdout

        rows = list(csv.DictReader(io.StringIO(out)))
        assert_equal(len(rows), 5)
        assert_equal(list(rows[0].keys()), EXPECTED_HEADER)

        for i, row in enumerate(rows):
            assert_equal(int(row["edgebits"]), 22)
            assert_equal(int(row["nonce"]), i)
            assert_equal(int(row["threads"]), 1)
            assert row["found"] in ("0", "1"), f"bad found value {row['found']}"
            assert float(row["seconds"]) > 0.0, "a graph cannot take zero time"

        self.log.info("qscalibrate emitted %d well-formed rows", len(rows))

        # An unbuilt size must fail loudly rather than emit an empty file that
        # later reads as "no cycles found at this size".
        bad = subprocess.run([self.binary, "--edgebits=31", "--graphs=1"],
                             capture_output=True, text=True)
        assert bad.returncode != 0, "unbuilt edgebits must be an error"
        assert "no bench solver" in bad.stderr, bad.stderr
        assert bad.stdout == "", "a failed run must not emit a CSV header"

        # A typo in a flag must not silently fall back to a default: a sweep run
        # under the wrong parameters is worse than one that did not run.
        typo = subprocess.run([self.binary, "--edgebits=22", "--graphs=notanumber"],
                              capture_output=True, text=True)
        assert typo.returncode != 0, "malformed argument must be an error"

        # Missing required arguments must not produce an empty-but-valid CSV.
        missing = subprocess.run([self.binary, "--edgebits=22"],
                                 capture_output=True, text=True)
        assert missing.returncode != 0, "missing --graphs must be an error"

        self.log.info("qscalibrate rejects unbuilt sizes, typos, and missing arguments")


if __name__ == "__main__":
    QsCalibrateTest(__file__).main()
