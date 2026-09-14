# Seeds

Utility to generate the seeds.txt list that is compiled into the client
(see [src/chainparamsseeds.h](../../src/chainparamsseeds.h) and other utilities in [contrib/seeds](.)).

Quicksilver ships one maintainer-operated Tor onion fixed seed for `main` and
one for `publictest`. Quicksilver does not ship DNS seed sources: there is no
domain to own, and a DNS seed is a renewal liability. The `sandbox` network
continues to clear fixed seeds and DNS seeds.

`makeseeds.py` accepts Quicksilver user agents and filters network compatibility
by protocol version, not by a hand-maintained release-version regex. Keep
`MIN_PROTOCOL_VERSION` aligned with the runtime peer-compatibility floor in
`src/node/protocol_version.h`.

`MIN_BLOCKS` defaults to `0` for the pre-launch network. Use `-m`/`--minblocks`
for seed-generation runs that need a higher chain-height floor.

Treat `contrib/seeds/nodes_main.txt` and `contrib/seeds/nodes_publictest.txt`
as the source records for the shipped fixed seed arrays. Do not regenerate
`src/chainparamsseeds.h` unless those Quicksilver-owned endpoint records change.
When they do, regenerate the header with `generate-seeds.py`.
