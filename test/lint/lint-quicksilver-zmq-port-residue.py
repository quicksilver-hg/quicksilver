#!/usr/bin/env python3
#
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
#
# Check that public ZMQ examples do not advertise inherited upstream sample
# ports after Quicksilver network defaults are active.

import re
import subprocess
import sys
from pathlib import Path


# There is deliberately no scope allowlist here. A list of files to scan is
# half of this lint's assertion, and it is the half that fails silently. The
# list named two files, so an upstream 28332 surviving anywhere else -- a
# functional test, an example conf, a contrib script -- passed unread.
SKIP_PATHS = {
    Path("test/lint/lint-quicksilver-zmq-port-residue.py"),
}

# This rule matches a bare five-digit number, so unlike the word-matching
# linters it does collide with vendored constant tables: secp256k1's
# precomputed_ecmult.c contains the literal "e3e28332".
SKIP_PREFIXES = [
    Path("src/crc32c"),
    Path("src/crypto/ctaes"),
    Path("src/leveldb"),
    Path("src/secp256k1"),
    Path("src/univalue"),
]

FORBIDDEN = re.compile(r"(?<![0-9])(?:28332|28333)(?![0-9])")


def repo_root() -> Path:
    return Path(subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True, encoding="utf8").strip())


def tracked_files(root: Path) -> list[Path]:
    raw = subprocess.check_output(["git", "ls-files", "-z"], text=True, encoding="utf8").split("\0")
    paths = []
    for name in raw:
        if not name:
            continue
        relpath = Path(name)
        if relpath in SKIP_PATHS:
            continue
        if any(relpath.is_relative_to(prefix) for prefix in SKIP_PREFIXES):
            continue
        paths.append(root / relpath)
    return paths


def is_binary(path: Path) -> bool:
    try:
        return b"\0" in path.read_bytes()[:8192]
    except OSError:
        return False


def main() -> int:
    root = repo_root()
    failures = []
    for path in tracked_files(root):
        if not path.exists() or is_binary(path):
            continue
        relpath = path.relative_to(root)
        for line_number, line in enumerate(path.read_text(encoding="utf8", errors="replace").splitlines(), start=1):
            if FORBIDDEN.search(line):
                failures.append(f"{relpath}:{line_number}: {line.strip()}")

    if failures:
        print("Public stale upstream ZMQ example port residue remains:")
        print("\n".join(failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
