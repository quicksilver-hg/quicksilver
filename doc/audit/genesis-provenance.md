# Genesis Provenance Verification

The genesis blocks for all three networks were audited.

## Current values

**These are the live values.** This document is a cascade of amending findings
in the order they happened, each superseding the one before, so the earliest
hashes on the page are the ones that are no longer true. Read this table for
what a node reports today; read the findings for how it got here.

| chain | genesis hash | merkle root | set by |
| --- | --- | --- | --- |
| MAIN | `c9144acae20212e57e9e59f34a681bd25f55d81438001c424ebb95cd4869bcf3` | `da9499c1476c92b69b7214ddb1a365548fff51159e523ca01c77686e022de966` | Ninth finding |
| PUBLICTEST | `917dde1f04c7470969bbdc344d39559e1af32b3b0e6caaed89db54637d6e46da` | `bdf0a510a4f1094ae464987ef01a0fe8409d7c61fee2ef4102d4d84159e78ad6` | Ninth finding |
| SANDBOX | `bd806e80eec28f4b16ab48db377ad6af369fa3b1197cb6d9d77d07db6d5e91e7` | `feacef8e2fca6169ea9726bbf4db05c5efa7c006c1f0d1f98c2cb056d7a06469` | Eighth finding (header layout); never re-minted since |

The authority for all six is the `assert` block in `src/kernel/chainparams.cpp`,
which aborts every binary at startup if the derived value disagrees. To check a
running node against this table:

```bash
quicksilver-cli getblockhash 0                 # main
quicksilver-cli -publictest getblockhash 0     # publictest
```

**The findings are numbered as they were filed, and there is no Seventh
finding.** The numbering jumps from Sixth to Eighth; nothing is missing from
the record. Later text refers to findings by these numbers, so they have been
left as filed rather than closed up.

## Finding

`CreateGenesisBlock` built the genesis coinbase scriptSig as

    CScript() << 486604799 << CScriptNum(4) << <headline>

## Resolution

The scriptSig carries the Quicksilver headline. Genesis was re-derived
for MAIN, PUBLIC_TEST, and SANDBOX, which changed each genesis and merkle hash:

- MAIN     `472a2f9c…` (merkle `520515ef…`) — superseded by the third finding below
- PUBLIC_TEST `311c54be…` (merkle `5b0d95d9…`)
- SANDBOX  `944364d0…` (merkle `9d175b28…`)

Genesis remains a PoW-exempt zero-cycle trust anchor.

## Cascade

Re-rooting the sandbox chain shifted every descendant block hash. UTXO-set
MuHash values are unaffected — the genesis output is an unspendable
`OP_RETURN`, so it never enters the UTXO set — so only block hashes were
regenerated:

- the `TestChain100Setup` 100-block tip fixture.

## Second finding (2026-07-19): publictest genesis `nBits`

The audit above cleared the coinbase scriptSig but left the genesis **header**
`nBits`. publictest's genesis was still built with `0x1d00ffff` while its
`powLimit` had moved to 2^255 (compact `0x207fffff`). MAIN and SANDBOX were
self-consistent — main's `powLimit` genuinely is 2^224, so `0x1d00ffff` is its
correct compact.

This was not cosmetic. `GetNextWorkRequired` (`src/pow.cpp`) applies: a block
more than `2 * nPowTargetSpacing` after its parent gets `powLimit`, otherwise
the code walks back to the last non-min-difficulty block and reuses its `nBits`.
On a young chain every block is min-difficulty, so the walk reached genesis and
demanded Bitcoin difficulty-1 for any block mined within 600s of its parent
— stalling block production on the private testrun. Blocks only landed after the
600s window elapsed, which read as a hardware-speed limit but was not.

Resolution: publictest genesis `nBits` is now `0x207fffff`, its own `powLimit`
compact. Only the genesis **hash** changed; the merkle root is unaffected because
`nBits` is not an input to it:

- PUBLIC_TEST `90328f83…` (merkle `5b0d95d9…`, unchanged)

Cascade is limited to the `chainparams.cpp` assert and the `pow_tests` genesis
hash vector — the sandbox-rooted fixtures from the first finding are untouched.

Guard: `pow_tests/genesis_nbits_equals_chain_powlimit` asserts, for all three
networks, that genesis `nBits` **equals** the chain's `powLimit` compact. The
pre-existing `sanity_check_chainparams` check is one-sided (`powLimit >= genesis
target`) and therefore admits a harder genesis; it could not catch this.

## Third finding (2026-07-21): mainnet genesis at the launch floor

The first two findings left mainnet self-consistent but at the wrong difficulty:
`powLimit` was still the inherited 2^224 and genesis carried its matching compact
`0x1d00ffff`. Consistent, and unmineable — at Quicksilver's measured 0.006228
cycles/s per commodity GPU, a 2^224 floor is days per block, so the chain could
not have been started by the hardware it is designed for.

P0b moves mainnet to the launch floor derived in
`doc/audit/mainnet-difficulty-floor-model.md`: `powLimit` 2^255 (compact
`0x207fffff`, 2 cycles per block, 321 s on one GPU). Genesis is re-minted in the
same change because `nBits` must equal the chain `powLimit`, and re-dated to
2026-08-01 00:00 UTC (`nTime` 1785542400), the intended launch window. Both the
headline and the header changed, so both hashes moved:

- MAIN `46ca06e23004fd503603ff4c08b017d534aaf153850bf55a06a15c3458a69b23` (merkle `5c8c122a7ce5fbe839cf026c5d8ee871b0a7f06e22e4d635b77bc414852a4d06`)

Cascade: the `chainparams.cpp` asserts, the `pow_tests` genesis vectors, and
`contrib/linearize/example-linearize.cfg`. No sandbox-rooted fixture is affected.
The superseded hash `472a2f9c…` is kept as a `BOOST_CHECK_NE` lock so the 2^224
genesis cannot return.

## Fourth finding (2026-08-02): re-mint for the E28 flag day

**This amends, and does not replace, the third finding above.** P0b marked genesis
FINALIZED; that record stands as what P0b did and why. This is the one change since
that has moved it, recorded here rather than silently overwritten.

Both Cuckatoo graph sizes moved 29 → 28 (`nEdgeBits` and `nTxEdgeBits`, on `main`
and `publictest`; `sandbox` is unaffected and keeps E19). A cycle at E28 is 2.06×
cheaper to find, and the difficulty floor is stated in *real work per block* rather
than in `nBits` — so holding `nBits` would have halved the work in the cheapest
possible block. The floor therefore moved 2 → 4 cycles, `powLimit` 2^255 − 1 →
2^254 − 1, and genesis `nBits` `0x207fffff` → `0x203fffff`. Derivation:
`tools/calibration/e28-floor.md`.

`nBits` is a **header** field, so the block hashes moved. The coinbase transaction
is untouched, so **the merkle roots did not**:

| chain | genesis hash | merkle root |
| --- | --- | --- |
| MAIN | `319aa1b4a8a7a6d7ea636cef44b9a38e92e7c3a1b1a3b72a3b1c40386936662e` | `5c8c122a7ce5fbe839cf026c5d8ee871b0a7f06e22e4d635b77bc414852a4d06` (unchanged) |
| PUBLICTEST | `06fb6c59c9aa03d0d0aa809a6a379bea1242a2aefd9c7dfbce7ec47cba191aea` | `48bc188c7ed1b304d99f3890e8ab6012b9bc0debf4757c9254511ba653b8426e` (unchanged) |

Superseded, and locked out by `BOOST_CHECK_NE` alongside the earlier retired
genesis hashes: MAIN `46ca06e2…`, PUBLICTEST `dc995052…`. A node carrying either
is running the old floor.

**Every `main` and `publictest` datadir is invalidated by this change** and must be
wiped. Same cascade as the third finding: the `chainparams.cpp` asserts, the
`pow_tests` genesis vectors, and `contrib/linearize/example-linearize.cfg`. No
sandbox-rooted fixture is affected.

## Verification

- New guard test `pow_tests/genesis_coinbase_carries_no_bitcoin_magic` asserts
  the `0x1D00FFFF` magic is absent from every network's genesis coinbase
  scriptSig, preventing reintroduction.
- `sanity_check_chainparams` proves each hardcoded genesis hash equals the live
  derivation (i.e. the asserts are current).
- Full unit suite (139/139) pass.
- A publictest node boots with no genesis assert and reports its tip as the
  re-derived genesis hash.

## Fifth finding (2026-08-16): launch remint of all three networks

**This amends, and does not replace, the fourth finding above.** The E28
record stands as what that flag day did and why. Live launch remints every
network so no published chain still names a window that already passed.

Headline and `nTime` both moved, so merkle roots and block hashes both moved.
`nBits`, nonce, version, reward, and the OP_RETURN genesis mark are unchanged.

| chain | `nTime` | genesis hash | merkle root |
| --- | --- | --- | --- |
| MAIN | `1786838400` (2026-08-16 00:00 UTC) | `37bf0859c1f99a2a22c827ae6f0d7371f9b4d71575e3b9c0c8819cc77f6be1ca` | `2bb1e798e84f0535c1de99e8d2cf451fc72fab3277fc20a084e749ae562807db` |
| PUBLICTEST | `1786838400` (2026-08-16 00:00 UTC) | `736a88327bca8f58b3b85ac7f7edb03066457f682215e1dd1f8fb0a7ddb28af3` | `d4be59182a109f30be9d89090fa3d63f9c4c1fa1c237d5760ae51c3e78cda08d` |
| SANDBOX | `1750000000` (test-chain clock; headline reminted) | `5199caf02a08c6e3a6817a8871d1f97e9f9fbbfab0ddd9de4f634d2095382028` | `526f2be458a70fb74502ddc8ec8323cccab80d3789b082b339aa891707953bc9` |

Superseded, and locked out by `BOOST_CHECK_NE`: MAIN `319aa1b4…`, PUBLICTEST
`06fb6c59…`, SANDBOX `468da7ae…`. A node carrying any of those is on a
retired chain.

Sandbox keeps `nTime` `1750000000`. That value is the local test-chain clock,
not a launch-date claim. Dating it to 2026-08-16 made every sandbox fixture
block "current" under `max_tip_age`, so `IsInitialBlockDownload()` went false.
The headline still remints, so the merkle root and block hash both move.

**Every `main`, `publictest`, and `sandbox` datadir is invalidated** and must
be wiped. Sandbox-rooted fixtures also moved, including the `TestChain100Setup`
100-block tip. UTXO-set MuHash values are unchanged — the genesis
output is an unspendable `OP_RETURN`, so it never enters the UTXO set.

Cascade: the `chainparams.cpp` asserts, the `pow_tests` genesis vectors,
`contrib/linearize/example-linearize.cfg`, `contrib/linearize/linearize-data.py`,
and `src/test/util/setup_common.cpp`.

## Sixth finding (2026-08-24): authorship added to the genesis mark

**This amends, and does not replace, the fifth finding above.** The 2026-08-16
remint stands as what that day did and why.

The OP_RETURN genesis mark gained the authors it names. It moves from

```
Quicksilver Genesis - for the agents, raised by human, Claude, and Codex
Quicksilver Genesis - for the agents, raised by human, Claude, Codex, and Grok
```

72 bytes to 78. This is the first remint in which the mark itself changed;
every previous one moved only the headline and `nTime`. Because the mark is the
sole output's `scriptPubKey`, it is committed by the coinbase txid, so the
merkle root and block hash both move on every network.

The headline and `nTime` move with it, to the date the mint actually happened.
A genesis that claims a date it was not minted on is a false statement carried
forever in the chain's first block.

| chain | `nTime` | genesis hash | merkle root |
| --- | --- | --- | --- |
| MAIN | `1787529600` (2026-08-24 00:00 UTC) | `b91f69d2d7ddfae4278bb7de73a264d5fad149f38067a9154598e7040b5f1e64` | `eb99b8f955a7eb78c224733d69254fb443f7c5664175e9275755c389298114bb` |
| PUBLICTEST | `1787529600` (2026-08-24 00:00 UTC) | `c8f8ac7b09069bc1cacf17a2db2e272262066051fc23e664be0171f86c5df974` | `2e96feb07ace548b48ce57ab15f16c5d3509a45671f9b3a09f77a33bd59608e8` |
| SANDBOX | `1750000000` (test-chain clock; headline reminted) | `3429dd6cc6de0548882758b92004a233b805b9b1e53c5d44ffe0bfd2e97189ca` | `feacef8e2fca6169ea9726bbf4db05c5efa7c006c1f0d1f98c2cb056d7a06469` |

Sandbox keeps `nTime` `1750000000` for the reason given in the fifth finding —
it is the local test-chain clock, not a launch-date claim, and moving it makes
`IsInitialBlockDownload()` go false across the sandbox fixtures.

Newly superseded, and locked out by `BOOST_CHECK_NE`: MAIN `37bf0859…`,
PUBLICTEST `736a8832…`, SANDBOX `5199caf0…`. Nothing was ever published on
them, but a node carrying one is on a chain nobody else is on.

`nBits`, nonce, version, and reward are unchanged. Script sizes stay inside
their consensus limits with room to spare: the longest coinbase `scriptSig` is
publictest's at 68 of the 100-byte limit, and the mark's `scriptPubKey` is 81
bytes against `MAX_OP_RETURN_RELAY` of 83 — the mark cannot grow by more than
two further bytes without a consensus change.

**Every `main`, `publictest`, and `sandbox` datadir is invalidated** and must be
wiped. Sandbox-rooted fixtures moved again: the `TestChain100Setup` 100-block
tip is now `882b6abd324deb420da008ac6bbce9f8659b8ec37fd0b6948742c8c3bb151ae0`.
(Superseded by the eighth finding below — these are no longer the live values.)
UTXO-set MuHash values are unchanged — the genesis output is an unspendable
`OP_RETURN`, so it never enters the UTXO set.

Cascade: the `chainparams.cpp` asserts, the `pow_tests` genesis vectors,
`contrib/linearize/example-linearize.cfg`, `contrib/linearize/linearize-data.py`,
`src/test/util/setup_common.cpp`, and `test/functional/rpc_getblockstats.py`.
That last one is new to this cascade and was not named in any previous finding.
It pins **two** genesis-dependent constants, neither visible from
`chainparams.cpp`:

1. the main genesis hash, in a "block not found on sandbox" fixture; and
2. `genesis_stats["utxo_size_inc"]`, the serialized size of the genesis
   OP_RETURN output, which moved 124 → 131.

The second is worth spelling out because the delta is not the obvious one. The
mark grew 6 bytes (72 → 78), but the script grew **7**: at 78 bytes the push no
longer fits a direct push opcode, so the encoding changes to `OP_PUSHDATA1` and
costs one more byte. The constant is
`GetSerializeSize(out) + PER_UTXO_OVERHEAD`, where `PER_UTXO_OVERHEAD` is
`sizeof(COutPoint) + sizeof(uint32_t) + sizeof(bool)` = 41:

| | scriptPubKey | serialize | + overhead |
| --- | --- | --- | --- |
| 72-byte mark | `OP_RETURN` + push + 72 = 74 | 8 + 1 + 74 = 83 | **124** |
| 78-byte mark | `OP_RETURN` + `OP_PUSHDATA1` + len + 78 = 81 | 8 + 1 + 81 = 90 | **131** |

Both values reproduce from the formula, so 131 is derived, not merely observed.
Only inspection found constant 1; constant 2 was found by the functional suite,
which is the argument for running all 233 rather than the files believed to be
touched.

## Verification

- The new hashes were derived independently of the build, by a serializer
  reimplemented from `CreateGenesisBlock`, `CBlockHeader`, and
  `SerializeTransaction`, and validated by reproducing all six *previous*
  hashes exactly from their own inputs before being trusted on the new ones.
- `pow_tests` (34 cases) then confirmed all six against the live derivation.
- The `TestChain100Setup` tip was harvested from a run, not computed, and
  cross-checked across two independent suites.

---

## Eighth finding (2026-08-26): the congestion multiplier enters the header

This is a **remint of the header format**, not of the genesis coinbase. It
supersedes the "final" framing of the 2026-08-24 remint above.

### Why

A thin client cannot compute a per-transaction proof-of-work target. The target
is `base * m / CONGESTION_ONE`. `base` derives from `GetBlockProof`, which reads
only `nBits`, so a header-only client can reach it. `m` — the EIP-1559 congestion
multiplier — was computed at `ConnectBlock` from the parent's `m` and *this
block's weight*, and block weight is neither in the header nor reachable from a
header chain. So `signbundle -prove=1` hard-failed at any anchor past genesis
(flag F-51), and the agent client could not spend at all.

`nCongestion` (4 bytes, fixed point, `CONGESTION_ONE == 65536` is 1.0) is now a
header field. `ConnectBlock` no longer computes it; it **verifies** it against
`NextCongestionMultiplier(pprev->m_congestion, GetBlockWeight(block))` and
rejects a mismatch as `bad-congestion`.

### The ordering is load-bearing

`nCongestion` is serialized **before** `nNonce`, not appended after it. Every
Cuckatoo solver entry point grinds the last four bytes of the pre-pow in place —
`solve_19`/`solve_28` via `mutate_nonce`, `Solve28Bytes`, `dispatch.cpp`'s key
reconstruction, both bench `KeyedPrepow` helpers, and the external CUDA solver
`gpu/qsgpusolve.cu`. Appending after `nNonce` would have left all six grinding the
congestion field while the nonce sat frozen, and would not have failed loudly.
`pow_tests/congestion_header_prepow_layout_keeps_nonce_at_the_tail` is the
permanent fence.

### What moved

The pre-pow grew 80 → **84** bytes and the serialized header 248 → **252**. The
genesis **coinbase is untouched, so every merkle root is unchanged**; only the
header hash moved, on all three networks. Genesis carries
`nCongestion = CONGESTION_ONE`, which is the seed of the entire recurrence.

| chain | `nTime` | genesis hash | merkle root |
| --- | --- | --- | --- |
| MAIN | `1787529600` (2026-08-24 00:00 UTC) | `e87427f26217fa34d390632aaf5d0d91f1c1fe3e6db4db9e4b643514772ff959` | `eb99b8f955a7eb78c224733d69254fb443f7c5664175e9275755c389298114bb` (unchanged) |
| PUBLICTEST | `1787529600` (2026-08-24 00:00 UTC) | `380519f5da3e0a735061c068eb3d3c507053697da8256fbc89d741d179dadafb` | `2e96feb07ace548b48ce57ab15f16c5d3509a45671f9b3a09f77a33bd59608e8` (unchanged) |
| SANDBOX | `1750000000` (test-chain clock) | `bd806e80eec28f4b16ab48db377ad6af369fa3b1197cb6d9d77d07db6d5e91e7` | `feacef8e2fca6169ea9726bbf4db05c5efa7c006c1f0d1f98c2cb056d7a06469` (unchanged) |

Newly superseded, and locked out by `BOOST_CHECK_NE` where already recorded:
MAIN `b91f69d2…`, PUBLICTEST `c8f8ac7b…`, SANDBOX `3429dd6c…`. The genesis mark
is unaffected by the widening and stays 81 of the 83-byte `MAX_OP_RETURN_RELAY`
ceiling.

**Every `main`, `publictest`, and `sandbox` datadir is invalidated** and must be
wiped — the header format changed, so this is not a reorg but a different chain.
The `TestChain100Setup` 100-block tip is now
`53fff1e90bbcd146a5f06aee8c27fcdea984b6618f1dda31cdc322609b31416a`.

### Cascade

Beyond the usual set (`chainparams.cpp` asserts, `pow_tests` vectors, both
`contrib/linearize` files, `setup_common.cpp`, `rpc_getblockstats.py`), the
header widening reached two places a genesis-only remint never touches:

1. **`-fastprune` block-file wrap points.** A sandbox block is now **601 bytes**
   (measured on the chain, not derived on paper — the old comment's "~605" was
   already stale), so 64 KiB files hold ~109 blocks and every wrap boundary
   moved. `feature_index_prune.py`'s three prune heights became 307 → **306**,
   739 → **734**, 2158 → **2139**. All three were harvested from a run.
2. **`getblocktemplate`.** External miners build the header themselves, so the
   template now publishes `congestion` alongside `bits`, plus a
   `pow.congestionparams` object (`one`, `targetpermille`, `stepdenom`,
   `maxmultiplier`). The published value is correct **only for the template's own
   transaction set**; `transactions` is in the `mutable` list, so a miner that
   changes the body must recompute from `congestionparams` before grinding.
   `BlockTemplate::submitSolution` gained a `congestion` argument for the same
   reason: it accepts a caller-supplied coinbase, which changes the weight.

### Disk impact

+4 bytes per block, in exactly one store. `blocks/blk*.dat` grows 0.42 MB/year
(+0.0003% of the measured 152.0 GB/year archival total); undo files and the UTXO
chainstate carry no header and do not move; the LevelDB block index does not
change format at all, because `m_congestion` was **already** a VARINT in
`CDiskBlockIndex` — only its provenance changed, from derived state to header
field. `MIN_DISK_SPACE_FOR_BLOCK_FILES` was checked rather than assumed: the +4
bytes move its 288-block derivation by ~1.2 KB against 1.7 MB of headroom.
Header-only agents pay 26.07 → 26.49 MB/year (+1.6%), which is what bought them
the ability to prove at all.

### Verification

- The three new hashes were derived offline from the *already asserted* merkle
  roots plus the header fields, and the derivation was validated by reproducing
  all three **previous** hashes exactly before being trusted on the new ones.
- The `bad-congestion` consensus rule was mutation-proven in both directions:
  with the check disabled a too-high multiplier connects, and with the check made
  one-sided a too-low multiplier connects. Only then was the green run trusted.
- The `TestChain100Setup` tip and all three prune heights were harvested from
  runs and confirmed stable across repeats, not predicted from arithmetic.

## Ninth finding (2026-09-05): re-mint for the F-147 retarget fix

⚠ **This re-mint was a choice, not a consequence.** The F-147 change does not
alter a single genesis byte: genesis carries `powLimit` as its `nBits`, and no
retarget occurs at height 0. The owner was told this explicitly and chose to
re-mint anyway, for clean network separation from the chains that had already
been mined under the old anchor. This is the **second** re-mint after one was
described as final — the Eighth finding above superseded the first such framing,
and this one supersedes the Eighth's.

### Why

The difficulty retarget anchored one block too late. `GetNextWorkRequired`
reached back `DifficultyAdjustmentInterval()` blocks from the last block of the
period, landing *inside* the period rather than on its first block, so every
window measured `interval - 1` spacings while dividing by `interval` worth of
target spacing. The chain therefore read every honest period as ~0.7 % faster
than it was and ratcheted difficulty up to a steady state ~1.39 % fast. That is
a leak, not a break — the retarget's own feedback bounds it — but it also left
`MAX_TIMEWARP` load-bearing, and both have now been removed.

Moving the anchor changes the target of the **first** retarget at height 144 and
of every retarget after it. MAIN had no published history to lose. PUBLICTEST
was live at tip ~304, so real mined history was discarded.

### What moved

Only the two re-mintable networks, and only two inputs each: the coinbase
headline date `2026-08-24` → `2026-09-05`, and `nTime` `1787529600` →
`1788566400` (2026-09-05 00:00 UTC). `nBits` (`0x203fffff`), `nNonce` (`0`),
block version, the 50 HgS reward and the genesis mark are all unchanged. Because
the headline is in the coinbase scriptSig, the merkle roots moved too.

| chain | `nTime` | genesis hash | merkle root |
| --- | --- | --- | --- |
| MAIN | `1788566400` (2026-09-05 00:00 UTC) | `c9144acae20212e57e9e59f34a681bd25f55d81438001c424ebb95cd4869bcf3` | `da9499c1476c92b69b7214ddb1a365548fff51159e523ca01c77686e022de966` |
| PUBLICTEST | `1788566400` (2026-09-05 00:00 UTC) | `917dde1f04c7470969bbdc344d39559e1af32b3b0e6caaed89db54637d6e46da` | `bdf0a510a4f1094ae464987ef01a0fe8409d7c61fee2ef4102d4d84159e78ad6` |

**SANDBOX is untouched** — not its genesis (`bd806e80…`), not its magic
(`0x52`). It sets `fPowNoRetargeting = true`, so the anchor move invalidates no
sandbox history at all.

Newly superseded and locked out by `BOOST_CHECK_NE` in
`pow_tests/quicksilver_public_genesis_is_fresh_zero_cycle_trust_anchor`: MAIN
`e87427f2…`, PUBLICTEST `380519f5…`. **Every `main` and `publictest` datadir is
invalidated** and must be wiped.

### Magic

PUBLICTEST's network magic moved `0x55` → `0x56` (`HGQU` → `HGQV`). Without it a
stale node completes a handshake and *then* never agrees on a chain, which
presents as a silent sync stall; with it, the peer fails at the header check and
says so. MAIN's magic is unchanged — nothing was ever published on mainnet.

🔴 The magic has **two** definitions and they must move together:
`src/kernel/chainparams.cpp` and `test/functional/test_framework/messages.py`.
Missing the second does not fail to compile — it fails every publictest P2P
functional test with `Wrong MessageStart`, which reads like a network fault
rather than a stale constant. Both moved in this change, and a sweep confirmed
there is no third definition.

### Cascade

`src/kernel/chainparams.cpp`, `src/test/pow_tests.cpp`,
`test/functional/test_framework/messages.py`,
`contrib/linearize/example-linearize.cfg`,
`contrib/linearize/linearize-data.py`, `test/functional/rpc_getblockstats.py`,
`contrib/genesis/derive-genesis.py`, and this document.

⚖ `src/test/util/setup_common.cpp` is deliberately **not** in the cascade. It
contains none of the four hashes and builds on the sandbox chain, which is not
re-minted. It appeared in the Eighth finding's list only because that change
widened the header and moved `TestChain100Setup`'s tip — a different mechanism.

### Verification

- The four new hashes were derived offline by `contrib/genesis/derive-genesis.py`,
  which reproduced all six **previously asserted** values — MAIN, PUBLICTEST and
  SANDBOX, merkle root and block hash each — exactly, before being trusted on the
  new ones. It refuses to derive anything if that self-check fails.
- SANDBOX is in the self-check even though it is never re-minted. MAIN and
  PUBLICTEST share an `nTime`, an `nNonce` of `0` and an `nBits` of `0x203fffff`,
  so on those two alone a serializer that dropped `nNonce` or `nBits` entirely
  would still reproduce all four hashes. SANDBOX differs in all three, so it is
  what actually binds those fields.
- The C++ `assert`s in `chainparams.cpp` are the independent second opinion: a
  wrong hash aborts every test binary at startup.
