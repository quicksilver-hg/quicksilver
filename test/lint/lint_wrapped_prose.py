#!/usr/bin/env python3
#
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
#
# Shared matcher for banned phrases that wrap across a line break.
#
# A linter that reads splitlines() and matches each rule against one line at a
# time cannot see a phrase that straddles a newline. The scope of a lint is one
# half of its assertion; the shape of its input is the other, and this half
# fails just as silently: a green run over line-shredded prose is
# indistinguishable from a green run over a clean tree.
#
# Not hypothetical. doc/design/desktop-application.md carried the literal phrase
# "agent wallets" as
#
#     across launch, consensus review, network context, mining, backup, send, and agent
#     wallets.
#
# while a rule banning `agent wallets?` had been in force for weeks. It was
# found by hand during the allotment rename, never by the linter, and the same
# hole was open under every other multi-word rule in the tree.
#
# The fix is to run each rule a second time over the file joined into one
# whitespace-normalised string, and report only matches that actually cross a
# line boundary -- everything else is already visible to the per-line pass.
#
# Two properties callers depend on:
#
#   * Blank lines are a hard break. A wrapped phrase never spans a paragraph,
#     so joining across one would only manufacture findings.
#   * Exemptions stay per-line. Allowlist patterns are matched against real
#     lines, never against the join; feeding an allowlist normalised text lets
#     it match more than it was written to match, which weakens the lint
#     instead of strengthening it. A wrapped finding is suppressed when any
#     line it spans is exempt.
#
# Run `--self-test` to exercise the matcher.

import re
import sys
from bisect import bisect_right
from dataclasses import dataclass

# Separates the two sides of a blank line. A newline is the one character that
# neither a literal space nor an unanchored `.` can match, so the two gap forms
# rules actually use -- " " between words and a bounded `.{0,n}` -- cannot leap
# a paragraph. (`\s` still matches it. A rule whose only gap is `\s` and whose
# halves sit either side of a blank line is not a shape that occurs here.) No
# caller compiles with re.DOTALL; one that did would lose this property.
PARAGRAPH_BREAK = "\n"

# Comment and quote markers that introduce a continuation of the preceding
# line rather than new text. Each must be followed by whitespace or end of
# line, so `#include <foo>` keeps its `#` and `*ptr = x;` keeps its `*`.
LEADER_RE = re.compile(r"^[ \t]*(?://[/!<]?|\*/?|/\*|#+|>+|;;+|--|%%?|rem)(?=[ \t]|$)[ \t]*", re.IGNORECASE)


@dataclass(frozen=True)
class WrappedMatch:
    """A rule match that begins on one line and ends on a later one."""

    first_line: int
    last_line: int
    text: str

    def where(self) -> str:
        return f"wrapped across lines {self.first_line}-{self.last_line}"


def strip_leader(line: str) -> str:
    """Drop indentation and any continuation marker from one line."""
    return LEADER_RE.sub("", line).strip()


def join_lines(lines: list[str]) -> "tuple[str, list[tuple[int, int]]]":
    """Join `lines` into one whitespace-normalised string.

    Returns the joined text and, for each line that contributed to it, the
    (character offset, 1-based line number) at which it starts. Consecutive
    lines are joined by a single space; a blank line becomes PARAGRAPH_BREAK.
    """
    parts: list[str] = []
    starts: list[tuple[int, int]] = []
    offset = 0
    pending_break = False
    for number, raw in enumerate(lines, start=1):
        stripped = strip_leader(raw)
        if not stripped:
            pending_break = True
            continue
        if parts:
            separator = PARAGRAPH_BREAK if pending_break else " "
            parts.append(separator)
            offset += len(separator)
        pending_break = False
        starts.append((offset, number))
        parts.append(stripped)
        offset += len(stripped)
    return "".join(parts), starts


def _line_number_at(offsets: list[int], numbers: list[int], position: int) -> int:
    return numbers[bisect_right(offsets, position) - 1]


# A match that crosses a line boundary has to consume the separator the join
# put there -- a space, or a newline at a paragraph break. A pattern that
# cannot match either of those characters cannot produce one, so there is no
# point running it over the joined text at all, and most rules are of that
# kind.
#
# This is deliberately over-inclusive: it asks whether the pattern *source*
# contains any construct that could conceivably match a separator, not whether
# it does. `\.` and `\[` are counted even though they match neither. Erring
# this way costs a little time; erring the other way would silently reopen the
# hole for whichever rule the test got wrong, which is the whole defect this
# module exists to close.
CAN_SPAN_RE = re.compile(r"[ .\[]|\\s|\\W|\\D|\\n")


def can_span(pattern: "re.Pattern[str]") -> bool:
    """Whether `pattern` could match text containing a line separator."""
    return CAN_SPAN_RE.search(pattern.pattern) is not None


class WrappedText:
    """One file's lines, joined once, ready to be matched against many rules.

    Built per file rather than per rule: the join is the expensive half, and a
    linter with a hundred rules would otherwise redo it a hundred times.
    """

    def __init__(self, lines: list[str]) -> None:
        self._text, starts = join_lines(lines)
        self._offsets = [offset for offset, _ in starts]
        self._numbers = [number for _, number in starts]

    def matches(self, pattern: "re.Pattern[str]") -> list[WrappedMatch]:
        """Every match of `pattern` on the joined text that crosses a boundary.

        Matches confined to a single line are omitted: the caller's per-line
        pass already reports those, and reporting them twice would only add
        noise.
        """
        if not self._offsets or not can_span(pattern):
            return []
        found = []
        for match in pattern.finditer(self._text):
            if match.start() == match.end():
                continue
            first = _line_number_at(self._offsets, self._numbers, match.start())
            last = _line_number_at(self._offsets, self._numbers, match.end() - 1)
            if first == last:
                continue
            found.append(WrappedMatch(first, last, match.group(0)))
        return found


def wrapped_matches(lines: list[str], pattern: "re.Pattern[str]") -> list[WrappedMatch]:
    """Convenience wrapper for a single pattern against a single file."""
    return WrappedText(lines).matches(pattern)


def self_test() -> list[str]:
    """Return a list of failure descriptions; empty means the matcher is sound."""
    failures = []

    def check(name: str, got, expected) -> None:
        if got != expected:
            failures.append(f"{name}: expected {expected!r}, got {got!r}")

    agent_wallets = re.compile(r"agent wallets?", re.IGNORECASE)

    # The defect this module exists for, verbatim from
    # doc/design/desktop-application.md as it stood at 17bc3ed1^.
    desktop = [
        "The desktop shell should feel like one application, not inherited Qt screens with",
        "new labels. Shared styling belongs at the shell level: page titles, card borders,",
        "status chips, warnings, primary actions, and data panels should read consistently",
        "across launch, consensus review, network context, mining, backup, send, and agent",
        "wallets.",
    ]
    got = wrapped_matches(desktop, agent_wallets)
    check("desktop-application wrap", [(m.first_line, m.last_line, m.text.lower()) for m in got],
          [(4, 5, "agent wallets")])

    # A match already on one line belongs to the per-line pass, not to this one.
    check("same-line match is not re-reported",
          wrapped_matches(["fund the agent wallets today"], agent_wallets), [])

    # A phrase may wrap across three lines when the middle word stands alone.
    three = re.compile(r"sqlite required for descriptor vault", re.IGNORECASE)
    check("three-line wrap",
          [(m.first_line, m.last_line) for m in wrapped_matches(
              ["... sqlite required", "for descriptor", "vault copy ..."], three)],
          [(1, 3)])

    # A blank line is a hard break: nothing may join across a paragraph.
    check("paragraph break blocks a literal-space join",
          wrapped_matches(["... and agent", "", "wallets are gone"], agent_wallets), [])
    check("paragraph break blocks a wildcard join",
          wrapped_matches(["(Deprecated) one", "", "two payment URI"],
                          re.compile(r"\(Deprecated\).{0,120}payment URI")), [])

    # Continuation markers are stripped so wrapped comments and quotes match.
    for name, wrapped in (
        ("c++ comment", ["    // fund the agent", "    // wallets here"]),
        ("doxygen comment", ["//! the agent", "//! wallets"]),
        ("block comment", [" * the agent", " * wallets"]),
        ("hash comment", ["# the agent", "# wallets"]),
        ("markdown quote", ["> the agent", "> wallets"]),
    ):
        check(f"{name} wrap", len(wrapped_matches(wrapped, agent_wallets)), 1)

    # ... but a marker that is not a continuation marker keeps its text intact.
    check("#include is not a comment leader",
          strip_leader("#include <agent/wallets.h>"), "#include <agent/wallets.h>")
    check("pointer deref is not a block comment",
          strip_leader("    *agent = wallets;"), "*agent = wallets;")

    # Line numbers survive blank lines and stripped leaders.
    check("line numbers are file line numbers",
          [(m.first_line, m.last_line) for m in wrapped_matches(
              ["intro", "", "", "   // trailing agent", "   // wallets"], agent_wallets)],
          [(4, 5)])

    # An unbounded wildcard spans the whole file once the newlines are gone, so
    # rules that reach this matcher must bound their gaps. Both halves of this
    # check matter: the bounded form must still match a real wrap.
    haystack = ["(Deprecated) one", "two", "three", "four", "five", "six", "seven",
                "eight nine ten eleven twelve thirteen fourteen payment URI"]
    check("unbounded wildcard reaches across the file",
          len(wrapped_matches(haystack, re.compile(r"\(Deprecated\).*payment URI"))), 1)
    check("bounded wildcard stops short",
          len(wrapped_matches(haystack, re.compile(r"\(Deprecated\).{0,40}payment URI"))), 0)
    check("bounded wildcard still catches a real wrap",
          len(wrapped_matches(["... (Deprecated) used to process BIP21 payment",
                               "URI requests."],
                              re.compile(r"\(Deprecated\).{0,120}payment URI", re.IGNORECASE))), 1)

    # The separator pre-filter must never turn away a rule that could span.
    for source in (r"agent wallets?", r"GetFee\s*\(", r"\(Deprecated\).{0,120}payment URI",
                   r'["\']fees?["\']\s*:', r"\bBIP ?\d+\b", r"[a-z ]+wallet"):
        check(f"can_span({source!r})", can_span(re.compile(source)), True)
    for source in (r"\blegacy_vault\b", r"quicksilverstrings", r"CONNECTIONS_NONE",
                   r"\bOP_EVAL\b", r"minisketch"):
        check(f"can_span({source!r})", can_span(re.compile(source)), False)

    # A pattern the pre-filter turns away must really be unable to span, so
    # spot-check the two forms against the matcher itself.
    check("filtered-out pattern finds nothing anyway",
          wrapped_matches(["... legacy", "_vault ..."], re.compile(r"\blegacy_vault\b")), [])

    return failures


def report_self_test() -> int:
    """Run the self-test, print any failures, and return a process exit code."""
    failures = self_test()
    if failures:
        print("Wrapped-prose matcher self-test failures:")
        print("\n".join(failures))
        return 1
    return 0


if __name__ == "__main__":
    if sys.argv[1:] not in ([], ["--self-test"]):
        print(f"Usage: {sys.argv[0]} [--self-test]", file=sys.stderr)
        sys.exit(2)
    if report_self_test() != 0:
        sys.exit(1)
    print("Wrapped-prose matcher self-test OK")
