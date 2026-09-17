# Vendored Cuckatoo Cycle sources

This directory holds third-party code. **It is not covered by the project's
MIT licence in `COPYING`.** Two upstreams are represented.

## John Tromp — Cuckoo Cycle (FAIR MINING License)

Source: <https://github.com/tromp/cuckoo>, licence text reproduced verbatim in
`LICENSE.txt` alongside this file.

| file | note |
| --- | --- |
| `barrier.hpp` | thread barrier used by the CPU lean solver |
| `bitmap.hpp` | edge bitmap |
| `compress.hpp` | cycle-node compressor |
| `cuckatoo.h` | Cuckatoo graph/proof definitions and `verify()` |
| `graph.hpp` | cycle-finding graph |
| `lean.cpp`, `lean.hpp` | CPU lean solver |
| `lean.cu` | CUDA lean solver |
| `siphash.cuh`, `siphash.hpp`, `siphashxN.h` | SipHash-2-4 keyed edge generator |

`LICENSE.txt` offers two options. **Quicksilver elects the FAIR MINING
License**, not the alternative GPL-2-or-later grant, so that this directory
stays distributable alongside the MIT-licensed remainder of the tree.

**Quicksilver's own modifications to these files are offered under the same two
options.** The election above describes which arm Quicksilver distributes under;
it is not a narrowing of what a recipient may choose. Tromp's dual offer reaches
every recipient directly — `LICENSE.txt` here is verbatim — and mirroring it for
the local changes keeps this copy exactly as free as the one it came from.

The FAIR MINING condition binds a derived miner that *charges a developer fee*
for mining a fair coin: it must offer to share half that fee revenue with the
coin's developers. Quicksilver's solver charges no developer fee, and
Quicksilver itself is a fair coin — no premine, no developer allocation — so
the condition imposes no obligation here. It is reproduced regardless, as the
licence requires.

## Samuel Neves — BLAKE2 reference implementation

`blake2.h`, `blake2-impl.h` and `blake2b-ref.c` are the BLAKE2 reference
sources, bundled by upstream Cuckoo Cycle. They are tri-licensed CC0 / OpenSSL
/ Apache-2.0 at the recipient's option, and each carries its own notice in the
file header. Quicksilver elects **Apache-2.0**.

## Upstream revision

The CPU-side files in this directory were imported from
<https://github.com/tromp/cuckoo> commit
`a69ad1d6beea9b063b89f8ce8b3e3c7af4c90e88` (2026-04-18, "more precise siphash
bounties"). The original vendoring commit `c6ec788f` carries the upstream
`cuckoo-master.zip`; that zip's comment is the commit id above. After
include-path flattening, those 11 CPU-side files match that revision.

The later GPU import (`853e9e2e`) brought `siphash.cuh`, which also matches
`a69ad1d6`. `lean.cu` does not, and is not pinned to an upstream commit.

## Local modifications

These files are *not* pristine. Portability and correctness changes were made
in-tree and are recorded in the git history:

- `757dd6c4` — MSVC + CUDA 12.9 portability: `pthread` to `std::thread`,
  `__builtin_ffsll` to `std::countr_zero`, `__builtin_clz` to
  `std::countl_zero`, VLA-`sizeof` removal, CUDA/MSVC-safe `ffsll`/`popcountll`/
  `htole32` helpers, POSIX-only `main()` guarded behind `CUCKATOO_NO_MAIN`, and
  removal of the redundant `portable_endian.h` in favour of `compat/endian.h`.
- `b689ae2f` — fix the CPU lean solver writing `Solution` structs past the end
  of its caller's stack buffer.
- `4b1f28c5` — free the `compressor` objects both `graph` constructors allocate;
  the vendored destructor freed neither, leaking 112 bytes per solver context.
- `36877a85` — trim every edge-bitmap word in `count_node_deg` and
  `kill_leaf_edges` when `nthreads` does not divide `NEDGES/64`. The truncated
  `nloops = NEDGES/64/nthreads` walk left the remainder unvisited, so a
  non-power-of-two thread count could return a 42-cycle `verify()` rejects.

Include paths were flattened to be path-local when the closure was vendored
(`c6ec788f`). Do not re-stamp these files with a Quicksilver copyright line;
`contrib/devtools/copyright_header.py` excludes this directory for that reason.
