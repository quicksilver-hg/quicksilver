#!/usr/bin/env python3
#
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
#
# Check that the surviving Qt GUI surface exposes Quicksilver branding.

import re
import subprocess
import sys
from pathlib import Path


# Scope is narrow on purpose, and was re-measured on 2026-08-25: these rules
# match Qt branding idioms that are ordinary prose everywhere else, so running
# them tree-wide reports 411 findings and none is residue. Unlike the
# dead-surface lint, the directory here *is* the question being asked.
SCOPES = [
    Path("src/qt"),
]

DISALLOWED_PATTERNS = [
    re.compile(pattern)
    for pattern in [
        r"\bBitcoin\b",
        r"\bBitcoin[A-Za-z0-9_]+\b",
        r"\b[Bb]itcoin[A-Za-z0-9_-]*\b",
        r"\bBITCOIN[A-Z0-9_]*\b",
        r"\bBitcoins\b",
        r"\bbitcoin-qt\b",
        r"\bbitcoin:\b",
        r"\bbitcoin://\b",
        r"\bBTC\b",
        r"\bSatoshi\b",
        r"\bsatoshi(?:s)?\b",
    ]
]

SKIP_PATH_PARTS: set[str] = set()

ALLOWED_LINE_PATTERNS = [
    re.compile(pattern)
    for pattern in [
        r"Copyright .* Bitcoin Core developers",
        r"\bBIP21\b",
        r"\bBIP 21\b",
    ]
]


def repo_root() -> Path:
    return Path(subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True, encoding="utf8").strip())


def tracked_files(root: Path) -> list[Path]:
    files = subprocess.check_output(["git", "ls-files", "-z"], text=True, encoding="utf8").split("\0")
    paths = []
    for file_name in files:
        if not file_name:
            continue
        path = Path(file_name)
        if any(part in SKIP_PATH_PARTS for part in path.parts):
            continue
        if any(path == scope or path.is_relative_to(scope) for scope in SCOPES):
            paths.append(root / path)
    return paths


def line_allowed(line: str) -> bool:
    return any(pattern.search(line) for pattern in ALLOWED_LINE_PATTERNS)


def main() -> int:
    root = repo_root()
    failures = []
    for path in tracked_files(root):
        relpath = path.relative_to(root)
        for line_number, line in enumerate(path.read_text(encoding="utf8", errors="replace").splitlines(), start=1):
            if line_allowed(line):
                continue
            if any(pattern.search(line) for pattern in DISALLOWED_PATTERNS):
                failures.append(f"{relpath}:{line_number}: {line.strip()}")

    if failures:
        print("Qt branding residue remains:")
        print("\n".join(failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
