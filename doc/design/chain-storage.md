# Who Stores the Chain

> Status: **decided** (2026-08-04), **reversed for the desktop** (2026-11-21, F-705).
> Consensus nodes keep the full chain by default, on the desktop as on the daemon;
> pruning is one checkbox away, with its cost shown. The maintainer runs one archival
> node at launch, and this document says what happens if it disappears.

## 1. The measured cost

Growth depends on what traffic looks like, not only on the block cap, so a single
headline number is not meaningful. M5 measured three shapes at the shipped 4 MB weight
cap and 300 s spacing (`tools/calibration/m5-disk-cost`), filling blocks to 99%+ of the
cap in every case:

| shape | tx/block | blocks+undo B/block | chainstate B/block | GB/year |
| --- | ---: | ---: | ---: | ---: |
| Honest, 2 outputs | 3,099 | 1,217,399 | 244,224 | **152.0** |
| Mid, 200 outputs | 114 | 998,125 | 2,960,064 | 412.2 |
| Adversarial, 2,300 outputs | 11 | 992,732 | 3,061,952 | 426.3 |

The byte-pricing change then cut sustained adversarial growth to **62.9 GB/year**,
because filling a block with UTXO-heavy transactions now costs at least as much work as
mining one. See [chain-growth.md](chain-growth.md) §5.

So the operating number for a launched chain carrying real traffic is **152 GB/year**,
and the adversarial shapes are no longer reachable at sustained volume.

### The measured total and the figure the product quotes differ by 2 GB

**Measured is 152.0; the screens say 154.** Both are honest, and the difference is worth
stating because it caused a real defect (F-126) when two screens picked different halves
of it.

The 152.0 total above is the sum of *three* measured components, and the third is
negative: `block_index` came back at −15,872 B/block (−1.7 GB/year) because LevelDB
compacted the index during the measurement window. It is compaction noise, not a saving
any operator banks. The two components that persist are blocks+undo at 128.0 GB/year and
chainstate at 25.7 GB/year, which is why §2's table sums to 153.7 rather than to 152.0.

The full-traffic capacity is 128 GB of block data and 26 GB of chain state a year,
154 GB in all. Since F-601 the parameters carry the measured estimate instead (1 / 1 GB,
see below), and the screens derive their totals from those two values, never a
written-out number: `Intro` computes the sum, and `DesktopLaunchPage` and
`ConsensusReviewPage` take the two sizes as constructor arguments and compute the same
sum (`StorageCostsTests` pins all three screens to one figure).

**Use 152.0 when quoting the measurement** — against the 200 GB/year ceiling in
[chain-growth.md](chain-growth.md), or in the calibration record. **Use 154 for the full-traffic capacity.**

**The 420 GB/year figure that appeared throughout this project's documents and screens
is retired.** It assumed 4 MB of block bytes per block, where realistic weight-full
blocks are about 1.2 MB, and it counted no chainstate at all. The two errors happened
to cancel to within 1.5% of the adversarial measurement, which is why it survived so
long. It was never a validated number.

## 2. What pruning actually removes

At the honest shape the split is:

| component | GB/year | share |
| --- | ---: | ---: |
| blocks + undo | 128.0 | 84% |
| chainstate (the UTXO set) | 25.7 | 16% |

Pruning deletes old block and undo files. It does not touch chainstate. So a pruned
node's ongoing cost is about **26 GB/year**, against 152 GB/year archival — pruning
removes roughly five sixths of the cost for an ordinary operator.

**This inverts under adversarial traffic**, where chainstate is 75–83% and pruning
removes only about a quarter. `chain-growth.md` §3 quotes those percentages and reads
as though they were general; they describe the 2,300-output shape specifically. Both
statements are true of the shape each was measured on. The honest shape is the one an
operator will actually experience, which is why it governs the pruning trade-off.

A pruned node validates every block fully. Nothing about its consensus judgement is
weaker. What it loses is the ability to serve history to somebody else.

## 3. The decision

**Keeping the full chain is the default for the consensus capability, on the desktop as
on the daemon. Pruning is one checkbox away, with its disk cost shown.**

This reverses the decision of 2026-08-04, which made pruning the desktop default. The
reversal is dated 2026-11-21 (F-705). Two facts decided it:

- **A pruned node does not help a new node sync.** It never advertises `NODE_NETWORK`
  (`src/init.cpp`), and it refuses requests for blocks deeper than its keep window even
  while it still holds every block, so peers looking for history pass it by. With pruning
  as the desktop default, the desktops a new node met could not give it the chain, which
  made the failure in §4 the likely outcome rather than an edge case.
- **The cost the default guarded against is not what the chain carries.** 152 GB/year is
  the full-traffic figure; the measured chain grows by about 0.07 GB/year (F-601, measured
  2026-11-10). That is the divergence from the design shape that §5 named as the trigger
  for revisiting.

The original reasoning stands as the case for turning pruning back on. Upstream's answer —
volunteers keep full history, because it is cheap enough — rests on upstream's roughly
60 GB/year, and at 152 GB/year the same assumption would not be safe to make silently.
Until traffic approaches that figure, an archival default costs a desktop operator very
little and gives the network the history it needs. Either way, the operator sees both
costs at the moment they choose.

`quicksilver-daemon` has always defaulted to `-prune=0`, and still does. The desktop now
matches it.

### What ships

- `Intro` leaves the pruning box unchecked by default (`src/qt/intro.cpp`), so a new
  desktop keeps the full chain. It does not re-derive that choice from how much free disk
  the machine happens to have; the free-space line says when the full chain will not fit.
  Ticking the box prunes to the default target below. A data directory set up before
  this change keeps the choice it made then; nothing is migrated.
- The dialog fires on whether this desktop has ever been set up, not on whether a data
  directory happens to exist (`Intro::IsNeeded`). Existence was the wrong question: any
  directory that was already there — an empty one, or one `quicksilver-daemon` had created —
  skipped the welcome screen, the storage figures and the data directory choice, and
  wrote no prune setting at all, which left that operator **archival by silence**. An
  operator should see the cost and the choice before taking either on, and this failure
  was reachable by following [getting-started.md](../getting-started.md), which leads
  with `quicksilver-daemon` (F-123).
- **The pruning choice is offered only where it is still free to make.** A data
  directory that already holds a chain has a settled storage policy, so the dialog
  discloses that rather than defaulting it: checking the box would delete history the
  operator already paid for, and clearing it would leave an already-pruned node unable
  to start without `-reindex`. No prune setting is written in that case
  (`Intro::getPruneMiB` returns nothing), so what the operator chose when they first ran
  the node stands, and Options is where they change it. This is the daemon-first
  operator, whose archival default §3 deliberately leaves alone.
- The consensus review screen states both numbers, pruned and archival, before the
  control that accepts the cost (`src/qt/consensusreviewpage.cpp`).
- `m_assumed_blockchain_size` / `m_assumed_chain_state_size` are 1 / 1 GB on `main`
  and `publictest` (`src/kernel/chainparams.cpp`): an estimate from the measured chain
  plus a margin (F-601, measured 2026-11-10 at about 0.07 GB/year). The 128 / 26 GB
  first-year capacity figures above remain the design ceiling.
- A pruned node keeps the last 2016 blocks, one week at 300 s spacing, and serves them to
  peers (F-703; upstream's 288 blocks are only one day here). It still asks a pruned peer
  only for blocks within 288 of that peer's tip, the depth older nodes serve, so old and
  new nodes work together; a later release raises that once the network has upgraded.
- The desktop keeps the full chain by default. If pruning is selected, its
  whole-GB control has a 4 GB minimum, rounded up from the 2,956 MiB floor.
- `MIN_DISK_SPACE_FOR_BLOCK_FILES` is 2,956 MiB on main and publictest. Its
  derivation in `src/validation.h` uses 2016 kept blocks, measured block and undo
  size, a 20% orphan margin and a block-file chunk. Sandbox retains a separate
  550 MiB floor because its blocks are smaller.

## 4. Who keeps history, and what happens if they stop

A chain where nobody keeps history cannot bootstrap a new node. Pruning by default
made that failure mode more likely, which is part of why the desktop default was
reversed (§3). It still has to be answered rather than assumed away, because any
operator can tick the box.

**At launch, the maintainer runs one archival node**, alongside the onion seed
described in [bootstrapping.md](../bootstrapping.md). That is an operational
commitment, accepted knowingly, and it is the same commitment the seed already
represents.

If that node disappears and no other archival node exists:

- Every running node keeps working. Consensus does not depend on history being served.
- Nodes already synced stay synced.
- **A new node cannot join.** It would have headers and no way to obtain the blocks
  those headers commit to.

The recovery from that state is not automatic: it needs somebody to restore an archival
copy. Anyone running a node without pruning — the default on both the daemon and the
desktop — is holding one.

This is a real single point of failure and it is stated rather than mitigated. The
mitigations that would remove it — several independent archival operators, or a
UTXO-snapshot bootstrap that lets a node start without history — are not things
v1 can manufacture. v1 does not ship UTXO-snapshot bootstrap.

## 5. Trigger for revisiting

- **Pruning should be turned back on by default** if measured honest growth approaches
  the design figures in §1 (128 GB of block data and 26 GB of chain state a year at full
  traffic). The full-chain default rests on the chain carrying far less than that; it
  was restored on 2026-11-21 at about 0.07 GB/year measured. Check the traffic shape at
  the same time: the case for pruning rests on its removing about five sixths of the
  cost at the 2-output shape (§2), and less under UTXO-heavy traffic.
- **If the known archival nodes fall to one** — the maintainer's node becomes the only
  copy in existence — the correct response is recruiting operators. Find out why
  desktops are pruning before changing anything else; the default already keeps the
  full chain.

## 6. Related

- [chain-growth.md](chain-growth.md) — the pricing rules that produced these numbers
- [m5-disk-cost](../../tools/calibration/m5-disk-cost/README.md) — the measurement itself
- [bootstrapping.md](../bootstrapping.md) — how a node finds its first peer
- [desktop-application.md](desktop-application.md) — how the cost is disclosed
- [v1-scope.md](v1-scope.md) — what v1 does and does not do
