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

This file deliberately names no solver filename that does not exist, including in
prose: the rule applies to the linter too, and an exemption for its own path would
be a permanent blind spot in exactly the place the drift shows up first.

Also asserts qsgpusolve.cu still documents its exit codes. gpu_solver.cpp maps
exit 4 to "no CUDA device" and exit 5 to "device faulted while solving", and
those mappings are only discoverable from the comment block -- a silently
renumbered exit code would turn a dead GPU back into an apparently empty search
window.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
PATTERN = re.compile(r"\b(qsgpusolve(?:\.cu|\.exe)?|solve_\d+\.cpp)\b")
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
EXIT_CODE_DOC_MARKERS = (
    "Exit codes",
    "no usable CUDA device",
    "faulted while solving",
)
STARTUP_PROBE_MARKERS = {
    "src/crypto/cuckatoo/gpu/device_health.cuh": "gpu_health_startup_kernel<<<1, 1>>>()",
    "src/crypto/cuckatoo/gpu/qsgpusolve.cu": "gpu_health_startup_fault()",
    "src/crypto/cuckatoo/gpu/qsgpucalibrate.cu": "gpu_health_startup_fault()",
}


def existing_solver_names() -> set[str]:
    """Every solver filename that legitimately exists right now."""
    names = {p.name for p in SOLVER_DIR.glob("solve_*.cpp")}
    names |= {p.name for p in (SOLVER_DIR / "gpu").glob("qsgpusolve*")}
    # The built binary is gitignored, so it may or may not be on disk. Both the
    # POSIX and the Windows spelling are legitimate references either way.
    names.add("qsgpusolve")
    names.add("qsgpusolve.exe")
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
            if any(rel.startswith(s) for s in SKIP_PREFIXES):
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                continue
            for lineno, line in enumerate(text.splitlines(), 1):
                for name in PATTERN.findall(line):
                    if name not in known:
                        failures.append(
                            f"{rel}:{lineno}: names {name}, which does not exist in "
                            f"src/crypto/cuckatoo/"
                        )
    return failures


def check_exit_codes_documented() -> list[str]:
    source = SOLVER_DIR / "gpu" / "qsgpusolve.cu"
    if not source.is_file():
        return [f"{source.relative_to(ROOT).as_posix()}: missing"]
    text = source.read_text(encoding="utf-8")
    missing = [m for m in EXIT_CODE_DOC_MARKERS if m not in text]
    if missing:
        rel = source.relative_to(ROOT).as_posix()
        return [
            f"{rel}: exit-code documentation lost (no {m!r}); gpu_solver.cpp maps "
            f"exit 4 to kNoCudaDevice and exit 5 to kDeviceFault, and nothing "
            f"else records that contract"
            for m in missing
        ]
    return []


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
