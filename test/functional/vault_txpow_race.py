#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Fire vault sends at the moment a block connects and compare the two tx-PoW logs.

The vault logs `tx-pow final` for the target it ground against. Consensus logs
`accepted` or `rejected reason=tx-pow-invalid` for the target it checked. Both
carry the txid, so a pair is the same transaction. F-333 was one such pair
whose verdict flipped (`failed=cycle` was the shape seen once, in
vault_taproot.py) after the vault had already paid for the proof.

test_runner.py lists this file in NON_SCRIPTS, so no suite runs it. It stays
there until a run reproduces the divergence. Run it by name.

Rows are one of:

  equal                  every compared field matches and consensus accepted
  differs-verdict-held   a field differs and consensus still accepted
  verdict-flipped        consensus rejected tx-pow-invalid
  nin-disagreement       nin or nout differ; the join is wrong, not a finding

A differing target with the verdict held is the finding. It is not folded into
`passed`.
"""

import re
import threading
import time
from decimal import Decimal

from test_framework.authproxy import JSONRPCException
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import get_rpc_proxy

# Sandbox: nPowTargetTimespan / nPowTargetSpacing = 86400/300. fPowNoRetargeting
# is true, so nBits does not move here; the block at this interval is still the
# retarget connect path.
RETARGET_INTERVAL = 288
# Offsets in milliseconds relative to the generatetoaddress call. Negative is
# before it, zero overlaps it, positive is after it returns.
OFFSETS_MS = (-40, -10, 0, 15, 80)
SEND_KINDS = ("sendtoaddress", "sendmany", "send")
COMPARE_FIELDS = ("anchor", "bytes", "nout", "nin", "target", "proof")

FINAL_RE = (
    r"tx-pow final: tx=([0-9a-f]+) anchor=(\d+) bytes=(\d+) "
    r"nout=(\d+) nin=(\d+) target=([0-9a-f]+) proof=([0-9a-f]+)"
)
ACCEPTED_RE = (
    r"accepted tx=([0-9a-f]+) anchor=(\d+) bytes=(\d+) "
    r"nout=(\d+) nin=(\d+) target=([0-9a-f]+) proof=([0-9a-f]+) "
    r"anchor_hash=([0-9a-f]+)"
)
REJECTED_RE = (
    r"rejected reason=tx-pow-invalid failed=(threshold|cycle) "
    r"tx=([0-9a-f]+) anchor=(\d+) bytes=(\d+) nout=(\d+) nin=(\d+) "
    r"target=([0-9a-f]+) proof=([0-9a-f]+) anchor_hash=([0-9a-f]+)"
)


FINAL_RX = re.compile(FINAL_RE)
ACCEPTED_RX = re.compile(ACCEPTED_RE)
REJECTED_RX = re.compile(REJECTED_RE)


def _txid_of(result):
    if isinstance(result, dict):
        return result["txid"]
    return result


def _record(groups, verdict, failed=""):
    return {
        "txid": groups[0],
        "anchor": groups[1],
        "bytes": groups[2],
        "nout": groups[3],
        "nin": groups[4],
        "target": groups[5],
        "proof": groups[6],
        "verdict": verdict,
        "failed": failed,
    }


class VaultTxPowRaceTest(QuicksilverTestFramework):
    def add_options(self, parser):
        self.add_vault_options(parser)
        parser.add_argument(
            "--race-iters",
            default=2000,
            type=int,
            help="Samples to aim at the connect window, split between "
                 "-txpownocycle and real cycles (default: 2000). The retarget "
                 "walk runs as well.",
        )

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = False
        self.rpc_timeout = 900
        # Half the samples restart without -txpownocycle. The flag is sandbox-only.
        self.extra_args = [["-txpownocycle=1", "-datacarriersize=12000"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_vault()

    def run_test(self):
        self.rows = []
        self.started = time.time()
        node = self.nodes[0]
        assert self.options.race_iters >= 2
        self._fund(node)
        self._require_congestion_moves(node)
        walked = self._walk_retarget(node)
        half = self.options.race_iters // 2
        self._loop(node, "nocycle", max(0, half - walked))
        self.restart_node(0, extra_args=["-datacarriersize=12000"])
        self._loop(self.nodes[0], "real-cycle", self.options.race_iters - half)
        self._finish()

    def _fund(self, node):
        addresses = [node.getnewaddress() for _ in range(200)]
        amounts = {address: Decimal("0.1") for address in addresses}
        node.sendmany(amounts=amounts)
        self.generate(node, 1)

    def _require_congestion_moves(self, node):
        tip = node.getbestblockhash()
        before = int(node.getblockheader(tip)["congestion"])
        self._send("heavy", node)
        self.generate(node, 1)
        tip = node.getbestblockhash()
        after = int(node.getblockheader(tip)["congestion"])
        header = node.getblockheader(tip)
        self.log.info(
            f"congestion {before} -> {after} at height {header['height']} "
            f"bits={header['bits']}"
        )
        assert after > before, (
            f"a full block did not move m_congestion ({before} -> {after}); "
            "the raced variable would be sitting on the floor"
        )
        text = self._read_log(0)
        counts = self._consume(text, "setup", "nocycle", reorg=False)
        self._raise_if_divergence(counts)
        assert counts["equal"] > 0, (
            "setup produced no joined equal pair; the detector lines are not visible"
        )

    def _walk_retarget(self, node):
        walked = 0
        while True:
            height = node.getblockcount()
            on_boundary = (height + 1) % RETARGET_INTERVAL == 0
            bits_before = node.getblockheader(node.getbestblockhash())["bits"]
            self._race_once(node, "retarget" if on_boundary else "approach", "nocycle", reorg=False)
            walked += 1
            if on_boundary:
                bits_after = node.getblockheader(node.getbestblockhash())["bits"]
                self.log.info(
                    f"retarget boundary height={node.getblockcount()} "
                    f"bits {bits_before} -> {bits_after} "
                    f"(sandbox fPowNoRetargeting keeps these equal)"
                )
                return walked

    def _loop(self, node, mode, count):
        for index in range(count):
            reorg = index % 5 == 4
            self._race_once(node, "reorg" if reorg else "connect", mode, reorg=reorg)

    def _race_once(self, node, tag, mode, reorg):
        offset = self._log_size()
        if not reorg:
            try:
                self._send("heavy", node)
            except JSONRPCException as exc:
                self.log.warning(f"heavy tx skipped: {exc}")
        found = []
        errors = []
        lock = threading.Lock()
        start = time.time() + 0.08

        def fire(kind, off_ms):
            proxy = self._proxy()
            wait = (start + off_ms / 1000.0) - time.time()
            if wait > 0:
                time.sleep(wait)
            try:
                txid = self._send(kind, proxy)
                with lock:
                    found.append(txid)
            except JSONRPCException as exc:
                with lock:
                    errors.append(f"{kind}@{off_ms}ms: {exc}")

        threads = []
        for index, off_ms in enumerate(OFFSETS_MS):
            kind = SEND_KINDS[index % len(SEND_KINDS)]
            thread = threading.Thread(target=fire, args=(kind, off_ms))
            thread.start()
            threads.append(thread)

        while time.time() < start:
            time.sleep(0.001)
        if reorg:
            tip = node.getbestblockhash()
            node.invalidateblock(tip)
            node.reconsiderblock(tip)
        else:
            self.generate(node, 1, sync_fun=self.no_op)
        for thread in threads:
            thread.join(timeout=self.rpc_timeout)

        self._await_txids(found, offset)
        text = self._read_log(offset)
        counts = self._consume(text, tag, mode, reorg=reorg)
        height = node.getblockcount()
        congestion = node.getblockheader(node.getbestblockhash())["congestion"]
        self.log.info(
            f"iter mode={mode} tag={tag} height={height} congestion={congestion} "
            f"equal={counts['equal']} held={counts['differs-verdict-held']} "
            f"flipped={counts['verdict-flipped']} "
            f"nin={counts['nin-disagreement']} unjoined={counts['unjoined']} "
            f"no_final={counts['no-final']} rpc_errors={len(errors)}"
        )
        for error in errors:
            self.log.info(f"rpc {tag}: {error}")
        self._raise_if_divergence(counts)

    def _send(self, kind, proxy):
        if kind == "heavy":
            # 8000-byte OP_RETURN plus one paying output. A data-only send is
            # rejected ("one destination of non-0 value or a pre-selected
            # input"). Sandbox congestion target is 0.5% of MAX_BLOCK_WEIGHT
            # (20000 weight); this carrier clears it. -datacarriersize is
            # raised on the node so the relay allows the carrier.
            address = proxy.getnewaddress()
            return _txid_of(proxy.send(outputs=[
                {"data": "aa" * 8000},
                {address: Decimal("0.001")},
            ]))
        address = proxy.getnewaddress()
        amount = Decimal("0.001")
        if kind == "sendtoaddress":
            return _txid_of(proxy.sendtoaddress(address, amount))
        if kind == "sendmany":
            return _txid_of(proxy.sendmany(amounts={address: amount}))
        if kind == "send":
            return _txid_of(proxy.send(outputs=[{address: amount}]))
        raise AssertionError(f"unknown send kind {kind}")

    def _proxy(self):
        node = self.nodes[0]
        return get_rpc_proxy(
            node.url, node.index, timeout=self.rpc_timeout, coveragedir=node.coverage_dir,
        )

    def _log_size(self):
        return self.nodes[0].debug_log_path.stat().st_size

    def _read_log(self, offset):
        with open(self.nodes[0].debug_log_path, encoding="utf8", errors="replace") as handle:
            handle.seek(offset)
            return handle.read()

    def _await_txids(self, txids, offset):
        """Wait until each returned txid has shown up in the log.

        The grind runs inside the RPC, and the caller joins those threads
        first, so this is only waiting for the log to be flushed. `send`
        goes through FundTransaction (sign=false) and never emits
        `tx-pow final`; its consensus line is the signal that it finished.
        """
        deadline = time.time() + 5
        pending = set(txids)
        while pending and time.time() < deadline:
            text = self._read_log(offset)
            pending = {
                txid for txid in pending
                if f"tx-pow final: tx={txid}" not in text
                and f"accepted tx={txid}" not in text
                and f"failed=threshold tx={txid}" not in text
                and f"failed=cycle tx={txid}" not in text
            }
            if pending:
                time.sleep(0.05)

    def _consume(self, text, tag, mode, reorg):
        finals = {}
        for match in FINAL_RX.finditer(text):
            finals[match.group(1)] = _record(match.groups(), "final")
        consensus = {}
        for match in ACCEPTED_RX.finditer(text):
            consensus[match.group(1)] = _record(match.groups(), "accepted")
        for match in REJECTED_RX.finditer(text):
            groups = match.groups()
            # groups: failed, txid, anchor, bytes, nout, nin, target, proof, anchor_hash
            consensus[groups[1]] = _record(groups[1:], "rejected", failed=groups[0])
        counts = {
            "equal": 0,
            "differs-verdict-held": 0,
            "verdict-flipped": 0,
            "nin-disagreement": 0,
            "unjoined": 0,
            # send() funds with sign=false, so CreateTransaction never emits
            # tx-pow final. The accept is real; it cannot be joined by txid.
            "no-final": 0,
        }
        for txid, other in consensus.items():
            if txid in finals:
                continue
            counts["no-final"] += 1
            self.rows.append({
                "kind": "no-final",
                "txid": txid,
                "tag": tag,
                "mode": mode,
                "fields": [],
                "reorg": reorg,
                "failed": other.get("failed", ""),
            })
        for txid, final in finals.items():
            other = consensus.get(txid)
            if other is None:
                counts["unjoined"] += 1
                self.rows.append({
                    "kind": "unjoined", "txid": txid, "tag": tag, "mode": mode,
                    "fields": [], "reorg": reorg,
                })
                continue
            fields = [
                name for name in COMPARE_FIELDS if final[name] != other[name]
            ]
            if "nin" in fields or "nout" in fields:
                kind = "nin-disagreement"
            elif other["verdict"] == "rejected":
                kind = "verdict-flipped"
            elif fields:
                kind = "differs-verdict-held"
            else:
                kind = "equal"
            counts[kind] += 1
            row = {
                "kind": kind,
                "txid": txid,
                "tag": tag,
                "mode": mode,
                "fields": fields,
                "failed": other.get("failed", ""),
                "reorg": reorg,
                "final": final,
                "consensus": other,
            }
            self.rows.append(row)
            if kind != "equal":
                self.log.error(
                    f"{kind} tx={txid} mode={mode} tag={tag} fields={fields} "
                    f"failed={other.get('failed', '')} "
                    f"vault_target={final['target']} consensus_target={other['target']} "
                    f"vault_anchor={final['anchor']} consensus_anchor={other['anchor']} "
                    f"vault_bytes={final['bytes']} consensus_bytes={other['bytes']}"
                )
        return counts

    def _raise_if_divergence(self, counts):
        bad = counts["differs-verdict-held"] + counts["verdict-flipped"] + counts["nin-disagreement"]
        if bad:
            raise AssertionError(
                "tx-pow divergence: "
                f"held={counts['differs-verdict-held']} "
                f"flipped={counts['verdict-flipped']} "
                f"nin={counts['nin-disagreement']}"
            )

    def _finish(self):
        totals = {
            "equal": 0,
            "differs-verdict-held": 0,
            "verdict-flipped": 0,
            "nin-disagreement": 0,
            "unjoined": 0,
            "no-final": 0,
        }
        for row in self.rows:
            totals[row["kind"]] = totals.get(row["kind"], 0) + 1
        elapsed = time.time() - self.started
        self.log.info(
            f"TABLE equal={totals['equal']} "
            f"differs-verdict-held={totals['differs-verdict-held']} "
            f"verdict-flipped={totals['verdict-flipped']} "
            f"nin-disagreement={totals['nin-disagreement']} "
            f"unjoined={totals['unjoined']} "
            f"no-final={totals['no-final']} "
            f"elapsed_s={elapsed:.1f} iters={self.options.race_iters}"
        )
        assert totals["equal"] > 0, "no joined sample; the harness saw nothing"
        assert totals["unjoined"] == 0, (
            f"{totals['unjoined']} final lines had no consensus line"
        )


if __name__ == "__main__":
    VaultTxPowRaceTest(__file__).main()
