# Windows / MSVC Build Guide

This guide describes how to build quicksilver-daemon, command-line utilities, and GUI on Windows using Microsoft Visual Studio.

For cross-compiling options, please see [`build-windows.md`](./build-windows.md).

## Preparation

### 1. Visual Studio

This guide relies on using CMake and vcpkg package manager provided with the Visual Studio installation.
Download Visual Studio 2022 from Microsoft's [Visual Studio 2022 download page](https://learn.microsoft.com/en-us/visualstudio/releases/2022/release-history#evergreen-bootstrappers).
Any edition works; Community is free.
Here are requirements for the Visual Studio installation:
1. Minimum required version: Visual Studio 2022 version 17.6. The presets use the "Visual Studio 17 2022" generator, so VS 2022 is required; a newer Visual Studio major version is not a substitute.
2. Installed components:
- The "Desktop development with C++" workload (`Microsoft.VisualStudio.Workload.NativeDesktop`).
- MSVC x64/x86 build tools (`Microsoft.VisualStudio.Component.VC.Tools.x86.x64`).
- C++ CMake tools for Windows (`Microsoft.VisualStudio.Component.VC.CMake.Project`).
- vcpkg package manager (`Microsoft.VisualStudio.Component.Vcpkg`).
- A Windows SDK (Windows 10 or Windows 11).

The installer GUI selects these components with the workload. A scripted `--add` of the workload omits recommended components unless `--includeRecommended` is also passed. For an existing Community installation, the unattended form in an elevated PowerShell is (adjust the quoted installation path for another edition):

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\Installer\setup.exe" modify --installPath "C:\Program Files\Microsoft Visual Studio\2022\Community" --add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended --quiet --norestart
```

The commands in this guide should be executed in "Developer PowerShell for VS 2022" or "Developer Command Prompt for VS 2022".
The former is assumed hereinafter. It sets `VCPKG_ROOT`, which both presets require to locate the vcpkg toolchain; configuring from a plain PowerShell without this variable fails.

### 2. Git

Download and install [Git for Windows](https://git-scm.com/download/win). Once installed, Git is available from PowerShell or the Command Prompt.

### 3. Clone Quicksilver Repository

Clone the Quicksilver repository to a short path without spaces, such as `C:\src\quicksilver`. All build scripts and commands will run from this directory. Even a short path can trigger vcpkg long-path warnings; see §7 for workarounds.
```
git clone https://github.com/quicksilver-hg/quicksilver.git C:\src\quicksilver
cd C:\src\quicksilver
```

Python is optional. Installing Python 3.10 or newer from [python.org](https://www.python.org/downloads/windows/) and making it available on `PATH` enables `util_test_runner` and `util_rpcauth_test`. The Microsoft Store `python` stub does not count as an installed interpreter. After configuring, `cmake -DBUILD_DIR=build -DCONFIG=Release -P cmake/script/CheckCtestManifest.cmake` checks `test/ctest-manifest.txt` and prints exactly how many tests your configuration registers, including without Python.


### 4. Tor (desktop only)

The Quicksilver desktop starts and supervises its own Tor, so a first run
reaches the onion-only seed with nothing to configure (see
[doc/tor.md](tor.md)). Unix delegates this to the platform package manager;
Windows has no package manager to delegate to, so fetch the Tor Project's
Expert Bundle:

```
powershell -NoProfile -ExecutionPolicy Bypass -File contrib\tor\fetch-tor.ps1
```

Windows PowerShell 5.1 — the one that ships with Windows — is enough; PowerShell
7 (`pwsh`) is not required and is not present on a stock machine.
`-ExecutionPolicy Bypass` applies only to this PowerShell process; it does not
change the policy for the user or machine. Run the script as a **file**, as
above: a script whose text is piped in has no `$PSScriptRoot`, so the `-OutDir`
default cannot resolve and it fails before it does anything, and the error names
`Join-Path` rather than the transport. Pass `-OutDir <path>` if you want it
somewhere other than `build\tor`.

The script verifies a pinned SHA-256 **before** extracting, and deletes the
download on a mismatch. It extracts into `build\tor\` and prints where
`tor.exe` landed.

`tor.exe` in this bundle is self-contained — the archive ships no DLLs beside
it — so copy that one file next to `quicksilver.exe`, or run the desktop
with `-bundledtorpath=<path to tor.exe>`. The installer (`--target deploy`)
needs it too: it ships `tor.exe` beside `quicksilver.exe`, takes it from
`build\tor\tor\tor.exe` (set `QUICKSILVER_BUNDLED_TOR_EXE` to use another
path), and refuses to build if it is missing or does not match the pinned
SHA-256 in `cmake/module/Maintenance.cmake`. Run this step before configuring
the build that makes the installer. The bundle's pluggable transports and
geoip data are not used: Quicksilver configures no bridges and no country
selection.

`quicksilver-daemon` does not need this step. It defaults to `-bundledtor=0` and
expects a Tor you run and configure yourself.

## Triplets and Presets

The Quicksilver project supports the following vcpkg triplets:
- `x64-windows` (both CRT and library linkage is dynamic)
- `x64-windows-static` (both CRT and library linkage is static)

To facilitate build process, the Quicksilver project provides presets, which are used in this guide.

Available presets can be listed as follows:
```
cmake --list-presets
```

By default, all presets set `BUILD_GUI` to `ON`.

## Building

CMake will put the resulting object files, libraries, and executables into a dedicated build directory.

In the following instructions, the "Debug" configuration can be specified instead of the "Release" one.

### 5. Building with Static Linking with GUI

```
cmake -B build --preset vs2022-static          # It might take a while if the vcpkg binary cache is unpopulated or invalidated.
cmake --build build --config Release           # Append "-j N" for N parallel jobs.
ctest --test-dir build --build-config Release  # Append "-j N" for N parallel tests. Python enables the two additional tests described in §3.
cmake --install build --config Release         # Optional.
```

### 6. Building with Dynamic Linking without GUI

```
cmake -B build --preset vs2022 -DBUILD_GUI=OFF # It might take a while if the vcpkg binary cache is unpopulated or invalidated.
cmake --build build --config Release           # Append "-j N" for N parallel jobs.
ctest --test-dir build --build-config Release  # Append "-j N" for N parallel tests. Python enables the two additional tests described in §3.
```

### 7. vcpkg-specific Issues and Workarounds

vcpkg installation during the configuration step might fail for various reasons unrelated to Quicksilver.

If the failure is due to a "Buildtrees path … is too long" error, which is often encountered when building
with `BUILD_GUI=ON` and using the default vcpkg installation provided by Visual Studio, you can
specify a shorter path to store intermediate build files by using
the [`--x-buildtrees-root`](https://learn.microsoft.com/en-us/vcpkg/commands/common-options#buildtrees-root) option:

```powershell
cmake -B build --preset vs2022-static -DVCPKG_INSTALL_OPTIONS="--x-buildtrees-root=C:\vcpkg"
```

If vcpkg installation fails with the message "Paths with embedded space may be handled incorrectly", which
can occur if your local Quicksilver repository path contains spaces, you can override the vcpkg install directory
by setting the [`VCPKG_INSTALLED_DIR`](https://github.com/microsoft/vcpkg-docs/blob/main/vcpkg/users/buildsystems/cmake-integration.md#vcpkg_installed_dir) variable:

```powershell
cmake -B build --preset vs2022-static -DVCPKG_INSTALLED_DIR="C:\path_without_spaces"
```

## Performance Notes

### 8. vcpkg Manifest Default Features

One can skip vcpkg manifest default features to speedup the configuration step.
For example, the following invocation will skip all features except for "vault" and "tests" and their dependencies:
```
cmake -B build --preset vs2022 -DVCPKG_MANIFEST_NO_DEFAULT_FEATURES=ON -DVCPKG_MANIFEST_FEATURES="vault;tests" -DBUILD_GUI=OFF
```

Available features are listed in the [`vcpkg.json`](../vcpkg.json) file.

### 9. Antivirus Software

To improve the build process performance, one might add the Quicksilver repository directory to the Microsoft Defender Antivirus exclusions.
