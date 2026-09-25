# Bootstrapping: how a Quicksilver node finds the network

A node that knows no peers can do nothing. It will not sync, it will not relay,
and it will not tell you anything is wrong beyond reporting zero connections.
This document describes every way a Quicksilver node reaches its first peer, and
what to do when none of them work.

## What Quicksilver ships

**No DNS seeds.** Quicksilver owns no domain. A DNS seed is a renewal liability
that has to outlive whoever registered it, and a name that lapses is worse than a
name that never existed — it can be re-registered by someone else and used to
feed a partitioned view of the chain to every new node. `vSeeds` is empty on all
three networks, and `p2p_dns_seeds.py` asserts it stays that way.

**One fixed onion seed**, on `main` and `publictest`. It is a Tor v3 hidden
service running on maintainer hardware. The address and its operator are recorded
in `contrib/seeds/nodes_main.txt` and `contrib/seeds/nodes_publictest.txt`; both
networks share one hidden service on their own P2P port, so there is one key to
keep rather than two.

**Nothing on `sandbox`.** A sandbox network is local by construction; there is
nothing to discover. `feature_config_args.py` enables `-fixedseeds` on sandbox
alone, and depends on that.

This is deliberately the smallest possible commitment: no domain, no static IP,
no VPS, and no paid, renumbering obligation. It is not obligation-free: the seed
onion runs on maintainer hardware, and its identity is a key that has to be kept.
What onion-only avoids is a bill and an address that changes when it is not paid.

## What that requires of you

The onion seed is reachable **only over Tor**. A node with no Tor available
cannot use it, and on a default configuration that leaves the node with no
automatic path to a peer at all.

**On the desktop, provide Tor and ask for a node.** The desktop starts and
supervises its own Tor process, but needs a `tor` executable beside the
Quicksilver executable, on `PATH`, or at `-bundledtorpath=<path>`. Install Tor
through your package manager on Unix; on Windows, obtain it separately as
described in [tor.md](tor.md#0-the-desktop-starts-its-own-tor). The desktop's
Tor needs no manual torrc or proxy configuration. That section also covers
`-bundledtor=0` for anyone who would rather run their own.

What it does *not* do is start a node unasked. A desktop first run is
vault-only: no node, no Tor and no peer until **Consensus** is enabled from
**Home → Review cost → Enable consensus**. Until then the Node window reports
`N/A` in every field and nothing on screen explains why, so the whole of this
document reads as broken when in fact nothing has been started. See
[Getting Started](getting-started.md#a-first-run-is-vault-only-it-runs-no-node-until-you-enable-consensus).
Everything below assumes a node that is actually running.

**On `quicksilver-daemon`, run a Tor daemon locally and let the node use it.** The
daemon defaults to `-bundledtor=0`: someone running a daemon can install and
configure Tor, and a headless node should not be starting processes its operator
did not ask for.

```bash
quicksilver-daemon -proxy=127.0.0.1:9050
```

If you also want to accept inbound connections over Tor, `-listenonion` is on by
default and will create a hidden service through Tor's control port.

## The fallback: -addnode

If you know a peer's address, name it directly. This bypasses seeds entirely.

The port belongs to the network, and naming the port does not select it. A
node started without `-publictest` is a `main` node whatever port you point it
at, so every non-`main` example carries the network option:

```bash
quicksilver-daemon -addnode=<host>:9555                                # main
quicksilver-daemon -publictest -addnode=<host>:19557                   # publictest
quicksilver-daemon -addnode=<onion>.onion:9555 -proxy=127.0.0.1:9050   # main, over Tor
```

Or in that network's `quicksilver.conf`, where the network section selects it:

```
addnode=<host>:9555

[publictest]
addnode=<host>:19557
```

On the desktop, the **Network** page has a *Connect to a peer* box that does the
same thing: type the address, press *Add peer*. It fills the same list `-addnode`
fills, so the peer is retried the same way and is protected from the same
disconnection rules. Nothing there reports success — a peer added this way is
queued for dialling, and the only evidence it worked is the peer count leaving
zero.

`-addnode` peers are retried and are not subject to the connection limits that
apply to peers discovered from the address manager. Once connected, the node
learns further addresses through normal `addr` gossip and no longer depends on
the seed.

That learning is not immediate. On a small network a node can sit at one
connection — the seed — for a good while after it has fully synced, because
`addr` gossip is deliberately slow and there are few addresses to relay. A
connection count of 1 on a synced node is normal here; it is a count of 0 that
means something is wrong.

## Mining while bootstrapping

The first node on a new chain has no peers, and `getblocktemplate` refuses to
build a template on the live network while the peer count is zero or the node is
still syncing. That gate is deliberate: it is what stops a node that has not yet
caught up from mining onto a stale fork. It is not relaxed for bootstrapping.

The bootstrap mining path in v1 is **`startmining`**:

    quicksilver-cli startmining <payout address>
    quicksilver-cli getminingstatus
    quicksilver-cli stopmining

`startmining` builds templates in-process and is not gated on peer count. That is
deliberate, and it is also a trap for anyone who did not mean to use it that way:
a node with zero peers cannot see the network's chain, so it mines onto a chain
of its own, and the coins it pays are discarded the moment it connects to a chain
with more work. The desktop asks before arming a miner with no peers, and says so
on the page for as long as the count stays at zero; `startmining` itself does not
ask, because bootstrapping a new network is exactly what it is for.

It still defers while the node knows of a more-work header chain it has not
connected — that check is on chain frontier, not on wall-clock tip age, so a
genesis timestamped in the past does not park it.

`getblocktemplate` is for external mining software on an already-populated
network. If you are running one node on a chain that has just started, you want
`startmining`.

## When a node finds nothing

State plainly what this looks like, because it is quiet:

```bash
quicksilver-cli getconnectioncount     # 0
quicksilver-cli getpeerinfo            # []
```

On the desktop, the same two answers are in the **Node window** — press
`Ctrl+Shift+D`, or pick any of Information / Console / Network Traffic / Peers
from the **Panels** menu. It is not a fallback for people without a terminal: the
Information tab shows the connection count directly, the Peers tab lists exactly
what `getpeerinfo` returns, and the Console tab runs every RPC named in this
document. `Ctrl+Shift+C` opens the Node window with the Console focused, but
only when the window is not already open; if it is, it is raised on whichever
tab it was left on.

The Console dispatches in-process, so it needs neither `-server` nor RPC
credentials — a desktop running with the JSON-RPC server switched off (the GUI
default) can still be interrogated this way. The window is unavailable until the
node has finished starting.

On the desktop the **Network** page states the diagnosis directly whenever the
peer count is zero: whether the network ships no seeds at all, or ships only
onion seeds with no Tor proxy configured to reach them, or is simply still
trying. The Mine / Mint page carries the same warning, because a node with no
peers that mines is building a chain of its own.

A node in this state logs no error. It is not broken, it is not banned, and it is
not syncing — it simply has nowhere to go. Check, in order:

1. `getnetworkinfo` → `networks[].reachable` for `onion`. If false, the node has
   no Tor proxy and the fixed seed is unusable.
2. Whether a Tor daemon is actually running and reachable at the proxy address.
   On a desktop that starts its own, that means reading `<datadir>/tor/tor.log`
   and the node's own `tor:` log lines rather than looking for a system service.
3. Whether outbound connections are disabled (`-connect=0`, `-maxconnections=0`,
   `-onlynet` excluding both `onion` and the seed's network).

If all three are fine and the count is still zero, the seed itself is down. Use
`-addnode` with any peer you can obtain out of band.

### Log lines that look like failures and are not

A first run writes several lines that read as errors while everything is
working. None of these indicates a bootstrapping problem:

| Line | Why it is there |
|---|---|
| `loaded source=dnsseed addresses=0` | Quicksilver ships no DNS seeds, as above. The thread runs and finds nothing because there is nothing to find. |
| `banlist=recreated reason=read-failed` | No ban list exists yet on a first run, so one is created. |
| `relaypool` … `loaded reason=file-open-failed` | Same: the file is written the first time there is something to write. |
| `QFSFileEngine::open: No file name specified` (desktop, a few per start) | Qt chatter during GUI startup. It does not affect the node. |

What a genuine failure to reach the network looks like is the section above: a
connection count that stays at 0, with `networks[].reachable` false for `onion`
or no Tor answering the proxy address.

If you *operate* the seed and it is reachable but never accumulates inbound
peers, check its own binds before anything else:

```bash
ss -ltn | grep 9556       # must show quicksilver-daemon listening
```

Nothing on 9556 means the onion bind was never created — see "Operating the
seed" for the `-listenonion=0` / `-bind=…=onion` pairing.

## Default ports

| Network | P2P | RPC | Onion service target |
|---|---|---|---|
| `main` | 9555 | 9554 | 9556 |
| `publictest` | 19557 | 19558 | 19559 |
| `sandbox` | 19556 | 19557 | 19555 |

The onion service target is the loopback port Tor forwards inbound hidden-service
connections to. It is named explicitly per network in `kernel/chainparams.cpp`
rather than derived as P2P+1: on `publictest` and `sandbox` that derivation landed
on the node's own RPC port, and the node could not start.

**19557 appears twice in that table, and this is not a typo.** It is
`publictest`'s P2P port and `sandbox`'s RPC port. The two networks are never
reached by the same client on the same interface, so the collision is harmless
in ordinary use — but a `publictest` node listening on `0.0.0.0:19557` and a
`sandbox` node serving RPC on `127.0.0.1:19557` cannot both run on one machine.
If you need both at once, move one with `-port=` or `-rpcport=`.

## What a running node advertises about itself

Reaching the seed over Tor does not make a node Tor-only. Once it is running,
with `-listen` at its default of 1:

- it binds the network's P2P port on all interfaces, IPv4 and IPv6, and accepts
  inbound clearnet connections there;
- unless something has switched discovery off — see the next paragraph, because
  one common option does — it registers the machine's own routable addresses,
  global IPv6 addresses included, as local addresses to advertise to peers
  alongside its onion address.

**The two programs differ here, and not in the direction you would guess.**
`-proxy=` switches address discovery off by itself — a daemon started the way
this document recommends logs `overrode option=-discover value=0
reason=proxy-set` and advertises nothing but its onion address. The desktop's
bundled Tor is wired in through the control port instead of `-proxy`, so that
interaction never fires: **the desktop discovers and advertises its own
addresses even though its only outbound path is Tor.** That is the case worth
knowing about before enabling Consensus on a machine whose address you would
rather not publish.

`getnetworkinfo` → `localaddresses` lists exactly what a node is advertising,
and the debug log records the same decisions (grep for `AddLocal`).

| To | Use | Note |
|---|---|---|
| Stop advertising your own addresses | `-discover=0` | Leaves the node listening and reachable; it simply stops telling peers where it is. |
| Reach peers only over Tor | `-onlynet=onion` | Outbound only; it does not affect what you advertise or accept. |
| Accept no inbound connections at all | `-listen=0` | See the warning below. |

⚠ On the desktop, `-listen=0` also switches off `-listenonion`, and **the
bundled Tor does not start without it** — the node is then left with no way to
reach the onion seed at all. A desktop that wants `-listen=0` has to bring its
own Tor: `-bundledtor=0` together with `-onion=` or `-proxy=`. On
`quicksilver-daemon`, which is already running its own Tor, `-listen=0` is
unremarkable. `doc/tor.md` §4 covers the rest of the Tor-only configuration.

## Operating the seed

For whoever runs it. The hidden service is defined statically in `torrc`, not
through the node's control port, so the address survives a node reinstall:

```
HiddenServiceDir /var/lib/tor/quicksilver/
HiddenServicePort 9555 127.0.0.1:9556
HiddenServicePort 19557 127.0.0.1:19559
```

The virtual port on the left is the port peers dial, so it must match what
`contrib/seeds/nodes_*.txt` advertises — the P2P port. The target on the right is
the **onion service target** from the table above, not the P2P port again: that
is the loopback address the node binds with the `=onion` tag. Pointing the target
at 9555 also works, but inbound Tor peers then arrive on the general P2P listener
and are never tagged as onion connections, which distorts both peer accounting
and the node's own address advertisement. `doc/tor.md` §3 shows the same mapping
for a non-seed node.

Because the service is static, run the node with **`-listenonion=0`**. Otherwise
it also creates an ephemeral onion of its own through the control port, and the
node advertises that throwaway address rather than the seed address every shipped
binary is compiled to dial.

**`-listenonion=0` also removes the default onion bind, so the seed must ask for
it explicitly.** These two flags are a matched pair and neither works alone:

```bash
quicksilver-daemon -listenonion=0 -bind=0.0.0.0:9555 -bind=127.0.0.1:9556=onion
```

`init.cpp` creates the default `127.0.0.1:9556=onion` bind only when
`-listenonion` is on — deliberately, so that a node which wants no onion service
binds no onion target (`feature_port.py` asserts exactly this). A seed with a
static `torrc` is the one configuration that wants the target *without* the
control-port service, and it is the one configuration the default does not cover.
Run `-listenonion=0` on its own and Tor forwards every inbound peer to a closed
port: the seed is advertised in `chainparamsseeds.h`, accepts nothing, and logs
nothing. `getconnectioncount` simply reads 0 — indistinguishable from the seed
being down, which is the failure this section exists to prevent.

The second `-bind` is needed because naming any `-bind` disables the implicit
bind-on-any: without `-bind=0.0.0.0:9555` the node would listen *only* on the
loopback onion target and drop off clearnet entirely. That is a legitimate choice
for a Tor-only seed — the address advertised to peers is an onion either way — but
it should be a decision, not a side effect. Dropping the clearnet bind also
suppresses `Discover()`, which otherwise advertises the host's own public IP to
every peer; on maintainer hardware that is worth thinking about, given this
design deliberately commits to no domain and no static IP.

Verify with a real client rather than by inspection — from a *different* machine
running its own Tor daemon:

```bash
nc -X 5 -x 127.0.0.1:9050 <onion>.onion 9555 < /dev/null
quicksilver-cli getpeerinfo    # on the seed, while that connection is open
```

The peer must show `"addrbind": "127.0.0.1:9556"` and `"network": "onion"`. A
`nc` listener standing in for the node on 9556 proves only that Tor's mapping is
right; it cannot detect the missing bind, because it *is* the missing bind.

The address lives in `/var/lib/tor/quicksilver/hostname`. **The private key in
that directory is the seed's identity** — losing it means every shipped binary
points at an address that no longer answers, and the fix requires a new release.
Back it up.

To publish a new or replacement seed, edit `contrib/seeds/nodes_main.txt` and
`contrib/seeds/nodes_publictest.txt`, then regenerate the header:

```bash
python3 contrib/seeds/generate-seeds.py contrib/seeds > src/chainparamsseeds.h
```

Never leave either file empty. `generate-seeds.py` emits `static const uint8_t
chainparams_seed_main[] = {}` for an empty file, and a zero-length array is a
GCC/Clang extension that MSVC rejects (C2466) — it breaks the Windows build.
