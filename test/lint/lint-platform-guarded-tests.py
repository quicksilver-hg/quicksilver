#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Freeze the set of C++ test cases that only one platform runs.

A `#ifndef WIN32` around a test case is a SILENT skip. The suite still reports
passed, the ctest count never moves, and nothing anywhere says the case did not
run -- unlike the functional suite, where `skip_if_platform_not_linux()` prints
an honest `○ Skipped` row in the results table. Coverage disappears without a
trace, and it disappears in the C++ layer, which is where the platform-specific
defects actually live.

That is not hypothetical. Every case in `gpu_solver_tests.cpp` was once
`#ifndef WIN32`, on the recorded grounds that porting them was "Layer-2 work".
(They all run on both platforms now -- see ALLOWED_WHOLE, which is empty.)
The Windows solver bridge then deadlocked a live node on the first cancelled
solve -- `_pclose` waiting on a child that could never exit, GPU pinned, the
node reporting `solver_ok: true` throughout. The liveness contract was the same
promise on both platforms; only one platform was asked to keep it.

So this lint does not ban platform guards -- some are correct, because some
contracts genuinely differ by platform. It bans *new* ones. Every case that one
platform skips is listed below with a reason, and the list must stay accurate in
both directions:

  * a guarded case that is not on the list  -> failure (new asymmetry)
  * a listed case that is no longer guarded -> failure (stale entry, delete it)

The second half matters as much as the first: without it the list rots into an
inventory of things that used to be true, which is how a register of known gaps
turns into a register of noise.

Two shapes are detected, and they are NOT the same problem:

  WHOLE   the entire case is inside the guard. Visible as a missing case name if
          you go looking, though nothing makes you look.
  PARTIAL the case runs on both platforms but a guard inside its body compiles
          out some of its assertions. Invisible to every count there is -- the
          case is present, named, and green with half its checks gone. This is
          the shape that hid the datadir lock gap (see test_LockDirectory).

See flag-register entry (w) 2026-09-02 for the full classification of the
current set, and `~/qs-planning/F-91/` for the sweep this was built from.
"""
import re
import subprocess
import sys

# Directories whose *.cpp files are test translation units.
TEST_DIRS = ("src/test/", "src/qt/test/", "src/vault/test/")

# Fuzz targets are built and run under a separate harness with its own platform
# story; they are not Boost cases and are out of scope here.
EXCLUDED = ("src/test/fuzz/",)

# A preprocessor conditional that gates on the platform rather than on a feature.
PLATFORM_TOKENS = r"(?:WIN32|_WIN32|__linux__|__APPLE__|__FreeBSD__|__OpenBSD__)"
IF_PLATFORM_POS = re.compile(  # true on POSIX only  -> case is lost on Windows
    r"^\s*#\s*if(?:ndef\s+" + PLATFORM_TOKENS + r"\b|\s+!\s*defined\s*\(\s*" + PLATFORM_TOKENS + r"\s*\))"
)
IF_PLATFORM_NEG = re.compile(  # true on that platform only -> lost elsewhere
    r"^\s*#\s*if(?:def\s+" + PLATFORM_TOKENS + r"\b|\s+defined\s*\(\s*" + PLATFORM_TOKENS + r"\s*\))"
)
COND = re.compile(r"^\s*#\s*(if|ifdef|ifndef|else|elif|endif)\b")
CASE = re.compile(r"^\s*BOOST_(?:AUTO|FIXTURE)_TEST_CASE\s*\(\s*([A-Za-z0-9_]+)")

# House style closes a test case with a brace in column 0, and every file this
# lint reads follows it. A stray unclosed case would only ever cause a false
# POSITIVE (guards after it attributed to it), never a false negative.
CASE_END = re.compile(r"^\}")

# --------------------------------------------------------------------------
# The frozen set. Key: (path, case name). Value: why it is guarded, and whether
# it is a real coverage gap or a contract that genuinely differs by platform.
#
# GAP    = a promise both platforms must keep, tested on only one. Owed work.
# BENIGN = the platforms genuinely promise different things here. Correct as is.
#
# Adding a line here is a deliberate act: you are recording that one platform
# will not run this code. Prefer a platform-neutral fixture -- system_tests.cpp
# `run_command` pairs every case with a platform-appropriate command, and
# gpu_solver_tests.cpp does the same with stub scripts: `write_scripted_stub`
# for anything a solver expresses by printing lines and exiting with a code,
# `write_dripping_stub` when the gaps between lines are the point, and the
# `*_both_platforms` stubs for the liveness cases. Between them those cover
# every shape the solver contract has needed so far, in both dialects.
# --------------------------------------------------------------------------
ALLOWED_WHOLE = {
    # EMPTY, and that is the point: as of F-91's last port every Boost case in
    # this tree runs on every platform it is built for. A whole-case exclusion is
    # now a thing you have to add deliberately, with a reason, rather than a
    # background condition nobody counts. The last five to go were the solver
    # bridge's fallback, watchdog and cancel cases; two of those five were
    # DELETED rather than ported, because a `*_both_platforms` twin already
    # tested the same invariant -- and each twin took over the deleted case's
    # tighter timing bound, so the merge cost no coverage on POSIX either.
}

ALLOWED_PARTIAL = {
    # -- BENIGN below: the platforms promise different things.
    ("src/test/fs_tests.cpp", "fsbridge_pathtostring"): "BENIGN: invalid UTF-8 round trip is undefined on Windows, where paths are Unicode",
    ("src/test/getarg_tests.cpp", "patharg"): "BENIGN: drive-letter paths have no POSIX counterpart",
    ("src/test/system_tests.cpp", "run_command"): "BENIGN: every case paired with a platform-appropriate command",
    ("src/test/gpu_solver_tests.cpp", "solver_fault_hardware_and_process_failures"): "BENIGN: asserts each platform's own remedy text (TdrDelay vs dmesg)",
    ("src/test/net_peer_connection_tests.cpp", "test_addnode_getaddednodeinfo_and_connection_detection"): "BENIGN: OpenBSD does not support the IPv4 shorthand notation with omitted zero-bytes",
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


def scan(path: str) -> tuple[set, set]:
    """Return (whole-case exclusions, partial-body exclusions) for one file."""
    whole, partial = set(), set()
    stack: list[str | None] = []   # per open conditional: 'pos', 'neg', or None
    case: str | None = None

    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            m = COND.match(line)
            if m:
                kind = m.group(1)
                if kind in ("if", "ifdef", "ifndef"):
                    if IF_PLATFORM_POS.match(line):
                        stack.append("pos")
                    elif IF_PLATFORM_NEG.match(line):
                        stack.append("neg")
                    else:
                        stack.append(None)
                    # A platform guard opened inside a case body compiles out
                    # part of a case that still runs on both platforms.
                    if case is not None and stack[-1] is not None:
                        partial.add((path, case))
                elif kind == "else" and stack:
                    stack[-1] = {"pos": "neg", "neg": "pos"}.get(stack[-1])
                elif kind == "elif" and stack:
                    stack[-1] = None
                elif kind == "endif" and stack:
                    stack.pop()
                continue

            c = CASE.match(line)
            if c:
                case = c.group(1)
                if any(s is not None for s in stack):
                    whole.add((path, case))
                continue

            if case is not None and CASE_END.match(line):
                case = None

    return whole, partial


def main() -> int:
    whole: set = set()
    partial: set = set()
    for path in tracked_test_sources():
        w, p = scan(path)
        whole |= w
        partial |= p

    # A case wholly inside a guard is reported as WHOLE only; the guard that
    # encloses it is not also a partial exclusion of it.
    partial -= whole

    new_whole = sorted(whole - set(ALLOWED_WHOLE))
    new_partial = sorted(partial - set(ALLOWED_PARTIAL))
    stale_whole = sorted(set(ALLOWED_WHOLE) - whole)
    stale_partial = sorted(set(ALLOWED_PARTIAL) - partial)

    if not (new_whole or new_partial or stale_whole or stale_partial):
        return 0

    if new_whole or new_partial:
        print("New platform-guarded test case(s). One platform will not run this code,")
        print("and nothing at runtime will say so -- no skip is reported and the ctest")
        print("count does not move.\n")
        print("Prefer a platform-neutral fixture: system_tests.cpp `run_command` pairs")
        print("each case with a platform-appropriate command, and gpu_solver_tests.cpp")
        print("`*_both_platforms` does the same with stub scripts. If the contract")
        print("genuinely differs by platform, add it to this lint's list with a reason.\n")
        for path, name in new_whole:
            print(f"  WHOLE   {path}: {name} (entire case skipped on one platform)")
        for path, name in new_partial:
            print(f"  PARTIAL {path}: {name} (case runs, some assertions compiled out)")
        print()

    if stale_whole or stale_partial:
        print("This lint's list names case(s) that are no longer platform-guarded.")
        print("If you ported them, delete the corresponding line(s) -- an allowlist")
        print("nobody prunes stops describing the tree.\n")
        for path, name in stale_whole:
            print(f"  STALE (whole)   {path}: {name}")
        for path, name in stale_partial:
            print(f"  STALE (partial) {path}: {name}")
        print()

    return 1


if __name__ == "__main__":
    sys.exit(main())
