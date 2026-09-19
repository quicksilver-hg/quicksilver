#!/usr/bin/env python3
# Copyright (c) 2018-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""External signer mock that returns two BASE58 descriptors for one chain.

pkh(...) and sh(...) both map to OutputType::BASE58. Importing both activates
two managers for the same (type, internal) key; LoadActiveScriptPubKeyMan then
silently displaces the first.

Nested witness descriptors are unparsable since 57ffdc8f, so a hardware
signer that offers BIP49 alongside BIP44 fails at Parse with
"Invalid descriptor" and never reaches the overwrite. The collision that
remains is any sh(...) that still parses — sh(pkh(...)) or sh(multi(...)).
"""

import os
import sys
import argparse
import json


def perform_pre_checks():
    mock_result_path = os.path.join(os.getcwd(), "mock_result")
    if os.path.isfile(mock_result_path):
        with open(mock_result_path, "r", encoding="utf8") as f:
            mock_result = f.read()
        if mock_result[0]:
            sys.stdout.write(mock_result[2:])
            sys.exit(int(mock_result[0]))


def enumerate(args):
    sys.stdout.write(json.dumps([{"fingerprint": "00000001", "type": "trezor", "model": "trezor_t"}]))


def getdescriptors(args):
    xpub = "squb6UShJgvLuWbXhZQZ1fML1Lg1yCsWwqoXY4vG1H3hFXrcUJ3kMeYf22JbXHi8iWoZQ8d1QbA5SBPxQw68h2mUNLdXNVQ4F5kgSzf8F3ox7o6"
    account = args.account

    sys.stdout.write(json.dumps({
        "receive": [
            "pkh([00000001/44h/1h/" + account + "']" + xpub + "/0/*)#apn6r9vf",
            "sh(pkh([00000001/44h/1h/" + account + "']" + xpub + "/0/*))#t8mp7fyn",
            "wpkh([00000001/84h/1h/" + account + "']" + xpub + "/0/*)#nk5qxdxk",
            "tr([00000001/86h/1h/" + account + "']" + xpub + "/0/*)#448kephy",
        ],
        "internal": [
            "pkh([00000001/44h/1h/" + account + "']" + xpub + "/1/*)#v4km7su3",
            "sh(pkh([00000001/44h/1h/" + account + "']" + xpub + "/1/*))#dyny9y08",
            "wpkh([00000001/84h/1h/" + account + "']" + xpub + "/1/*)#zz3pmckw",
            "tr([00000001/86h/1h/" + account + "']" + xpub + "/1/*)#ypzhy58u",
        ]
    }))


parser = argparse.ArgumentParser(prog='./colliding_signer.py', description='External signer mock with BASE58 collision')
parser.add_argument('--fingerprint')
parser.add_argument('--chain', default='main')
parser.add_argument('--stdin', action='store_true')

subparsers = parser.add_subparsers(description='Commands', dest='command')
subparsers.required = True

parser_enumerate = subparsers.add_parser('enumerate', help='list available signers')
parser_enumerate.set_defaults(func=enumerate)

parser_getdescriptors = subparsers.add_parser('getdescriptors')
parser_getdescriptors.set_defaults(func=getdescriptors)
parser_getdescriptors.add_argument('--account', metavar='account')

if not sys.stdin.isatty():
    buffer = sys.stdin.read()
    if buffer and buffer.rstrip() != "":
        sys.argv.extend(buffer.rstrip().split(" "))

args = parser.parse_args()

perform_pre_checks()

args.func(args)
