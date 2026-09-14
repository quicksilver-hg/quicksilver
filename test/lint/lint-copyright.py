#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Check copyright attribution across non-vendored source, templates and docs.

The header-attribution checks delegate to contrib/devtools/copyright_header.py
(the single source of truth for which files must carry a source header). This
wrapper also rejects output copyrights that give Bitcoin Core's 2009 start year
to the Quicksilver holder.

That second check deliberately scans a wider set than the header check. The
header scope is keyed on source extensions, so it never saw doc/man/*.1 -- and
those generated man pages were exactly where the 2009 claim outlived the fixes
to the binaries (64e49e9c) and the Windows installer (6e882005). Any tracked
file can render a copyright, so every tracked file outside the vendored trees
is scanned here.
"""

import re
import runpy
import subprocess
import sys
from pathlib import Path

PRODUCT_ROOT = Path(__file__).resolve().parents[2]
TOOL = PRODUCT_ROOT / "contrib" / "devtools" / "copyright_header.py"
DEVTOOLS_README = PRODUCT_ROOT / "contrib" / "devtools" / "README.md"

# A copyright output may name both holders on one line, so the order matters:
# 2009 is valid when it begins the Bitcoin Core clause, but never when the next
# holder in that clause is Quicksilver (or a build-time token resolving to it).
QUICKSILVER_HOLDER_REFERENCE = (
    r"(?:The Quicksilver developers|"
    r"@?COPYRIGHT_HOLDERS(?:_FINAL|_SUBSTITUTION)?@?|"
    r"\bcopyright_devs\b)"
)
QUICKSILVER_2009_CLAUSE_RE = re.compile(
    rf"\b2009\b(?:(?![,;]).)*?{QUICKSILVER_HOLDER_REFERENCE}"
)

# CopyrightHolders() supplies the Quicksilver holder internally. Guard the old
# source form too, including a call split over multiple lines. Stop at the end
# of the statement so the function declaration/definition cannot absorb an
# unrelated 2009 later in the file.
QUICKSILVER_2009_CALL_RE = re.compile(
    r"\bCopyrightHolders\s*\((?:(?!;).){0,500}?\b2009\b",
    re.DOTALL,
)


def quicksilver_2009_lines(contents):
    """Return source lines that date the Quicksilver holder to 2009."""
    lines = set()
    for line_number, line in enumerate(contents.splitlines(), start=1):
        if QUICKSILVER_2009_CLAUSE_RE.search(line):
            lines.add(line_number)
    for match in QUICKSILVER_2009_CALL_RE.finditer(contents):
        lines.add(contents.count("\n", 0, match.start()) + 1)
    return sorted(lines)


def tracked_files_to_scan(tool_globals):
    """Every tracked file outside the vendored trees.

    Vendored trees are skipped because upstream's own 2009 notices legitimately
    live there. The exclusion list is taken from copyright_header.py so the two
    checks cannot disagree about what counts as vendored.
    """
    excluded_dirs = tuple(tool_globals["EXCLUDE_DIRS"])
    tracked = subprocess.run(
        ["git", "ls-files", "--full-name"],
        cwd=PRODUCT_ROOT,
        capture_output=True,
        text=True,
        check=True,
    ).stdout.split("\n")
    return [name for name in tracked if name and not name.startswith(excluded_dirs)]


def check_quicksilver_start_year():
    tool_globals = runpy.run_path(str(TOOL))
    failures = []
    this_file = Path(__file__).resolve()
    for name in tracked_files_to_scan(tool_globals):
        path = PRODUCT_ROOT / name
        if path.resolve() == this_file or not path.is_file():
            continue
        try:
            contents = path.read_text(encoding="utf8")
        except (UnicodeDecodeError, OSError):
            continue  # binary asset or unreadable; carries no copyright text
        for line_number in quicksilver_2009_lines(contents):
            failures.append(
                f"{name}:{line_number}: "
                "2009 must not be assigned to The Quicksilver developers"
            )

    regression_cases = [
        (
            'VIAddVersionKey LegalCopyright "Copyright (C) '
            '2009-@COPYRIGHT_YEAR@ @COPYRIGHT_HOLDERS_FINAL@"',
            [1],
        ),
        ("Copyright (C) 2009-2026 The Quicksilver developers", [1]),
        (
            "return CopyrightHolders(\n"
            '    strprintf("Copyright (C) %i-%i", 2009, COPYRIGHT_YEAR));',
            [1],
        ),
        (
            "Copyright (C) @COPYRIGHT_YEAR@ @COPYRIGHT_HOLDERS_FINAL@, "
            "2009-@COPYRIGHT_YEAR@ The Bitcoin Core developers",
            [],
        ),
        ("Copyright (c) 2009-present The Bitcoin Core developers", []),
        ("U+2009 THIN SPACE", []),
    ]
    for contents, expected in regression_cases:
        actual = quicksilver_2009_lines(contents)
        if actual != expected:
            failures.append(
                "lint-copyright.py matcher regression: "
                f"expected lines {expected}, got {actual} for {contents!r}"
            )

    if failures:
        for failure in failures:
            print(failure)
        return 1
    return 0


def check_insert_header_holder():
    tool_globals = runpy.run_path(str(TOOL))
    failures = []
    for template_name in ("CPP_HEADER", "SCRIPT_HEADER"):
        template = tool_globals[template_name]
        if "The Quicksilver developers" not in template:
            failures.append(
                f"{TOOL}: {template_name} must insert a Quicksilver copyright"
            )
        if "The Bitcoin Core developers" in template:
            failures.append(
                f"{TOOL}: {template_name} must not insert a Bitcoin Core copyright"
            )

    include = tool_globals["INCLUDE"]
    for ext in ("*.cu", "*.cuh"):
        if ext not in include:
            failures.append(
                f"{TOOL}: INCLUDE must cover first-party CUDA sources ({ext})"
            )

    insert_usage = tool_globals["INSERT_USAGE"]
    if (
        'Inserts a copyright header for "The Quicksilver developers"'
        not in insert_usage
    ):
        failures.append(
            f"{TOOL}: insert usage must document Quicksilver as the inserted holder"
        )

    readme = DEVTOOLS_README.read_text(encoding="utf8")
    if "Inserts a copyright header for `The Quicksilver developers`" not in readme:
        failures.append(
            f"{DEVTOOLS_README}: insert documentation must name Quicksilver as the inserted holder"
        )

    if failures:
        for failure in failures:
            print(failure)
        return 1
    return 0


def main():
    result = subprocess.run(
        [sys.executable, str(TOOL), "verify", str(PRODUCT_ROOT)], text=True
    )
    insert_result = check_insert_header_holder()
    start_year_result = check_quicksilver_start_year()
    sys.exit(result.returncode or insert_result or start_year_result)


if __name__ == "__main__":
    main()
