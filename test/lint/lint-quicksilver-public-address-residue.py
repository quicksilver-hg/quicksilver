#!/usr/bin/env python3
#
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
#
# Check that public help, GUI placeholders, and user docs do not advertise
# retired bech32 HRPs after Quicksilver address parameters are active.

import re
import subprocess
import sys
from pathlib import Path


# Scope is narrow on purpose, and was re-measured on 2026-08-25: address-shaped
# strings are everywhere in test vectors and fixtures, so tree-wide these rules
# report 164 findings and none is residue. The listed paths are the places a
# stale address would actually reach a reader.
SCOPES = [
    Path("src/rpc"),
    Path("src/vault/rpc"),
    Path("src/qt/guiutil.cpp"),
    Path("src/qt/test/uritests.cpp"),
    Path("src/qt/forms/receiverequestdialog.ui"),
    Path("doc"),
    Path("contrib/seeds/README.md"),
    Path("share"),
    Path("README.md"),
    Path("INSTALL.md"),
    Path("CONTRIBUTING.md"),
    Path("SECURITY.md"),
    Path("src/bench"),
    Path("src/univalue/test"),
    Path("test/functional"),
]

SKIP_PATHS = {
    Path("test/lint/lint-quicksilver-public-address-residue.py"),
    # Serialized PSQT fixture whose outputs still decode to historical bcrt1 scripts.
    Path("test/functional/data/rpc_psqt.json"),
    # Decoder still names foreign HRPs so it can reject them.
    Path("test/functional/test_framework/address.py"),
    # Keeps one bc1 string as an explicit wrong-prefix fixture.
    Path("test/functional/rpc_invalid_address_message.py"),
}

FORBIDDEN = re.compile(
    r"\b(?:bc1|tb1|bcrt1)"
    r"|(?:1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX|175tWpb8K1S7NmH4Zx6rewF9WQrcZv245W|17VZNX1SN5NtKa8UQFxwQbFeFc3iqRYhem|3J98t1WpEZ73CNmQviecrnyiWrnqRhWNL|12cbQLTFMXRnSzktFkuoG3eHoMeFtpTu3S)",
    re.IGNORECASE,
)


def repo_root() -> Path:
    return Path(subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True, encoding="utf8").strip())


def tracked_files(root: Path) -> list[Path]:
    raw = subprocess.check_output(["git", "ls-files", "-z"], text=True, encoding="utf8").split("\0")
    paths = []
    for name in raw:
        if not name:
            continue
        relpath = Path(name)
        if relpath in SKIP_PATHS:
            continue
        if any(relpath == scope or relpath.is_relative_to(scope) for scope in SCOPES):
            paths.append(root / relpath)
    return paths


def is_binary(path: Path) -> bool:
    try:
        return b"\0" in path.read_bytes()[:8192]
    except OSError:
        return False


def main() -> int:
    root = repo_root()
    failures = []
    for path in tracked_files(root):
        if not path.exists():
            continue
        if is_binary(path):
            continue
        relpath = path.relative_to(root)
        for line_number, line in enumerate(path.read_text(encoding="utf8", errors="replace").splitlines(), start=1):
            if FORBIDDEN.search(line):
                failures.append(f"{relpath}:{line_number}: {line.strip()}")

    if failures:
        print("Public retired bech32 address residue remains:")
        print("\n".join(failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
