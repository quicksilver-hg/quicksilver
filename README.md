# Quicksilver

Quicksilver is an experimental UTXO network for exact-value payments. It has
no transaction fees: senders provide verifiable proof-of-work instead, and
recipients receive the full amount sent.

> [!WARNING]
> Quicksilver is a pre-1.0 development project. The current 0.1.x series is
> published for source review, testing, and early network operation. It does
> not provide a stable API, vault format, consensus contract, upgrade path, or
> supported binary-release channel.

## What is different

- **Feeless transactions.** Inputs and outputs balance exactly. Transaction
  proof-of-work provides admission and anti-spam cost.
- **Cuckatoo proof-of-work.** Blocks and transactions are secured by Cuckatoo,
  the graph-cycle proof-of-work from the Cuckoo Cycle family; this is not a
  SHA256d mining network. Quicksilver runs it at a 42-edge cycle and uses the
  same graph size for both the block and the transaction layer.
- **Vault and agent tooling.** The source tree includes a desktop vault, a
  headless node, developer command-line tools, and an experimental agent client.
- **Fresh network identity.** Quicksilver has its own genesis blocks, network
  magic, address formats, ports, and `main`, `publictest`, and `sandbox`
  environments.

The scope boundary—including what the agent client does not yet guarantee—is
documented in [What v1 Does and Does Not Do](doc/design/v1-scope.md).

## Before you run it

Sending a transfer performs proof-of-work before broadcast. The desktop vault
can fall back to the built-in CPU solver, but the measured reference time is
about 16 minutes on an 8-thread desktop. An external CUDA solver reduced the
same reference workload to about 50 seconds and is strongly recommended. The
current command-line agent requires that external solver on `main` and
`publictest`. Running a node, validating the chain, and receiving funds do not
require a GPU. See [GPU Solver](doc/gpu-solver.md).

Quicksilver has no DNS seeds. `main` and `publictest` each ship a fixed Tor
onion seed, so a new node needs Tor or an explicit peer configured with
`-addnode`, `-connect`, or `-seednode`. The desktop is the exception: it starts
and supervises a Tor of its own and needs none of that. See
[Bootstrapping](doc/bootstrapping.md).

The desktop's first run is **vault-only**. It runs no node, dials no peer and
starts no Tor until you enable **Consensus** from Home → Review cost, and
nothing on screen fails while it is off. Read
[A first run is vault-only](doc/getting-started.md#a-first-run-is-vault-only-it-runs-no-node-until-you-enable-consensus)
before concluding that the desktop cannot reach the network.

## Build from source

Platform dependencies and complete instructions are in [INSTALL.md](INSTALL.md).
**Install the dependencies for your platform first**, then:

```bash
git clone https://github.com/quicksilver-hg/quicksilver.git
cd quicksilver
```

Every command below is run from the top of that checkout. On a Unix-like system
with the required dependencies already installed, a headless development build
is:

```bash
cmake -B build
cmake --build build -j "$(nproc)"
ctest --test-dir build
```

Build in parallel. Without `-j`, CMake compiles one file at a time: a cold
serial build of this tree took 53 minutes on an 8-core machine. With `-j` it
took 20 minutes on 4 cores. `nproc` is Linux; use `$(sysctl -n hw.ncpu)` on
the BSDs, or an explicit count such as `-j 8` anywhere else.

The GUI is opt-in for ordinary CMake builds:

```bash
cmake -B build -DBUILD_GUI=ON
cmake --build build -j "$(nproc)"
./build/bin/quicksilver-qt
```

For the headless node:

```bash
./build/bin/quicksilverd -proxy=127.0.0.1:9050        # main
./build/bin/quicksilver-cli getblockchaininfo
```

Add `-publictest` to both commands for the public test network. The daemon does
not start a Tor of its own, which is why it is given a proxy here; the desktop
does, and is not.

The [Getting Started](doc/getting-started.md) guide continues from build output
to network selection, peer discovery, and first health checks.

## Documentation

- [Documentation index](doc/README.md)
- [Getting started](doc/getting-started.md)
- [Build guides](INSTALL.md)
- [Bootstrapping and ports](doc/bootstrapping.md)
- [Vault operation](doc/managing-vaults.md)
- [GPU solver](doc/gpu-solver.md)
- [Design and scope](doc/design/README.md)
- [Transaction relay policy](doc/policy/README.md)
- [Mining integration](doc/mining/README.md)
- [0.1.x release notes](doc/release-notes.md)
- [Source publication procedure](doc/source-publication.md)

## Contributing and security

Quicksilver welcomes focused review, testing, and patches. Read
[CONTRIBUTING.md](CONTRIBUTING.md) before opening a pull request.

Do not report vulnerabilities in a public issue. Follow the private reporting
process in [SECURITY.md](SECURITY.md).

## License and provenance

Quicksilver is distributed under the MIT software license. See
[COPYING](COPYING).

Quicksilver is a fork of [Bitcoin Core](https://github.com/bitcoin/bitcoin)
29.1. Much of this tree is Bitcoin Core's work, still under its copyright and
the same MIT terms.

Parts of the tree are third-party code under other licences — notably the
vendored Cuckoo Cycle solver, which is John Tromp's under the FAIR MINING
License, not MIT. COPYING lists them; `contrib/debian/copyright` is the
authoritative per-file record.
