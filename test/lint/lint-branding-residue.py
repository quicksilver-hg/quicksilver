#!/usr/bin/env python3
#
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
#
# Check that surviving non-Qt Bitcoin-derived residue is explicitly classified.

import re
import subprocess
import sys
from pathlib import Path


EXCLUDED_PREFIXES = (
    "research/tier0-pow/",
    "src/qt/",
    "src/secp256k1/",
    "src/leveldb/",
    "src/crc32c/",
    "src/univalue/",
    "src/crypto/ctaes/",
)

SEARCH_RE = re.compile(r"\b(?:bitcoin|satoshi|nakamoto)\b", re.IGNORECASE)

ALLOW_CATEGORIES = {
    "LEGAL_ATTRIBUTION": [
        re.compile(r"Copyright .*Bitcoin", re.IGNORECASE),
        re.compile(r"Copyright .*Satoshi Nakamoto", re.IGNORECASE),
        re.compile(r"Satoshi Nakamoto"),
        re.compile(r"Bitcoin Core developers"),
        re.compile(r"Bitcoin Core Developers"),
        re.compile(r"Bitcoin Developers"),
        re.compile(r"Bitcoin Core copyright"),
        re.compile(r"Upstream Bitcoin Core attribution"),
        re.compile(r"bitcoin-core-dev", re.IGNORECASE),
        re.compile(r"BITCOIN_CORE_ANCHOR_RE"),
        re.compile(r"file_has_bitcoin_core_anchor"),
        re.compile(r"The following files carry a Bitcoin Core copyright"),
    ],
    "UPSTREAM_URL": [
        re.compile(r"https?://[^ \t\"'<>]*bitcoin", re.IGNORECASE),
        re.compile(r"git@github\.com:bitcoin/", re.IGNORECASE),
        re.compile(r"bitcoin-dev", re.IGNORECASE),
        re.compile(r"bitcointalk", re.IGNORECASE),
        re.compile(r"bitcoincore\.org", re.IGNORECASE),
    ],
    "DEFERRED_STANDARD_TERM": [
        re.compile(r"\bBIP ?\d+\b"),
        re.compile(r"\bBIP-\d+\b"),
    ],
    "UPSTREAM_LINEAGE_TEXT": [
        re.compile(r"Bitcoin Core", re.IGNORECASE),
        re.compile(r"stock Bitcoin", re.IGNORECASE),
        re.compile(r"upstream(?:'s)? Bitcoin", re.IGNORECASE),
        re.compile(r"inherited Bitcoin", re.IGNORECASE),
        re.compile(r"Bitcoin-era", re.IGNORECASE),
        re.compile(r"old Bitcoin cap", re.IGNORECASE),
        re.compile(r"Bitcoin'?s familiar", re.IGNORECASE),
        re.compile(r"Bitcoin'?s 1209600s", re.IGNORECASE),
        re.compile(r"Bitcoin 0\.8", re.IGNORECASE),
        re.compile(r"historical Bitcoin", re.IGNORECASE),
        re.compile(r"old Bitcoin 210000", re.IGNORECASE),
        re.compile(r"Bitcoin would reject", re.IGNORECASE),
        re.compile(r"Bitcoin subsidy", re.IGNORECASE),
        re.compile(r"Bitcoin fee/conflict policy", re.IGNORECASE),
        re.compile(r"Bitcoin transaction serialization", re.IGNORECASE),
        re.compile(r"network names from Bitcoin", re.IGNORECASE),
        re.compile(r"describes how Bitcoin", re.IGNORECASE),
        re.compile(r"Bitcoin'?s continuous integration", re.IGNORECASE),
        re.compile(r"divergence from Bitcoin", re.IGNORECASE),
        re.compile(r"CPFP exists in Bitcoin", re.IGNORECASE),
        re.compile(r"not historically been enforced in Bitcoin", re.IGNORECASE),
        re.compile(r"As Bitcoin relies", re.IGNORECASE),
        re.compile(r"Satoshi's original implementation", re.IGNORECASE),
        re.compile(r"Satoshi codebase", re.IGNORECASE),
        re.compile(r"length-prefixed bitcoin strings", re.IGNORECASE),
        re.compile(r"Pre-version-0\.6, Bitcoin", re.IGNORECASE),
        re.compile(r"Weaknesses in Bitcoin", re.IGNORECASE),
        re.compile(r"Eclipse Attacks on Bitcoin", re.IGNORECASE),
        re.compile(r"python-bitcoinlib", re.IGNORECASE),
        # Sub-project B (transaction cost and chain growth) argues from what the
        # inherited design does and Quicksilver deliberately does not. These are
        # narrow phrases, not a blanket allowance for the word.
        re.compile(r"Bitcoin'?s fee market", re.IGNORECASE),
        re.compile(r"Bitcoin'?s weight is", re.IGNORECASE),
        re.compile(r"Bitcoin commits to txid", re.IGNORECASE),
        # BIP 158 conformance (src/test/blockfilter_tests.cpp) argues from the format the
        # vectors are recorded in against the formats we have since diverged to. Narrow
        # phrases, not a blanket allowance for the word.
        re.compile(r"in Bitcoin'?s serialization", re.IGNORECASE),
        re.compile(r"diverged from Bitcoin'?s serialization", re.IGNORECASE),
        re.compile(r"over ten fixed Bitcoin blocks", re.IGNORECASE),
    ],
    "GENESIS_PROVENANCE_TEXT": [
        re.compile(r"no Bitcoin-linked residue", re.IGNORECASE),
        re.compile(r"Bitcoin's genesis `?nBits`?, historically embedded", re.IGNORECASE),
        re.compile(r"Bitcoin's genesis coinbase", re.IGNORECASE),
        re.compile(r"Bitcoin residue baked into the Quicksilver chain root", re.IGNORECASE),
        re.compile(r"built with Bitcoin's `?0x1d00ffff`?", re.IGNORECASE),
        re.compile(r"shipped Bitcoin's 0x1d00ffff", re.IGNORECASE),
        re.compile(r"applies Bitcoin's", re.IGNORECASE),
        re.compile(r"demanded Bitcoin difficulty-1", re.IGNORECASE),
        re.compile(r"genesis_coinbase_carries_no_bitcoin_magic"),
        re.compile(r"No Bitcoin nBits magic", re.IGNORECASE),
        re.compile(r"NOT a Bitcoin hash", re.IGNORECASE),
        re.compile(r"Bitcoin-linked value must not appear", re.IGNORECASE),
        re.compile(r"\bbitcoin_magic\b"),
        re.compile(r"Bitcoin genesis nBits magic found in coinbase scriptSig", re.IGNORECASE),
        re.compile(r"dropped the Bitcoin 0x1D00FFFF", re.IGNORECASE),
    ],
    "COMPAT_INFRA": [
        re.compile(r"bitcoin/bitcoin"),
        re.compile(r"bitcoin-core/", re.IGNORECASE),
        re.compile(r"/bitcoin\b"),
        re.compile(r"bitcoin-fork"),
        re.compile(r"bitcoin-wizards"),
    ],
    "TEST_FIXTURE_LITERAL": [
        re.compile(r"pw = bitcoin"),
        re.compile(r"call_with_auth\(.*'bitcoin'\)"),
    ],
    "LINT_PATTERN": [
        re.compile(r"renamed Bitcoin bitmap residue"),
        re.compile(r"OLD_BITCOIN_DOXYGEN_SHA256"),
        re.compile(r"renamed Bitcoin Doxygen logo"),
        re.compile(r"Check that public docs and RPC help do not advertise Bitcoin's default P2P"),
        re.compile(r"Public stale Bitcoin P2P port residue remains"),
        re.compile(r"r\"\\bBitcoin"),
        re.compile(r"r\"\\bBITCOIN"),
        re.compile(r"r\"\\bBitcoins"),
        re.compile(r"r\"\\bbitcoin"),
        re.compile(r"r\"\\b\(\?:bitcoin"),
        re.compile(r"Bitcoin denomination", re.IGNORECASE),
        re.compile(r"Bitcoin monetary unit", re.IGNORECASE),
        re.compile(r"Bitcoin monetary-unit", re.IGNORECASE),
        re.compile(r"Bitcoin-unit residue lint self-test", re.IGNORECASE),
        re.compile(r"Bitcoin-denomination residue", re.IGNORECASE),
        re.compile(r"plain \"Bitcoin\" branding scan", re.IGNORECASE),
        re.compile(r"source comments, tests, docs, or public artifacts", re.IGNORECASE),
        re.compile(r"Full-token Bitcoin currency abbreviations", re.IGNORECASE),
        re.compile(r"bitcoin\\\|bitcoins\\\|btc\\\|satoshi", re.IGNORECASE),
        re.compile(r"Bitcoin'?s genesis nBits", re.IGNORECASE),
        re.compile(r"Bitcoin'?s genesis coinbase", re.IGNORECASE),
        re.compile(r"Bitcoin-linked value", re.IGNORECASE),
        re.compile(r"Bitcoin genesis nBits magic found", re.IGNORECASE),
        re.compile(r"shipped Bitcoin'?s 0x1d00ffff", re.IGNORECASE),
        re.compile(r"Bitcoin'\?s genesis nBits", re.IGNORECASE),
        re.compile(r"Bitcoin'\?s genesis coinbase", re.IGNORECASE),
        re.compile(r"shipped Bitcoin'\?s 0x1d00ffff", re.IGNORECASE),
        # The BIP 158 conformance phrases, as they appear escaped in the unit-residue
        # linter's own allow list.
        re.compile(r"in Bitcoin'\?s serialization", re.IGNORECASE),
        re.compile(r"diverged from Bitcoin'\?s serialization", re.IGNORECASE),
        re.compile(r"Copyright \.\* Satoshi Nakamoto", re.IGNORECASE),
        re.compile(r"r\\?\"Satoshi Nakamoto\\?\""),
        re.compile(r'return "satoshi"'),
        re.compile(r"BITCOIN_TERM_VARIANT_RESIDUE"),
        re.compile(r"lint-quicksilver-bitcoin-(?:term-variants|unit-residue|source-unit-residue)\\.py"),
        # The same script names, unescaped, as plain paths: test/lint/first-party-files.txt
        # has to list them, and they are named for the residue they hunt.
        re.compile(r"^test/lint/lint-quicksilver-bitcoin-(?:term-variants|unit-residue|source-unit-residue)\.py$"),
    ],
    "LINT_SELF": [
        re.compile(r"SEARCH_RE = re\.compile"),
        re.compile(r"re\.compile\(r\".*bitcoin", re.IGNORECASE),
        re.compile(r"re\.compile\(r'.*bitcoin", re.IGNORECASE),
        re.compile(r"# Check that surviving non-Qt Bitcoin-derived residue is explicitly classified\."),
        re.compile(r'print\("Uncategorized Bitcoin-derived residue remains:"\)'),
    ],
}


def repo_root() -> Path:
    return Path(subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True, encoding="utf8").strip())


def tracked_files(root: Path) -> list[Path]:
    raw = subprocess.check_output(["git", "ls-files", "-z"], text=True, encoding="utf8").split("\0")
    paths = []
    for rel in raw:
        if not rel or rel.startswith(EXCLUDED_PREFIXES):
            continue
        paths.append(root / rel)
    return paths


def classify(relpath: str, line: str) -> str | None:
    for category, patterns in ALLOW_CATEGORIES.items():
        if category == "LINT_SELF" and relpath != "test/lint/lint-branding-residue.py":
            continue
        if any(pattern.search(line) for pattern in patterns):
            return category
    return None


def is_binary(path: Path) -> bool:
    try:
        return b"\0" in path.read_bytes()[:8192]
    except OSError:
        return False


def main() -> int:
    root = repo_root()
    counts = {category: 0 for category in ALLOW_CATEGORIES}
    uncategorized = []

    for path in tracked_files(root):
        relpath = path.relative_to(root).as_posix()
        if not path.exists():
            continue
        if is_binary(path):
            continue
        try:
            lines = path.read_text(encoding="utf8", errors="replace").splitlines()
        except OSError as err:
            print(f"failed to read {relpath}: {err}", file=sys.stderr)
            return 1
        for line_number, line in enumerate(lines, start=1):
            if not SEARCH_RE.search(line):
                continue
            category = classify(relpath, line)
            if category is None:
                uncategorized.append(f"{relpath}:{line_number}: {line.strip()}")
            else:
                counts[category] += 1

    for category in sorted(counts):
        print(f"{category}={counts[category]}")
    print(f"UNCATEGORIZED={len(uncategorized)}")
    if uncategorized:
        print("Uncategorized Bitcoin-derived residue remains:")
        print("\n".join(uncategorized))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
