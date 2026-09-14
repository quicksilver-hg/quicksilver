# Mining

This directory contains public mining and operator integration notes for
Quicksilver.

- [External mining](external-mining.md) describes the `getblocktemplate`
  interface, Cuckatoo block proof fields, Frame-B coinbase values, and
  `submitblock` flow used by external miners.

## Which mining path to use

`startmining` is the bootstrap path. It builds templates in-process, is not
gated on peer count, and is what you want when you are the only node on a chain
or are otherwise starting one from nothing.

`getblocktemplate` is for external pool and miner software on an already-
populated network. On `main` it is peer-gated: with zero peers, or while the
node is still syncing, it refuses rather than hand out a template that could be
built on a stale fork. That gate is deliberate and is not relaxed for
bootstrapping.

See [bootstrapping](../bootstrapping.md#mining-while-bootstrapping) for the
first-node case in full.
