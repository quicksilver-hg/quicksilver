#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Assert doc/bootstrapping.md still tells the seed operator to bind the onion target.

The fixed onion seed is the only peer discovery Quicksilver ships, and a
misconfigured seed fails **silently**: it is advertised in chainparamsseeds.h,
Tor forwards inbound peers to a closed loopback port, the node logs nothing, and
`getconnectioncount` reads 0 -- indistinguishable from the seed being down.

Two independently correct decisions collide to produce it:

  1. init.cpp creates the default `127.0.0.1:<target>=onion` bind only when
     -listenonion is on, so a node wanting no onion service binds no onion
     target. feature_port.py asserts this.
  2. doc/bootstrapping.md tells the seed operator to run -listenonion=0, so the
     node advertises the static torrc address instead of an ephemeral one.

Together they leave the seed with nothing on the onion target. The only thing
holding the two halves together is the doc, so the doc is what gets linted:
it must keep -listenonion=0 and an explicit `-bind=...=onion` in the same
section, and the ports it prints must still match kernel/chainparams.cpp.

This was found by connecting to the live seed from another machine's Tor daemon.
It was NOT found by a stand-in `nc` listener on the target port, which passes
precisely because it supplies the bind whose absence is the defect.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
DOC = ROOT / "doc" / "bootstrapping.md"
CHAINPARAMS = ROOT / "src" / "kernel" / "chainparams.cpp"
SECTION = "## Operating the seed"


def onion_service_ports() -> list[int]:
    """Every m_onion_service_port literal in chainparams.cpp, in file order."""
    text = CHAINPARAMS.read_text(encoding="utf-8")
    return [int(m) for m in re.findall(r"m_onion_service_port\s*=\s*(\d+)\s*;", text)]


def seed_section(text: str) -> str:
    start = text.find(SECTION)
    if start == -1:
        return ""
    nxt = text.find("\n## ", start + len(SECTION))
    return text[start:] if nxt == -1 else text[start:nxt]


def main() -> int:
    failures = []

    if not DOC.is_file():
        print(f"{DOC.relative_to(ROOT).as_posix()}: missing")
        return 1
    text = DOC.read_text(encoding="utf-8")

    section = seed_section(text)
    if not section:
        failures.append(
            f"doc/bootstrapping.md: no {SECTION!r} section; the seed's "
            f"-listenonion/-bind pairing is documented nowhere else"
        )
    else:
        if "-listenonion=0" not in section:
            failures.append(
                "doc/bootstrapping.md: 'Operating the seed' no longer names "
                "-listenonion=0; without it the node advertises a throwaway "
                "ephemeral onion instead of the compiled-in seed address"
            )
        if not re.search(r"-bind=[^\s`]*=onion", section):
            failures.append(
                "doc/bootstrapping.md: 'Operating the seed' no longer shows an "
                "explicit -bind=...=onion. With -listenonion=0 the default onion "
                "bind is never created, so Tor forwards inbound peers to a closed "
                "port and the seed silently accepts nothing"
            )

    ports = onion_service_ports()
    if not ports:
        failures.append(
            "src/kernel/chainparams.cpp: no m_onion_service_port found; "
            "doc/bootstrapping.md's onion targets can no longer be checked"
        )
    for port in ports:
        if str(port) not in text:
            failures.append(
                f"doc/bootstrapping.md: does not mention onion service target "
                f"{port}, which chainparams.cpp defines. A torrc pointing at the "
                f"wrong target is the same silent failure as no target at all"
            )

    for f in failures:
        print(f)
    if failures:
        print(
            "\nThe fixed onion seed is Quicksilver's only shipped peer discovery, "
            "and a misconfigured seed fails silently. See the docstring in "
            f"{pathlib.Path(sys.argv[0]).name}."
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
