#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Fail Boost cases that skip by logging a message and returning.

`BOOST_TEST_MESSAGE` + `return` is a PASS. The case ran, produced no
assertion failure, and ctest counts the suite green. Nothing in the report
says the body did not run. That is how `gpu_parity_tests` reported 145/145
on machines with no GPU and no `CUCKATOO_GPU_SOLVER`.

The same hole with a different spelling is `getenv` + `return`: an env
gate that exits the case without a skip. Do not fire on `getenv` used to
configure a case that then actually runs (args merge, mining_service).

A visible skip is `BOOST_TEST_SKIP` (not in Boost 1.83, which this tree
ships) or `boost::unit_test::precondition`, which marks the case skipped
and prints `is skipped because`. Either one on the case is enough to
excuse a getenv gate. Neither excuses `BOOST_TEST_MESSAGE` + `return`,
because that shape is still a silent pass if the case body runs.

`BOOST_TEST_MESSAGE` used as a log line that continues is not this defect
(net peer tests). Mid-case early exits after assertions have already run
are a different shape; list them below with a reason rather than
converting them into a skip of work that already happened.
"""
import re
import subprocess
import sys

TEST_DIRS = ("src/test/", "src/qt/test/", "src/vault/test/")
EXCLUDED = ("src/test/fuzz/",)

CASE = re.compile(
    r"^\s*BOOST_(?:AUTO|FIXTURE)_TEST_CASE\s*\(\s*([A-Za-z0-9_]+)(.*)$"
)
CASE_END = re.compile(r"^\}")
GETENV = re.compile(r"\b(?:std::)?getenv\s*\(")
MESSAGE = re.compile(r"\bBOOST_TEST_MESSAGE\s*\(")
RETURN = re.compile(r"\breturn\s*;")
HAS_SKIP_MACRO = re.compile(r"\bBOOST_TEST_SKIP\s*\(")
HAS_PRECONDITION = re.compile(r"\bprecondition\s*\(")
# BOOST_TEST_MESSAGE / BOOST_TEST_INFO / BOOST_TEST_SKIP are not assertions.
ASSERTION = re.compile(r"\bBOOST_(?:CHECK|REQUIRE)\b|\bBOOST_TEST\s*[\(\{]")

# BENIGN: the case is not silently skipping coverage. Adding a line here is a
# claim that the early return is not a hidden skip.
ALLOWED = {
    # Assertions above this return already ran. The remainder of the case is
    # memory-layout arithmetic that does not hold on this arch. Converting it
    # to a skip would report the work that DID run as skipped.
    ("src/test/validation_flush_tests.cpp", "getcoinscachesizestate"):
        "BENIGN: mid-case exit after assertions, unsupported arch memory layout",
}


def tracked_test_sources() -> list[str]:
    out = subprocess.run(
        ["git", "ls-files", "--", "src/test/*.cpp", "src/test/**/*.cpp",
         "src/qt/test/*.cpp", "src/vault/test/*.cpp"],
        check=True,
        stdout=subprocess.PIPE,
        text=True,
    ).stdout.split()
    return [
        f for f in out
        if f.startswith(TEST_DIRS) and not f.startswith(EXCLUDED)
    ]


def scan_case(lines: list[str]) -> tuple[bool, bool]:
    """Return (message_then_return, getenv_then_return) for one case body."""
    message_return = False
    getenv_return = False
    pending_message = False
    pending_getenv = False

    for line in lines:
        if ASSERTION.search(line):
            pending_message = False
            pending_getenv = False
        if MESSAGE.search(line):
            pending_message = True
        if GETENV.search(line):
            pending_getenv = True
        if RETURN.search(line):
            if pending_message:
                message_return = True
            if pending_getenv:
                getenv_return = True
            pending_message = False
            pending_getenv = False
    return message_return, getenv_return


def scan(path: str) -> list[tuple[str, str, str]]:
    """Return (path, case, kind) hits for one file."""
    hits = []
    case: str | None = None
    case_header = ""
    body: list[str] = []

    def finish() -> None:
        if case is None:
            return
        message_return, getenv_return = scan_case(body)
        visible_skip = bool(HAS_SKIP_MACRO.search(case_header) or
                            HAS_PRECONDITION.search(case_header) or
                            any(HAS_SKIP_MACRO.search(l) or HAS_PRECONDITION.search(l)
                                for l in body))
        if message_return:
            hits.append((path, case, "MESSAGE_RETURN"))
        if getenv_return and not visible_skip:
            hits.append((path, case, "GETENV_RETURN"))

    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            c = CASE.match(line)
            if c:
                finish()
                case = c.group(1)
                case_header = line
                body = []
                continue
            if case is not None and CASE_END.match(line):
                finish()
                case = None
                case_header = ""
                body = []
                continue
            if case is not None:
                body.append(line)
    finish()
    return hits


def main() -> int:
    found: list[tuple[str, str, str]] = []
    for path in tracked_test_sources():
        found.extend(scan(path))

    found_keys = {(p, n) for p, n, _ in found}
    new = sorted((p, n, k) for p, n, k in found if (p, n) not in ALLOWED)
    stale = sorted(set(ALLOWED) - found_keys)

    if not new and not stale:
        return 0

    if new:
        print("Boost test case(s) skip by returning, which is a silent pass.")
        print("ctest still counts the suite green. Use a precondition (or")
        print("BOOST_TEST_SKIP where the Boost in tree has it) so the skip is")
        print("visible, and put the skip reason on this suite's")
        print("SKIP_REGULAR_EXPRESSION if the whole ctest row should skip.\n")
        for path, name, kind in new:
            print(f"  {kind:16} {path}: {name}")
        print()

    if stale:
        print("This lint's allowlist names case(s) that no longer match.")
        print("If you converted them, delete the corresponding line(s).\n")
        for path, name in stale:
            print(f"  STALE  {path}: {name}")
        print()

    return 1


if __name__ == "__main__":
    sys.exit(main())
