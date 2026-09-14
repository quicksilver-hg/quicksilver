#!/usr/bin/env python3
#
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
#
# Check source/bench/unit tests for Bitcoin denomination residue.
#
# Patterns are matched per line and again over the file joined into one
# whitespace-normalised string, so "100\nsats" and "sat\nper vbyte" cannot
# hide from them. See test/lint/lint_wrapped_prose.py.

import re
import subprocess
import sys
from pathlib import Path

sys.path.append(str(Path(__file__).parent))
from lint_wrapped_prose import WrappedText, report_self_test  # noqa: E402


SCOPES = (
    Path("src/bench"),
    Path("src/test"),
    Path("src/univalue/test"),
)

DISALLOWED_PATTERNS = (
    re.compile(r"\bbitcoin(?:s)?\b", re.IGNORECASE),
    re.compile(r"\b[A-Za-z0-9_]*(?:btc|xbt|mbtc|ubtc|μbtc)[A-Za-z0-9_]*\b", re.IGNORECASE),
    re.compile(r"\bsatoshis?\b", re.IGNORECASE),
    re.compile(
        r"\b\d+(?:\.\d+)?\s*sats?\b|"
        r"\bsats?\s*(?:/|per\s+)(?:vbytes?|vbyte|vb|kvb|kwu|wu|bytes?|byte|b)\b|"
        r"\b(?:sat/vb|sat/kvb|sat/kwu|sat/b|sats/vb|sats/kvb|sats/kwu|sats/b)\b",
        re.IGNORECASE,
    ),
)

ALLOWED_LINE_PATTERNS = (
    re.compile(r"Copyright .* Bitcoin Core developers", re.IGNORECASE),
    re.compile(r"Copyright .* Satoshi Nakamoto", re.IGNORECASE),
    re.compile(r"Satoshi codebase", re.IGNORECASE),
    re.compile(r"Bitcoin'?s genesis nBits", re.IGNORECASE),
    re.compile(r"Bitcoin'?s genesis coinbase", re.IGNORECASE),
    re.compile(r"Bitcoin-linked value", re.IGNORECASE),
    re.compile(r"Bitcoin genesis nBits magic found", re.IGNORECASE),
    re.compile(r"shipped Bitcoin'?s 0x1d00ffff", re.IGNORECASE),
    re.compile(r"demanded Bitcoin difficulty-1", re.IGNORECASE),
    re.compile(r"inherited Bitcoin", re.IGNORECASE),
    re.compile(r"upstream Bitcoin block hashes", re.IGNORECASE),
    re.compile(r"dropped the Bitcoin 0x1D00FFFF", re.IGNORECASE),
    re.compile(r"https://github\.com/bitcoin/bitcoin/issues/25055", re.IGNORECASE),
    # BIP 158 conformance (src/test/blockfilter_tests.cpp). The vectors are ten fixed
    # Bitcoin blocks recorded in Bitcoin's serialization; naming that is the point of the
    # test, since decoding them with our own diverged formats is what turned the previous
    # copy of blockfilters.json into a mirror. Narrow phrases, not a blanket allowance.
    re.compile(r"in Bitcoin'?s serialization", re.IGNORECASE),
    re.compile(r"diverged from Bitcoin'?s serialization", re.IGNORECASE),
    re.compile(r"over ten fixed Bitcoin blocks", re.IGNORECASE),
    re.compile(r"https://github\.com/bitcoin/bitcoin/blob/[^ ]*blockfilters\.json", re.IGNORECASE),
)


def repo_root() -> Path:
    return Path(subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True, encoding="utf8").strip())


def tracked_files(root: Path) -> list[Path]:
    files = subprocess.check_output(["git", "ls-files", "-z"], text=True, encoding="utf8").split("\0")
    paths = []
    for file_name in files:
        if not file_name:
            continue
        relpath = Path(file_name)
        if any(relpath == scope or relpath.is_relative_to(scope) for scope in SCOPES):
            paths.append(root / relpath)
    return paths


def line_allowed(line: str) -> bool:
    return any(pattern.search(line) for pattern in ALLOWED_LINE_PATTERNS)


def wrapped_failures(relpath: Path, lines: list[str]) -> list[str]:
    r"""Denomination residue that straddles a line break.

    The per-line loop below cannot see it: "100\nsats" and "sat\nper vbyte"
    both defeat a pattern written with `\s` between the halves. Exemptions stay
    per-line -- see test/lint/lint_wrapped_prose.py for why -- so a finding is
    dropped when any line it spans is allowed.
    """
    failures = []
    reported: set[int] = set()
    joined = WrappedText(lines)
    for pattern in DISALLOWED_PATTERNS:
        for match in joined.matches(pattern):
            if match.first_line in reported:
                continue
            if any(line_allowed(line) for line in lines[match.first_line - 1:match.last_line]):
                continue
            reported.add(match.first_line)
            failures.append(f"{relpath}:{match.first_line}: {match.text.strip()} ({match.where()})")
    return failures


def main() -> int:
    if report_self_test() != 0:
        return 1

    root = repo_root()
    failures = []
    for path in tracked_files(root):
        relpath = path.relative_to(root)
        lines = path.read_text(encoding="utf8", errors="replace").splitlines()
        for line_number, line in enumerate(lines, start=1):
            if line_allowed(line):
                continue
            if any(pattern.search(line) for pattern in DISALLOWED_PATTERNS):
                failures.append(f"{relpath}:{line_number}: {line.strip()}")
        failures.extend(wrapped_failures(relpath, lines))

    if failures:
        print("Source test Bitcoin-denomination residue remains:")
        print("\n".join(failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
