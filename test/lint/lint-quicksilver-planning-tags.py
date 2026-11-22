#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Keep internal planning labels out of first-party source and tests."""

import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SELF = Path("test/lint/lint-quicksilver-planning-tags.py")
VENDORED = (
    "src/secp256k1/", "src/leveldb/", "src/crc32c/",
    "src/univalue/", "src/crypto/cuckatoo/vendor/", "src/crypto/ctaes/",
)
RULE = re.compile(
    r"#5[a-c](?:-[0-9]+)?\b|\(#[3-7]\)|#[3-7]/|\bStage [12]\b|"
    r"\bPhase [0-9]+ S[0-9]+|de-Bitcoinify|decision note|\bM[0-9]+/M[0-9]+\b"
)


def main() -> int:
    names = subprocess.check_output(["git", "ls-files", "-z"], cwd=ROOT).decode().split("\0")
    failures = []
    for name in names:
        if not name or not name.startswith(("src/", "test/", "contrib/")):
            continue
        if name == str(SELF) or name.startswith(VENDORED):
            continue
        try:
            lines = (ROOT / name).read_text(encoding="utf8").splitlines()
        except (UnicodeDecodeError, OSError):
            continue
        for number, line in enumerate(lines, 1):
            if RULE.search(line):
                failures.append(f"{name}:{number}: {line.strip()}")
    for failure in failures:
        print(failure)
    if failures:
        print(f"planning tags: {len(failures)} line(s)")
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
