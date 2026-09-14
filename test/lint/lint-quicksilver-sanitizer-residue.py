#!/usr/bin/env python3
#
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/licenses/mit/.
#
# F-151: sanitizer allowlists must not hide removed fee code, first-party
# Cuckatoo, or the whole GUI.
#
# A suppression is a silent pass. TxConfirmStats is gone with the fee
# estimator; a leftover line would cover a reintroduction. A bare `crypto/`
# prefix matches src/crypto/cuckatoo/ as well as hash primitives.
# `race:quicksilver-qt` and `src/qt/test/*` hide allotment, mining-model,
# and GPU-option races. TSan CI does not even build the GUI.

import re
import subprocess
import sys
from pathlib import Path


SUPPRESSION_DIR = Path("test/sanitizer_suppressions")

# Non-comment lines only. Comments in the suppression files may name a
# forbidden pattern in order to tell the next editor not to restore it.
FORBIDDEN = [
    (
        "removed fee estimator",
        re.compile(r"^[^#]*TxConfirmStats"),
    ),
    (
        "blanket crypto/ prefix (covers first-party cuckatoo)",
        re.compile(
            r"^(?:unsigned-integer-overflow|implicit-integer-sign-change|"
            r"implicit-signed-integer-truncation|implicit-unsigned-integer-truncation|"
            r"shift-base):crypto/\s*$"
        ),
    ),
    (
        "GUI binary wildcard",
        re.compile(r"^race:quicksilver-qt\s*$"),
    ),
    (
        "GUI test wildcard",
        re.compile(r"^(?:race|deadlock):src/qt/test/\*"),
    ),
]


def repo_root() -> Path:
    return Path(
        subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"], text=True, encoding="utf8"
        ).strip()
    )


def check_line(line: str) -> list[str]:
    hits = []
    for name, pattern in FORBIDDEN:
        if pattern.search(line):
            hits.append(name)
    return hits


def check_tree(root: Path) -> list[str]:
    failures = []
    directory = root / SUPPRESSION_DIR
    if not directory.is_dir():
        return [f"missing {SUPPRESSION_DIR}"]
    for path in sorted(directory.iterdir()):
        if not path.is_file():
            continue
        rel = path.relative_to(root).as_posix()
        for lineno, line in enumerate(path.read_text(encoding="utf8", errors="replace").splitlines(), 1):
            for name in check_line(line):
                failures.append(f"{rel}:{lineno}: {name}: {line}")
    return failures


def self_test() -> list[str]:
    cases = [
        ("unsigned-integer-overflow:TxConfirmStats::EstimateMedianVal", "removed fee estimator"),
        ("implicit-integer-sign-change:TxConfirmStats::removeTx", "removed fee estimator"),
        ("unsigned-integer-overflow:crypto/", "blanket crypto/ prefix"),
        ("implicit-integer-sign-change:crypto/", "blanket crypto/ prefix"),
        ("implicit-signed-integer-truncation:crypto/", "blanket crypto/ prefix"),
        ("implicit-unsigned-integer-truncation:crypto/", "blanket crypto/ prefix"),
        ("shift-base:crypto/", "blanket crypto/ prefix"),
        ("race:quicksilver-qt", "GUI binary wildcard"),
        ("race:src/qt/test/*", "GUI test wildcard"),
        ("deadlock:src/qt/test/*", "GUI test wildcard"),
        ("unsigned-integer-overflow:crypto/sha", None),
        ("unsigned-integer-overflow:crypto/cuckatoo/vendor", None),
        ("# race:quicksilver-qt", None),
        ("unsigned-integer-overflow:MurmurHash3", None),
    ]
    failures = []
    for line, expected in cases:
        hits = check_line(line)
        if expected is None:
            if hits:
                failures.append(f"self-test: {line!r} should pass, got {hits}")
        else:
            if not any(expected in hit for hit in hits):
                failures.append(f"self-test: {line!r} should match {expected!r}, got {hits}")
    return failures


def main() -> int:
    failures = self_test()
    failures.extend(check_tree(repo_root()))
    if failures:
        print("Quicksilver sanitizer residue remains:")
        print("\n".join(failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
