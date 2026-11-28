#!/usr/bin/env python3
#
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
#
# Check that the Qt resource file and the GUI sources agree:
#   (a) every <file> in src/qt/quicksilver.qrc exists;
#   (b) every literal :/icons/NAME or :/fonts/NAME in src/qt (.cpp, .h, .ui)
#       names an alias in the qrc;
#   (c) every alias is named by at least one literal, or by a computed name
#       listed in COMPUTED_NAMES below.

import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


QRC = Path("src/qt/quicksilver.qrc")
SOURCE_SUFFIXES = (".cpp", ".h", ".ui")
REFERENCE_RE = re.compile(r":/(icons|fonts)/([A-Za-z0-9_%.-]+)")

# A name built at run time. Each entry is the literal as written in the source,
# and the regex of the aliases it can produce.
COMPUTED_NAMES = {
    # transactiontablemodel.cpp txStatusDecoration: immature coinbase progress,
    # QString(":/icons/transaction_%1").arg(part) with part in 1..5.
    "/icons/transaction_%1": re.compile(r"/icons/transaction_[1-5]"),
}

# A literal that names a resource which must NOT exist.
DELIBERATELY_MISSING = {
    # unitstests.cpp: LoadFont/LoadUnitGlyph must fail on a missing resource.
    "/fonts/no-such-font",
}


def repo_root() -> Path:
    return Path(subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True, encoding="utf8").strip())


def qrc_entries(root: Path) -> list[tuple[str, str]]:
    """(resource path, file path relative to the qrc) for every <file>."""
    entries = []
    tree = ET.parse(root / QRC)
    for qresource in tree.getroot().iter("qresource"):
        prefix = qresource.get("prefix", "/").rstrip("/")
        for file in qresource.iter("file"):
            name = file.get("alias") or file.text.strip()
            entries.append((f"{prefix}/{name}", file.text.strip()))
    return entries


def source_references(root: Path) -> dict[str, list[str]]:
    """Resource path -> list of "file:line" naming it."""
    files = subprocess.check_output(["git", "ls-files", "--", "src/qt"], cwd=root, text=True, encoding="utf8").splitlines()
    refs: dict[str, list[str]] = {}
    for rel in files:
        if not rel.endswith(SOURCE_SUFFIXES):
            continue
        text = (root / rel).read_text(encoding="utf8")
        for lineno, line in enumerate(text.splitlines(), 1):
            for match in REFERENCE_RE.finditer(line):
                refs.setdefault(f"/{match.group(1)}/{match.group(2)}", []).append(f"{rel}:{lineno}")
    return refs


def main() -> int:
    root = repo_root()
    errors = []
    entries = qrc_entries(root)
    aliases = {resource for resource, _ in entries}

    for resource, file in entries:
        if not (root / QRC.parent / file).is_file():
            errors.append(f"{QRC}: {resource} points at {file}, which does not exist")

    refs = source_references(root)
    for resource, where in sorted(refs.items()):
        if resource in aliases or resource in COMPUTED_NAMES or resource in DELIBERATELY_MISSING:
            continue
        errors.append(f"{where[0]}: :{resource} has no alias in {QRC}")

    for resource in DELIBERATELY_MISSING:
        if resource in aliases:
            errors.append(f"{QRC}: {resource} must not exist (a test relies on it missing)")

    for resource in sorted(aliases):
        if resource in refs:
            continue
        if any(literal in refs and pattern.fullmatch(resource) for literal, pattern in COMPUTED_NAMES.items()):
            continue
        errors.append(f"{QRC}: alias :{resource} is not named by any source in src/qt")

    for literal in COMPUTED_NAMES:
        if literal not in refs:
            errors.append(f"lint-qt-resources.py: computed name :{literal} is no longer in src/qt; drop it from COMPUTED_NAMES")

    for error in errors:
        print(error)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
