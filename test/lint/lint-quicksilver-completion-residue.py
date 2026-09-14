#!/usr/bin/env python3
#
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
#
# Check that shipped shell completions carry Quicksilver attribution and do not
# advertise retired upstream command names, chain names, or fee options.

import re
import subprocess
import sys
from pathlib import Path


# Scope is narrow on purpose, and was re-measured on 2026-08-25: these rules
# describe what a shell completion file may name, so they only mean anything
# inside contrib/completions. Tree-wide they report 238 findings, none residue.
# Unlike the dead-surface lint, the directory here *is* the question.
SCOPES = [
    Path("contrib/completions"),
]

QUICKSILVER_HOLDER = "The Quicksilver developers"

FORBIDDEN = [
    re.compile(pattern, re.IGNORECASE)
    for pattern in [
        r"\bbitcoin(?:d|-cli|-qt|-tx|-util)?\b",
        r"(?<![A-Za-z0-9_])-(?:testnet|signet)\b",
        r"\b(?:testnet3|testnet4|signet)\b",
        r"(?<![A-Za-z0-9_])-(?:discardfee|minrelaytxfee|blockmintxfee|consolidatefeerate|fallbackfee|mintxfee|paytxfee|maxtxfee)\b",
    ]
]

COPYRIGHT_LINE = re.compile(r"Copyright .*Bitcoin", re.IGNORECASE)


def repo_root() -> Path:
    return Path(subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True, encoding="utf8").strip())


def tracked_completion_files(root: Path) -> list[Path]:
    raw = subprocess.check_output(["git", "ls-files", "-z"], text=True, encoding="utf8").split("\0")
    paths = []
    for name in raw:
        if not name:
            continue
        relpath = Path(name)
        if any(relpath == scope or relpath.is_relative_to(scope) for scope in SCOPES):
            paths.append(root / relpath)
    return paths


def is_binary(path: Path) -> bool:
    return b"\0" in path.read_bytes()[:8192]


def main() -> int:
    root = repo_root()
    failures = []
    for path in tracked_completion_files(root):
        if not path.exists() or is_binary(path):
            continue
        relpath = path.relative_to(root)
        lines = path.read_text(encoding="utf8", errors="replace").splitlines()
        header = "\n".join(lines[:8])
        if QUICKSILVER_HOLDER not in header:
            failures.append(f"{relpath}: missing Quicksilver copyright header")
        for line_number, line in enumerate(lines, start=1):
            if COPYRIGHT_LINE.search(line):
                continue
            if any(pattern.search(line) for pattern in FORBIDDEN):
                failures.append(f"{relpath}:{line_number}: {line.strip()}")

    if failures:
        print("Shell completion hygiene failures:")
        print("\n".join(failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
