#!/usr/bin/env python3
# Copyright (c) 2014-2021 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the RPC HTTP basics."""

from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal, str_to_b64str

import http.client
import json
import urllib.parse


def rpc_request(method):
    """JSON-RPC 2.0 request with an id so the node replies (no id is a notification → 204)."""
    return json.dumps({"jsonrpc": "2.0", "method": method, "id": 1})


def assert_jsonrpc2_ok(http_response):
    assert_equal(http_response.status, 200)
    body = json.loads(http_response.read())
    assert_equal(body["jsonrpc"], "2.0")
    assert "result" in body
    assert "error" not in body
    return body


class HTTPBasicsTest (QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.supports_cli = False

    def setup_network(self):
        self.setup_nodes()

    def test_http_threads_are_wrapped(self):
        # The HTTP event loop and every RPC worker run on their own thread. An
        # exception escaping a thread entry point calls std::terminate(), which
        # on Windows is a __fastfail exiting 0xC0000409 with no log line, no
        # exception type and no message -- and that code is also what a /GS
        # stack-cookie failure reports, so the two are indistinguishable after
        # the fact. util::TraceThread logs the exception before rethrowing.
        #
        # These start lines are emitted by TraceThread and by nothing else, so
        # reverting either spawn to a bare std::thread fails this check.
        self.log.info("Check the HTTP threads are spawned through util::TraceThread")
        with open(self.nodes[0].debug_log_path, encoding="utf-8") as dl:
            log = dl.read()
        assert "http thread start" in log, "HTTP event-loop thread is not wrapped by util::TraceThread"
        assert "httpworker.0 thread start" in log, "HTTP worker threads are not wrapped by util::TraceThread"

    def run_test(self):

        self.test_http_threads_are_wrapped()

        #################################################
        # lowlevel check for http persistent connection #
        #################################################
        url = urllib.parse.urlparse(self.nodes[0].url)
        authpair = f'{url.username}:{url.password}'
        headers = {"Authorization": f"Basic {str_to_b64str(authpair)}"}

        conn = http.client.HTTPConnection(url.hostname, url.port)
        conn.connect()
        conn.request('POST', '/', rpc_request("getbestblockhash"), headers)
        assert_jsonrpc2_ok(conn.getresponse())
        assert conn.sock is not None  #according to http/1.1 connection must still be open!

        #send 2nd request without closing connection
        conn.request('POST', '/', rpc_request("getchaintips"), headers)
        assert_jsonrpc2_ok(conn.getresponse())
        assert conn.sock is not None  #according to http/1.1 connection must still be open!
        conn.close()

        #same should be if we add keep-alive because this should be the std. behaviour
        headers = {"Authorization": f"Basic {str_to_b64str(authpair)}", "Connection": "keep-alive"}

        conn = http.client.HTTPConnection(url.hostname, url.port)
        conn.connect()
        conn.request('POST', '/', rpc_request("getbestblockhash"), headers)
        assert_jsonrpc2_ok(conn.getresponse())
        assert conn.sock is not None  #according to http/1.1 connection must still be open!

        #send 2nd request without closing connection
        conn.request('POST', '/', rpc_request("getchaintips"), headers)
        assert_jsonrpc2_ok(conn.getresponse())
        assert conn.sock is not None  #according to http/1.1 connection must still be open!
        conn.close()

        #now do the same with "Connection: close"
        headers = {"Authorization": f"Basic {str_to_b64str(authpair)}", "Connection":"close"}

        conn = http.client.HTTPConnection(url.hostname, url.port)
        conn.connect()
        conn.request('POST', '/', rpc_request("getbestblockhash"), headers)
        assert_jsonrpc2_ok(conn.getresponse())
        assert conn.sock is None  #now the connection must be closed after the response

        #node1 (2nd node) is running with disabled keep-alive option
        urlNode1 = urllib.parse.urlparse(self.nodes[1].url)
        authpair = f'{urlNode1.username}:{urlNode1.password}'
        headers = {"Authorization": f"Basic {str_to_b64str(authpair)}"}

        conn = http.client.HTTPConnection(urlNode1.hostname, urlNode1.port)
        conn.connect()
        conn.request('POST', '/', rpc_request("getbestblockhash"), headers)
        assert_jsonrpc2_ok(conn.getresponse())

        #node2 (third node) is running with standard keep-alive parameters which means keep-alive is on
        urlNode2 = urllib.parse.urlparse(self.nodes[2].url)
        authpair = f'{urlNode2.username}:{urlNode2.password}'
        headers = {"Authorization": f"Basic {str_to_b64str(authpair)}"}

        conn = http.client.HTTPConnection(urlNode2.hostname, urlNode2.port)
        conn.connect()
        conn.request('POST', '/', rpc_request("getbestblockhash"), headers)
        assert_jsonrpc2_ok(conn.getresponse())
        assert conn.sock is not None  #connection must be closed because quicksilverd should use keep-alive by default

        # Check excessive request size
        conn = http.client.HTTPConnection(urlNode2.hostname, urlNode2.port)
        conn.connect()
        conn.request('GET', f'/{"x"*1000}', '', headers)
        out1 = conn.getresponse()
        assert_equal(out1.status, http.client.NOT_FOUND)

        conn = http.client.HTTPConnection(urlNode2.hostname, urlNode2.port)
        conn.connect()
        conn.request('GET', f'/{"x"*10000}', '', headers)
        out1 = conn.getresponse()
        assert_equal(out1.status, http.client.BAD_REQUEST)


if __name__ == '__main__':
    HTTPBasicsTest(__file__).main()
