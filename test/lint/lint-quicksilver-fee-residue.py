#!/usr/bin/env python3
#
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
#
# Check that removed no-fee Quicksilver fee surfaces do not return as live
# source, RPC help, functional-test output fields, or policy docs.
#
# Rule families run against different scopes:
#   * removed fee RPC names across every configured scope,
#   * broad source rules over src/ (with per-file allowlists for documented
#     comments), and
#   * narrow output-field rules over test/functional, test/lint and doc/policy
#     that catch a live "fee"/"fees" dict key or RPC output field re-appearing.
#
# Every rule is matched per line and again over the file joined into one
# whitespace-normalised string, so a banned phrase that wraps across a line
# break cannot hide from it. See test/lint/lint_wrapped_prose.py.
#
# Run `--self-test` to exercise the matcher without touching the tree.

import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.append(str(Path(__file__).parent))
from lint_wrapped_prose import WrappedText, report_self_test  # noqa: E402


@dataclass(frozen=True)
class Rule:
    name: str
    pattern: re.Pattern[str]
    # If set, the rule only applies to POSIX rel-paths matching this pattern.
    path_scope: "re.Pattern[str] | None" = None


# There is deliberately no scope allowlist here. A list of directories to scan
# is half of this lint's assertion, and it is the half that fails silently: a
# green run over the wrong file set cannot be told from a green run over a
# clean tree. Every tracked file is scanned instead, minus the vendored trees
# below and the files that must quote the forbidden tokens as literals.

SKIP_PREFIXES = [
    Path("src/crc32c"),
    Path("src/crypto/ctaes"),
    Path("src/leveldb"),
    Path("src/secp256k1"),
    Path("src/univalue"),
]

# Whole files excluded from scanning: fee-residue linters necessarily embed the
# very tokens they hunt for as literals.
SKIP_FILES = {
    Path("test/lint/lint-quicksilver-fee-residue.py"),
    Path("test/lint/lint-stale-chain-fee-residue.py"),
    Path("test/functional/p2p_segwit.py"),
}

SRC = re.compile(r"^src/")
TEST_AND_DOCS = re.compile(r"^(?:test/functional|test/lint|doc/policy)/")
# The functional test framework is the suite's model of *our* protocol, so a
# removed wire message must not survive there either -- see the msg_feefilter
# purge. Individual tests are deliberately out of this scope: p2p_feefilter.py
# and rpc_net.py have to name the message to assert that it is absent.
SRC_AND_FRAMEWORK = re.compile(r"^(?:src/|test/functional/test_framework/)")

RULES = [
    # --- removed RPC names (all configured scopes) ---
    Rule("removed fee RPC", re.compile(r"\b(?:bumpfee|psqtbumpfee|settxfee|estimatesmartfee|estimaterawfee|prioritisetransaction|getprioritisedtransactions)\b")),
    Rule("fee-named functional helper", re.compile(r"\b(?:def |self\.)test_(?:fee_[A-Za-z0-9_]*|[A-Za-z0-9_]*_fee)\b"), TEST_AND_DOCS),

    # --- broad source rules (src/ only) ---
    Rule("removed public fee field", re.compile(r'(?<![A-Za-z0-9_])"(?:fee|fees|total_fee)"(?![A-Za-z0-9_])'), SRC),
    Rule("removed relay pool fee field", re.compile(r"\b(?:nFee|m_modified_fee|m_modified_fees|m_total_modified_fees|m_conflicting_fees|m_wtxids_fee_calculations|nModFeesWithAncestors|nModFeesWithDescendants|m_base_fees|base_fees|total_fee|modifiedfee|modified_fee|ancestorfees|descendantfees|nFeeDelta|override_min_fee)\b"), SRC),
    Rule("removed relay pool fee accessor", re.compile(r"\b(?:GetFee|GetModifiedFee|GetModFeesWithAncestors|GetModFeesWithDescendants|GetTotalFee)\s*\("), SRC),
    Rule("removed fee-delta surface", re.compile(r"\b(?:fee_delta|apply_fee_delta_priority|getTxFees)\b"), SRC),
    Rule("removed fee-estimation residue", re.compile(r"\b(?:IsCurrentForFeeEstimation|MAX_FEE_ESTIMATION_TIP_AGE|m_chainstate_is_current|chainstate_is_current|TxConfirmStats)\b"), SRC),
    Rule("removed vault fee option", re.compile(r"\b(?:maxapsfee|subtractfeefromamount|subtractfeefrom|subtractFeeFromAmount|subtractFeeFromOutputs|subtract_fee_from_outputs|fSubtractFeeFromAmount|m_subtract_fee_outputs)\b"), SRC),
    Rule(
        "removed vault fee configuration constant",
        re.compile(
            r"\b(?:DEFAULT_PAY_TX_FEE|DEFAULT_FALLBACK_FEE|DEFAULT_DISCARD_FEE|"
            r"DEFAULT_TRANSACTION_MINFEE|DEFAULT_CONSOLIDATE_FEERATE|DEFAULT_TX_CONFIRM_TARGET|"
            r"DEFAULT_TRANSACTION_MAXFEE|HIGH_TX_FEE_PER_KB|HIGH_MAX_TX_FEE)\b"
        ),
        SRC,
    ),
    # RPC output-field construction anywhere in src.
    Rule("removed fee output field", re.compile(r'\b(?:pushKV|push_back)\s*\(\s*["\']fees?["\']'), SRC),
    Rule("removed fee RPCResult", re.compile(r'\bRPCResult\b[^\n]*["\']fees?["\']'), SRC),
    Rule("removed feefilter message", re.compile(r"\b(?:FEEFILTER|feefilter)\b"), SRC_AND_FRAMEWORK),

    # --- narrow output-field rules (test/functional, test/lint, doc/policy) ---
    Rule("live fee output field", re.compile(r'["\']fees?["\']\s*:'), TEST_AND_DOCS),
    Rule("live fee output field", re.compile(r'\[\s*["\']fees?["\']\s*\]'), TEST_AND_DOCS),
    Rule("removed fee filter output field", re.compile(r'(?:\[\s*["\']minfeefilter["\']\s*\]|["\']minfeefilter["\']\s*:)'), TEST_AND_DOCS),
]

ALLOWED = {
    # Negative coverage for removed RPCs and public output fields.
    Path("test/functional/feature_quicksilver_no_fee_rpcs.py"): [
        re.compile(r"\b(?:bumpfee|psqtbumpfee|settxfee|estimatesmartfee|estimaterawfee|prioritisetransaction|getprioritisedtransactions)\b"),
        re.compile(r"\bfee\b"),
    ],
    Path("test/functional/feature_quicksilver_feeless.py"): [
        re.compile(r'assert "fees" not in '),
        re.compile(r"\bzero fee\b"),
        re.compile(r"\bzero-fee\b"),
    ],
    Path("test/functional/feature_quicksilver_gbt.py"): [
        re.compile(r'assert "fees" not in '),
    ],
    Path("test/functional/feature_quicksilver_mine.py"): [
        re.compile(r'assert "fees" not in '),
    ],
    Path("test/functional/interface_quicksilver_cli.py"): [
        re.compile(r"assert .*paytxfee.* not in "),
    ],
    Path("test/functional/rpc_psqt.py"): [
        re.compile(r'assert "fee" not in '),
        re.compile(r'f"\{old_format\}bumpfee"'),
    ],
    Path("test/functional/rpc_packages.py"): [
        re.compile(r'assert "fees" not in '),
    ],
    Path("test/functional/vault_create_tx.py"): [
        re.compile(r'assert "fee" not in '),
    ],
    Path("test/functional/vault_fundrawtransaction.py"): [
        re.compile(r'assert "fee" not in '),
        re.compile(r'assert "fees" not in '),
        re.compile(r"\bno min relay fee\b"),
    ],
    Path("test/functional/vault_keypool.py"): [
        re.compile(r'assert "fee" not in '),
    ],
    Path("test/functional/vault_multivault.py"): [
        re.compile(r"assert .*paytxfee.* not in "),
    ],
    Path("test/functional/vault_send.py"): [
        re.compile(r'assert "fees" not in '),
    ],
    Path("test/functional/vault_sendall.py"): [
        re.compile(r'assert "fee" not in '),
        re.compile(r"\bdoes not create a fee\b"),
    ],
    Path("test/functional/vault_sendmany.py"): [
        re.compile(r'assert "fee" not in '),
    ],
    Path("test/functional/vault_simulaterawtx.py"): [
        re.compile(r'assert "fee" not in '),
    ],
    Path("test/functional/vault_txn_doublespend.py"): [
        re.compile(r'assert "fee" not in '),
    ],
    Path("src/test/net_tests.cpp"): [
        re.compile(r'ALL_NET_MESSAGE_TYPES, "feefilter"'),
    ],
}


def repo_root() -> Path:
    return Path(subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True, encoding="utf8").strip())


def tracked_files(root: Path) -> list[Path]:
    raw = subprocess.check_output(["git", "ls-files", "-z"], text=True, encoding="utf8").split("\0")
    paths = []
    for name in raw:
        if not name:
            continue
        relpath = Path(name)
        if relpath in SKIP_FILES:
            continue
        if any(relpath == prefix or relpath.is_relative_to(prefix) for prefix in SKIP_PREFIXES):
            continue
        paths.append(root / relpath)
    return paths


def is_binary(path: Path) -> bool:
    try:
        return b"\0" in path.read_bytes()[:8192]
    except OSError:
        return False


def line_allowed(relpath: Path, line: str) -> bool:
    return any(pattern.search(line) for pattern in ALLOWED.get(relpath, []))


def match_rule(relpath: str, line: str) -> "Rule | None":
    for rule in RULES:
        if rule.path_scope is not None and not rule.path_scope.search(relpath):
            continue
        if rule.pattern.search(line):
            return rule
    return None


def wrapped_failures(relpath: Path, rel: str, lines: list[str]) -> list[str]:
    """Rule matches that straddle a line break, which match_rule cannot see.

    First rule wins per starting line, as in the per-line pass, and a finding
    is dropped when any line it spans carries an exemption. See
    test/lint/lint_wrapped_prose.py for why exemptions stay per-line.
    """
    failures = []
    reported: set[int] = set()
    joined = WrappedText(lines)
    for rule in RULES:
        if rule.path_scope is not None and not rule.path_scope.search(rel):
            continue
        for match in joined.matches(rule.pattern):
            if match.first_line in reported:
                continue
            span = lines[match.first_line - 1:match.last_line]
            if any(line_allowed(relpath, line) for line in span):
                continue
            reported.add(match.first_line)
            failures.append(
                f"{relpath}:{match.first_line}: {rule.name}: "
                f"{match.text.strip()} ({match.where()})"
            )
    return failures


def self_test() -> int:
    matcher_failures = report_self_test()
    # (rel-path, line, expected rule name or None)
    cases = [
        # Removed RPC names fire in every configured scope.
        ("src/rpc/client.cpp", '"bumpfee",', "removed fee RPC"),
        ("test/functional/vault_basic.py", "node.bumpfee(txid)", "removed fee RPC"),
        ("test/functional/vault_fundrawtransaction.py", "    def test_fee_p2pkh(self):", "fee-named functional helper"),
        ("test/functional/vault_fundrawtransaction.py", "        self.test_many_inputs_fee()", "fee-named functional helper"),
        ("doc/policy/relaypool.md", "Do not call settxfee.", "removed fee RPC"),
        # Other broad source rules remain limited to src/.
        ("src/rpc/relaypool.cpp", 'entry.pushKV("fee", fee);', "removed public fee field"),
        ("src/txrelaypool.h", "CAmount m_base_fees;", "removed relay pool fee field"),
        ("src/vault/spend.cpp", "if (fSubtractFeeFromAmount) {", "removed vault fee option"),
        ("src/vault/vault.h", "static const CAmount DEFAULT_PAY_TX_FEE{0};", "removed vault fee configuration constant"),
        ("src/rpc/mining.cpp", "return pool.GetFee(txid);", "removed relay pool fee accessor"),
        ("src/policy/fees.cpp", "CAmount TxConfirmStats::EstimateMedianVal(double, unsigned int) const", "removed fee-estimation residue"),
        # bare "fee" mention in a src comment is only caught if quoted-field; prose isn't
        ("src/foo.cpp", "// this used to compute the fee", None),
        # narrow test rules: a live output field is residue
        ("test/functional/vault_basic.py", 'assert_equal(res["fee"], 0)', "live fee output field"),
        ("test/functional/vault_basic.py", 'x = tx["fees"]["base"]', "live fee output field"),
        ("test/functional/rpc_net.py", 'assert_equal(peer["minfeefilter"], Decimal("0E-8"))', "removed fee filter output field"),
        ("src/protocol.h", 'inline constexpr const char* FEEFILTER{"feefilter"};', "removed feefilter message"),
        ("src/net.cpp", "    NetMsgType::FEEFILTER,", "removed feefilter message"),
        ("test/functional/test_framework/messages.py", '    msgtype = b"feefilter"', "removed feefilter message"),
        ("test/functional/test_framework/p2p.py", '    b"feefilter": msg_feefilter,', "removed feefilter message"),
        # ...but a test that asserts the absence of the message must name it.
        ("test/functional/p2p_feefilter.py", 'assert "feefilter" not in recv', None),
        ("test/functional/rpc_net.py", 'assert "feefilter" not in peer_info[0][0]["bytessent_per_msg"]', None),
        ("src/net.cpp", '    "", // BIP324 id 5; unused so later short ids stay stable', None),
        # negative assertions are NOT residue (no colon / not an index read of a live field)
        ("test/functional/vault_basic.py", 'assert "fee" not in res', None),
        # Other src rules must NOT fire on test paths.
        ("test/functional/x.py", "m_base_fees = 3", None),
        # doc prose is not a live field
        ("doc/policy/relaypool-replacements.md", "a higher fee than the original", None),
    ]
    failures = []
    for rel, line, expected in cases:
        rule = match_rule(rel, line)
        got = rule.name if rule else None
        if got != expected:
            failures.append(f"{rel}: {line!r} expected {expected!r}, got {got!r}")
    allowance_cases = [
        (Path("test/functional/feature_quicksilver_no_fee_rpcs.py"), '("bumpfee", [txid])', True),
        (Path("test/functional/vault_listsinceblock.py"), "node.bumpfee(txid)", False),
    ]
    for rel, line, expected in allowance_cases:
        got = line_allowed(rel, line)
        if got != expected:
            failures.append(f"{rel}: {line!r} allowance expected {expected!r}, got {got!r}")
    if failures:
        print("Fee-residue lint self-test failures:")
        print("\n".join(failures))
        return 1
    if matcher_failures != 0:
        return 1
    print("Fee-residue lint self-test OK")
    return 0


def main() -> int:
    if sys.argv[1:] == ["--self-test"]:
        return self_test()
    if len(sys.argv) != 1:
        print(f"Usage: {sys.argv[0]} [--self-test]", file=sys.stderr)
        return 2

    if report_self_test() != 0:
        return 1

    root = repo_root()
    failures = []
    for path in tracked_files(root):
        if not path.exists() or is_binary(path):
            continue
        relpath = path.relative_to(root)
        rel = relpath.as_posix()
        try:
            lines = path.read_text(encoding="utf8", errors="replace").splitlines()
        except OSError as err:
            print(f"failed to read {relpath}: {err}", file=sys.stderr)
            return 1
        for line_number, line in enumerate(lines, start=1):
            if line_allowed(relpath, line):
                continue
            rule = match_rule(rel, line)
            if rule is not None:
                failures.append(f"{relpath}:{line_number}: {rule.name}: {line.strip()}")
        failures.extend(wrapped_failures(relpath, rel, lines))

    if failures:
        print("Quicksilver fee residue remains:")
        print("\n".join(failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
