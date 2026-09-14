#!/usr/bin/env python3
#
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
#
# Check that public help/config artifacts advertise Quicksilver's per-network
# onion service target ports rather than the inherited P2P+1 derivation.

import subprocess
import sys
from pathlib import Path


SOURCE_SCOPE = Path("src/init.cpp")

ARTIFACT_SCOPES = [
    Path("share/examples/quicksilver.conf"),
    Path("doc/man/quicksilverd.1"),
    Path("doc/man/quicksilver-qt.1"),
]

STALE_DEFAULTS = [
    "127.0.0.1:19558=onion, sandbox: 127.0.0.1:19557=onion",
    "publictestChainParams->GetDefaultPort() + 1",
    "sandboxChainParams->GetDefaultPort() + 1",
    "If set to\n# -port is set explicitly",
    "If set to\n\\fB\\-port\\fR is set explicitly",
]

REQUIRED_DEFAULTS = [
    "127.0.0.1:9556=onion",
    "127.0.0.1:19559=onion",
    "127.0.0.1:19555=onion",
]


def repo_root() -> Path:
    return Path(subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True, encoding="utf8").strip())


def main() -> int:
    root = repo_root()
    failures = []

    source_text = (root / SOURCE_SCOPE).read_text(encoding="utf8", errors="replace")
    for stale in STALE_DEFAULTS:
        if stale in source_text:
            failures.append(f"{SOURCE_SCOPE}: stale onion default remains: {stale}")
    for required in [
        "defaultChainParams->OnionServicePort()",
        "publictestChainParams->OnionServicePort()",
        "sandboxChainParams->OnionServicePort()",
        "If -port is set explicitly to a value x",
    ]:
        if required not in source_text:
            failures.append(f"{SOURCE_SCOPE}: missing source help guard: {required}")

    for relpath in ARTIFACT_SCOPES:
        path = root / relpath
        text = path.read_text(encoding="utf8", errors="replace")
        for stale in STALE_DEFAULTS:
            if stale in text:
                failures.append(f"{relpath}: stale onion default remains: {stale}")
        for required in REQUIRED_DEFAULTS:
            if required not in text:
                failures.append(f"{relpath}: missing onion default: {required}")

    if failures:
        print("Quicksilver onion-port help residue remains:")
        print("\n".join(failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
