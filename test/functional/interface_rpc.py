#!/usr/bin/env python3
# Copyright (c) 2018-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests some generic aspects of the RPC interface."""

import json
import os
import subprocess
from dataclasses import dataclass
from threading import Thread

from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal, assert_greater_than_or_equal


RPC_INVALID_PARAMETER      = -8
RPC_METHOD_NOT_FOUND       = -32601
RPC_INVALID_REQUEST        = -32600
RPC_PARSE_ERROR            = -32700


@dataclass
class BatchOptions:
    notification: bool = False
    request_fields: dict | None = None
    response_fields: dict | None = None


def format_request(options, idx, fields):
    request = {"jsonrpc": "2.0"}
    if not options.notification:
        request.update(id=idx)
    request.update(fields)
    if options.request_fields:
        request.update(options.request_fields)
    return request


def format_response(options, idx, fields):
    if options.notification:
        return None
    response = {"jsonrpc": "2.0", "id": idx}
    response.update(fields)
    if options.response_fields:
        response.update(options.response_fields)
    return response


def send_raw_rpc(node, raw_body: bytes) -> tuple[object, int]:
    return node._request("POST", "/", raw_body)


def send_json_rpc(node, body: object) -> tuple[object, int]:
    raw = json.dumps(body).encode("utf-8")
    return send_raw_rpc(node, raw)


def expect_http_rpc_status(expected_http_status, expected_rpc_error_code, node, method, params, notification=False):
    req = format_request(BatchOptions(notification), 0, {"method": method, "params": params})
    response, status = send_json_rpc(node, req)

    if expected_rpc_error_code is not None:
        assert_equal(response["error"]["code"], expected_rpc_error_code)

    assert_equal(status, expected_http_status)


def test_work_queue_getblock(node, got_exceeded_error):
    while not got_exceeded_error:
        try:
            node.cli("waitfornewblock", "500").send_cli()
        except subprocess.CalledProcessError as e:
            assert_equal(e.output, 'error: Server response: Work queue depth exceeded\n')
            got_exceeded_error.append(True)


class RPCInterfaceTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.supports_cli = False

    def test_getrpcinfo(self):
        self.log.info("Testing getrpcinfo...")

        info = self.nodes[0].getrpcinfo()
        assert_equal(len(info['active_commands']), 1)

        command = info['active_commands'][0]
        assert_equal(command['method'], 'getrpcinfo')
        assert_greater_than_or_equal(command['duration'], 0)
        assert_equal(info['logpath'], os.path.join(self.nodes[0].chain_path, 'debug.log'))

    def test_batch_request(self, call_options):
        calls = [
            # A basic request that will work fine.
            {"method": "getblockcount"},
            # Request that will fail.  The whole batch request should still
            # work fine.
            {"method": "invalidmethod"},
            # Another call that should succeed.
            {"method": "getblockhash", "params": [0]},
            # Invalid request format
            {"pizza": "sausage"}
        ]
        results = [
            {"result": 0},
            {"error": {"code": RPC_METHOD_NOT_FOUND, "message": "Method not found"}},
            {"result": self.nodes[0].getblockhash(0)},
            {"error": {"code": RPC_INVALID_REQUEST, "message": "Missing method"}},
        ]

        request = []
        response = []
        for idx, (call, result) in enumerate(zip(calls, results), 1):
            options = call_options(idx)
            if options is None:
                continue
            request.append(format_request(options, idx, call))
            r = format_response(options, idx, result)
            if r is not None:
                response.append(r)
            elif "method" not in call:
                invalid_response = {"jsonrpc": "2.0", "id": None}
                invalid_response.update(result)
                response.append(invalid_response)

        rpc_response, http_status = send_json_rpc(self.nodes[0], request)
        if len(request) == 0:
            assert_equal(http_status, 400)
            assert_equal(rpc_response, {"jsonrpc": "2.0", "error": {"code": RPC_INVALID_REQUEST, "message": "Invalid Request"}, "id": None})
        elif len(response) == 0:
            assert_equal(http_status, 204)
            assert_equal(rpc_response, None)
        else:
            assert_equal(http_status, 200)
            assert_equal(rpc_response, response)

    def test_batch_requests(self):
        self.log.info("Testing empty batch request...")
        self.test_batch_request(lambda idx: None)

        self.log.info("Testing basic JSON-RPC 2.0 batch request...")
        self.test_batch_request(lambda idx: BatchOptions())

        self.log.info("Testing JSON-RPC 2.0 batch with notifications...")
        self.test_batch_request(lambda idx: BatchOptions(notification=idx < 2))

        self.log.info("Testing valid notifications are suppressed but malformed members receive errors...")
        self.test_batch_request(lambda idx: BatchOptions(notification=True))

        self.log.info("Testing malformed batch members receive errors with independent IDs...")
        response, status = send_json_rpc(self.nodes[0], [
            {"jsonrpc": "2.0", "method": "getblockcount"},
            {"id": 7, "method": "getblockcount"},
            17,
            {"jsonrpc": "2.0", "method": "getblockcount"},
        ])
        assert_equal(status, 200)
        assert_equal(response, [
            {"jsonrpc": "2.0", "error": {"code": RPC_INVALID_REQUEST, "message": "Missing jsonrpc field"}, "id": 7},
            {"jsonrpc": "2.0", "error": {"code": RPC_INVALID_REQUEST, "message": "Invalid Request object"}, "id": None},
        ])

        self.log.info("Testing requests without a JSON-RPC marker are rejected...")
        response, status = send_json_rpc(self.nodes[0], {"id": 1, "method": "getblockcount"})
        assert_equal(status, 400)
        assert_equal(response, {"jsonrpc": "2.0", "error": {"code": RPC_INVALID_REQUEST, "message": "Missing jsonrpc field"}, "id": 1})

        self.log.info("Testing explicit JSON-RPC 1.1 requests are rejected...")
        response, status = send_json_rpc(self.nodes[0], {"version": "1.1", "id": 1, "method": "getblockcount"})
        assert_equal(status, 400)
        assert_equal(response, {"jsonrpc": "2.0", "error": {"code": RPC_INVALID_REQUEST, "message": "Missing jsonrpc field"}, "id": 1})

        self.log.info("Testing jsonrpc 1.0 alias is rejected...")
        response, status = send_json_rpc(self.nodes[0], {"jsonrpc": "1.0", "id": 1, "method": "getblockcount"})
        assert_equal(status, 400)
        assert_equal(response, {"jsonrpc": "2.0", "error": {"code": RPC_INVALID_REQUEST, "message": "JSON-RPC version not supported"}, "id": 1})

        self.log.info("Testing unrecognized jsonrpc version number is rejected...")
        response, status = send_json_rpc(self.nodes[0], {"jsonrpc": "2.1", "id": 1, "method": "getblockcount"})
        assert_equal(status, 400)
        assert_equal(response, {"jsonrpc": "2.0", "error": {"code": RPC_INVALID_REQUEST, "message": "JSON-RPC version not supported"}, "id": 1})

    def test_http_status_codes(self):
        # force-send empty request
        response, status = send_raw_rpc(self.nodes[0], b"")
        assert_equal(response, {"jsonrpc": "2.0", "id": None, "error": {"code": RPC_PARSE_ERROR, "message": "Parse error"}})
        assert_equal(status, 400)
        # force-send invalidly formatted request
        response, status = send_raw_rpc(self.nodes[0], b"this is bad")
        assert_equal(response, {"jsonrpc": "2.0", "id": None, "error": {"code": RPC_PARSE_ERROR, "message": "Parse error"}})
        assert_equal(status, 400)

        self.log.info("Testing HTTP status codes for JSON-RPC 2.0 requests...")
        expect_http_rpc_status(200, None,                   self.nodes[0], "getblockhash", [0])
        expect_http_rpc_status(200, RPC_METHOD_NOT_FOUND,   self.nodes[0], "invalidmethod", [])
        expect_http_rpc_status(200, RPC_INVALID_PARAMETER,  self.nodes[0], "getblockhash", [42])
        # force-send invalidly formatted requests
        response, status = send_json_rpc(self.nodes[0], {"jsonrpc": 2, "method": "getblockcount"})
        assert_equal(response, {"jsonrpc": "2.0", "error": {"code": RPC_INVALID_REQUEST, "message": "jsonrpc field must be a string"}, "id": None})
        assert_equal(status, 400)
        response, status = send_json_rpc(self.nodes[0], {"jsonrpc": "3.0", "method": "getblockcount"})
        assert_equal(response, {"jsonrpc": "2.0", "error": {"code": RPC_INVALID_REQUEST, "message": "JSON-RPC version not supported"}, "id": None})
        assert_equal(status, 400)

        self.log.info("Testing HTTP status codes for JSON-RPC 2.0 notifications...")
        # Not notification: id exists
        response, status = send_json_rpc(self.nodes[0], {"jsonrpc": "2.0", "id": None, "method": "getblockcount"})
        assert_equal(response["result"], 0)
        assert_equal(status, 200)
        # Not notification: has "id" field
        expect_http_rpc_status(200, None,                   self.nodes[0], "getblockcount", [])
        block_count = self.nodes[0].getblockcount()
        # Notification response status code: HTTP_NO_CONTENT
        expect_http_rpc_status(204, None,                   self.nodes[0], "generatetoaddress", [1, "shg1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqtsgs9d"], True)
        # The command worked even though there was no response
        assert_equal(block_count + 1, self.nodes[0].getblockcount())
        # No error response for notifications even if they are invalid
        expect_http_rpc_status(204, None, self.nodes[0], "generatetoaddress", [1, "invalid_address"], True)
        # Sanity check: command was not executed
        assert_equal(block_count + 1, self.nodes[0].getblockcount())

    def test_work_queue_exceeded(self):
        self.log.info("Testing work queue exceeded...")
        self.restart_node(0, ['-rpcworkqueue=1', '-rpcthreads=1'])
        got_exceeded_error = []
        threads = []
        for _ in range(3):
            t = Thread(target=test_work_queue_getblock, args=(self.nodes[0], got_exceeded_error))
            t.start()
            threads.append(t)
        for t in threads:
            t.join()

    def run_test(self):
        self.test_getrpcinfo()
        self.test_batch_requests()
        self.test_http_status_codes()
        self.test_work_queue_exceeded()


if __name__ == '__main__':
    RPCInterfaceTest(__file__).main()
