#!/usr/bin/env python3
#
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
#
# Assert the tree contains no "mempool" spelling anywhere.
#
# Both halves of the relay pool rename have landed: Spec B renamed the internal
# C++ identifiers, filenames and docs, and Spec A renamed every externally
# observable name -- the wire command, the datadir file, the RPC methods, the
# command-line flags, the JSON fields, the REST paths and the net permission.
# The allowlist this file used to carry was the written contract between those
# two specs, and it is now empty.
#
# One exemption remains, and it is the last one: the release notes, which have
# to quote pre-rename names to be worth reading. Do not add a second. Reaching
# green by widening the exemption defeats the point of the gate.
#
# Usage: lint-quicksilver-relaypool-residue.py [--summary]

import re
import subprocess
import sys
from pathlib import Path

SKIP_PREFIXES = [
    Path("src/crc32c"),
    Path("src/crypto/ctaes"),
    Path("src/leveldb"),
    Path("src/secp256k1"),
    Path("src/univalue"),
    # Historical planning records for already-merged work. Two files here were
    # committed before docs/superpowers/{plans,specs}/ was gitignored; they
    # describe the tree as it was named when they were written, and rewriting
    # them would falsify the record rather than purge residue.
    Path("docs/superpowers"),
]

# Whole files excluded from scanning: a residue classifier necessarily embeds
# the very tokens it hunts for as literals.
SKIP_FILES = {
    Path("test/lint/lint-quicksilver-relaypool-residue.py"),
}

# The one exemption: the release notes.
#
# A migration note that documents a rename has to quote the pre-rename name --
# that is the whole service it performs for a reader upgrading. Rewriting those
# quotations would not purge residue, it would falsify the record and leave the
# reader unable to map their old configuration onto the new one.
#
# Scoped to this one file rather than to particular line shapes: the shapes
# change every time the note is edited, and a pattern list that has to be
# updated in lockstep with prose is a pattern list that gets widened until it
# means nothing.
RECORD_FILES = {
    "doc/release-notes.md",
}

# The USDT tracepoint provider used to be exempt here. It is not any more: the
# provider is `relaypool`, doc/tracing.md records why the rename was made before
# the first public release, and this gate is now the thing that keeps a
# TRACEPOINT(mempool, ...) from creeping back in. The bcc test that checks the
# emitted names, test/functional/interface_usdt_relaypool.py, needs root for BPF
# and skips without it -- so on an unprivileged run this linter is the only
# coverage that surface has.

HIT_RE = re.compile(r"[Mm]em[Pp]ool|MEMPOOL")


def repo_root() -> Path:
    out = subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True, encoding="utf8")
    return Path(out.strip())


def tracked_files() -> list[Path]:
    raw = subprocess.check_output(["git", "ls-files", "-z"], text=True, encoding="utf8").split("\0")
    files = []
    for name in raw:
        if not name:
            continue
        path = Path(name)
        if path in SKIP_FILES:
            continue
        if any(path == prefix or path.is_relative_to(prefix) for prefix in SKIP_PREFIXES):
            continue
        files.append(path)
    return files


def is_binary(path: Path) -> bool:
    try:
        return b"\0" in path.read_bytes()[:8192]
    except OSError:
        return False


def is_allowed(rel: str, line: str, match: "re.Match[str]") -> bool:
    del line, match  # The one remaining exemption is whole-file.
    return rel in RECORD_FILES


IDENT_CHAR = re.compile(r"[A-Za-z0-9_]")


def surrounding_identifier(line: str, match: "re.Match[str]") -> str:
    """Widen a hit to the whole identifier containing it.

    A bare "mempool" token is not diagnostic on its own; reporting m_mempool or
    raw_mempool says which rename rule the hit is waiting on.
    """
    start, end = match.start(), match.end()
    while start > 0 and IDENT_CHAR.match(line[start - 1]):
        start -= 1
    while end < len(line) and IDENT_CHAR.match(line[end]):
        end += 1
    return line[start:end]


def main() -> int:
    args = sys.argv[1:]
    summary = "--summary" in args
    if any(a != "--summary" for a in args):
        print(f"Usage: {sys.argv[0]} [--summary]", file=sys.stderr)
        return 2

    root = repo_root()
    residue: list[tuple[str, int, str, str]] = []
    for path in tracked_files():
        rel = path.as_posix()
        full = root / path
        if not full.exists() or is_binary(full):
            continue
        # Filenames count as residue too.
        if HIT_RE.search(rel):
            residue.append((rel, 0, "<filename>", rel))
        try:
            text = full.read_text(encoding="utf8", errors="replace")
        except OSError as err:
            print(f"failed to read {rel}: {err}", file=sys.stderr)
            return 1
        for lineno, line in enumerate(text.splitlines(), start=1):
            for match in HIT_RE.finditer(line):
                if not is_allowed(rel, line, match):
                    residue.append((rel, lineno, surrounding_identifier(line, match), line.strip()))
                    break

    if summary:
        counts: dict[str, int] = {}
        for rel, _, _, _ in residue:
            top = "/".join(rel.split("/")[:2]) if "/" in rel else rel
            counts[top] = counts.get(top, 0) + 1
        for top, count in sorted(counts.items(), key=lambda kv: -kv[1]):
            print(f"{count:6d}  {top}")
    else:
        for rel, lineno, token, line in residue:
            print(f"{rel}:{lineno}: [{token}] {line}")

    print(f"\nrelaypool residue: {len(residue)} line(s)", file=sys.stderr)
    return 1 if residue else 0


if __name__ == "__main__":
    sys.exit(main())
