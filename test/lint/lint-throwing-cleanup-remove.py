#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Ban unqualified `remove(...)` calls in first-party C++.

`fs` is `std::filesystem` (src/util/fs.h does `using namespace std::filesystem`),
so an unqualified `remove(some_path)` resolves by ADL to
`std::filesystem::remove` -- the THROWING overload. It does not resolve to C's
`::remove(const char*)`, which is what the spelling suggests to a reader and
which returns an int and cannot throw.

That mismatch killed a node. `SerializeFileDB` in src/addrdb.cpp cleaned up its
temp file with a bare `remove(pathTmp)` on four failure paths. On Windows the
temp file is routinely still held by another process (Defender, the indexer)
immediately after `fclose()`, so the cleanup itself fails, the throwing overload
raises `std::filesystem_error` out of the SCHEDULER thread, nothing catches it,
and the process dies:

    EXCEPTION: class std::filesystem::filesystem_error
    remove: The process cannot access the file because it is being used by
    another process.: "...\\publictest\\peers.14c2"

POSIX unlinks a file other handles still hold, so Linux never saw it. The code
reads as correct on both platforms; only the platform difference exposes it.

A best-effort cleanup must not be able to throw. Use the `std::error_code`
overload -- `fs::remove(p, ec)` -- as src/init.cpp and src/node/blockstorage.cpp
already do.

The rule is deliberately blunt: NO unqualified `remove(` call, whatever its
argument. A regex cannot tell a path argument from an iterator pair, and the
bare spelling is the whole defect -- `std::remove` (the algorithm) and
`fs::remove` (the filesystem call) are each unambiguous when written out, so
requiring the qualification costs nothing and removes the trap.

Declarations and definitions of a member function named `remove` are not calls
and are not matched.
"""
import re
import subprocess
import sys

from lint_ignore_dirs import SHARED_EXCLUDED_SUBTREES

# A call, not a declaration: `remove(` preceded by start-of-line whitespace or
# by punctuation that can precede an expression. A declaration such as
# `void remove(...)` or `virtual void remove(...)` has an identifier
# immediately before, so it is not matched.
# The `:` of a `::` qualifier is deliberately NOT in the class below: it
# would make every `fs::remove` / `std::remove` match, which is the
# spelling this lint is asking for.
CALL_RE = re.compile(r"(?:^\s*|[;{}(\[,=!&|?]\s*)remove\s*\(")


def tracked_cpp_sources() -> list[str]:
    out = subprocess.run(
        ["git", "ls-files", "--", "src/*.cpp", "src/*.h", "src/**/*.cpp", "src/**/*.h"],
        check=True,
        stdout=subprocess.PIPE,
        text=True,
    ).stdout.split()
    return [f for f in out if not any(f.startswith(d) for d in SHARED_EXCLUDED_SUBTREES)]


def strip_line_comment(line: str) -> str:
    """Drop a trailing // comment. Good enough: no // appears inside a string
    literal in the files this lint covers."""
    return line.split("//", 1)[0]


def main() -> int:
    offenders = []
    for path in tracked_cpp_sources():
        with open(path, "r", encoding="utf-8") as f:
            for lineno, line in enumerate(f, start=1):
                if CALL_RE.search(strip_line_comment(line)):
                    offenders.append((path, lineno, line.rstrip()))

    if offenders:
        print("Unqualified remove(...) call found. This resolves by ADL to the")
        print("THROWING std::filesystem::remove when given a path, not to C's")
        print("::remove. Write fs::remove(p, ec) for a best-effort cleanup, or")
        print("std::remove(first, last, v) for the algorithm.\n")
        for path, lineno, text in offenders:
            print(f"{path}:{lineno}: {text.strip()}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
