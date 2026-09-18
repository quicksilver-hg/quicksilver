#!/usr/bin/env python3
#
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
#
# F-262: every first-party include of the vendored blake2.h must go through
# blake2_prelude.h (the one header that owns the MSVC C4804 sandwich), and
# every first-party include of vendor/cuckatoo.h must follow vendor_prelude.h
# or vendor_prelude_solve.h in the same file so that include is a no-op.

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

INCLUDE_RE = re.compile(r'^\s*#include\s*(<[^>]+>|"[^"]+")')


SUFFIXES = (".cpp", ".h", ".hpp", ".c", ".cu", ".cuh", ".in")
SKIP_PREFIXES = (
    "src/crypto/cuckatoo/vendor/",
    "src/leveldb/",
    "src/crc32c/",
    "src/secp256k1/",
)
BLAKE2_PRELUDE = Path("src/crypto/cuckatoo/blake2_prelude.h")
VENDOR_PRELUDE = Path("src/crypto/cuckatoo/vendor_prelude.h")
VENDOR_PRELUDE_SOLVE = Path("src/crypto/cuckatoo/vendor_prelude_solve.h")


def rel(path: Path, base: Path) -> str:
    return path.relative_to(base).as_posix()


def read(path: Path) -> str:
    return path.read_text(encoding="utf8", errors="replace")


def normalize_include(token: str) -> str:
    return token.replace("\\", "/")


def include_lines(path: Path) -> list[tuple[int, str]]:
    lines = []
    for line_number, line in enumerate(read(path).splitlines(), start=1):
        match = INCLUDE_RE.match(line)
        if match:
            token = match.group(1)
            lines.append((line_number, normalize_include(token[1:-1])))
    return lines


def is_direct_blake2(include_path: str) -> bool:
    return include_path.endswith("vendor/blake2.h") or include_path == "blake2.h"


def is_prelude(include_path: str) -> bool:
    name = Path(include_path).name
    return name in ("vendor_prelude.h", "vendor_prelude_solve.h")


def is_vendor_cuckatoo(include_path: str) -> bool:
    return include_path.endswith("vendor/cuckatoo.h")


def iter_source_files(repo_root: Path) -> list[Path]:
    try:
        output = subprocess.check_output(
            ["git", "-C", str(repo_root), "ls-files", "-z"],
            stderr=subprocess.DEVNULL,
        )
        rels = [item.decode("utf8") for item in output.split(b"\0") if item]
        candidates = [repo_root / item for item in rels]
    except (subprocess.CalledProcessError, FileNotFoundError, OSError):
        candidates = [path for path in repo_root.rglob("*") if path.is_file()]

    paths: list[Path] = []
    for path in candidates:
        if path.suffix not in SUFFIXES:
            continue
        try:
            relative = rel(path, repo_root)
        except ValueError:
            continue
        if any(relative.startswith(prefix) for prefix in SKIP_PREFIXES):
            continue
        paths.append(path)
    return sorted(paths)


def has_include(path: Path, predicate) -> bool:
    return any(predicate(include_path) for _, include_path in include_lines(path))


def check_tree(repo_root: Path) -> list[str]:
    violations: list[str] = []

    blake2_prelude = repo_root / BLAKE2_PRELUDE
    if not blake2_prelude.is_file():
        violations.append(f"{BLAKE2_PRELUDE.as_posix()}: required owned blake2 include is missing")
        return violations
    if not has_include(blake2_prelude, is_direct_blake2):
        violations.append(
            f"{BLAKE2_PRELUDE.as_posix()}: must include vendor/blake2.h "
            "(it is the one header that owns the MSVC C4804 sandwich)"
        )

    vendor_prelude = repo_root / VENDOR_PRELUDE
    if not vendor_prelude.is_file():
        violations.append(f"{VENDOR_PRELUDE.as_posix()}: required verify prelude is missing")
    elif not has_include(vendor_prelude, lambda p: Path(p).name == "blake2_prelude.h"):
        violations.append(
            f"{VENDOR_PRELUDE.as_posix()}: must include blake2_prelude.h so "
            "vendor/cuckatoo.h sees BLAKE2_H already defined"
        )

    vendor_prelude_solve = repo_root / VENDOR_PRELUDE_SOLVE
    if not vendor_prelude_solve.is_file():
        violations.append(f"{VENDOR_PRELUDE_SOLVE.as_posix()}: required solver prelude is missing")
    elif not has_include(vendor_prelude_solve, lambda p: Path(p).name == "blake2_prelude.h"):
        violations.append(
            f"{VENDOR_PRELUDE_SOLVE.as_posix()}: must include blake2_prelude.h so "
            "lean.cpp -> vendor/cuckatoo.h sees BLAKE2_H already defined"
        )

    for path in iter_source_files(repo_root):
        relative = rel(path, repo_root)
        seen_prelude = False
        for line_number, include_path in include_lines(path):
            if is_prelude(include_path):
                seen_prelude = True
            if is_direct_blake2(include_path) and relative != BLAKE2_PRELUDE.as_posix():
                violations.append(
                    f"{relative}:{line_number}: direct include of {include_path}; "
                    "use blake2_prelude.h"
                )
            if is_vendor_cuckatoo(include_path) and not seen_prelude:
                violations.append(
                    f"{relative}:{line_number}: vendor/cuckatoo.h without "
                    "vendor_prelude.h or vendor_prelude_solve.h earlier in this file"
                )
    return violations


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf8")


def create_valid_fixture(repo_root: Path) -> None:
    write(
        repo_root / BLAKE2_PRELUDE,
        '#include "vendor/blake2.h"     // trailing comment must not hide the include\n',
    )
    write(
        repo_root / VENDOR_PRELUDE,
        '#include "blake2_prelude.h"    // owns the MSVC C4804 sandwich\n'
        '#include "vendor/siphash.hpp"\n',
    )
    write(
        repo_root / VENDOR_PRELUDE_SOLVE,
        '#include "blake2_prelude.h"    // owns the MSVC C4804 sandwich\n',
    )
    write(
        repo_root / "src/crypto/cuckatoo/keys.cpp",
        '#include "blake2_prelude.h"\n',
    )
    write(
        repo_root / "src/crypto/cuckatoo/verify_19.cpp",
        '#include <crypto/cuckatoo/vendor_prelude.h>\n'
        '#include "vendor/cuckatoo.h"\n',
    )
    write(
        repo_root / "src/crypto/cuckatoo/solve_19.cpp",
        '#include <crypto/cuckatoo/vendor_prelude_solve.h>\n'
        '#include "vendor/lean.cpp"\n',
    )
    write(
        repo_root / "src/test/cuckatoo_verify_codes_tests.cpp",
        '#include <crypto/cuckatoo/vendor_prelude.h>\n'
        '#include <crypto/cuckatoo/vendor/cuckatoo.h>\n',
    )
    write(
        repo_root / "src/crypto/cuckatoo/vendor/cuckatoo.h",
        '#include "blake2.h"\n',
    )
    write(
        repo_root / "src/crypto/cuckatoo/vendor/blake2.h",
        "#ifndef BLAKE2_H\n#define BLAKE2_H\n#endif\n",
    )


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
    tmp = Path(tempfile.mkdtemp(prefix="cuckatoo-blake2-test-"))
    try:
        clean = tmp / "clean"
        create_valid_fixture(clean)
        failures.extend(expect_clean("clean fixture", clean))

        # Vendored blake2.h / cuckatoo.h includes are skipped. A fixture that
        # only has those must stay clean, otherwise the lint is fighting the
        # pin rather than enforcing the owned-header rule.
        vendor_only = tmp / "vendor_only"
        create_valid_fixture(vendor_only)
        failures.extend(expect_clean("vendor includes skipped", vendor_only))

        direct_quote = tmp / "direct_quote"
        create_valid_fixture(direct_quote)
        write(
            direct_quote / "src/crypto/cuckatoo/keys.cpp",
            '#include "vendor/blake2.h"\n',
        )
        failures.extend(expect_violation("direct vendor/blake2.h", direct_quote, "keys.cpp"))

        bare_blake2 = tmp / "bare_blake2"
        create_valid_fixture(bare_blake2)
        write(
            bare_blake2 / "src/crypto/cuckatoo/proofhash.cpp",
            '#include "blake2.h"\n',
        )
        failures.extend(expect_violation("bare blake2.h", bare_blake2, "proofhash.cpp"))

        angled = tmp / "angled"
        create_valid_fixture(angled)
        write(
            angled / "src/test/cuckatoo_tests.cpp",
            '#include <crypto/cuckatoo/vendor/blake2.h>\n',
        )
        failures.extend(expect_violation("angled vendor/blake2.h", angled, "cuckatoo_tests.cpp"))

        missing_prelude = tmp / "missing_prelude"
        create_valid_fixture(missing_prelude)
        write(
            missing_prelude / "src/crypto/cuckatoo/verify_19.cpp",
            '#include "vendor/cuckatoo.h"\n',
        )
        failures.extend(expect_violation(
            "cuckatoo.h without prelude", missing_prelude, "vendor/cuckatoo.h without"))

        prelude_after = tmp / "prelude_after"
        create_valid_fixture(prelude_after)
        write(
            prelude_after / "src/crypto/cuckatoo/verify_19.cpp",
            '#include "vendor/cuckatoo.h"\n'
            '#include <crypto/cuckatoo/vendor_prelude.h>\n',
        )
        failures.extend(expect_violation(
            "prelude after cuckatoo.h", prelude_after, "vendor/cuckatoo.h without"))

        solve_prelude_ok = tmp / "solve_prelude_ok"
        create_valid_fixture(solve_prelude_ok)
        write(
            solve_prelude_ok / "src/crypto/cuckatoo/extra.cpp",
            '#include <crypto/cuckatoo/vendor_prelude_solve.h>\n'
            '#include "vendor/cuckatoo.h"\n',
        )
        failures.extend(expect_clean("vendor_prelude_solve.h counts as prelude", solve_prelude_ok))

        empty_blake2_prelude = tmp / "empty_blake2_prelude"
        create_valid_fixture(empty_blake2_prelude)
        write(empty_blake2_prelude / BLAKE2_PRELUDE, "// no include\n")
        failures.extend(expect_violation(
            "blake2_prelude.h must include blake2.h", empty_blake2_prelude,
            "must include vendor/blake2.h"))

        dropped_from_verify_prelude = tmp / "dropped_from_verify_prelude"
        create_valid_fixture(dropped_from_verify_prelude)
        write(dropped_from_verify_prelude / VENDOR_PRELUDE, '#include "vendor/siphash.hpp"\n')
        failures.extend(expect_violation(
            "vendor_prelude.h must include blake2_prelude.h", dropped_from_verify_prelude,
            "vendor_prelude.h"))

        dropped_from_solve_prelude = tmp / "dropped_from_solve_prelude"
        create_valid_fixture(dropped_from_solve_prelude)
        write(dropped_from_solve_prelude / VENDOR_PRELUDE_SOLVE, "// no blake2\n")
        failures.extend(expect_violation(
            "vendor_prelude_solve.h must include blake2_prelude.h", dropped_from_solve_prelude,
            "vendor_prelude_solve.h"))
    finally:
        shutil.rmtree(tmp)

    if failures:
        print("\n".join(failures))
        return 1
    print("cuckatoo blake2 include self-tests passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Enforce that blake2.h is only included through blake2_prelude.h")
    parser.add_argument("--self-test", action="store_true", help="run fixture-based self-tests")
    args = parser.parse_args()
    if args.self_test:
        return run_self_tests()

    self_test_status = run_self_tests()
    if self_test_status != 0:
        return self_test_status

    violations = check_tree(Path.cwd())
    if violations:
        print("Cuckatoo blake2 include violations:")
        print("\n".join(violations))
        return 1
    print("Cuckatoo blake2 include OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
