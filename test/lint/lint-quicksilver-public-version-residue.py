#!/usr/bin/env python3
#
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
#
# Check that public docs do not describe Quicksilver behavior through inherited
# upstream version milestones.

import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Rule:
    name: str
    pattern: re.Pattern[str]


# There is deliberately no scope allowlist here. A list of directories to scan
# is half of this lint's assertion, and it is the half that fails silently: a
# green run over the wrong file set cannot be told from a green run over a
# clean tree. Every tracked file is scanned instead, minus the vendored trees
# below and the files that must quote the forbidden tokens as literals.

SKIP_PATHS = {
    Path("test/lint/lint-quicksilver-public-version-residue.py"),
}

SKIP_PREFIXES = [
    Path("doc/release-notes"),
]

RULES = [
    Rule(
        "upstream version era",
        re.compile(r"\bupstream\s+0\.\d+(?:\.\d+)?(?:-[A-Za-z0-9]+)?(?:-era)?\b", re.IGNORECASE),
    ),
    Rule(
        "inherited version milestone",
        re.compile(r"\binherited\b.*\bversion\s+0\.\d+(?:\.\d+)?\b", re.IGNORECASE),
    ),
    Rule(
        "current behavior tied to old version",
        re.compile(r"\b(?:Since|before|created before)\s+version\s+0\.\d+(?:\.\d+)?\b", re.IGNORECASE),
    ),
    Rule(
        "old era milestone",
        re.compile(r"\b0\.\d+(?:\.\d+)?-era\b", re.IGNORECASE),
    ),
    Rule(
        "inherited Core 22/23 version number",
        re.compile(r"\bversion\s+2[0-9]\.\d+(?:\.x)?\b", re.IGNORECASE),
    ),
    Rule(
        "inherited Core 0.21+ CLI gate",
        re.compile(r"\bv0\.2\d+\.\d+\b"),
    ),
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
        if any(relpath == prefix or relpath.is_relative_to(prefix) for prefix in SKIP_PREFIXES):
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
            for rule in RULES:
                if rule.pattern.search(line):
                    failures.append(f"{relpath}:{line_number}: {rule.name}: {line.strip()}")
                    break

    if failures:
        print("Public upstream-version residue remains:")
        print("\n".join(failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
