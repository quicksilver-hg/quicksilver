Quicksilver 0.1.x release notes
===============================

These notes describe the public pre-1.0 Quicksilver source series. The 0.1.x
series is intended for source builds, review, testing, and early network
bring-up; it is not a stable release series.

Release status
==============

- This repository is a public pre-1.0 development source tree built from source.
- The 0.1.x version line identifies development snapshots, not a stable API,
  vault, consensus, or packaging contract.
- Binary release artifacts, checksums, and update-notification channels are not
  part of the 0.1.x source-only posture unless maintainers publish them
  separately.
- Quicksilver publishes no maintainer signing keys (see [../SECURITY.md](../SECURITY.md)).
- Report ordinary bugs through this repository's issue tracker.
- Report security-sensitive issues using the process in [../SECURITY.md](../SECURITY.md).

How to upgrade
==============

No stable upgrade path is promised for 0.1.x development snapshots. Before
moving between snapshots, stop Quicksilver cleanly, back up vaults and
configuration, and review the changed consensus, vault, RPC, and data-directory
notes in the target source tree.

Compatibility
=============

GitHub Actions workflows run for repository pushes and pull requests. Source
builds are expected to follow the platform build notes in `doc/build-*.md`, but
0.1.x does not yet promise a stable supported-platform matrix.

Notable changes
===============

- Fresh Quicksilver mainnet, publictest, and sandbox chain parameters.
- Feeless transaction policy with per-transaction proof-of-work.
- Cuckatoo block proof-of-work and mining interfaces for external solvers.
- CoinStats index reward accounting includes Frame-B mint allowances. A local
  CoinStats index created by an earlier development snapshot must be rebuilt
  once with `-reindex`; the node rejects the old semantic index version rather
  than reporting incorrect negative unclaimed rewards.
- Quicksilver vault, RPC, GUI, package, and desktop identity.
- An onion fixed seed on `main` and `publictest`, and no DNS seeds at all. A
  node that cannot reach Tor needs an explicit peer configured with `-addnode`,
  `-connect`, or `-seednode`; see `doc/bootstrapping.md`.

The compatibility boundary
==========================

Quicksilver reads its own formats and speaks its own RPC dialect. It does not
upgrade, translate, or fall back to anything written by another chain's software
or by a pre-remint Quicksilver build. There is no deprecation period, because
there is nothing in the field to deprecate: the launch remint invalidated every
data directory that had ever been written, and no binaries have been published.
Stating the boundary once, at the only moment it costs nothing, is preferred to
carrying compatibility branches that no user can ever exercise.

**On-disk formats fail loudly rather than upgrading.** Each reader supports one
version and rejects anything else with an explicit pre-Quicksilver message:

| File | What is supported |
|---|---|
| `peers.dat` | format 4 only; addresses always read as `CAddress::V2_DISK`. A lower format is rejected with an explicit pre-Quicksilver message rather than misparsed |
| chainstate | the current key layout only. The `DB_COINS` probe, `CCoinsViewDB::NeedsUpgrade`, and the startup check that asked for `-reindex-chainstate` are all gone |
| `vault.dat` | file version 100, and `AgentAllotmentRecord` version 1 — any other record version throws `Unsupported agent allotment record version` |

A data directory from any earlier build must be removed, not migrated.

Vaults are descriptor-only from creation, so the ten legacy record prefixes
(`ckey`, `cscript`, `defaultkey`, `hdchain`, `keymeta`, `key`, `wkey`, `pool`,
`watchmeta`, `watchs`) are no longer declared at all. The loader reads named
prefixes and never enumerates the database, so a row of one of those types is
simply not read. The guard that still matters is untouched: an unrecognised
*descriptor* record aborts the load with `UNKNOWN_DESCRIPTOR`, because that is a
type this tree does write and a newer one can genuinely be unreadable.

**JSON-RPC 2.0 is mandatory.** Every request must carry `"jsonrpc": "2.0"`. A
missing marker, a non-string marker, and any other version — including the
`"1.0"` spelling, which appeared in inherited documentation but in no
specification — are rejected as invalid requests. There is no `"version"`
escape hatch and no 1.1 mode.

**Four request forms are errors, not aliases.**

| Removed form | Replacement |
|---|---|
| boolean `verbosity` on `getblock` / `getrawtransaction` | the integer `verbosity` |
| dictionary-form `outputs` on `send` and related calls | the array form |
| `changeAddress`, `changePosition`, `lockUnspents` | `change_address`, `change_position`, `lock_unspents` |
| `decodepsqt` `global_xpubs` | `global_qpubs`, matching the `qpub` encoding emitted |

The camelCase fund options route through the outdated-option check, so a stale
caller is told `Use change_address instead of changeAddress` rather than getting
an unknown-option error. `createmultisig` now defaults to `bech32`.

Logging and rejection reasons
=============================

Log categories are renamed to Quicksilver's own vocabulary: `net` is now `mesh`,
`mempool` is `relaypool`, `mempoolrej` is `relaypoolrej`, `validation` is
`ledger`, `addrman` is `peerbook`, `selectcoins` is `selectinputs`, and
`blockstorage` is `blockstore`. Four categories are new: `forge` (mining and the
GPU solver bridge), `txpow` (per-transaction proof of work), `vault`, and
`init`. Update any `-debug=<category>` argument accordingly.

Every log line now carries a `[category:level]` prefix by default. Pass
`-loglevelalways=0` for the previous, terser output.

Five rejection reasons are renamed to match:

| Old | New |
|---|---|
| `no-mempool` | `no-relaypool` |
| `too-long-mempool-chain` | `too-long-relaypool-chain` |
| `txn-already-in-mempool` | `txn-already-in-relaypool` |
| `txn-same-nonwitness-data-in-mempool` | `txn-same-nonwitness-data-in-relaypool` |
| `package-mempool-limits` | `package-relaypool-limits` |

These values are returned in the `reject-reason` field of `testrelaypoolaccept`,
so tooling that matches on them needs updating.

Command-line help, RPC help, and RPC error messages adopt the same vocabulary:
the transaction pool is the *relay pool*, and the block index the *ledger
index*. Tooling that matches on RPC error text — `Transaction not in mempool` is
now `Transaction not in the relay pool` — needs updating.

Internal naming
===============

The transaction relay pool's internal C++ identifiers, source filenames, tests,
and documentation say `relaypool` rather than `mempool`, matching the log
category and rejection reasons renamed above.

The relay pool rename
=====================

Every externally observable name for the transaction pool is `relaypool`. There
are no aliases and no deprecation period: an old RPC method returns
method-not-found, an old command-line flag stops the node from starting, and an
old REST path returns 404. This is the only opportunity to make that change
without coordinating across node operators, so it is made in full, at once.

**P2P wire command.** `mempool` is `relaypool`. This is a network-protocol
change: an unrecognised command is ignored rather than rejected, so peers on
mixed builds stop honouring each other's pool requests quietly. Upgrade every
node in a network together.

**Datadir file.** `mempool.dat` is `relaypool.dat`. There is no fallback read —
an existing `mempool.dat` is ignored and left in place. The pool is ephemeral,
so the cost is one repopulation cycle.

**RPC methods.**

| Old | New |
|---|---|
| `getrawmempool` | `getrawrelaypool` |
| `getmempoolinfo` | `getrelaypoolinfo` |
| `getmempoolentry` | `getrelaypoolentry` |
| `getmempoolancestors` | `getrelaypoolancestors` |
| `getmempooldescendants` | `getrelaypooldescendants` |
| `testmempoolaccept` | `testrelaypoolaccept` |
| `savemempool` | `saverelaypool` |
| `importmempool` | `importrelaypool` |

**Command-line flags.** `-maxmempool` is `-maxrelaypool`, `-mempoolexpiry` is
`-relaypoolexpiry`, `-persistmempool` is `-persistrelaypool`, and `-checkmempool` is
`-checkrelaypool`.

**JSON fields and request parameters.** `maxmempool` is `maxrelaypool`,
`mempool_sequence` is `relaypool_sequence`, `mempoolconflicts` is
`relaypoolconflicts`, and the `include_mempool` argument to `gettxout` and
`getdescriptoractivity` is `include_relaypool`.

**REST.** `/rest/mempool/info.json` and `/rest/mempool/contents.json` are
`/rest/relaypool/…`; the `mempool_sequence` query parameter renames with its
JSON counterpart; and the `/rest/getutxos/checkmempool/…` path component is
`checkrelaypool`.

**Network permission.** `-whitelist=mempool@…` is `-whitelist=relaypool@…`. The
permission's whole meaning is "may send the wire command renamed above", so it
renames with it.

**USDT tracepoints.** The provider is `relaypool`, not `mempool`:
`relaypool:added`, `relaypool:removed`, `relaypool:replaced` and
`relaypool:rejected`. Attaching by the old provider name matches nothing —
`bpftrace` and `bcc` report no such probe rather than silently reading zero
events. The argument lists are unchanged; only the names moved. See
[`doc/tracing.md`](tracing.md) for the full definitions and for why the
interface was renamed with everything else rather than kept for
recognisability.

## Credits

Credits are maintained in the source history and project documentation.
