#!/usr/bin/env python3
# Copyright (c) 2022-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

""" Tests the relaypool:* tracepoint API interface.

The tracepoint provider name is a published external interface: third-party
bpftrace and bcc scripts attach to it by name. It took upstream's spelling until
the relay pool rename, and before this file existed nothing guarded it -- a
mistake in the provider or event name would have failed silently, since a probe
that does not attach is indistinguishable from a tracepoint that did not fire.

The old provider name is spelled in fragments so that a tree-wide rename sweep
cannot rewrite it and turn the negative assertion into a tautology.
"""

import ctypes

# Test will be skipped if we don't have bcc installed
try:
    from bcc import BPF, USDT  # type: ignore[import]
except ImportError:
    pass

from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import (
    assert_equal,
    bpf_cflags,
)
from test_framework.vault import MiniVault

OLD_PROVIDER = "mem" + "pool"


def copy_perf_event(struct_type, data):
    # The callback's data is borrowed from the perf buffer and is invalid after
    # bpf.cleanup(). from_buffer_copy on a bytes object cannot alias the ring.
    return struct_type.from_buffer_copy(ctypes.string_at(data, ctypes.sizeof(struct_type)))


relaypool_added_program = """
#include <uapi/linux/ptrace.h>

struct added_event
{
    u8   hash[32];
    int  vsize;
};

BPF_PERF_OUTPUT(relaypool_added);
int trace_relaypool_added(struct pt_regs *ctx) {
    struct added_event added = {};
    void *phash = NULL;
    bpf_usdt_readarg(1, ctx, &phash);
    bpf_probe_read_user(&added.hash, sizeof(added.hash), phash);
    bpf_usdt_readarg(2, ctx, &added.vsize);
    relaypool_added.perf_submit(ctx, &added, sizeof(added));
    return 0;
}
"""


class RelayPoolTracepointTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-txpownocycle=1"]]

    def skip_test_if_missing_module(self):
        self.skip_if_platform_not_linux()
        self.skip_if_no_quicksilverd_tracepoints()
        self.skip_if_no_python_bcc()
        self.skip_if_no_bpf_permissions()

    def run_test(self):
        self.test_provider_is_relaypool()
        self.test_old_provider_is_gone()

    def test_provider_is_relaypool(self):
        class Added(ctypes.Structure):
            _fields_ = [
                ("hash", ctypes.c_ubyte * 32),
                ("vsize", ctypes.c_int),
            ]

        events = []

        self.log.info("hook into the relaypool:added tracepoint")
        ctx = USDT(pid=self.nodes[0].process.pid)
        ctx.enable_probe(probe="relaypool:added", fn_name="trace_relaypool_added")
        bpf = BPF(text=relaypool_added_program, usdt_contexts=[ctx], debug=0,
                  cflags=bpf_cflags())

        def handle_added(_, data, __):
            events.append(copy_perf_event(Added, data))

        bpf["relaypool_added"].open_perf_buffer(handle_added)

        self.log.info("submit a transaction and check the tracepoint fires")
        mini_vault = MiniVault(self.nodes[0])
        self.generate(mini_vault, 101)
        del events[:]  # coinbase maturity generation churns the pool
        tx = mini_vault.send_self_transfer(from_node=self.nodes[0], confirmed_only=True)

        bpf.perf_buffer_poll(timeout=200)
        bpf.cleanup()

        assert_equal(1, len(events))
        event = events[0]
        assert_equal(tx["txid"], bytes(event.hash[::-1]).hex())
        assert event.vsize > 0, "the tracepoint reported a zero-size transaction"

        self.log.info("check the other three relaypool tracepoints are present")
        for event_name in ("removed", "replaced", "rejected"):
            probe = USDT(pid=self.nodes[0].process.pid)
            # enable_probe raises if the provider:event pair does not exist.
            probe.enable_probe(probe=f"relaypool:{event_name}",
                               fn_name="trace_relaypool_added")

    def test_old_provider_is_gone(self):
        self.log.info("check the pre-rename provider no longer exists")
        for event_name in ("added", "removed", "replaced", "rejected"):
            probe = USDT(pid=self.nodes[0].process.pid)
            try:
                probe.enable_probe(probe=f"{OLD_PROVIDER}:{event_name}",
                                   fn_name="trace_relaypool_added")
            except Exception:
                continue
            raise AssertionError(
                f"{OLD_PROVIDER}:{event_name} still exists; the rename left a "
                "second provider behind")


if __name__ == "__main__":
    RelayPoolTracepointTest(__file__).main()
