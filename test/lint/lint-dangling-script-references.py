#!/usr/bin/env python3
#
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
#
# Check that tracked first-party text files do not reference deleted scripts.

import re
import subprocess
import sys
from pathlib import Path


SKIP_PREFIXES = (
    Path("src/crc32c"),
    Path("src/crypto/ctaes"),
    Path("src/leveldb"),
    Path("src/secp256k1"),
    Path("src/univalue"),
)

PATH_RE = re.compile(
    r"(?<![A-Za-z0-9_./-])"
    r"(?:\./|build/|\$\{BASE_ROOT_DIR\}/)?"
    r"((?:ci|contrib|test)/[A-Za-z0-9_./-]+\.(?:py|sh))"
    r"(?![A-Za-z0-9_./-])"
)

# Functional tests are routinely cited by bare filename, with no directory
# prefix for PATH_RE to anchor on. Match on the documented area prefixes from
# test/functional/README.md so this stays tight enough not to flag arbitrary
# Python filenames.
FUNCTIONAL_AREAS = (
    "calibration",
    "feature",
    "interface",
    "mining",
    "p2p",
    "relaypool",
    "rpc",
    "tool",
    "vault",
)

BARE_FUNCTIONAL_RE = re.compile(
    r"(?<![A-Za-z0-9_./-])"
    r"((?:" + "|".join(FUNCTIONAL_AREAS) + r")_[A-Za-z0-9_]+\.py)"
    r"(?![A-Za-z0-9_./-])"
)

SELF_PATH = Path("test/lint/lint-dangling-script-references.py")

# Intentional fixture strings that look like missing source-tree paths.
ALLOWED_MISSING = {
    (SELF_PATH, "test/functional/x.py"),
    (Path("test/lint/lint-quicksilver-fee-residue.py"), "test/functional/x.py"),
    # Naming-guideline counter-examples: the README cites these precisely
    # because they are the wrong name and must NOT exist. The self-entries
    # cover this file quoting those same names just above.
    (Path("test/functional/README.md"), "test/functional/rpc_decode_script.py"),
    (Path("test/functional/README.md"), "test/functional/interface_zmq_test.py"),
    (SELF_PATH, "test/functional/rpc_decode_script.py"),
    (SELF_PATH, "test/functional/interface_zmq_test.py"),
    # Negative fixtures naming a helper this sweep deleted.
    (SELF_PATH, "contrib/devtools/test_deterministic_coverage.sh"),
    (Path("test/lint/lint-quicksilver-dead-surface-residue.py"), "contrib/devtools/test_deterministic_coverage.sh"),
    # The dead-surface rule that forbids the pre-segwit node-upgrade test from
    # coming back has to quote the name it forbids, and so does its self-test
    # case. The script is gone precisely because that rule wants it gone.
    (Path("test/lint/lint-quicksilver-dead-surface-residue.py"), "test/functional/feature_presegwit_node_upgrade.py"),
    (SELF_PATH, "test/functional/feature_presegwit_node_upgrade.py"),
}


def repo_root() -> Path:
    return Path(
        subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"], text=True, encoding="utf8"
        ).strip()
    )


def tracked_files(root: Path) -> list[Path]:
    raw = subprocess.check_output(
        ["git", "ls-files", "-z"], text=True, encoding="utf8"
    ).split("\0")
    paths = []
    for name in raw:
        if not name:
            continue
        relpath = Path(name)
        if any(relpath == prefix or relpath.is_relative_to(prefix) for prefix in SKIP_PREFIXES):
            continue
        paths.append(root / relpath)
    return paths


def is_binary(path: Path) -> bool:
    try:
        return b"\0" in path.read_bytes()[:8192]
    except OSError:
        return False


def is_allowed_missing(root: Path, source_relpath: Path, candidate: str) -> bool:
    if (source_relpath, candidate) in ALLOWED_MISSING:
        return True
    # Some helper scripts are generated from tracked .in templates; the
    # template is the source-tree file that must exist.
    return (root / f"{candidate}.in").exists()


def main() -> int:
    root = repo_root()
    failures = []

    for path in tracked_files(root):
        if not path.exists() or is_binary(path):
            continue
        source_relpath = path.relative_to(root)
        try:
            lines = path.read_text(encoding="utf8", errors="replace").splitlines()
        except OSError as err:
            print(f"failed to read {source_relpath}: {err}", file=sys.stderr)
            return 1
        for line_number, line in enumerate(lines, start=1):
            for match in PATH_RE.finditer(line):
                candidate = match.group(1)
                if (root / candidate).exists():
                    continue
                if is_allowed_missing(root, source_relpath, candidate):
                    continue
                failures.append(f"{source_relpath}:{line_number}: missing {candidate}")
            for match in BARE_FUNCTIONAL_RE.finditer(line):
                if (path.parent / match.group(1)).exists():
                    continue
                candidate = f"test/functional/{match.group(1)}"
                if (root / candidate).exists():
                    continue
                if is_allowed_missing(root, source_relpath, candidate):
                    continue
                failures.append(f"{source_relpath}:{line_number}: missing {candidate}")

    if failures:
        print("Dangling script references remain:")
        print("\n".join(failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
