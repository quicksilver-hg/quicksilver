#!/usr/bin/env python3
#
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
#
# Guard the agent log register.
#
# Three mechanically decidable rules from doc/design/logging.md:
#
#   1. LogPrintf does not reappear. The category is mandatory, and deleting the
#      uncategorized macro is what makes an un-migrated site a compile error.
#   2. No log message contains the literal "Quicksilver". Every line in
#      quicksilver.log is a Quicksilver line by construction, so the word
#      carries no information and taxes the primary consumer.
#   3. No log message embeds a known reject token in prose instead of routing
#      it through reason=.
#
# The verb vocabulary is deliberately NOT linted. A closed verb set enforced by
# regex produces false positives on legitimate lines and trains authors to word
# things to satisfy the linter. It is a convention, not a gate.
#
# Rules 2 and 3 inspect whole log calls, not single lines: a format string
# wrapped across several source lines is one message.
#
# Run `--self-test` to exercise the matchers without touching the tree.

import re
import subprocess
import sys
from pathlib import Path

# There is deliberately no scope allowlist here. A list of directories to scan
# is half of this lint's assertion, and it is the half that fails silently: a
# green run over the wrong file set cannot be told from a green run over a
# clean tree. Every tracked file is scanned instead, minus the vendored trees
# below and the files that must quote the forbidden tokens as literals.

SKIP_PREFIXES = [
    Path("src/crc32c"),
    Path("src/crypto/ctaes"),
    Path("src/leveldb"),
    Path("src/secp256k1"),
    Path("src/test"),
    Path("src/univalue"),
]

# The linter necessarily embeds the very tokens it hunts for as literals.
SKIP_FILES = {
    Path("test/lint/lint-quicksilver-log-register.py"),
}

# LogPrintf is retired. The negative lookbehind keeps VaultLogPrintf -- the
# per-vault wrapper that routes through LogInfo(HgLog::VAULT, ...) and appends
# vault=<name> -- out of the rule.
LOG_PRINTF = re.compile(r"(?<![A-Za-z0-9_])LogPrintf\b")

# Start of a log call. VaultLogPrintf is included: it emits log lines, so rules
# 2 and 3 apply to its messages too.
LOG_CALL_START = re.compile(
    r"(?<![A-Za-z0-9_])(?:Log(?:Info|Warning|Error|Debug|Trace)|VaultLogPrintf)\s*\("
)

REJECT_TOKENS = [
    "bad-txns-pow-anchor",
    "tx-pow-invalid",
    "no-relaypool",
    "too-long-relaypool-chain",
    "txn-already-in-relaypool",
    "txn-same-nonwitness-data-in-relaypool",
    "package-relaypool-limits",
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
        if relpath.suffix not in (".cpp", ".h"):
            continue
        if relpath in SKIP_FILES:
            continue
        if any(relpath == prefix or relpath.is_relative_to(prefix) for prefix in SKIP_PREFIXES):
            continue
        paths.append(root / relpath)
    return paths


def blank_literals(text: str) -> str:
    """Replace the contents of string and char literals with spaces.

    Parenthesis counting has to ignore parens that live inside a message, and
    "// comment)" tails likewise. Length is preserved so offsets still line up.
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if ch in "\"'":
            quote = ch
            out.append(ch)
            i += 1
            while i < n:
                if text[i] == "\\" and i + 1 < n:
                    out.append("  ")
                    i += 2
                    continue
                if text[i] == quote:
                    out.append(quote)
                    i += 1
                    break
                out.append(" ")
                i += 1
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            out.append(" " * (n - i))
            break
        out.append(ch)
        i += 1
    return "".join(out)


def log_calls(lines: list[str]):
    """Yield (start_lineno, call_text) for every log call in a source file.

    A call runs from its opening parenthesis to the parenthesis that closes it,
    which may be several lines later.
    """
    for index, line in enumerate(lines):
        masked = blank_literals(line)
        for match in LOG_CALL_START.finditer(masked):
            open_pos = match.end() - 1
            depth = 0
            pieces = []
            cursor = index
            pos = open_pos
            while cursor < len(lines):
                current = lines[cursor]
                current_masked = blank_literals(current) if cursor != index else masked
                closed = False
                for offset in range(pos, len(current)):
                    if current_masked[offset] == "(":
                        depth += 1
                    elif current_masked[offset] == ")":
                        depth -= 1
                        if depth == 0:
                            pieces.append(current[pos:offset + 1])
                            closed = True
                            break
                if closed:
                    break
                pieces.append(current[pos:])
                cursor += 1
                pos = 0
            yield index + 1, " ".join(pieces)


def call_failures(relpath: Path, lineno: int, call: str) -> list[str]:
    failures = []
    if "Quicksilver" in call:
        failures.append(
            f'{relpath}:{lineno}: log message contains the literal "Quicksilver"; '
            f"the [tag:level] prefix already carries it"
        )
    for token in REJECT_TOKENS:
        if token in call and f"reason={token}" not in call:
            failures.append(
                f"{relpath}:{lineno}: reject token {token!r} embedded in prose; "
                f"route it through reason={token}"
            )
    return failures


def scan(relpath: Path, lines: list[str]) -> list[str]:
    failures = []
    for lineno, line in enumerate(lines, start=1):
        if LOG_PRINTF.search(line):
            failures.append(
                f"{relpath}:{lineno}: LogPrintf is retired; use LogInfo(<category>, ...)"
            )
    for lineno, call in log_calls(lines):
        failures.extend(call_failures(relpath, lineno, call))
    return failures


def self_test() -> int:
    # (source lines, expected number of failures, substring the report must contain)
    cases = [
        (["LogPrintf(\"hi\\n\");"], 1, "LogPrintf is retired"),
        (["// LogPrintf, maybe indirectly"], 1, "LogPrintf is retired"),
        (["    vault.VaultLogPrintf(\"loaded\\n\");"], 0, ""),
        (["LogInfo(HgLog::INIT, \"Quicksilver version %s\\n\", v);"], 1, '"Quicksilver"'),
        (["LogInfo(HgLog::INIT, \"start version=%s\\n\", v);"], 0, ""),
        (["vault.VaultLogPrintf(\"Releasing Quicksilver vault\\n\");"], 1, '"Quicksilver"'),
        # Multi-line call: the offending literal is not on the call's first line.
        (
            [
                "LogInfo(HgLog::LEDGER,",
                '        "Quicksilver ledger ready\\n");',
            ],
            1,
            '"Quicksilver"',
        ),
        # Reject token in prose vs routed through reason=.
        (
            ['LogInfo(HgLog::LEDGER, "rejected tx-pow-invalid\\n");'],
            1,
            "embedded in prose",
        ),
        (
            ['LogInfo(HgLog::LEDGER, "reject reason=tx-pow-invalid\\n");'],
            0,
            "",
        ),
        # A paren inside the message must not truncate the call.
        (
            ['LogInfo(HgLog::MESH, "connect (Quicksilver) failed\\n");'],
            1,
            '"Quicksilver"',
        ),
        # Not a log call at all.
        (['const char* s = "Quicksilver";'], 0, ""),
    ]
    failures = []
    for lines, expected_count, expected_substr in cases:
        got = scan(Path("src/example.cpp"), lines)
        if len(got) != expected_count:
            failures.append(f"{lines!r}: expected {expected_count} failure(s), got {got!r}")
        elif expected_substr and expected_substr not in got[0]:
            failures.append(f"{lines!r}: expected report containing {expected_substr!r}, got {got[0]!r}")
    if failures:
        print("Log-register lint self-test failures:")
        print("\n".join(failures))
        return 1
    print("Log-register lint self-test OK")
    return 0


def main() -> int:
    if sys.argv[1:] == ["--self-test"]:
        return self_test()
    if len(sys.argv) != 1:
        print(f"Usage: {sys.argv[0]} [--self-test]", file=sys.stderr)
        return 2

    root = repo_root()
    failures = []
    for path in tracked_files(root):
        if not path.exists():
            continue
        relpath = path.relative_to(root)
        try:
            lines = path.read_text(encoding="utf8", errors="replace").splitlines()
        except OSError as err:
            print(f"failed to read {relpath}: {err}", file=sys.stderr)
            return 1
        failures.extend(scan(relpath, lines))

    if failures:
        print("Agent log register violations:")
        print("\n".join(failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
