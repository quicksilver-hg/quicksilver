#!/usr/bin/env python3
#
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/licenses/mit/.
#
# F-148: -prove help must not claim proving fails on a header store.
#
# F-51 put nCongestion in the header. Proving reads that field from the
# ordinary header store. The operator-facing -prove help (and the desktop
# design doc) still described the pre-F-51 refusal, so an operator who
# believed the help would never prove.

import subprocess
import sys
from pathlib import Path


# Phrases that describe the discharged F-51 refusal. Matching any of them in
# src/ or doc/ is the defect. The lint file itself is skipped because these
# strings have to appear here as the self-test fixtures.
STALE_PHRASES = (
    "ordinary-header store does not carry",
    "ordinary thin header store does not yet carry",
    "-prove=1 fails",
    "default proving mode fails",
    "until that state has a thin-client transport",
    "until a thin-client state transport",
)

AGENT_SOURCE = Path("src/quicksilver-agent.cpp")

# Positive pins on the -prove AddArg help. The old sentence contains
# "congestion" and "header store" and still lies; these snippets do not.
REQUIRED_AGENT_SNIPPETS = (
    "congestion multiplier from the header",
    "works at any synced header",
)

SKIP_PATHS = {
    Path("test/lint/lint-quicksilver-agent-prove-help.py"),
}

SCAN_PREFIXES = ("src/", "doc/")


def repo_root() -> Path:
    return Path(
        subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"], text=True, encoding="utf8"
        ).strip()
    )


def tracked_scan_files(root: Path) -> list[Path]:
    raw = subprocess.check_output(
        ["git", "ls-files", "-z", "--", "src", "doc"], text=True, encoding="utf8"
    ).split("\0")
    paths = []
    for name in raw:
        if not name:
            continue
        relpath = Path(name)
        if relpath in SKIP_PATHS:
            continue
        if not name.startswith(SCAN_PREFIXES):
            continue
        paths.append(relpath)
    return paths


def contains_stale(text: str) -> list[str]:
    return [phrase for phrase in STALE_PHRASES if phrase in text]


def check_tree(root: Path) -> list[str]:
    failures = []
    agent_text = (root / AGENT_SOURCE).read_text(encoding="utf8", errors="replace")
    for snippet in REQUIRED_AGENT_SNIPPETS:
        if snippet not in agent_text:
            failures.append(f"{AGENT_SOURCE}: -prove help missing required snippet: {snippet}")

    for relpath in tracked_scan_files(root):
        path = root / relpath
        try:
            text = path.read_text(encoding="utf8", errors="replace")
        except OSError:
            continue
        for phrase in contains_stale(text):
            failures.append(f"{relpath}: stale -prove claim remains: {phrase}")
    return failures


def self_test() -> list[str]:
    """The old sentence must fail; the replacement must pass."""
    failures = []
    old = (
        "Create per-tx proof-of-work for signbundle. Proving requires authenticated "
        "anchor congestion state, which the current ordinary-header store does not "
        "carry; until that state has a thin-client transport, -prove=1 fails instead "
        "of emitting an under-proved transaction. Set -prove=0 for offline tests only; "
        "the transaction will not relay. (default: 1)"
    )
    new = (
        "Create per-tx proof-of-work for signbundle. Proving reads the anchor's "
        "congestion multiplier from the header store, so -prove=1 works at any "
        "synced header, not only at genesis. Set -prove=0 for offline tests only; "
        "the transaction will not relay. (default: 1)"
    )
    old_hits = contains_stale(old)
    if not old_hits:
        failures.append("self-test: the pre-F-51 -prove sentence must match STALE_PHRASES")
    new_hits = contains_stale(new)
    if new_hits:
        failures.append(f"self-test: replacement -prove help still matches {new_hits}")
    for snippet in REQUIRED_AGENT_SNIPPETS:
        if snippet not in new:
            failures.append(f"self-test: replacement help missing required snippet: {snippet}")
        if snippet in old:
            failures.append(
                f"self-test: required snippet {snippet!r} already appears in the stale sentence, "
                "so it cannot distinguish the two"
            )
    return failures


def main() -> int:
    failures = self_test()
    failures.extend(check_tree(repo_root()))
    if failures:
        print("Quicksilver -prove help residue remains:")
        print("\n".join(failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
