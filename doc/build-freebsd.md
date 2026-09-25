# FreeBSD Build Guide

**Updated for FreeBSD [14.0](https://www.freebsd.org/releases/14.0R/announce/)**

This guide describes how to build quicksilver-daemon, command-line utilities, and GUI on FreeBSD.

## Preparation

### 1. Install Required Dependencies
Run the following as root to install the base dependencies for building.

```bash
pkg install boost-libs cmake git libevent pkgconf
```

See [dependencies.md](dependencies.md) for a complete overview.

### 2. Clone Quicksilver Repo
Now that `git` and all the required dependencies are installed, let's clone the Quicksilver repository to a directory. All build scripts and commands will run from this directory.
```bash
git clone https://github.com/quicksilver-hg/quicksilver.git
cd quicksilver
```

### 3. Install Optional Dependencies

#### Vault Dependencies
It is not necessary to build vault functionality to run either `quicksilver-daemon` or `quicksilver`.

###### Vault Support

`sqlite3` is required to support vaults.
```bash
pkg install sqlite3
```

#### GUI Dependencies
###### Qt5

Quicksilver includes a GUI built with the cross-platform Qt Framework. To compile the GUI, we need to install
the necessary parts of Qt, the libqrencode and pass `-DBUILD_GUI=ON`. Skip if you don't intend to use the GUI.

```bash
pkg install qt5-buildtools qt5-core qt5-gui qt5-testlib qt5-widgets
```

###### libqrencode

The GUI will be able to encode addresses in QR codes unless this feature is explicitly disabled. To install libqrencode, run:

```bash
pkg install libqrencode
```

Otherwise, if you don't need QR encoding support, use the `-DWITH_QRENCODE=OFF` option to disable this feature in order to compile the GUI.

---

#### Notifications
###### ZeroMQ

Quicksilver can provide notifications via ZeroMQ. This is off by default: build with
`-DWITH_ZMQ=ON` to enable it, and install the package below first.
```bash
pkg install libzmq4
```

#### Test Suite Dependencies
There is an included test suite that is useful for testing code changes when developing.
To run the test suite (recommended), you will need to have Python 3 installed:

```bash
pkg install python3 databases/py-sqlite3 net/py-pyzmq
```
---

## Building Quicksilver

### 1. Configuration

There are many ways to configure Quicksilver, here are a few common examples:

##### Vault and GUI:
This enables the GUI, assuming `sqlite` and `qt` are installed.
```bash
cmake -B build -DBUILD_GUI=ON
```

Run `cmake -B build -LH` to see the full list of available options.

##### No Vault or GUI
```bash
cmake -B build -DENABLE_VAULT=OFF
```

### 2. Compile

```bash
cmake --build build     # Append "-j N" for N parallel jobs.
ctest --test-dir build  # Append "-j N" for N parallel tests. Some tests are disabled if Python 3 is not available.
```
