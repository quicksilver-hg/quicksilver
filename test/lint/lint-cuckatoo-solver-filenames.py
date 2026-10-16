#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Assert every hardcoded Cuckatoo solver filename names a file that exists.

Several unrelated files name the solver binary and the per-edgebits translation
units -- CMakeLists, the bridge, two other linters, a testrun script and the unit
tests -- and nothing made them agree. Deleting the old edgebits-31 solver left
lint-cuckatoo-source-policy.py referencing a missing path, where it crashed for
weeks; the lint stage was reported as a single aggregate result, so a crashing
linter and a passing one looked the same.

This file deliberately names no live solver filename that does not exist, including
in prose: the rule applies to the linter too, and an exemption for its own path
would be a permanent blind spot in exactly the place the drift shows up first.
The EXEMPT reasons are the exception. They name the retired fleet binary on
purpose, and only a line that carries one of those reasons is ignored.

Also asserts both helpers still document their exit codes. gpu_solver.cpp maps
exit 4 to "no usable GPU device" and exit 5 to "faulted while solving", and
those mappings are only discoverable from the comment block -- a silently
renumbered exit code would turn a dead GPU back into an apparently empty search
window.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
# The stem is split so this source file is not itself a reference to the
# retired unsuffixed binary. The compiled expression is the one the plan states.
_STEM = "qsgpu" "solve"
PATTERN = re.compile(r"\b(" + _STEM + r"(?:-cuda|-opencl)?(?:\.cu|\.cpp|\.exe)?|solve_\d+\.cpp)\b")
SEARCH_ROOTS = ("src", "test", "cmake", "contrib")
SKIP_PREFIXES = (
    "src/secp256k1",
    "src/leveldb",
    "src/crc32c",
    "src/univalue",
    "src/crypto/cuckatoo/vendor",
)
SUFFIXES = (".cpp", ".h", ".cu", ".cuh", ".py", ".txt", ".cmake", ".sh", ".md")

SOLVER_DIR = ROOT / "src" / "crypto" / "cuckatoo"
EXIT_CODE_SOURCES = (
    "gpu/qsgpusolve-cuda.cu",
    "gpu/qsgpusolve-opencl.cpp",
)
EXIT_CODE_DOC_MARKERS = (
    "Exit codes",
    "no usable GPU device",
    "faulted while solving",
)
STARTUP_PROBE_MARKERS = {
    "src/crypto/cuckatoo/gpu/device_health.cuh": "gpu_health_startup_kernel<<<1, 1>>>()",
    "src/crypto/cuckatoo/gpu/qsgpusolve-cuda.cu": "gpu_health_startup_fault()",
    "src/crypto/cuckatoo/gpu/qsgpucalibrate.cu": "gpu_health_startup_fault()",
}
EXEMPT = {
    "test/testrun/qs-solver-dispatch.sh": "runs the fleet's box-local qsgpusolve; retired in F-469 phase 4",
    "test/testrun/harness/scenarios.py": "records an incident on the fleet's box-local qsgpusolve.exe; retired in F-469 phase 4",
}


def existing_solver_names() -> set[str]:
    """Every solver filename that legitimately exists right now."""
    names = {p.name for p in SOLVER_DIR.glob("solve_*.cpp")}
    names |= {p.name for p in (SOLVER_DIR / "gpu").glob(_STEM + "*")}
    # The built binaries are gitignored, so they may or may not be on disk.
    # Both spellings of each helper are legitimate references either way.
    # The unsuffixed binary name is not: that binary was renamed.
    names.add("qsgpusolve-cuda")
    names.add("qsgpusolve-cuda.exe")
    names.add("qsgpusolve-opencl")
    names.add("qsgpusolve-opencl.exe")
    return names


def check_filenames(known: set[str]) -> list[str]:
    failures = []
    for root in SEARCH_ROOTS:
        base = ROOT / root
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if not path.is_file() or path.suffix not in SUFFIXES:
                continue
            rel = path.relative_to(ROOT).as_posix()
            if rel in EXEMPT or any(rel.startswith(s) for s in SKIP_PREFIXES):
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                continue
            own_reasons = rel.endswith("lint-cuckatoo-solver-filenames.py")
            for lineno, line in enumerate(text.splitlines(), 1):
                # The reason strings name the retired fleet binary. They are
                # printed by --verbose. Ignoring only a line that is one of
                # those reasons keeps every other line in this file in scope.
                if own_reasons and any(reason in line for reason in EXEMPT.values()):
                    continue
                for name in PATTERN.findall(line):
                    if name not in known:
                        failures.append(
                            f"{rel}:{lineno}: names {name}, which does not exist in "
                            f"src/crypto/cuckatoo/"
                        )
    return failures


def check_exit_codes_documented() -> list[str]:
    failures = []
    for relative in EXIT_CODE_SOURCES:
        source = SOLVER_DIR / relative
        if not source.is_file():
            failures.append(f"{source.relative_to(ROOT).as_posix()}: missing")
            continue
        text = source.read_text(encoding="utf-8")
        missing = [m for m in EXIT_CODE_DOC_MARKERS if m not in text]
        rel = source.relative_to(ROOT).as_posix()
        for marker in missing:
            failures.append(
                f"{rel}: exit-code documentation lost (no {marker!r}); gpu_solver.cpp maps "
                f"exit 4 to kNoGpuDevice and exit 5 to kDeviceFault, and nothing "
                f"else records that contract"
            )
    return failures


def check_startup_probe_present() -> list[str]:
    failures = []
    for relative, marker in STARTUP_PROBE_MARKERS.items():
        source = ROOT / relative
        if not source.is_file():
            failures.append(f"{relative}: missing")
            continue
        if marker not in source.read_text(encoding="utf-8"):
            failures.append(
                f"{relative}: CUDA architecture startup probe lost (no {marker!r}); "
                "a wrong-architecture helper can otherwise report a successful empty run"
            )
    return failures


def main() -> int:
    verbose = "--verbose" in sys.argv
    if verbose:
        for path, reason in EXEMPT.items():
            print(f"exempt {path}: {reason}")
    failures = check_filenames(existing_solver_names())
    failures += check_exit_codes_documented()
    failures += check_startup_probe_present()

    for f in failures:
        print(f)
    if failures:
        print("\nA hardcoded solver reference no longer matches src/crypto/cuckatoo/.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
