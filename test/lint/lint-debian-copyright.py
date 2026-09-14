#!/usr/bin/env python3
#
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
#
# Check that contrib/debian/copyright actually describes the tree it ships.
#
# The catch-all "Files: *" stanza declares everything to be MIT and held by the
# Bitcoin Core and Quicksilver developers. Anything third-party that no later
# stanza overrides is therefore silently misdeclared -- which is how the
# vendored Cuckoo Cycle sources came to be shipped as MIT with no trace of
# their FAIR MINING licence. A grep cannot see that: the misdeclaration is the
# *absence* of a stanza, and the offending code names no upstream project at
# all -- there is no residue string to search for.

import fnmatch
import re
import subprocess
import sys
from pathlib import Path

COPYRIGHT = Path("contrib/debian/copyright")

# Third-party paths that must never fall through to "Files: *". Each entry is a
# prefix; some stanza whose Files patterns are not the bare "*" must cover at
# least one file under it, and every file under it must be covered.
THIRD_PARTY_PREFIXES = [
    "src/crc32c/",
    "src/crypto/ctaes/",
    "src/crypto/cuckatoo/vendor/",
    "src/leveldb/",
    "src/secp256k1/",
    "src/univalue/",
]

# Individual third-party files embedded in otherwise first-party directories.
THIRD_PARTY_FILES = [
    "src/bench/nanobench.h",
    "src/cuckoocache.h",
    "src/test/fuzz/FuzzedDataProvider.h",
    "src/tinyformat.h",
    "src/util/subprocess.h",
]


def repo_root() -> Path:
    return Path(subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True, encoding="utf8").strip())


def tracked_files(root: Path) -> list[str]:
    out = subprocess.check_output(["git", "-C", str(root), "ls-files"], text=True, encoding="utf8")
    return out.splitlines()


def parse(text: str) -> tuple[list[dict], set[str]]:
    """Return (file stanzas, standalone licence names).

    A file stanza is {"patterns": [...], "license": name, "line": n}. A
    standalone licence paragraph is one with a License field and no Files
    field -- that is where the licence body lives.
    """
    stanzas = []
    standalone = set()
    paragraph: list[tuple[int, str]] = []

    def flush():
        if not paragraph:
            return
        patterns: list[str] = []
        license_name = None
        in_files = False
        for _, line in paragraph:
            if line.startswith("Files:"):
                in_files = True
                rest = line[len("Files:"):].strip()
                if rest:
                    patterns.append(rest)
                continue
            if line.startswith("License:"):
                in_files = False
                license_name = line[len("License:"):].strip()
                continue
            if line[:1].isspace():
                if in_files:
                    patterns.append(line.strip())
                continue
            in_files = False
        if patterns:
            stanzas.append({"patterns": patterns, "license": license_name, "line": paragraph[0][0]})
        elif license_name is not None:
            standalone.add(license_name)

    for number, line in enumerate(text.splitlines(), start=1):
        if not line.strip():
            flush()
            paragraph = []
            continue
        paragraph.append((number, line))
    flush()
    return stanzas, standalone


def main() -> int:
    root = repo_root()
    path = root / COPYRIGHT
    text = path.read_text(encoding="utf8")
    stanzas, standalone = parse(text)
    files = tracked_files(root)
    errors: list[str] = []

    # 1. Every Files pattern must match something. A pattern that matches
    #    nothing is a stanza describing files that were deleted or renamed --
    #    it looks like coverage and provides none.
    covered: dict[str, list[dict]] = {}
    for stanza in stanzas:
        for pattern in stanza["patterns"]:
            if pattern == "*":
                continue
            # The packaging stanza is written for the unpacked source tree,
            # where contrib/debian is the top-level debian/ directory.
            search = "contrib/" + pattern if pattern.startswith("debian/") else pattern
            regex = re.compile(fnmatch.translate(search))
            matched = [f for f in files if regex.match(f)]
            if not matched:
                errors.append(f"{COPYRIGHT}:{stanza['line']}: Files pattern matches no tracked file: {pattern}")
            for f in matched:
                covered.setdefault(f, []).append(stanza)

    # 2. Every stanza's licence must have a body, and every body must be used.
    #    An undefined licence makes the stanza meaningless to a reader or a
    #    packaging tool; an unused body is residue from a stanza that is gone.
    used = {s["license"] for s in stanzas if s["license"]}
    for stanza in stanzas:
        name = stanza["license"]
        if name is None:
            errors.append(f"{COPYRIGHT}:{stanza['line']}: Files stanza has no License field")
        elif name not in standalone:
            errors.append(f"{COPYRIGHT}:{stanza['line']}: License '{name}' has no licence text in this file")
    for name in sorted(standalone - used):
        errors.append(f"{COPYRIGHT}: licence text for '{name}' is defined but no Files stanza uses it")

    # 3. Third-party code must never be left to the catch-all.
    for prefix in THIRD_PARTY_PREFIXES:
        under = [f for f in files if f.startswith(prefix)]
        if not under:
            errors.append(f"{COPYRIGHT}: third-party prefix '{prefix}' matches no tracked file; update this linter")
            continue
        uncovered = [f for f in under if f not in covered]
        if uncovered:
            shown = ", ".join(uncovered[:3]) + (f", ... ({len(uncovered)} total)" if len(uncovered) > 3 else "")
            errors.append(
                f"{COPYRIGHT}: third-party files under '{prefix}' fall through to 'Files: *', "
                f"which declares them MIT and held by the Bitcoin Core and Quicksilver developers: {shown}")
    for name in THIRD_PARTY_FILES:
        if name not in files:
            errors.append(f"{COPYRIGHT}: third-party file '{name}' is not tracked; update this linter")
        elif name not in covered:
            errors.append(
                f"{COPYRIGHT}: third-party file '{name}' falls through to 'Files: *', "
                f"which declares it MIT and held by the Bitcoin Core and Quicksilver developers")

    # 4. Vendored trees that carry their own licence grant must ship it.
    for grant in ["src/crypto/cuckatoo/vendor/LICENSE.txt", "src/leveldb/LICENSE",
                  "src/crc32c/LICENSE", "src/secp256k1/COPYING"]:
        if grant not in files:
            errors.append(f"{grant}: vendored licence grant is missing from the tree")

    if errors:
        for error in errors:
            print(error)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
