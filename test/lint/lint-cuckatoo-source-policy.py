#!/usr/bin/env python3
#
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
#
# Enforce Quicksilver-owned Cuckatoo source policy:
# - locale-neutral solver bridge code;
# - narrow vendored solver include seams;
# - upstream `vendor/siphashxN.h` filename preserved.

import argparse
import shutil
import sys
import tempfile
from pathlib import Path


def find_cuckatoo_root(repo_root: Path) -> Path | None:
    candidate = repo_root / "src/crypto/cuckatoo"
    if candidate.is_dir():
        return candidate
    return None


def rel(path: Path, base: Path) -> str:
    return path.relative_to(base).as_posix()


def read(path: Path) -> str:
    return path.read_text(encoding="utf8", errors="replace")


def iter_owned_source(cuckatoo: Path) -> list[Path]:
    # ".in" is scanned too: a configure_file template that includes a vendored
    # solver is a vendor seam like any other, and leaving it out would let the
    # policy be escaped by naming a file solve_one.cpp.in instead of solve_one.cpp.
    suffixes = (".cpp", ".h", ".cu", ".cuh", ".hpp", ".in")
    paths: list[Path] = []
    for path in cuckatoo.rglob("*"):
        if not path.is_file() or path.suffix not in suffixes:
            continue
        relative = rel(path, cuckatoo)
        if relative.startswith("vendor/"):
            continue
        paths.append(path)
    return sorted(paths)


def iter_test_cuckatoo_sources(repo_root: Path) -> list[Path]:
    # The owned-source scan only walks src/crypto/cuckatoo/. A test TU under
    # src/test/cuckatoo_*.cpp that includes vendor/cuckatoo.h would otherwise
    # smuggle a second verifier seam past this policy.
    test_dir = repo_root / "src/test"
    if not test_dir.is_dir():
        return []
    return sorted(path for path in test_dir.glob("cuckatoo_*.cpp") if path.is_file())


def include_lines(path: Path) -> list[tuple[int, str]]:
    lines = []
    for line_number, line in enumerate(read(path).splitlines(), start=1):
        stripped = line.strip()
        if stripped.startswith("#include"):
            lines.append((line_number, stripped))
    return lines


def check_tree(repo_root: Path) -> list[str]:
    cuckatoo = find_cuckatoo_root(repo_root)
    if cuckatoo is None:
        return ["missing Cuckatoo source root: expected src/crypto/cuckatoo"]

    violations: list[str] = []

    locale_patterns = (
        "setlocale",
        "std::locale",
        "#include <locale>",
        "#include <iostream>",
        "std::stringstream",
        "std::stod",
        "std::stof",
        "LC_ALL",
        "LC_NUMERIC",
    )
    for path in iter_owned_source(cuckatoo):
        text = read(path)
        for pattern in locale_patterns:
            if pattern in text:
                violations.append(f"{rel(path, repo_root)}: banned locale-sensitive pattern {pattern!r}")

    allowed_vendor_includes = {
        "solve_19.cpp": '"vendor/lean.cpp"',
        "solve_28.cpp": '"vendor/lean.cpp"',
        "verify_19.cpp": '"vendor/cuckatoo.h"',
        "verify_28.cpp": '"vendor/cuckatoo.h"',
        "gpu/qsgpusolve.cu": '"../vendor/lean.cu"',
        # Calibration-only CUDA twin of qsgpusolve: times every graph instead of
        # stopping at the first cycle. Built only by gpu/Makefile (nvcc), never by
        # CMake, so it cannot reach a node binary; see gpu/qsgpucalibrate.cu.
        "gpu/qsgpucalibrate.cu": '"../vendor/lean.cu"',
        # Calibration-only template, generated per EDGEBITS by configure_file.
        # EXCLUDE_FROM_ALL and never linked into a node binary; see
        # src/crypto/cuckatoo/bench/CMakeLists.txt.
        "bench/solve_one.cpp.in": '"vendor/lean.cpp"',
    }
    guarded_vendor_tokens = (
        '"vendor/lean.cpp"',
        '"vendor/lean.cu"',
        '"../vendor/lean.cu"',
        '"vendor/cuckatoo.h"',
        '"siphashxN.h"',
        "<siphashxN.h>",
    )
    for path in iter_owned_source(cuckatoo):
        relative = rel(path, cuckatoo)
        allowed = allowed_vendor_includes.get(relative)
        for line_number, line in include_lines(path):
            for token in guarded_vendor_tokens:
                if token in line and token != allowed:
                    violations.append(f"{rel(path, repo_root)}:{line_number}: disallowed direct include {token}")

    siphash_path = cuckatoo / "vendor/siphashxN.h"
    if not siphash_path.is_file():
        violations.append(f"{rel(siphash_path, repo_root)}: required upstream filename is missing")

    lean_hpp = cuckatoo / "vendor/lean.hpp"
    if lean_hpp.is_file():
        for line_number, line in include_lines(lean_hpp):
            if "siphashxN.h" in line and line != '#include "siphashxN.h"':
                violations.append(f"{rel(lean_hpp, repo_root)}:{line_number}: unexpected siphashxN.h include form")
    else:
        violations.append(f"{rel(lean_hpp, repo_root)}: required vendored lean.hpp is missing")

    for path in cuckatoo.rglob("*"):
        if not path.is_file() or rel(path, cuckatoo) == "vendor/lean.hpp":
            continue
        if rel(path, cuckatoo).startswith("vendor/"):
            continue
        for line_number, line in include_lines(path):
            if "siphashxN.h" in line:
                violations.append(f"{rel(path, repo_root)}:{line_number}: siphashxN.h may only be included by vendor/lean.hpp")

    for solve_name in ("solve_19.cpp", "solve_28.cpp"):
        path = cuckatoo / solve_name
        if not path.is_file():
            # Report rather than raise: a deleted solver is a policy violation with a
            # readable message, not a traceback that aborts the whole lint stage.
            violations.append(f"{rel(path, repo_root)}: expected solver source is missing")
            continue
        text = read(path)
        prelude_index = text.find("<crypto/cuckatoo/vendor_prelude_solve.h>")
        lean_index = text.find('"vendor/lean.cpp"')
        if prelude_index == -1 or lean_index == -1 or prelude_index > lean_index:
            violations.append(f"{rel(path, repo_root)}: vendor_prelude_solve.h must appear before vendor/lean.cpp")

    # The calibration template is optional — the bench harness is Stage-1 scaffolding
    # and may be removed after the flag day — but while it exists it must observe the
    # same prelude ordering as a hand-written solver TU.
    bench_template = cuckatoo / "bench/solve_one.cpp.in"
    if bench_template.is_file():
        text = read(bench_template)
        prelude_index = text.find("<crypto/cuckatoo/vendor_prelude_solve.h>")
        lean_index = text.find('"vendor/lean.cpp"')
        if prelude_index == -1 or lean_index == -1 or prelude_index > lean_index:
            violations.append(
                f"{rel(bench_template, repo_root)}: vendor_prelude_solve.h must appear before vendor/lean.cpp")

    cmake_path = cuckatoo / "CMakeLists.txt"
    cmake_text = read(cmake_path)
    for pattern in (".cu", "CUDA", "nvcc"):
        if pattern in cmake_text:
            violations.append(f"{rel(cmake_path, repo_root)}: CMake must not mention {pattern!r}")

    header = cuckatoo / "cuckatoo.h"
    if header.is_file():
        header_text = read(header)
        if "{19,29}" in header_text:
            violations.append(
                f"{rel(header, repo_root)}: supported edgebits are 19 and 28, not 29"
            )

    # Tokens match any include form (`"vendor/cuckatoo.h"` or
    # `<crypto/cuckatoo/vendor/cuckatoo.h>`). Allowlist is the filename stem
    # plus the token it may mention — currently only the F-255 verify-code TU.
    allowed_test_vendor_includes = {
        "cuckatoo_verify_codes_tests.cpp": ("vendor/cuckatoo.h",),
    }
    test_guarded_tokens = (
        "vendor/cuckatoo.h",
        "vendor/lean.cpp",
        "vendor/lean.cu",
        "siphashxN.h",
    )
    for path in iter_test_cuckatoo_sources(repo_root):
        allowed = allowed_test_vendor_includes.get(path.name, ())
        for line_number, line in include_lines(path):
            for token in test_guarded_tokens:
                if token in line and token not in allowed:
                    violations.append(
                        f"{rel(path, repo_root)}:{line_number}: disallowed direct include {token}")

    return violations


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf8")


def create_valid_fixture(repo_root: Path) -> Path:
    cuckatoo = repo_root / "src/crypto/cuckatoo"
    write(cuckatoo / "vendor/siphashxN.h", "#ifndef INCLUDE_SIPHASHXN_H\n#define INCLUDE_SIPHASHXN_H\n#endif\n")
    write(cuckatoo / "vendor/lean.hpp", '#include "siphashxN.h"\n')
    write(cuckatoo / "CMakeLists.txt", "add_library(quicksilver_cuckatoo STATIC proofhash.cpp)\n")
    for bits in ("19", "28"):
        write(
            cuckatoo / f"solve_{bits}.cpp",
            '#include <crypto/cuckatoo/cuckatoo.h>\n'
            '#include <crypto/cuckatoo/vendor_prelude_solve.h>\n'
            f"namespace cuckatoo_solve_e{bits} {{\n"
            '#include "vendor/lean.cpp"\n'
            "}\n",
        )
        write(
            cuckatoo / f"verify_{bits}.cpp",
            '#include <crypto/cuckatoo/vendor_prelude.h>\n'
            f"namespace cuckatoo_verify_e{bits} {{\n"
            '#include "vendor/cuckatoo.h"\n'
            "}\n",
        )
    write(
        cuckatoo / "gpu/qsgpusolve.cu",
        "#define CUCKATOO_NO_MAIN\n"
        "#define SQUASH_OUTPUT 1\n"
        '#include "../vendor/lean.cu"\n'
        "int main() { return 0; }\n",
    )
    write(cuckatoo / "gpu_solver.cpp", "int parse_nonce() { return 0; }\n")
    write(cuckatoo / "dispatch.cpp", '#include <crypto/cuckatoo/cuckatoo.h>\n')
    write(
        cuckatoo / "bench/solve_one.cpp.in",
        '#include <crypto/cuckatoo/vendor_prelude_solve.h>\n'
        "namespace cuckatoo_bench_e@QS_EDGEBITS@ {\n"
        '#include "vendor/lean.cpp"\n'
        "}\n",
    )
    write(
        repo_root / "src/test/cuckatoo_verify_codes_tests.cpp",
        '#include <crypto/cuckatoo/vendor_prelude.h>\n'
        '#include <crypto/cuckatoo/vendor/cuckatoo.h>\n',
    )
    write(
        repo_root / "src/test/cuckatoo_tests.cpp",
        '#include <crypto/cuckatoo/cuckatoo.h>\n',
    )
    return cuckatoo


def expect_clean(name: str, root: Path) -> list[str]:
    violations = check_tree(root)
    if violations:
        return [f"{name}: expected clean fixture, got violations: {violations}"]
    return []


def expect_violation(name: str, root: Path, expected: str) -> list[str]:
    violations = check_tree(root)
    if any(expected in violation for violation in violations):
        return []
    return [f"{name}: expected violation containing {expected!r}, got {violations}"]


def run_self_tests() -> int:
    failures: list[str] = []
    tmp = Path(tempfile.mkdtemp(prefix="cuckatoo-policy-test-"))
    try:
        clean = tmp / "clean"
        create_valid_fixture(clean)
        failures.extend(expect_clean("clean fixture", clean))

        bad_include = tmp / "bad_include"
        cuckatoo = create_valid_fixture(bad_include)
        write(cuckatoo / "dispatch.cpp", '#include "vendor/lean.cpp"\n')
        failures.extend(expect_violation("bad include", bad_include, "vendor/lean.cpp"))

        bad_locale = tmp / "bad_locale"
        cuckatoo = create_valid_fixture(bad_locale)
        write(cuckatoo / "gpu_solver.cpp", "#include <locale>\nvoid f() { (void)LC_NUMERIC; }\n")
        failures.extend(expect_violation("bad locale", bad_locale, "<locale>"))

        missing_siphash = tmp / "missing_siphash"
        cuckatoo = create_valid_fixture(missing_siphash)
        (cuckatoo / "vendor/siphashxN.h").unlink()
        failures.extend(expect_violation("missing siphash", missing_siphash, "vendor/siphashxN.h"))

        bad_siphash_include = tmp / "bad_siphash_include"
        cuckatoo = create_valid_fixture(bad_siphash_include)
        write(cuckatoo / "dispatch.cpp", '#include "siphashxN.h"\n')
        failures.extend(expect_violation("bad siphash include", bad_siphash_include, "siphashxN.h"))

        bad_cmake = tmp / "bad_cmake"
        cuckatoo = create_valid_fixture(bad_cmake)
        write(cuckatoo / "CMakeLists.txt", "add_library(quicksilver_cuckatoo STATIC gpu/qsgpusolve.cu)\n")
        failures.extend(expect_violation("bad cmake", bad_cmake, ".cu"))

        missing_solver = tmp / "missing_solver"
        cuckatoo = create_valid_fixture(missing_solver)
        (cuckatoo / "solve_28.cpp").unlink()
        failures.extend(expect_violation("missing solver", missing_solver, "expected solver source is missing"))

        # A .in template must not be able to escape the vendor-seam policy by
        # virtue of its file extension.
        bad_template_include = tmp / "bad_template_include"
        cuckatoo = create_valid_fixture(bad_template_include)
        write(cuckatoo / "bench/other.cpp.in", '#include "vendor/lean.cpp"\n')
        failures.extend(expect_violation("bad template include", bad_template_include,
                                         "other.cpp.in"))

        bad_template_prelude = tmp / "bad_template_prelude"
        cuckatoo = create_valid_fixture(bad_template_prelude)
        write(
            cuckatoo / "bench/solve_one.cpp.in",
            "namespace cuckatoo_bench_e@QS_EDGEBITS@ {\n"
            '#include "vendor/lean.cpp"\n'
            "}\n"
            '#include <crypto/cuckatoo/vendor_prelude_solve.h>\n',
        )
        failures.extend(expect_violation("bad template prelude order", bad_template_prelude,
                                         "vendor_prelude_solve.h must appear before"))

        bad_prelude_order = tmp / "bad_prelude_order"
        cuckatoo = create_valid_fixture(bad_prelude_order)
        write(
            cuckatoo / "solve_28.cpp",
            '#include <crypto/cuckatoo/cuckatoo.h>\n'
            "namespace cuckatoo_solve_e28 {\n"
            '#include "vendor/lean.cpp"\n'
            "}\n"
            '#include <crypto/cuckatoo/vendor_prelude_solve.h>\n',
        )
        failures.extend(expect_violation("bad prelude order", bad_prelude_order, "vendor_prelude_solve.h"))

        stale_edgebits = tmp / "stale-edgebits"
        cuckatoo = create_valid_fixture(stale_edgebits)
        write(cuckatoo / "cuckatoo.h", "//! edgebits must be in {19,29}.\n")
        failures.extend(expect_violation("stale edgebits comment", stale_edgebits, "19 and 28, not 29"))

        # An undeclared vendor/cuckatoo.h include in a *different*
        # src/test/cuckatoo_*.cpp must be caught. The F-255 TU is allowlisted;
        # cuckatoo_tests.cpp is not.
        bad_test_include = tmp / "bad_test_include"
        create_valid_fixture(bad_test_include)
        write(
            bad_test_include / "src/test/cuckatoo_tests.cpp",
            '#include <crypto/cuckatoo/vendor/cuckatoo.h>\n',
        )
        failures.extend(expect_violation("undeclared test vendor include", bad_test_include,
                                         "src/test/cuckatoo_tests.cpp"))
    finally:
        shutil.rmtree(tmp)

    if failures:
        print("\n".join(failures))
        return 1
    print("cuckatoo source policy self-tests passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Enforce Cuckatoo source policy")
    parser.add_argument("--self-test", action="store_true", help="run fixture-based self-tests")
    args = parser.parse_args()
    if args.self_test:
        return run_self_tests()

    violations = check_tree(Path.cwd())
    if violations:
        print("Cuckatoo source policy violations:")
        print("\n".join(violations))
        return 1
    print("Cuckatoo source policy OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
