# Getting Started

This guide starts from a completed source build that passes its own test suite
(`ctest --test-dir build`). For dependencies, build commands and what that run
should look like, see [INSTALL.md](../INSTALL.md).

Quicksilver 0.1.x is experimental. Back up vault data, expect interfaces and
on-disk formats to change, and do not treat development snapshots as a stable
release series.

## Choose a program

- `quicksilver` is the desktop application and vault interface. Build it
  with `-DBUILD_GUI=ON`.
- `quicksilver-daemon` is the headless full node.
- `quicksilver-cli` sends RPC commands to a running node.
- `quicksilver-agent` is the experimental thin agent client. Its limits are
  client-side guardrails, not protocol-enforced spending guarantees; read the
  [v1 scope](design/v1-scope.md) before using it.

The remaining command-line programs are described in
[Developer Tools](developer-tools.md).

## Choose a network

| Network | Start option | Purpose |
| --- | --- | --- |
| `main` | none | Live Quicksilver network |
| `publictest` | `-publictest` | Public test network |
| `sandbox` | `-sandbox` | Private local development and tests |

Network datadirs, address formats, and ports are separate. Do not copy vaults
or configuration between networks without understanding the consequences.

The network option applies to every program in the tree, the desktop included.

## Start the desktop

```bash
./build/bin/quicksilver -publictest
```

With no network option the desktop starts on `main`.

### A first run is vault-only: it runs no node until you enable Consensus

This is the single most important thing to know about the desktop, because
nothing fails and nothing warns:

- The first-run screen asks where to store data and how much disk to use. It
  says the ledger will be built from the genesis block onward when you press
  OK. That describes what happens *after* the step below, not on OK.
- The main window then opens with **Consensus** shown as *Optional* and not
  running. No node has started, no peer has been dialled, no Tor has been
  launched, and there is no `debug.log` in the network's datadir yet.
- Until you enable it, the **Node window** described under
  [Check node health](#check-node-health) reports `N/A` in every field —
  including Client version and Datadir. That is the opt-in being off, not a
  fault, and nothing on the window says so.
- **Mine / Mint** and the **Network** page both redirect to the opt-in page
  while it is off.

To turn it on: **Home → Review cost → Enable consensus**.

The opt-in page states the storage cost and makes *Decline for now* its
highlighted button, so a reader whose goal is to run a node has to choose the
other one. The choice is remembered per network, in that network's Qt settings
(`Desktop/ConsensusEnabled`) rather than in `quicksilver.conf`, so it is made
once per network and not per launch.

Consensus needs a `tor` executable. The Windows installer puts one beside
`quicksilver.exe`, so an installed desktop needs nothing more. On Unix, install
Tor through your package manager; a Windows source build fetches it as described
in [tor.md](tor.md#0-the-desktop-starts-its-own-tor).
The desktop looks beside its own executable and on `PATH`; `-bundledtorpath`
can name another location. If it finds none, it does not start the node: the
Network page says Tor is missing and the vault stays usable. A `tor` placed
beside `quicksilver.exe` is found when you enable Consensus again; a change to
`PATH` or `quicksilver.conf` needs a restart of Quicksilver first. The first time the
node starts on Windows, Windows Defender Firewall may ask whether to allow
Quicksilver on your networks; the node works either way, because it reaches its
peers through Tor. Once enabled, the node starts, the desktop's own
Tor bootstraps, and the shipped onion seed is dialled with no further
configuration — no manual torrc or `-addnode`. On a measured first run against
a short `publictest` chain this reached the seed about 65 seconds after the
opt-in and the chain tip about 10 seconds after that.

Tor hides the network path your node uses. It does not hide the ledger, which
is public either way — see [The ledger is public](#the-ledger-is-public).

**Before the opt-in a vault-only desktop can hold a vault and receive an
address, but it has no validated chain to read balances or history from and no
peer to broadcast a transfer to.** Enable Consensus for anything that depends
on the network.

### The desktop prunes by default

The first-run screen's storage limit **arrives already ticked**, at 2 GB. This
is deliberate, and it is the opposite of the headless default below: this chain
writes on the order of 128 GB of block data a year, so an archival default
would hand every desktop operator a cost they did not choose. Untick it on the
first-run screen to keep full history. The reasoning is in
[Who Stores the Chain](design/chain-storage.md).

## Start the headless node

`quicksilver-daemon` does not start a Tor of its own — it defaults to
`-bundledtor=0`, because someone running a daemon can install and configure
Tor. `main` and `publictest` have no DNS seeds, so to use the shipped onion
seed, run a Tor daemon and point the node at its SOCKS proxy:

```bash
./build/bin/quicksilver-daemon -proxy=127.0.0.1:9050
```

For `publictest`:

```bash
./build/bin/quicksilver-daemon -publictest -proxy=127.0.0.1:9050
```

With `-proxy=`, `quicksilver-daemon` also turns address discovery off, so it does not
advertise the machine's routable addresses.

A SOCKS proxy is the whole requirement for *reaching* the seed. The control
port, cookie authentication and group membership in [tor.md](tor.md) are needed
only to *host* an onion service of your own; skipping them does not stop a node
syncing. The desktop needs none of it — see
[tor.md §0](tor.md#0-the-desktop-starts-its-own-tor).

If Tor is unavailable, supply a peer you obtained out of band. The port belongs
to the network, so name the network too:

```bash
./build/bin/quicksilver-daemon -addnode=<host>:9555               # main
./build/bin/quicksilver-daemon -publictest -addnode=<host>:19557  # publictest
```

The full discovery model, network ports, and failure checks are in
[Bootstrapping](bootstrapping.md).

The daemon keeps full block history by default; the desktop does not, as above.
Use `-prune=<MiB>` only after reading
[Who Stores the Chain](design/chain-storage.md); pruning removes old block data
and is incompatible with some indexes.

## Check node health

These checks only answer once a node is running. On the desktop that means
Consensus is enabled; if every field below reads `N/A`, read
[A first run is vault-only](#a-first-run-is-vault-only-it-runs-no-node-until-you-enable-consensus)
before treating it as a fault.

If you are running `quicksilver`, do not reach for `quicksilver-cli` — the
desktop runs no RPC server by default, so it cannot answer. The same checks are
in the **Node window**: press `Ctrl+Shift+D`, or pick Information, Console,
Network Traffic or Peers from the **Window** menu. The Information tab reports
the connection count and sync height, the Peers tab is `getpeerinfo`, and the
Console tab runs the RPCs below. `Ctrl+Shift+C` opens the Node window with the
Console focused; if the window is already open it raises it without changing
tabs, so select Console yourself. The Console needs no `-server` option and no
RPC credentials, because it calls the node in process. The window becomes
available once startup finishes.

For `quicksilver-daemon`, run these from another terminal:

```bash
./build/bin/quicksilver-cli getblockchaininfo
./build/bin/quicksilver-cli getnetworkinfo
./build/bin/quicksilver-cli getconnectioncount
```

Repeat only the daemon's **network selection** option — `-publictest`,
`-sandbox`, or nothing for `main` — and none of the rest of its command line.
`quicksilver-cli` rejects daemon-only options and stops with
`Error parsing command line arguments`, so `-proxy=127.0.0.1:9050` does not
belong here:

```bash
./build/bin/quicksilver-cli -publictest getblockchaininfo
```

A connection count of zero is not a synchronized node. Follow the diagnostic
order in [Bootstrapping](bootstrapping.md#when-a-node-finds-nothing).

Stop the daemon cleanly, with the same network selection:

```bash
./build/bin/quicksilver-cli stop                 # main
./build/bin/quicksilver-cli -publictest stop     # publictest
```

## The ledger is public

Quicksilver's ledger is public. Every transfer is recorded on it, so anyone can
look up an address and read the amounts it has sent and received. Your name is
not part of a transfer, but anyone who learns one of your addresses can read
that address's history. Use **Generate receiving address** to give a different
address to each person, so they cannot see each other's payments to you.

## Prepare to send

Every non-sandbox transfer performs Cuckatoo proof-of-work before broadcast.
Quicksilver does this on a graphics card when it can, through its graphics
helper, `qsgpusolve-opencl`. It runs on AMD, NVIDIA and Intel cards that have
an OpenCL driver. A source build places the helper beside the application in
`build/bin`, where Quicksilver finds it on its own; on Ubuntu it is the
`quicksilver-gpu` package. Without a usable card, Quicksilver uses this
computer's processor, which can take many minutes per transfer.

On Ubuntu 22.04, AMD cards run on the Mesa driver that the `quicksilver-gpu`
package installs. That driver doesn't report which slot a card is in, so the
helper reads the slot from the card's render node instead and lists it as
`pci:0000:04:00.0`. An id found this way names the same card when another card
is added or removed. Where a device has no render node — a processor device, for
instance — the helper falls back to listing it by its order, as `opencl:0:0`.

Every graphics card except built-in graphics is used by default, and Quicksilver solves on all the chosen cards at once. Choose cards in Settings >
Options > Hardware > Graphics cards, or at startup with `-cuckatoodevice`, using
the id the helper lists:

```bash
./build/bin/qsgpusolve-opencl --list-devices
./build/bin/quicksilver -cuckatoodevice=pci:0000:04:00.0
```

To build a CUDA or ROCm helper yourself and use it instead, see
[GPU Solver](gpu-solver.md).

## Get help or report a problem

For an ordinary bug, open an issue with the platform, build options, network,
command line, and relevant log messages. Never post private keys, vault files,
authentication cookies, or RPC credentials.

Report security-sensitive issues privately as described in
[SECURITY.md](../SECURITY.md).
