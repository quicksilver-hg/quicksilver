#!/usr/bin/env python3
#
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
#
# Check that public help, examples, and UI command fixtures do not advertise
# retired upstream P2P ports after Quicksilver network defaults are active.

import re
import subprocess
import sys
from pathlib import Path


# Scope is narrow on purpose, and was re-measured on 2026-08-25: this rule
# matches a bare port number, which collides with vendored constant tables.
# secp256k1's precomputed_ecmult.c supplies eleven of the twelve tree-wide
# hits as substrings of its hex constants, and a research timing CSV the
# twelfth. None is residue. Widen only with vendored-tree skips, and
# re-measure first -- and note that spelling a port out in a comment here is
# itself enough to fail the sibling ZMQ-port lint.
SCOPES = [
    Path("README.md"),
    Path("src/rpc/net.cpp"),
    Path("src/netbase.h"),
    Path("src/qt"),
    Path("doc"),
    Path("share/examples"),
    Path("contrib/init"),
    Path("contrib/qos"),
    Path("contrib/tracing/README.md"),
    # Test fixtures: an arbitrary-looking port in an addrman/net fixture is still
    # a retired upstream default sitting in the tree, so hold them to the same bar.
    Path("src/bench"),
    Path("src/test"),
    Path("test/functional"),
]

SKIP_PATHS = {
    Path("test/lint/lint-quicksilver-public-port-residue.py"),
    # Consensus/filter test vectors: these are hex byte streams in which "8333"
    # occurs by coincidence, not as a port. Rewriting them would corrupt the
    # vectors, so they are excluded by path rather than by pattern.
    Path("src/test/data/sighash.json"),
    Path("src/test/data/blockfilters.json"),
}

FORBIDDEN = re.compile(r"(?<![0-9])(?:8332|8333|18332|18333|18443|18444|38332|38333)(?![0-9])")


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
        if any(relpath == scope or relpath.is_relative_to(scope) for scope in SCOPES):
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
        print("Public stale Bitcoin P2P port residue remains:")
        print("\n".join(failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
