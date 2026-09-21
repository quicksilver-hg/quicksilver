Quicksilver
=============

Setup
---------------------
Quicksilver is the reference client. It downloads and, by default, stores the
full Quicksilver chain history. Early public networks are expected to be much
smaller than mature chains, but disk, CPU, memory, and network requirements
will grow with usage.

Quicksilver is currently built from source. See the build notes in this [doc folder](.) to get started.

Start with:

- [Getting Started](getting-started.md) — choose a program and network, start a
  node, and check connectivity
- [Building Quicksilver](../INSTALL.md) — platform build guides and build profiles
- [GPU Solver](gpu-solver.md) — optional desktop acceleration and the agent
  client's live-network requirement
- [Bootstrapping](bootstrapping.md) — Tor, explicit peers, ports, and zero-peer
  diagnosis

Running
---------------------
The following are some helpful notes on how to run Quicksilver on your native platform.

### Unix

Build Quicksilver from source with the Unix build notes below, then run the
generated executable from the build directory:

- `build/bin/quicksilver-qt` (GUI, if built) or
- `build/bin/quicksilverd` (headless)

### Windows

Build Quicksilver from source with the Windows build notes below, then run
`quicksilver-qt.exe` or `quicksilverd.exe` from the generated build output.

### macOS (unsupported)

macOS is not a supported platform. See the macOS support status below for
the technical blockers: Apple Silicon cannot compile, and Intel Macs are
not gated.

### Need Help?

* See the documents in this directory for build, runtime, vault, networking,
  and RPC guidance.
* For suspected bugs, crashes, or documentation gaps, open an issue in this
  repository with the platform, build options, command line, and relevant logs.
* For security-sensitive reports, follow the private reporting guidance in
  [SECURITY.md](../SECURITY.md) instead of opening a public issue.

Building
---------------------
The following are developer notes on how to build Quicksilver on your native platform. They are not complete guides, but include notes on the necessary libraries, compile flags, etc.

- [Dependencies](dependencies.md)
- [macOS Support Status (unsupported)](build-osx.md)
- [Unix Build Notes](build-unix.md)
- [Windows Build Notes (Visual Studio)](build-windows-msvc.md)
- [Windows Build Notes (cross-compilation)](build-windows.md)
- [FreeBSD Build Notes](build-freebsd.md)
- [OpenBSD Build Notes](build-openbsd.md)
- [NetBSD Build Notes](build-netbsd.md)

Quicksilver Design
---------------------
- [Design Notes](design/README.md)
- [Transaction Relay Policy](policy/README.md)
- [Mining Integration](mining/README.md)
- [Audit Notes](audit/README.md)

Development
---------------------
The Quicksilver repo's [root README](../README.md) contains relevant information on the development process and automated testing.

- [Developer Notes](developer-notes.md)
- [Developer Tools](developer-tools.md)
- [Productivity Notes](productivity.md)
- [Source Code Documentation](README_doxygen.md)
- [JSON-RPC Interface](JSON-RPC-interface.md)
- [Output Descriptors](descriptors.md)
- [USDT Tracing](tracing.md)
- [Unauthenticated REST Interface](REST-interface.md)
- [BIPS](bips.md)
- [0.1.x Release Notes](release-notes.md)
- [Publishing the Source Tree](source-publication.md)
- [Benchmarking](benchmarking.md)
- [Design Docs](design/README.md)

### Resources
* Discuss project-specific development in this repository's GitHub issues and
  pull requests.
* See [CONTRIBUTING.md](../CONTRIBUTING.md) for contribution workflow, review
  expectations, and project communication guidance.

### Miscellaneous
- [quicksilver.conf Configuration File](quicksilver-conf.md)
- [CJDNS Support](cjdns.md)
- [Files](files.md)
- [Fuzz-testing](fuzzing.md)
- [I2P Support](i2p.md)
- [Init Scripts (systemd/upstart/openrc)](init.md)
- [Managing Vaults](managing-vaults.md)
- [GPU Solver](gpu-solver.md)
- [Multisig Tutorial](multisig-tutorial.md)
- [Offline Signing Tutorial](offline-signing-tutorial.md)
- [External Signer](external-signer.md)
- [P2P bad ports definition and list](p2p-bad-ports.md)
- [PSQT support](psqt.md)
- [Reduce Memory](reduce-memory.md)
- [Reduce Traffic](reduce-traffic.md)
- [Tor Support](tor.md)
- [ZMQ](zmq.md)

License
---------------------
Distributed under the [MIT software license](../COPYING).
