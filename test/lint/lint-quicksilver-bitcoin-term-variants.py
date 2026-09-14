#!/usr/bin/env python3
#
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
#
# Check for Bitcoin denomination and abbreviation residue that is not covered
# by a plain "Bitcoin" branding scan.

import re
import subprocess
import sys
from pathlib import Path


EXCLUDED_PREFIXES = (
    "research/tier0-pow/",
    "src/crc32c/",
    "src/crypto/ctaes/",
    "src/leveldb/",
    "src/secp256k1/",
    "test/lint/",
)

# Plain "sat" is too ambiguous for a repository-wide grep: it appears as an
# English verb and in Miniscript satisfaction internals. These patterns cover
# denomination/fee contexts while avoiding those false positives.
TERM_RE = re.compile(
    r"(?ix)"
    r"(?<![A-Za-z])(?:xbt|m?btc|u?btc|μbtc)(?![A-Za-z])"
    r"|"
    r"\bsatoshis?\b"
    r"|"
    r"\b\d+(?:\.\d+)?\s*sats?\b"
    r"|"
    r"\bsats?\s*/\s*(?:vB|kvB|byte)s?\b"
    r"|"
    r"\bsats?\s*(?:per|-per-)\s*(?:byte|vbyte|kvB)\b"
)

ALLOW_RE = (
    re.compile(r"Copyright .*Satoshi Nakamoto"),
    re.compile(r"Satoshi Nakamoto"),
    re.compile(r"Satoshi's original implementation"),
    re.compile(r"Satoshi codebase"),
)


def repo_root() -> Path:
    return Path(
        subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"], text=True, encoding="utf8"
        ).strip()
    )


def is_binary(path: Path) -> bool:
    try:
        return b"\0" in path.read_bytes()[:8192]
    except OSError:
        return False


def tracked_files(root: Path) -> list[Path]:
    raw = subprocess.check_output(
        ["git", "ls-files", "-z"], text=True, encoding="utf8"
    ).split("\0")
    paths = []
    for rel in raw:
        if not rel:
            continue
        if rel.startswith(EXCLUDED_PREFIXES):
            continue
        paths.append(root / rel)
    return paths


def is_allowed_provenance_or_client_name(line: str) -> bool:
    return any(pattern.search(line) for pattern in ALLOW_RE)


def main() -> int:
    root = repo_root()
    residue = []

    for path in tracked_files(root):
        relpath = path.relative_to(root).as_posix()
        if not path.exists() or is_binary(path):
            continue
        try:
            lines = path.read_text(encoding="utf8", errors="replace").splitlines()
        except OSError as err:
            print(f"failed to read {relpath}: {err}", file=sys.stderr)
            return 1
        for line_number, line in enumerate(lines, start=1):
            if TERM_RE.search(line) and not is_allowed_provenance_or_client_name(line):
                residue.append(f"{relpath}:{line_number}: {line.strip()}")

    print(f"BITCOIN_TERM_VARIANT_RESIDUE={len(residue)}")
    if residue:
        print("Bitcoin denomination or abbreviation residue remains:")
        print("\n".join(residue))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
