#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""The REST interface is removed; -rest is accepted, logged and ignored (F-643).

A configuration that still says rest=1 must start, say once that the option is
ignored, and serve no /rest/ path. Every path below answered 200 while REST
existed.
"""
import http.client
import urllib.parse

from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal

REST_PATHS = [
    "/rest/chaininfo.json",
    "/rest/deploymentinfo.json",
    "/rest/relaypool/info.json",
    "/rest/relaypool/contents.json",
]


class RestRemovedTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def get_status(self, path):
        url = urllib.parse.urlparse(self.nodes[0].url)
        conn = http.client.HTTPConnection(url.hostname, url.port)
        conn.request("GET", path)
        resp = conn.getresponse()
        resp.read()
        conn.close()
        return resp.status

    def run_test(self):
        node = self.nodes[0]
        self.log.info("-rest on the command line: the node starts, logs once, and serves nothing")
        with node.assert_debug_log(["The REST interface was removed; -rest is ignored"]):
            self.restart_node(0, extra_args=["-rest"])
        for path in REST_PATHS:
            assert_equal(self.get_status(path), 404)
        # The RPC equivalent of /rest/chaininfo.json still answers.
        assert_equal(node.getblockchaininfo()["chain"], self.chain)

        self.log.info("rest=1 in the configuration file starts too")
        self.stop_node(0)
        with open(node.datadir_path / "quicksilver.conf", "a", encoding="utf8") as f:
            f.write("rest=1\n")
        with node.assert_debug_log(["The REST interface was removed; -rest is ignored"]):
            self.start_node(0)
        assert_equal(self.get_status("/rest/chaininfo.json"), 404)

        self.log.info("Without -rest nothing is logged about it")
        node.replace_in_config([("rest=1\n", "")])
        with node.assert_debug_log(expected_msgs=[], unexpected_msgs=["The REST interface was removed"]):
            self.restart_node(0)


if __name__ == '__main__':
    RestRemovedTest(__file__).main()
