#!/usr/bin/env python3
#
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
#
# Check that Bitcoin monetary unit terminology does not leak into Quicksilver
# source comments, tests, docs, or public artifacts. The broad "bitcoin" brand
# sweep is separate; this catches abbreviations and unit spellings such as BTC
# and sat/vB that otherwise slip past brand-only searches.

import re
import subprocess
import sys
from pathlib import Path


SKIP_PREFIXES = [
    Path("research/tier0-pow"),
    Path("src/crc32c"),
    Path("src/crypto/ctaes"),
    Path("src/leveldb"),
    Path("src/secp256k1"),
    Path("src/univalue"),
    Path("test/lint"),
]

# Full-token Bitcoin currency abbreviations. Keep case-insensitive because stale
# comments often use lowercase "btc".
BTC_FAMILY_RE = re.compile(r"\b(?:btc|xbt|mbtc|ubtc|μbtc)\b", re.IGNORECASE)

# "satoshi" is a Bitcoin monetary unit when not part of legal attribution or
# explicit upstream lineage text.
SATOSHI_RE = re.compile(r"\bsatoshis?\b", re.IGNORECASE)

# "sat" is too noisy as a blanket token: Miniscript uses it for satisfaction,
# and normal prose uses "sat" as a verb. Catch unit/rate contexts instead.
SAT_UNIT_CONTEXT_RE = re.compile(
    r"(?:\b\d+(?:\.\d+)?\s*(?:sat|sats)\b|\b(?:sat|sats)\s*(?:/|per\s+)(?:v?b|k?vb|kwu|byte|kbyte)\b)",
    re.IGNORECASE,
)

ALLOWED = {
    Path("test/lint/lint-desktop-packaging.py"): [
        re.compile(r"bitcoin\|bitcoins\|btc\|satoshi"),
    ],
    Path("contrib/devtools/copyright_header.py"): [
        re.compile(r"r\"Satoshi Nakamoto\""),
    ],
    Path("src/arith_uint256.h"): [
        re.compile(r"Satoshi's original implementation used BN_bn2mpi"),
    ],
    Path("test/functional/test_framework/script.py"): [
        re.compile(r"FindAndDelete\(\) in Satoshi codebase"),
    ],
}


def repo_root() -> Path:
    return Path(subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True, encoding="utf8").strip())


def tracked_files(root: Path) -> list[Path]:
    raw = subprocess.check_output(["git", "ls-files", "-z"], text=True, encoding="utf8").split("\0")
    paths = []
    for name in raw:
        if not name:
            continue
        relpath = Path(name)
        if any(relpath == prefix or relpath.is_relative_to(prefix) for prefix in SKIP_PREFIXES):
            continue
        paths.append(root / relpath)
    return paths


def is_binary(path: Path) -> bool:
    try:
        return b"\0" in path.read_bytes()[:8192]
    except OSError:
        return False


def line_allowed(relpath: Path, line: str) -> bool:
    if re.search(r"Copyright .*Satoshi Nakamoto", line):
        return True
    return any(pattern.search(line) for pattern in ALLOWED.get(relpath, []))


def matched_rule(line: str) -> str | None:
    if BTC_FAMILY_RE.search(line):
        return "btc-family"
    if SATOSHI_RE.search(line):
        return "satoshi"
    if SAT_UNIT_CONTEXT_RE.search(line):
        return "sat-unit-context"
    return None


def self_test() -> int:
    cases = [
        ("10 btc output", "btc-family"),
        ("Transaction fee rate (BTC/kvB)", "btc-family"),
        ("100 sat/vB", "sat-unit-context"),
        ("5 sats per vbyte", "sat-unit-context"),
        ("1000 sat output", "sat-unit-context"),
        ("Satoshi's original implementation", "satoshi"),
        ("the GUI thread sat blocked", None),
        ("MaxSatSize(use_max_sig)", None),
    ]
    failures = []
    for line, expected in cases:
        got = matched_rule(line)
        if got != expected:
            failures.append(f"{line!r}: expected {expected!r}, got {got!r}")
    if failures:
        print("Bitcoin-unit residue lint self-test failures:")
        print("\n".join(failures))
        return 1
    print("Bitcoin-unit residue lint self-test OK")
    return 0


def main() -> int:
    if sys.argv[1:] == ["--self-test"]:
        return self_test()
    if len(sys.argv) != 1:
        print(f"Usage: {sys.argv[0]} [--self-test]", file=sys.stderr)
        return 2

    root = repo_root()
    failures = []
    for path in tracked_files(root):
        if not path.exists() or is_binary(path):
            continue
        relpath = path.relative_to(root)
        for line_number, line in enumerate(path.read_text(encoding="utf8", errors="replace").splitlines(), start=1):
            rule = matched_rule(line)
            if rule is None or line_allowed(relpath, line):
                continue
            failures.append(f"{relpath}:{line_number}: [{rule}] {line.strip()}")

    if failures:
        print("Bitcoin monetary-unit residue remains:")
        print("\n".join(failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
