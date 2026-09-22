UNIX BUILD NOTES
====================
Some notes on how to build Quicksilver in Unix.

(For BSD specific instructions, see `build-*bsd.md` in this directory.)

Getting the Source
---------------------

Install the dependencies for your distribution first (see [below](#linux-distribution-specific-instructions)),
then clone the tree and enter it. Every command in this document is run from the
top of that checkout:

```bash
git clone https://github.com/quicksilver-hg/quicksilver.git
cd quicksilver
```

To Build
---------------------

```bash
cmake -B build   # headless profile: command-line node and vault, not the GUI
```
This selects `RelWithDebInfo`, so the binaries carry debug information. A Linux
build with GCC 13.3 produced a 251.6 MiB unstripped `quicksilverd`, against
13.1 MiB for `cmake -B build -DCMAKE_BUILD_TYPE=Release` — a factor of 19.
Exact sizes vary by platform and compiler. Debug information can also be
dropped from the default build type; see
[Memory Requirements](#memory-requirements) below.

Run `cmake -B build -LH` to see the full list of available options.

```bash
cmake --build build -j "$(nproc)"   # parallel jobs; a serial build of this
                                    # tree took 53 minutes on 8 cores, a
                                    # parallel one 20 minutes on 4
cmake --install build               # Optional
```

This default build is a command-line node and vault. The optional features
below are **off unless you ask for them** — installing a dependency does not
enable the feature that uses it. In particular `-DBUILD_GUI=ON`,
`-DWITH_ZMQ=ON` and `-DWITH_USDT=ON` are all required to turn on the
corresponding parts, so if you install the Qt, ZeroMQ or USDT packages, pass the
matching option too or the build will silently ignore them.

See below for instructions on how to [install the dependencies on popular Linux
distributions](#linux-distribution-specific-instructions), or the
[dependencies](#dependencies) section for a complete overview.

## Memory Requirements

C++ compilers are memory-hungry. It is recommended to have at least 1.5 GB of
memory available when compiling Quicksilver. On systems with less, gcc can be
tuned to conserve memory with additional `CMAKE_CXX_FLAGS`:


    cmake -B build -DCMAKE_CXX_FLAGS="--param ggc-min-expand=1 --param ggc-min-heapsize=32768"

Alternatively, or in addition, debugging information can be skipped for compilation.
For the default build type `RelWithDebInfo`, the default compile flags are
`-O2 -g`, and can be changed with:

    cmake -B build -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g0"

Finally, clang (often less resource hungry) can be used instead of gcc, which is used by default:

    cmake -B build -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang

## Linux Distribution Specific Instructions

### Ubuntu & Debian

These instructions also apply to Ubuntu and Debian derivatives — Linux Mint,
Pop!_OS, elementary OS, Zorin and the rest — which share the same package
names. Match the Ubuntu or Debian release your distribution is built on rather
than its own version number.

#### Dependency Build Instructions

To install everything the GUI build recommended by [README.md](../README.md)
and [INSTALL.md](../INSTALL.md) needs, update the package index and then:

    sudo apt-get update
    sudo apt-get install build-essential cmake pkgconf python3 git \
        libevent-dev libboost-dev libsqlite3-dev qtbase5-dev libqrencode-dev tor

Add `qtwayland5` on a Wayland desktop. For a headless node, drop `qtbase5-dev`
and `libqrencode-dev`. Keep `tor`: without it, and without an explicit peer, a
headless node cannot reach the only seed (see
[Bootstrapping](bootstrapping.md)). The itemized lists below say what each
package is for and cover the optional features.

Two things the install command does which are worth knowing:

- It installs `git`, so it also works on a machine that does not have it yet —
  but you then need it *before* cloning, which is why the dependencies come
  first here.
- The `tor` package installs a system Tor service and starts it on port 9050,
  and it installs the `tor` binary the desktop execs. The service is what
  `quicksilverd` needs. The desktop does not use that service: it starts a Tor
  of its own, on ports Tor picks, so the two never collide. Keep `tor` on the
  line even if you only run the desktop; what the desktop does not use is the
  system service on 9050.

Build requirements:

    sudo apt-get install build-essential cmake pkgconf python3 git

Now, you can either build from self-compiled [depends](#dependencies) or install the required dependencies:

    sudo apt-get install libevent-dev libboost-dev

SQLite is required for the vault:

    sudo apt install libsqlite3-dev

`tor` is a **runtime** dependency of the desktop, not a build-time one: the
desktop starts and supervises it, and needs no Tor configuration — no torrc,
no proxy setting — once Consensus is enabled. It is the opt-in, not
configuration, that stands between a first run and the seed (see
[Getting Started](getting-started.md#a-first-run-is-vault-only-it-runs-no-node-until-you-enable-consensus)).
`quicksilverd` needs a Tor proxy to reach the onion seed unless it is given an
explicit peer, and needs Tor's control port additionally to host an onion
service (see [Bootstrapping](bootstrapping.md)).

    sudo apt-get install tor

To build Quicksilver without vault, see [*Disable-vault mode*](#disable-vault-mode)

ZMQ-enabled binaries are compiled with `-DWITH_ZMQ=ON` and require the following dependency:

    sudo apt-get install libzmq3-dev

USDT-enabled binaries are compiled with `-DWITH_USDT=ON` and require the following dependency:

    sudo apt install systemtap-sdt-dev

GUI dependencies:

Quicksilver includes a GUI built with the cross-platform Qt Framework. To compile the GUI, we need to install
the necessary parts of Qt, the libqrencode and pass `-DBUILD_GUI=ON`. Skip if you don't intend to use the GUI.

    sudo apt-get install qtbase5-dev

Additionally, to support Wayland protocol for modern desktop environments:

    sudo apt install qtwayland5

The GUI will be able to encode addresses in QR codes unless this feature is explicitly disabled. To install libqrencode, run:

    sudo apt-get install libqrencode-dev

Otherwise, if you don't need QR encoding support, use the `-DWITH_QRENCODE=OFF` option to disable this feature in order to compile the GUI.


### Fedora

#### Dependency Build Instructions

To install everything the GUI build recommended by [README.md](../README.md)
and [INSTALL.md](../INSTALL.md) needs, in one command:

    sudo dnf install gcc-c++ cmake make python3 git \
        libevent-devel boost-devel sqlite-devel qt5-qtbase-devel qrencode-devel tor

Add `qt5-qtwayland` on a Wayland desktop. For a headless node, drop
`qt5-qtbase-devel` and `qrencode-devel`. Keep `tor`: without it, and without an
explicit peer, a headless node cannot reach the only seed (see
[Bootstrapping](bootstrapping.md)). The itemized lists below say what each
package is for and cover the optional features.

Build requirements:

    sudo dnf install gcc-c++ cmake make python3 git

Now, you can either build from self-compiled [depends](#dependencies) or install the required dependencies:

    sudo dnf install libevent-devel boost-devel

SQLite is required for the vault:

    sudo dnf install sqlite-devel

`tor` is a **runtime** dependency of the desktop, not a build-time one: the
desktop starts and supervises it, and needs no Tor configuration — no torrc,
no proxy setting — once Consensus is enabled. It is the opt-in, not
configuration, that stands between a first run and the seed (see
[Getting Started](getting-started.md#a-first-run-is-vault-only-it-runs-no-node-until-you-enable-consensus)).
`quicksilverd` needs a Tor proxy to reach the onion seed unless it is given an
explicit peer, and needs Tor's control port additionally to host an onion
service (see [Bootstrapping](bootstrapping.md)).

    sudo dnf install tor

To build Quicksilver without vault, see [*Disable-vault mode*](#disable-vault-mode)

ZMQ-enabled binaries are compiled with `-DWITH_ZMQ=ON` and require the following dependency:

    sudo dnf install zeromq-devel

USDT-enabled binaries are compiled with `-DWITH_USDT=ON` and require the following dependency:

    sudo dnf install systemtap-sdt-devel

GUI dependencies:

Quicksilver includes a GUI built with the cross-platform Qt Framework. To compile the GUI, we need to install
the necessary parts of Qt, the libqrencode and pass `-DBUILD_GUI=ON`. Skip if you don't intend to use the GUI.

    sudo dnf install qt5-qtbase-devel

Additionally, to support Wayland protocol for modern desktop environments:

    sudo dnf install qt5-qtwayland

The GUI will be able to encode addresses in QR codes unless this feature is explicitly disabled. To install libqrencode, run:

    sudo dnf install qrencode-devel

Otherwise, if you don't need QR encoding support, use the `-DWITH_QRENCODE=OFF` option to disable this feature in order to compile the GUI.

## Dependencies

See [dependencies.md](dependencies.md) for a complete overview, and
[depends](../depends/README.md) on how to compile them yourself, if you wish to
not use the packages of your Linux distribution.

Disable-vault mode
--------------------
When the intention is to only run a P2P node, without a vault, Quicksilver can
be compiled in disable-vault mode with:

    cmake -B build -DENABLE_VAULT=OFF

In this case there is no dependency on SQLite.

Mining is also possible in disable-vault mode using the `getblocktemplate` RPC call.

Setup and Build Example: Arch Linux
-----------------------------------
This example lists the steps necessary to setup and build a command line only distribution of the latest changes on Arch Linux:

    pacman --sync --needed cmake boost gcc git libevent make python sqlite
    git clone https://github.com/quicksilver-hg/quicksilver.git
    cd quicksilver/
    cmake -B build
    cmake --build build -j "$(nproc)"
    ctest --test-dir build
    ./build/bin/quicksilverd
