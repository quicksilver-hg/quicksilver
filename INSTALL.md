# Building Quicksilver

Quicksilver 0.1.x is distributed as source. Choose the guide for your platform:

- [Unix and Linux](doc/build-unix.md)
- [macOS support status (unsupported)](doc/build-osx.md)
- [Windows with Visual Studio](doc/build-windows-msvc.md)
- [Windows cross-compilation](doc/build-windows.md)
- [FreeBSD](doc/build-freebsd.md)
- [OpenBSD](doc/build-openbsd.md)
- [NetBSD](doc/build-netbsd.md)

The complete dependency list is in [doc/dependencies.md](doc/dependencies.md).
The reproducible dependency build system is documented in
[depends/README.md](depends/README.md).

## Build profiles

The default CMake configuration builds the headless node, RPC client, agent
client, developer utilities, vault support, and tests. It does not build the Qt
GUI.

```bash
cmake -B build
cmake --build build -j "$(nproc)"
ctest --test-dir build
```

Enable the desktop application explicitly:

```bash
cmake -B build -DBUILD_GUI=ON
cmake --build build -j "$(nproc)"
ctest --test-dir build
```

The GUI needs Qt, which the default profile does not. Each platform guide gives
the package names in one line — on Debian and Ubuntu, `qtbase5-dev` and
`libqrencode-dev` are both in the single command under
[Ubuntu & Debian](doc/build-unix.md#ubuntu--debian). Installing them is not
enough on its own: `-DBUILD_GUI=ON` is what turns the GUI on, and without it
CMake silently builds the headless profile anyway.

Always pass `-j`. Without it CMake compiles one file at a time, and a cold
serial build of this tree took 53 minutes on an 8-core machine. With `-j` a
cold GUI build took 20 minutes on 4 cores, so expect tens of minutes rather
than minutes. `nproc` is Linux; use `$(sysctl -n hw.ncpu)` on the BSDs, or an
explicit count such as `-j 8` anywhere else.

`ctest --test-dir build` runs serially and takes about 9 minutes for the GUI
profile. The expected tests for your configuration are recorded in
`test/ctest-manifest.txt`. Run
`cmake -DBUILD_DIR=build -P cmake/script/CheckCtestManifest.cmake` after
configuring: it prints the expected total and fails on missing or unexpected
tests. Run `ctest --test-dir build --output-junit ctest-results.xml`, then
`cmake -DBUILD_DIR=build "-DRESULTS=build/ctest-results.xml" -P cmake/script/CheckCtestManifest.cmake`
to check the results. For multi-config builds, pass `-C Release` to `ctest`
and `-DCONFIG=Release` to the manifest check. In PowerShell, keep the quotes:
an unquoted `-D` value containing a `.` is split there.

If `CUCKATOO_GPU_SOLVER` is unset, `gpu_parity_tests` is skipped; set it to a
`qsgpusolve` binary to run the test. On MSVC, `compilerbug_tests` also skips
because its only case is GNU-specific. A skipped test is still counted in
`100%` — the results check rejects other skips. See [GPU Solver](doc/gpu-solver.md)
for what the parity test needs. Add `-j` to `ctest` as well if you want it to
finish sooner.

Useful focused configurations include:

```bash
# Headless node without vault support or SQLite
cmake -B build -DENABLE_VAULT=OFF

# Desktop application without QR encoding support
cmake -B build -DBUILD_GUI=ON -DWITH_QRENCODE=OFF
```

Run `cmake -B build -LH` to list the available project options. Build products
are placed under `build/bin/`.

## GPU helper

The optional CUDA helper is built separately from the CMake project. It is
strongly recommended for desktop transfers and is required by the current
agent client on `main` and `publictest`. See [doc/gpu-solver.md](doc/gpu-solver.md).

## After building

Continue with [Getting Started](doc/getting-started.md) for network selection,
Tor or explicit-peer bootstrapping, and basic health checks.
