# macOS

**macOS is not a supported platform.** There is no build recipe on this page
because there is no build that has been produced, run, or gated on macOS.
Apple Silicon cannot compile, and that does not expire with a version
number. Intel Macs keep the repository; nobody has built one, and we do
not have the hardware to gate it. This page records the technical reasons,
so that anyone who is about to spend an afternoon on it knows in advance
what stops where.

Use [Unix and Linux](build-unix.md) or
[Windows with Visual Studio](build-windows-msvc.md). Both are mineable and both
are gated on real hardware before every release.

## Where it stops

| | Apple Silicon (arm64) | Intel Mac (x86-64) |
|---|---|---|
| Compiles | **No** — the solver does not compile | Not known; never built |
| GPU mining | **No** — CUDA is unavailable | **No** — CUDA is unavailable |
| CPU mining | **No** — the solver is x86-only | Not known; never run |
| GUI | Never launched on macOS | Never launched on macOS |

### GPU mining: there is no CUDA on macOS

The GPU solver is CUDA. `src/crypto/cuckatoo/gpu/qsgpusolve.cu` is compiled by
`nvcc` and talks to an NVIDIA device through the CUDA runtime; see
[gpu-solver.md](gpu-solver.md), which documents Linux and Windows only.

Apple dropped NVIDIA driver support after macOS 10.13, and Apple Silicon has no
NVIDIA hardware at all. Neither Mac generation can run the GPU solver, and that
is not something a build flag changes.

### CPU mining: the lean solver is x86 SIMD

The fallback CPU solver is Tromp's lean solver, vendored under
`src/crypto/cuckatoo/vendor/`. Its edge-generation inner loop is hand-written
SSE2/AVX2: `vendor/siphashxN.h` is built out of `_mm_*` and `_mm256_*`
intrinsics over `__m128i`/`__m256i`, and `vendor_prelude_solve.h` includes
`<immintrin.h>` unconditionally to supply them.

On arm64 that header is rejected outright — *"This header is only meant to be
used on x86 and x64 architecture"* — and the `__builtin_ia32_crc32*` builtins
behind it do not exist. The solver would have to be ported to NEON to run on
Apple Silicon.

### On Apple Silicon this stops the whole build, not just mining

`src/crypto/cuckatoo/CMakeLists.txt` compiles `solve_19.cpp` and `solve_28.cpp`
into `quicksilver_cuckatoo` unconditionally, and every binary that validates a
block links that library. So the x86 dependency above is not confined to the
mining path: on arm64 nothing builds, including the headless node.

Proof-of-work **verification** is a different translation unit and is portable —
`verify_19.cpp` and `verify_28.cpp` go through `vendor_prelude.h`, which pulls no
intrinsics. A non-mining arm64 node is therefore feasible in principle; it needs
the solver translation units excluded from the build and the mining paths wired
to say so. That is a contributor port. We do not ship an arm64 binary.

## Continuous integration

No macOS job runs on any push. The jobs that used to run on every push —
a GUI build and a fuzz build, both on `macos-14`, which is arm64 — could
not pass: the solver is x86 SIMD, every binary that validates a block
links it, and CUDA does not exist on the platform. They were removed.

While they ran they caught defects that were not macOS bugs at all: an
undeclared `libevent` dependency that breaks any prefixed dependency
layout, and a lambda capture that was wrong in every build with
tracing disabled. Both were fixed on Linux and Windows on the strength of a red
macOS log.

## If you build one anyway

There is no supported recipe, and nothing on this page should be read as one.
The dependency set is the same as [build-unix.md](build-unix.md) — CMake, Boost,
libevent, pkgconf, and SQLite and Qt 5 for the vault and the GUI — and on an
Intel Mac the compile may well succeed, because the x86 blocker above does not
apply there. Nobody has tried it, nothing is gated on it, and the result is
unsupported. Runtime file locations are in [files.md](files.md).
