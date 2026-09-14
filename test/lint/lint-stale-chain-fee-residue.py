#!/usr/bin/env python3
#
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
#
# Check that removed chains and fee arguments are not advertised by user-facing
# generated docs, installer templates, contrib tooling, or functional tests.

import re
import subprocess
import sys
from pathlib import Path


# There is deliberately no scope allowlist here. A list of directories to scan
# is half of this lint's assertion, and it is the half that fails silently. The
# list omitted src/ entirely, so a stale signet or -paytxfee in the node itself
# was never read by any pattern below.
SKIP_PATHS = {
    Path("test/lint/lint-stale-chain-fee-residue.py"),
    # Another linter's forbidden-name table. A rule that bans a name, its
    # self-test case and the entry exempting them all contain that name, so
    # exempting one side alone just moves the failure into the other file.
    Path("test/lint/lint-quicksilver-completion-residue.py"),
}

FORBIDDEN = [
    re.compile(pattern)
    for pattern in [
        r"(?<![A-Za-z0-9_])-signet\b",
        r"(?<![A-Za-z0-9_])-testnet\b",
        r"(?<![A-Za-z0-9_])-discardfee\b",
        r"(?<![A-Za-z0-9_])-minrelaytxfee\b",
        r"(?<![A-Za-z0-9_])-blockmintxfee\b",
        r"(?<![A-Za-z0-9_])-consolidatefeerate\b",
        r"(?<![A-Za-z0-9_])-fallbackfee\b",
        r"(?<![A-Za-z0-9_])-mintxfee\b",
        r"(?<![A-Za-z0-9_])-paytxfee\b",
        r"(?<![A-Za-z0-9_])-maxtxfee\b",
        r"\bchain=signet\b",
        r"\bchain=test\b",
        r"^\[signet\]$",
        r"^\[test\]$",
        r"\btestnet3\b",
        r"\bsignet\b",
        r"\bsignetchallenge\b",
        r"\bsignetseednode\b",
        r"\bnodes_signet\.txt\b",
        r"\bseeds_signet\.txt\b",
        r"\bchainparams_seed_signet\b",
    ]
]

ALLOWED_NEGATIVE_TESTS = [
    re.compile(pattern)
    for pattern in [
        r"Invalid parameter -testnet",
        r"\['-testnet'\]",
        r"assert .* not in ",
        # Absence assertions in the unit tests: the chain name has to be named
        # in order to prove the node rejects it.
        r"!ChainTypeFromString\("
    ]
]


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
        paths.append(root / relpath)
    return paths


def is_binary(path: Path) -> bool:
    return b"\0" in path.read_bytes()[:8192]


def main() -> int:
    root = repo_root()
    failures = []
    for path in tracked_files(root):
        if not path.exists():
            continue
        if is_binary(path):
            continue
        relpath = path.relative_to(root)
        for line_number, line in enumerate(path.read_text(encoding="utf8", errors="replace").splitlines(), start=1):
            if any(pattern.search(line) for pattern in ALLOWED_NEGATIVE_TESTS):
                continue
            if any(pattern.search(line) for pattern in FORBIDDEN):
                failures.append(f"{relpath}:{line_number}: {line.strip()}")

    if failures:
        print("Stale chain/fee residue remains:")
        print("\n".join(failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
