# Network Naming

> Status: implemented. The inherited `testnet4` and `regtest` tokens are rejected;
> current code uses `publictest` and `sandbox` as described here.

Quicksilver inherits three network names from Bitcoin — `main`, `testnet4`, and
`regtest`. All three are replaced. Two of them are inaccurate for this chain and
the third is opaque to the people the desktop application is built for.

## Three surfaces, one cost

Network naming touches three independent surfaces. Only the first has any real
cost, and understanding the split is what makes this change cheap.

| Surface | Where it appears | Cost to change |
| --- | --- | --- |
| Token | `-chain=` argument, config file section headers, datadir subdirectory, `getblockchaininfo.chain` | Breaks configs, datadir paths, scripts, RPC consumers |
| User-facing label | Application strings | None |
| Source symbol | `ChainType` enumerators | None; mechanical |

The token is **not** a peer-to-peer wire change. Network identity on the wire is
carried by `pchMessageStart` in [chainparams.cpp](../../src/kernel/chainparams.cpp),
which is independent of the name. Renaming therefore affects configuration, the
command line, datadir layout, and RPC output only.

Every one of those consumers is internal today. The change costs a rename pass
before launch and a migration after it, which is why it precedes the other
launch work.

## Names

| Inherited | Formal name | Token | Source symbol |
| --- | --- | --- | --- |
| `main` | Quicksilver Distributed Ledger System | `main` | `ChainType::MAIN` |
| `testnet4` | Quicksilver Public Test Network | `publictest` | `ChainType::PUBLIC_TEST` |
| `regtest` | Quicksilver Sandbox | `sandbox` | `ChainType::SANDBOX` |

### The live network

The formal name exists for documentation, the about box, and the developer
network selector. It does **not** appear on the application's launch screen,
in the status bar, or in `getblockchaininfo`.

The reasoning is that a network name only does work when networks are being
distinguished, and the only people distinguishing them are developers. Naming
the live network on the default path invites a first-time user to wonder which
other kind exists — the precise confusion the desktop application is designed to
remove. In the developer selector, where three networks sit side by side and
distinguishing them is the entire purpose, the formal name is correct.

### The public test network

`Public` names the distinction the inherited names never made: this network is
shared with strangers, and the sandbox is private. That difference matters more
to someone choosing between them than any other property.

The inherited `4` is inaccurate. Quicksilver has never had a test network 1, 2,
or 3; the number is carried over from a chain whose reset history is not ours.
The unnumbered token also leaves room for a real suffix when Quicksilver's own
public test chain is eventually reset.

### The sandbox

`regtest` abbreviates *regression test*, which describes how Bitcoin's continuous
integration used it. Quicksilver uses this network for local development,
functional tests, demonstrations, and regression tests alike. `Sandbox` is both
more accurate and comprehensible without explanation.

## What changed

Token and symbol occurrences, measured before implementation:

- `testnet4` — 60 files
- `regtest` — 118 files
- `ChainType::TESTNET4` — 14 files
- `ChainType::REGTEST` — 39 files

The mapping between symbol and token remains confined to
[chaintype.cpp](../../src/util/chaintype.cpp); everything else consumes it. Datadir
layout follows the token, so the public test network moves to
`<datadir>/publictest/` and the sandbox to `<datadir>/sandbox/`.

[chain-identity.md](chain-identity.md) was updated alongside this change.

## Non-goals

- No change to `pchMessageStart` or ports. Address prefixes and the publictest
  and sandbox genesis timestamp strings were renamed before launch; their
  genesis hashes were intentionally rederived.
- No compatibility alias for the inherited tokens. Quicksilver has not launched;
  there is no deployed configuration to preserve, and an alias would be exactly
  the deprecated surface this project does not ship.
- No change to the coin's own naming. Denominations are covered separately.
