#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Assert no relay pool surface still answers to a pre-rename name.

A rename that leaves an alias behind is indistinguishable from a finished one
until an external caller depends on the alias. These are the negative
assertions the rename's own passing tests cannot make: the source lint gate
proves the old spellings are absent from the tree, and this proves the running
node rejects them.

The old names are assembled from fragments so that a future tree-wide rename
sweep cannot quietly rewrite them and turn every assertion here into a
tautology.
"""
import http.client
import urllib.parse

from test_framework.authproxy import JSONRPCException
from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal

OLD = "mem" + "pool"

OLD_RPCS = [
    f"getraw{OLD}",
    f"get{OLD}info",
    f"get{OLD}entry",
    f"get{OLD}ancestors",
    f"get{OLD}descendants",
    f"test{OLD}accept",
    f"save{OLD}",
    f"import{OLD}",
]

OLD_FLAGS = [
    f"-max{OLD}=300",
    f"-{OLD}expiry=336",
    f"-persist{OLD}=1",
    f"-persist{OLD}v1=0",
    f"-check{OLD}=0",
]

RPC_METHOD_NOT_FOUND = -32601


class RelayPoolNamesTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        # Without -rest every path 404s, which would make the old-path
        # assertions below pass without proving anything.
        self.extra_args = [["-rest"]]

    def run_test(self):
        self.test_old_rpc_names_do_not_resolve()
        self.test_new_rpc_names_resolve()
        self.test_old_json_names_are_gone()
        self.test_old_rest_paths_404()
        self.test_old_flags_refuse_to_start()

    def test_old_rpc_names_do_not_resolve(self):
        self.log.info("Old RPC method names must not resolve")
        node = self.nodes[0]
        for name in OLD_RPCS:
            try:
                getattr(node, name)()
            except JSONRPCException as e:
                assert_equal(e.error["code"], RPC_METHOD_NOT_FOUND)
            else:
                raise AssertionError(f"{name} still resolves")

    def test_new_rpc_names_resolve(self):
        self.log.info("New RPC method names must resolve")
        node = self.nodes[0]
        assert_equal(node.getrelaypoolinfo()["size"], 0)
        assert_equal(node.getrawrelaypool(), [])

    def test_old_json_names_are_gone(self):
        self.log.info("JSON fields and request parameters must not use the old spelling")
        node = self.nodes[0]

        info = node.getrelaypoolinfo()
        assert "maxrelaypool" in info
        assert not any(OLD in key for key in info), f"old spelling in {sorted(info)}"

        raw = node.getrawrelaypool(verbose=False, relaypool_sequence=True)
        assert "relaypool_sequence" in raw
        assert not any(OLD in key for key in raw), f"old spelling in {sorted(raw)}"

        # The request parameters renamed too, so the old keywords must not bind.
        try:
            node.getrawrelaypool(verbose=False, **{f"{OLD}_sequence": True})
        except JSONRPCException as e:
            assert_equal(e.error["code"], -8)  # RPC_INVALID_PARAMETER
        else:
            raise AssertionError(f"getrawrelaypool still accepts {OLD}_sequence")

        try:
            node.gettxout(txid="00" * 32, n=0, **{f"include_{OLD}": True})
        except JSONRPCException as e:
            assert_equal(e.error["code"], -8)
        else:
            raise AssertionError(f"gettxout still accepts include_{OLD}")

    def test_old_rest_paths_404(self):
        self.log.info("Old REST paths must 404 and new ones must serve")
        url = urllib.parse.urlparse(self.nodes[0].url)
        conn = http.client.HTTPConnection(url.hostname, url.port)
        for path in (f"/rest/{OLD}/info.json", f"/rest/{OLD}/contents.json"):
            conn.request("GET", path)
            resp = conn.getresponse()
            resp.read()
            assert_equal(resp.status, 404)
        for path in ("/rest/relaypool/info.json", "/rest/relaypool/contents.json"):
            conn.request("GET", path)
            resp = conn.getresponse()
            resp.read()
            assert_equal(resp.status, 200)
        conn.close()

    def test_old_flags_refuse_to_start(self):
        self.log.info("Old command-line flags must be rejected, not migrated")
        self.stop_node(0)
        for flag in OLD_FLAGS:
            self.nodes[0].assert_start_raises_init_error(
                extra_args=[flag],
                expected_msg=f"Error: Error parsing command line arguments: Invalid parameter {flag}",
            )
        self.start_node(0)


if __name__ == "__main__":
    RelayPoolNamesTest(__file__).main()
