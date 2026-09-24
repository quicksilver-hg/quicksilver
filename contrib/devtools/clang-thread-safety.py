#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Replay a clang compile_commands.json with -fsyntax-only -Werror=thread-safety.

g++ builds expand the annotations in src/threadsafety.h to nothing, and the
default CMake build leaves -Werror off, so a local build can exit 0 while
this analysis is warning. That is how F-352 shipped. This script is the
check that sees the diagnostic.

The mode is always printed. A missing database, a compiler that is not
clang, or a selection that matches nothing exits non-zero: each of those
has, elsewhere in this tree, been reported as a pass that checked nothing.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Sequence

# Workstation ceiling. This box has 12 cores; saturating all of them freezes
# the owner's session. Callers on a larger machine pass --jobs.
DEFAULT_JOBS = 10

# Exit 1: the analysis reported a diagnostic.
# Exit 2: the check did not run to completion, so a pass would be a lie.
EXIT_DIAGNOSTIC = 1
EXIT_UNCHECKED = 2

# Narrow on purpose. "clang-tidy" and "clang-format" start with "clang" and
# must not count as a compiler that ran the analysis.
_CLANG_NAME = re.compile(r"^clang(\+\+)?(-\d+)?$")

# The diagnostic is emitted as [-Wthread-safety-analysis], or, once promoted
# to an error, as [-Werror,-Wthread-safety-analysis]. Matching the flag name
# anywhere on the line covers both spellings.
_THREAD_SAFETY = re.compile(r"-Wthread-safety")

_SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".mm"}
_HEADER_SUFFIXES = {".h", ".hh", ".hpp", ".hxx"}

# A missing header can dump an include stack. Keep the reason; drop the rest.
_MAX_OUTPUT_LINES = 200


@dataclass(frozen=True)
class TuOutcome:
    source: str
    # "clean", "diagnostic", or "could_not_check". Missing sources never get here.
    kind: str
    output: str


def log(message: str) -> None:
    print(f"clang-thread-safety: {message}", flush=True)


def git_toplevel() -> Path | None:
    proc = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],
        check=False,
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        return None
    return Path(proc.stdout.strip())


def load_entries(database: Path) -> list[dict[str, Any]]:
    try:
        raw = json.loads(database.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        log(f"cannot read {database}: {exc}")
        raise SystemExit(EXIT_UNCHECKED)
    if not isinstance(raw, list):
        log(f"{database} is not a compile_commands.json list")
        raise SystemExit(EXIT_UNCHECKED)
    entries: list[dict[str, Any]] = []
    for item in raw:
        if not isinstance(item, dict):
            log(f"{database} contains a non-object entry")
            raise SystemExit(EXIT_UNCHECKED)
        entries.append(item)
    return entries


def entry_argv(entry: dict[str, Any]) -> list[str]:
    arguments = entry.get("arguments")
    if isinstance(arguments, list) and all(isinstance(arg, str) for arg in arguments):
        return list(arguments)
    command = entry.get("command")
    if isinstance(command, str) and command:
        return shlex.split(command)
    source = entry.get("file", "<unknown>")
    log(f"{source} has neither a command string nor an arguments list")
    raise SystemExit(EXIT_UNCHECKED)


def entry_source(entry: dict[str, Any]) -> str:
    source = entry.get("file")
    if not isinstance(source, str) or not source:
        log("a compile_commands entry has no file")
        raise SystemExit(EXIT_UNCHECKED)
    return source


def entry_directory(entry: dict[str, Any]) -> str:
    directory = entry.get("directory")
    if not isinstance(directory, str) or not directory:
        log(f"{entry_source(entry)} has no directory")
        raise SystemExit(EXIT_UNCHECKED)
    return directory


def compiler_of(argv: Sequence[str]) -> str:
    """Return the compiler argv0, looking past ccache.

    ccache is a launcher. Treating it as the compiler would refuse a real
    clang build, or — if the check were skipped — would say nothing about
    whether the underlying compiler erases the annotations.
    """
    if not argv:
        return ""
    if Path(argv[0]).name == "ccache":
        for arg in argv[1:]:
            if not arg.startswith("-"):
                return arg
        return ""
    return argv[0]


def is_clang(compiler: str) -> bool:
    return _CLANG_NAME.match(Path(compiler).name) is not None


def compiler_exists(compiler: str) -> bool:
    if not compiler:
        return False
    if os.path.isabs(compiler):
        return os.access(compiler, os.X_OK)
    return shutil.which(compiler) is not None


def syntax_argv(argv: list[str]) -> list[str]:
    """Turn one compile command into a read-only thread-safety check.

    -Werror is stripped, including every -Werror=<other> form. An unrelated
    warning failing this check is how the check gets disabled. -Werror=thread-safety
    is the only error promotion added back.

    -Wthread-safety-negative is not added. It is a separate, noisier flag.
    The diagnostic this sweep exists for is -Wthread-safety-analysis, which
    the -Wthread-safety group already includes.
    """
    rewritten: list[str] = []
    skip_next = False
    for arg in argv:
        if skip_next:
            skip_next = False
            continue
        if arg == "-c":
            continue
        if arg == "-o":
            # CMake emits the object path as the next argument, not -ofile.
            skip_next = True
            continue
        if arg in {"-MD", "-MMD", "-MG", "-MP"}:
            # Dependency output would write into the build tree.
            continue
        if arg in {"-MF", "-MT", "-MQ"}:
            skip_next = True
            continue
        if arg == "-Werror" or (
            arg.startswith("-Werror=") and arg != "-Werror=thread-safety"
        ):
            continue
        if arg.startswith("-Wno-thread-safety") or arg == "-Wno-error=thread-safety":
            # The database must not be able to mute the diagnostic.
            continue
        if arg == "-fsyntax-only":
            continue
        rewritten.append(arg)
    if skip_next:
        raise ValueError("compile command ended on a flag that takes an argument")
    if "-Wthread-safety" not in rewritten:
        rewritten.append("-Wthread-safety")
    if "-Werror=thread-safety" not in rewritten:
        rewritten.append("-Werror=thread-safety")
    rewritten.append("-fsyntax-only")
    return rewritten


def repo_relative(path: str, repo: Path) -> str | None:
    candidate = Path(path)
    if not candidate.is_absolute():
        candidate = repo / candidate
    try:
        return candidate.resolve().relative_to(repo.resolve()).as_posix()
    except ValueError:
        return None


def changed_paths(repo: Path, revspec: str) -> list[str]:
    """Files `git diff REVSPEC` names, as repo-relative paths.

    The revspec is passed through unchanged, so both `HEAD~1` and
    `base..HEAD` mean what they mean to git diff. Deletions are excluded:
    there is nothing left to syntax-check. Untracked files are not included,
    because git diff does not list them.
    """
    proc = subprocess.run(
        [
            "git",
            "-C",
            str(repo),
            "diff",
            "--name-status",
            "--find-renames",
            "--diff-filter=ACMR",
            revspec,
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        detail = proc.stderr.strip() or proc.stdout.strip() or f"exit {proc.returncode}"
        log(f"git diff {revspec} failed: {detail}")
        raise SystemExit(EXIT_UNCHECKED)
    paths: list[str] = []
    for line in proc.stdout.splitlines():
        if not line:
            continue
        parts = line.split("\t")
        # Rename lines are "R100\told\tnew". The file to check is the new path.
        paths.append(parts[-1])
    return paths


def select_entries(
    entries: list[dict[str, Any]],
    repo: Path | None,
    changed: set[str] | None,
) -> list[dict[str, Any]]:
    if changed is None:
        return entries
    if repo is None:
        log("--changed needs a git checkout (cwd is not inside one)")
        raise SystemExit(EXIT_UNCHECKED)
    selected: list[dict[str, Any]] = []
    for entry in entries:
        relative = repo_relative(entry_source(entry), repo)
        if relative is not None and relative in changed:
            selected.append(entry)
    return selected


def uncovered_changes(
    changed: set[str],
    selected_sources: set[str],
) -> tuple[list[str], list[str]]:
    """Changed headers, and changed sources that this database does not build.

    Changed-file mode checks a translation unit only when its own source file
    is in the diff. A header edit is invisible to it, and so is a .cpp this
    configure did not emit. Both are named rather than folded into a pass.
    """
    headers: list[str] = []
    missing_entries: list[str] = []
    for path in sorted(changed):
        suffix = Path(path).suffix
        if suffix in _HEADER_SUFFIXES:
            headers.append(path)
        elif suffix in _SOURCE_SUFFIXES and path not in selected_sources:
            missing_entries.append(path)
    return headers, missing_entries


def clip(output: str) -> str:
    lines = output.splitlines()
    if len(lines) <= _MAX_OUTPUT_LINES:
        return output
    kept = "\n".join(lines[:_MAX_OUTPUT_LINES])
    return f"{kept}\n... truncated {len(lines) - _MAX_OUTPUT_LINES} more lines\n"


def run_syntax(task: tuple[str, str, list[str]]) -> TuOutcome:
    directory, source, argv = task
    try:
        proc = subprocess.run(
            argv,
            cwd=directory,
            check=False,
            capture_output=True,
            text=True,
        )
    except OSError as exc:
        return TuOutcome(source, "could_not_check", f"could not execute {argv[0]}: {exc}\n")
    output = clip(proc.stdout + proc.stderr)
    if _THREAD_SAFETY.search(output):
        # A warning that did not fail the compiler is still a finding.
        # -Werror=thread-safety is supposed to promote it; if a future clang
        # spells the flag differently and exits 0, the text is the evidence.
        return TuOutcome(source, "diagnostic", output)
    if proc.returncode != 0:
        return TuOutcome(source, "could_not_check", output)
    return TuOutcome(source, "clean", "")


def compiler_version(compiler: str) -> str:
    proc = subprocess.run(
        [compiler, "--version"],
        check=False,
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        log(f"{compiler} --version failed")
        raise SystemExit(EXIT_UNCHECKED)
    line = proc.stdout.splitlines()
    return line[0] if line else compiler


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Syntax-check translation units from a clang compile_commands.json "
            "with -Werror=thread-safety. Prints the mode it ran."
        )
    )
    parser.add_argument(
        "--build-dir",
        required=True,
        type=Path,
        help="Directory that contains compile_commands.json",
    )
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument(
        "--whole-tree",
        action="store_true",
        help="Check every translation unit in the database",
    )
    mode.add_argument(
        "--changed",
        metavar="REVSPEC",
        help=(
            "Check translation units whose source file is in `git diff REVSPEC` "
            "(for example HEAD~1, or base..HEAD). Header-only edits are named "
            "and are not themselves syntax-checked."
        ),
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=DEFAULT_JOBS,
        help=(
            f"Parallel syntax checks (default {DEFAULT_JOBS}). "
            "The workstation this tree is built on freezes if every core is busy; "
            "do not raise this there."
        ),
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    if args.jobs < 1:
        log("--jobs must be at least 1")
        return EXIT_UNCHECKED

    database = args.build_dir / "compile_commands.json"
    if not database.is_file():
        log(f"no compile_commands.json in {args.build_dir}")
        return EXIT_UNCHECKED

    if args.whole_tree:
        log("mode=whole-tree")
    else:
        log(f"mode=changed revspec={args.changed}")
    log(f"compile_commands={database}")
    log(f"jobs={args.jobs}")

    repo = git_toplevel()
    changed: set[str] | None = None
    if args.changed is not None:
        if repo is None:
            log("--changed needs a git checkout (cwd is not inside one)")
            return EXIT_UNCHECKED
        changed = set(changed_paths(repo, args.changed))
        log(f"diff_paths={len(changed)}")

    entries = load_entries(database)
    selected = select_entries(entries, repo, changed)
    headers: list[str] = []
    missing_entries: list[str] = []
    if not selected:
        # A glob that matches nothing must not look like a pass.
        log("matched 0 translation units; refusing to pass without checking anything")
        if changed is not None and repo is not None:
            headers, missing_entries = uncovered_changes(changed, set())
            _print_named("changed headers not rechecked", headers)
            _print_named("changed sources with no compile_commands entry", missing_entries)
        return EXIT_UNCHECKED

    compilers = {compiler_of(entry_argv(entry)) for entry in selected}
    if any(not is_clang(compiler) for compiler in compilers):
        joined = ", ".join(sorted(compilers)) or "<none>"
        # g++ accepts the source and exits 0: the annotations are empty macros.
        # Replaying those commands would be a silent pass.
        log(f"compiler is not clang ({joined}); refusing to report a pass")
        return EXIT_UNCHECKED
    missing_bins = sorted(compiler for compiler in compilers if not compiler_exists(compiler))
    if missing_bins:
        log("clang not found: " + ", ".join(missing_bins))
        return EXIT_UNCHECKED
    for compiler in sorted(compilers):
        log(f"compiler={compiler}")
        log(f"compiler_version={compiler_version(compiler)}")

    present: list[dict[str, Any]] = []
    skipped: list[str] = []
    for entry in selected:
        source = entry_source(entry)
        if os.path.isfile(source):
            present.append(entry)
        else:
            # Qt autogen and other generated sources are in the database before
            # their targets have been built. Checking a path that is not there
            # is not a clean result.
            skipped.append(source)

    selected_rels: set[str] = set()
    if repo is not None:
        for entry in selected:
            relative = repo_relative(entry_source(entry), repo)
            if relative is not None:
                selected_rels.add(relative)

    if changed is not None and repo is not None:
        headers, missing_entries = uncovered_changes(changed, selected_rels)

    if not present:
        log(f"attempted=0 skipped={len(skipped)}")
        _print_named("skipped, source file does not exist (generated source not built)", skipped)
        _print_named("changed headers not rechecked", headers)
        _print_named("changed sources with no compile_commands entry", missing_entries)
        log("checked nothing; refusing to pass")
        return EXIT_UNCHECKED

    tasks: list[tuple[str, str, list[str]]] = []
    for entry in present:
        source = entry_source(entry)
        try:
            argv = syntax_argv(entry_argv(entry))
        except ValueError as exc:
            log(f"{source}: {exc}")
            return EXIT_UNCHECKED
        tasks.append((entry_directory(entry), source, argv))

    log(f"checking {len(tasks)} translation units")
    outcomes: list[TuOutcome] = []
    with ProcessPoolExecutor(max_workers=args.jobs) as pool:
        outcomes.extend(pool.map(run_syntax, tasks))

    outcomes.sort(key=lambda item: item.source)
    diagnostics = [item for item in outcomes if item.kind == "diagnostic"]
    unchecked = [item for item in outcomes if item.kind == "could_not_check"]
    clean = [item for item in outcomes if item.kind == "clean"]

    for item in diagnostics:
        sys.stdout.write(item.output)
        if not item.output.endswith("\n"):
            sys.stdout.write("\n")
    for item in unchecked:
        log(f"could not check {item.source}")
        sys.stdout.write(item.output)
        if item.output and not item.output.endswith("\n"):
            sys.stdout.write("\n")

    _print_named("skipped, source file does not exist (generated source not built)", skipped)
    _print_named("changed headers not rechecked (changed-file mode does not follow #include)", headers)
    _print_named("changed sources with no compile_commands entry", missing_entries)

    log(
        "SUMMARY "
        f"attempted={len(outcomes)} "
        f"clean={len(clean)} "
        f"diagnostics={len(diagnostics)} "
        f"could_not_check={len(unchecked)} "
        f"skipped={len(skipped)}"
    )

    if diagnostics:
        return EXIT_DIAGNOSTIC
    # A partial sweep is not a green result. Skipped generated sources and
    # translation units that failed to preprocess are holes, same as matching
    # nothing.
    if unchecked or skipped or missing_entries:
        return EXIT_UNCHECKED
    return 0


def _print_named(reason: str, paths: Sequence[str]) -> None:
    if not paths:
        return
    log(f"{reason}: {len(paths)}")
    for path in paths:
        print(f"  {path}")


if __name__ == "__main__":
    raise SystemExit(main())
