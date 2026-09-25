# Who Stores the Chain

> Status: **decided** (2026-08-04). Consensus nodes prune by default; keeping the full
> chain is a deliberate opt-in. The maintainer runs one archival node at launch, and
> this document says what happens if it disappears.

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

`m_assumed_blockchain_size` and `m_assumed_chain_state_size` therefore carry **128** and
**26**, and every user-facing figure is derived as their sum — **154 GB** — rather than
written out. The GUI never hardcodes it: `Intro` computes it, and `DesktopLaunchPage` and
`ConsensusReviewPage` take the two sizes as constructor arguments and compute the same
sum (`StorageCostsTests` pins all three to one number).

**Use 152.0 when quoting the measurement** — against the 200 GB/year ceiling in
[chain-growth.md](chain-growth.md), or in the calibration record. **Use 154 when telling
an operator what a year of full history costs them.**

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
operator will actually experience, which is why it governs the default.

A pruned node validates every block fully. Nothing about its consensus judgement is
weaker. What it loses is the ability to serve history to somebody else.

## 3. The decision

**Pruning is the default for the consensus capability. Archival storage is the
documented opt-in.**

Upstream's answer — volunteers keep full history, because it is cheap enough — rests on
upstream's roughly 60 GB/year. At 152 GB/year the same assumption is not safe to make
silently. An operator who wants to serve history should choose to, and should see what
it costs at the moment they choose.

The daemon is deliberately **not** changed. `quicksilver-daemon` still defaults to
`-prune=0`, so an operator who runs the node directly gets full history unless they ask
otherwise. The default that changed is the desktop's, because the desktop is what an
ordinary user runs without reading the manual.

### What ships

- `Intro` checks the pruning box by default (`src/qt/intro.cpp`), and no longer
  re-derives that choice from how much free disk the machine happens to have.
- The dialog fires on whether this desktop has ever been set up, not on whether a data
  directory happens to exist (`Intro::IsNeeded`). Existence was the wrong question: any
  directory that was already there — an empty one, or one `quicksilver-daemon` had created —
  skipped the welcome screen, the storage figures and the data directory choice, and
  wrote no prune setting at all, which left that operator **archival by silence**. That
  is the outcome this section exists to prevent, and it was reachable by following
  [getting-started.md](../getting-started.md), which leads with `quicksilver-daemon` (F-123).
- **The pruning choice is offered only where it is still free to make.** A data
  directory that already holds a chain has a settled storage policy, so the dialog
  discloses that rather than defaulting it: checking the box would delete history the
  operator already paid for, and clearing it would leave an already-pruned node unable
  to start without `-reindex`. No prune setting is written in that case
  (`Intro::getPruneMiB` returns nothing), so what the operator chose when they first ran
  the node stands, and Options is where they change it. This is the daemon-first
  operator, whose archival default §3 deliberately leaves alone.
- The consensus review screen states both numbers, defaulted and archival, before the
  control that accepts the cost (`src/qt/consensusreviewpage.cpp`).
- `m_assumed_blockchain_size` / `m_assumed_chain_state_size` are 128 / 26 GB on `main`
  and `publictest` (`src/kernel/chainparams.cpp`), which are **first-year growth**
  figures. This chain starts at height 0, so upstream's "size of the chain today" would
  be zero and would tell a prospective operator nothing.
- `MIN_DISK_SPACE_FOR_BLOCK_FILES` stays at 550 MiB, but its derivation comment in
  `src/validation.h` was rebuilt from Quicksilver's block size and spacing rather than
  the inherited ones. The re-derivation lands at 575.0 MB required against 576.7 MB allowed —
  0.3% of headroom, where upstream had 5.5%.

## 4. Who keeps history, and what happens if they stop

A chain where nobody keeps history cannot bootstrap a new node. Pruning by default
makes that failure mode more likely, so it has to be answered rather than assumed away.

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
copy. Anyone running `quicksilver-daemon` without `-prune` is holding one, which is a further
reason the daemon default was left alone.

This is a real single point of failure and it is stated rather than mitigated. The
mitigations that would remove it — several independent archival operators, or a
UTXO-snapshot bootstrap that lets a node start without history — are not things
v1 can manufacture. v1 does not ship UTXO-snapshot bootstrap.

## 5. Trigger for revisiting

- **The archival default should be reconsidered** if the number of known archival nodes
  falls to one — that is, if the maintainer's node becomes the only copy in existence.
  At that point the correct response is recruiting operators, not changing the default.
- **The pruning default should be reconsidered** if measured honest traffic diverges
  from the 2-output shape enough to move the 26 GB/year figure materially, since the
  whole case for the default rests on that split.

## 6. Related

- [chain-growth.md](chain-growth.md) — the pricing rules that produced these numbers
- [m5-disk-cost](../../tools/calibration/m5-disk-cost/README.md) — the measurement itself
- [bootstrapping.md](../bootstrapping.md) — how a node finds its first peer
- [desktop-application.md](desktop-application.md) — how the cost is disclosed
- [v1-scope.md](v1-scope.md) — what v1 does and does not do
