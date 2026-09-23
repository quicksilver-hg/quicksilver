# Developer tools

**None of these is needed to use Quicksilver.** The application provisions
consensus itself, keeps your vault, and mines if you ask it to. If you
downloaded Quicksilver to hold or send it, you already have everything you
need and can stop reading here.

These are the command-line programs for development, scripting and headless
operation. They are packaged separately on purpose. A release install places
exactly two executables on your system — the application and the agent client —
because a first-time user opening a directory of eight similarly-named binaries
has to make a decision they have no way to make, and the usual outcome is that
they close the directory and do not come back.

## Getting them

**From source.** They are built by default; nothing to enable.

```
cmake -B build
cmake --build build -j"$(nproc)"
```

`QS_DEVELOPER_TOOLS` controls only whether `cmake --install` copies them onto
the system. It defaults `ON` for a source build, and release packaging sets it
`OFF`. The targets are always built and always tested either way, so a
contributor loses nothing and a test can never silently stop running because of
a packaging switch.

**Debian and derivatives.**

```
apt install quicksilver-devtools
```

**Windows.** Tick "Developer tools" in the installer, which is unchecked by
default. They install to `developer-tools\` beside the application.

## What each one is

| Program | What it is for |
|---|---|
| `quicksilverd` | The headless node. Run this on a server, or where you want consensus without a desktop. The application runs its own node, so you do not need both. |
| `quicksilver-cli` | Sends RPC calls to a running node. The tool you use to script against `quicksilverd`. |
| `quicksilver-tx` | Builds and modifies raw transactions offline. No node required. |
| `quicksilver-util` | Odd jobs that need no node — currently proof-of-work and hashing helpers. |
| `quicksilver-vault` | Offline vault file maintenance: inspect, salvage or dump a vault without starting a node. |

One more is built only when explicitly enabled, and is experimental:

| Program | What it is for |
|---|---|
| `quicksilver-chainstate` | Applies blocks to a chainstate using the kernel library directly. Requires `-DBUILD_UTIL_CHAINSTATE=ON`, and is not installed even then. |

## What is not here

`quicksilver-agent` ships with the application rather than with these tools. It
is the client an agent runs, and agents are who Quicksilver is for — it is not a
development aid.

`qsgpusolve`, the CUDA solver, is built outside CMake and is not placed on
`PATH`. Users who need transfer or mining acceleration configure its absolute
path in the application or with `-cuckatoosolver`; see
[GPU Solver](gpu-solver.md).
