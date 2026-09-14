# Reference Implementation Interfaces

## Cuckoo Cycle — Cuckatoo variant (snapshot a69ad1d)

Tromp's repo contains several graph variants (cuckoo, cuckaroo, cuckatoo, cuckaroom, cuckarooz).
The **cuckatoo** variant is the one used by Grin mainnet and has the clearest solver+verifier
split. All signatures below are from `src/cuckatoo/`.

---

### Header(s) the driver must include

```
third_party/cuckoo/src/cuckatoo/cuckatoo.h   ← defines word_t, EDGEBITS, PROOFSIZE,
                                                  siphash_keys, verify(), setheader(),
                                                  SolverParams, SolverSolutions,
                                                  SolverStats, Solution, verify_code enum
third_party/cuckoo/src/crypto/siphash.hpp    ← siphash_keys struct + setkeys/siphash24
                                                  (included transitively by cuckatoo.h)
third_party/cuckoo/src/crypto/blake2.h       ← blake2b() declaration
                                                  (included transitively by cuckatoo.h)
```

For the lean CPU solver entry points, also include (or compile):

```
third_party/cuckoo/src/cuckatoo/lean.hpp     ← cuckoo_ctx class ONLY (does NOT declare run_solver)
third_party/cuckoo/src/cuckatoo/lean.cpp     ← run_solver / create_solver_ctx / destroy_solver_ctx
                                                  definitions, AND `typedef cuckoo_ctx SolverCtx`.
                                                  These solver entry points exist ONLY here (no
                                                  header declares them) — compile lean.cpp into the
                                                  driver; do not #include a header expecting them.
```

---

### EDGEBITS / PROOFSIZE compile-time constants

Both are `#define`d in `cuckatoo.h` with defaults:

```c
#ifndef EDGEBITS
#define EDGEBITS 31        // default; override with -DEDGEBITS=N
#endif
#ifndef PROOFSIZE
#define PROOFSIZE 42       // always 42 in practice; can be overridden
#endif
```

Set at compile time via `-DEDGEBITS=29` (or 19, 31, 32, …).
**Practical small values for testing:** 19 (fast, ~64 KB memory).
**Production value used by Grin:** 29 or 31.

Derived constants used in type selection and sizing:

```c
// word_t is u32 for EDGEBITS 17–32, u64 for EDGEBITS > 32
typedef u32 word_t;   // when EDGEBITS in [17, 32]

typedef word_t proof[PROOFSIZE];  // from graph.hpp — an array of PROOFSIZE edge indices
```

---

### Verify entry (the key API the driver times)

Defined **inline in `cuckatoo.h`** (no separate .cpp needed — include the header):

```c
int verify(word_t edges[PROOFSIZE], siphash_keys *keys);
```

- `edges`: array of `PROOFSIZE` (42) edge indices, sorted ascending.
- `keys`: siphash keys derived from the block header via `setheader()`.
- Returns `POW_OK` (0) on success; non-zero `verify_code` on failure.
- All arithmetic is done in the `word_t` width set by `EDGEBITS`.

**Return codes** (enum `verify_code` in `cuckatoo.h`):

```c
enum verify_code {
    POW_OK,              // 0 — valid proof
    POW_HEADER_LENGTH,   // 1
    POW_TOO_BIG,         // 2 — an edge index exceeds NODEMASK
    POW_TOO_SMALL,       // 3 — edges not strictly ascending
    POW_NON_MATCHING,    // 4 — XOR of endpoints non-zero
    POW_BRANCH,          // 5 — branch found (not a simple cycle)
    POW_DEAD_END,        // 6
    POW_SHORT_CYCLE      // 7 — cycle length < PROOFSIZE
};
const char *errstr[];  // NOTE: source defines this NON-extern, NON-inline directly in cuckatoo.h
                       // (as it also does for setheader/verify/sipnode/print_log/timestamp).
                       // ODR HAZARD: include cuckatoo.h in EXACTLY ONE translation unit, or the
                       // link fails with multiple-definition errors. (Task 8 driver: keep the
                       // cuckatoo.h include confined to a single .cpp.)
```

**Header-to-keys helper** (also in `cuckatoo.h`):

```c
void setheader(const char *header, const u32 headerlen, siphash_keys *keys);
```

Internally: `blake2b(hdrkey, 32, header, headerlen, 0, 0)` then `keys->setkeys(hdrkey)`.

---

### Solve entry (CPU lean miner)

Defined in `lean.cpp` with C or C++ linkage controlled by `C_CALL_CONVENTION`:

```c
// create / configure context
SolverCtx* create_solver_ctx(SolverParams* params);

// run solver over nonces [nonce, nonce+range)
int run_solver(SolverCtx* ctx,
               char* header,
               int header_length,
               u32 nonce,
               u32 range,
               SolverSolutions *solutions,   // may be NULL
               SolverStats *stats            // may be NULL
               );

// tear down
void destroy_solver_ctx(SolverCtx* ctx);
void stop_solver(SolverCtx* ctx);          // signal abort from another thread
```

`SolverSolutions`, `SolverStats`, `SolverParams`, and `Solution` are all declared in `cuckatoo.h`.

Proof lives in:

```c
struct Solution {
    u64 id;
    u64 nonce;
    u64 proof[PROOFSIZE];   // PROOFSIZE=42 edge indices as u64
};

struct SolverSolutions {
    u32 edge_bits;
    u32 num_sols;
    Solution sols[MAX_SOLS];  // MAX_SOLS=4 by default
};
```

`run_solver` calls `verify()` internally and prints the result; it also fills `SolverSolutions`
if non-NULL.

---

### Proof representation + size in bytes

At the default `EDGEBITS=29` (production-like):

- `word_t` = `u32` (32 bits)
- Proof = `word_t[42]` = 42 × 4 bytes = **168 bytes**
- In `SolverSolutions.sols[i].proof`: stored as `u64[42]` = 42 × 8 bytes = **336 bytes** (u64 for uniformity across parameter sizes)

At `EDGEBITS=19` (test/benchmark):

- `word_t` = `u32`
- Proof = `word_t[42]` = **168 bytes** (same, indices are smaller but type is still u32)

---

### Build notes: CPU solver + verifier (no GPU required)

All source and bundled dependencies are self-contained in the repo.
**Bundled crypto deps:** `blake2b-ref.c` (blake2b) and `siphash.hpp` (SipHash-2-4).
No external libraries required.

#### Build lean CPU solver (EDGEBITS=19, single-threaded siphash — fastest to compile/test):

```bash
cd third_party/cuckoo/src/cuckatoo
g++ -march=native -std=c++11 \
    -Wall -Wno-format -Wno-deprecated-declarations \
    -D_POSIX_C_SOURCE=200112L -O3 -DPREFETCH -I. \
    -pthread \
    -o lean19x1 \
    -DNSIPHASH=1 -DATOMIC -DEDGEBITS=19 \
    lean.cpp ../crypto/blake2b-ref.c
```

Or equivalently via the repo Makefile:

```bash
cd third_party/cuckoo/src/cuckatoo
make lean19x1
```

#### Build standalone command-line verifier (EDGEBITS=19):

```bash
cd third_party/cuckoo/src/cuckatoo
g++ -march=native -std=c++11 \
    -Wall -Wno-format -Wno-deprecated-declarations \
    -D_POSIX_C_SOURCE=200112L -O3 -DPREFETCH -I. \
    -pthread \
    -o verify19 \
    -DPROOFSIZE=42 -DEDGEBITS=19 \
    cuckatoo.c ../crypto/blake2b-ref.c
```

Or: `make verify19`

#### To build the driver's verifier as a compiled-in object (not the CLI tool):

The driver only needs to `#include "cuckatoo.h"` — `verify()` and `setheader()` are defined
inline in that header. Compile with `-DEDGEBITS=N` and link `../crypto/blake2b-ref.c` (for
blake2b, called by `setheader()`). No additional .cpp files are needed for verify-only use.

#### Production EDGEBITS=29 (mean miner, 4-thread SIMD — bigger/slower):

```bash
cd third_party/cuckoo/src/cuckatoo
make mean29x4
```

Command: `g++ -march=native -std=c++11 ... -mno-avx2 -DNSIPHASH=4 -DEDGEBITS=29 mean.cpp ../crypto/blake2b-ref.c`

#### Verified working (proof of build + run):

```
$ ./lean19x1 -n 74
Looking for 42-cycle on cuckatoo19("000...000",74) with trimming to 11 bits, 96 trimming rounds, 1 threads
Using 64KB edge and 64KB node memory, and 1-way siphash
nonce 74 k0 k1 k2 k3 d23109bd4dac0bdf 76fbe03c31ad8133 d17f301a7a865e22 baf57c804957a342
95 trims completed  644 edges left
  2-cycle found
  42-cycle found
  24-cycle found
Time: 84 ms
Solution 19ce c1c9 e95f fc42 1034f 1192c 129a4 12a44 12a58 178c4 1b393 1eeb6 1f047 23272 2449b 25635 263ca 2b2ce 2db7e 2f224 3054d 3468d 374c4 3c4e4 451e1 47cbb 4c7d3 53b59 555af 560c4 5a67c 62c04 65f3d 695e0 6fa68 77639 7a06b 7b2f2 7cba1 7d03e 7d5b2 7f26b
Verified with cyclehash 73b0e7ad8f0b4d0e2515a6d4a2eeaa9241493d44a6dd23015326f652e27fb130
1 total solutions

$ ./lean19x1 -n 74 | grep ^Sol | ./verify19 -n 74
Verified with cyclehash 73b0e7ad8f0b4d0e2515a6d4a2eeaa9241493d44a6dd23015326f652e27fb130
```

---

### Driver integration notes for Task 8

1. `verify()` is header-only — include `cuckatoo.h` with the target `-DEDGEBITS` and compile in `blake2b-ref.c`.
2. The proof array in `verify()` is `word_t[PROOFSIZE]` (not `u64`). Cast from `SolverSolutions.sols[i].proof` (`u64[]`) if using the solver structs.
3. `NSIPHASH` must be set to 1 (scalar), 4 (AVX2 4-lane), or 8 (AVX2 8-lane) at compile time. `lean19x1` uses `NSIPHASH=1` (no SIMD needed).
4. Warnings are benign (signed/unsigned comparisons in upstream code).
5. No GPU runtime libraries are referenced in the CPU build path.

---

## Equihash (snapshot fab686e)

Tromp's Equihash solver — Wagner's algorithm for the Generalized Birthday Paradox.
Classic Zcash parameters are n=200, k=9 (default in this repo).

---

### Header(s) the driver must include

```
third_party/equihash/equi.h        ← defines WN, WK, PROOFSIZE, HEADERNONCELEN,
                                      typedef u32 proof[PROOFSIZE],
                                      setheader(), genhash(), verifyrec(), verify(),
                                      verify_code enum, errstr[]
                                      (also pulls in blake/blake2.h transitively)

third_party/equihash/equi_miner.h  ← defines struct equi (the solver), struct thread_ctx,
                                      void *worker(void *vp), MAXSOLS=8
                                      (include ONLY if using the solver, not for verify-only)
```

For verify-only use, include only `equi.h` and compile `blake/blake2b.cpp`.

**ODR note:** `equi.h` defines `errstr[]`, `setheader()`, `genhash()`, `verifyrec()`, and
`verify()` as plain C functions (not `inline`, not `static`) directly in the header. This is
a multiple-definition ODR hazard: `equi.h` must be included in **exactly one translation unit**.
For verify-only use in the driver, confine the `#include "equi.h"` to a single `.cpp` file.

`equi_miner.h` defines `struct equi`, `thread_ctx`, `worker()`, and `barrier()` in the header
as well (not guarded by `inline`). Include it in **exactly one** `.cpp` too.

---

### (WN, WK) algorithm parameters — compile-time

Set via `-DWN=N -DWK=K`. Defaults in `equi.h`:

```c
#ifndef WN
#define WN  200
#endif

#ifndef WK
#define WK  9
#endif
```

Derived constants (all computed at compile time from WN and WK):

```c
#define NDIGITS      (WK+1)            // 10 at default
#define DIGITBITS    (WN/(NDIGITS))    // 20 at default

static const u32 PROOFSIZE = 1<<WK;   // 512 at WK=9
static const u32 BASE      = 1<<DIGITBITS;     // 1048576 at default
static const u32 NHASHES   = 2*BASE;           // 2097152
static const u32 HASHESPERBLAKE = 512/WN;      // 2 at WN=200
static const u32 HASHOUT = HASHESPERBLAKE*WN/8;// 50 bytes at default
```

For faster/smaller testing, use `-DWN=48 -DWK=5 -DRESTBITS=4` (see `eq4851` make target).

---

### Verify entry (the key API the driver times)

Defined in `equi.h` as a plain C function (NOT inline, NOT static — ODR hazard: one TU only):

```c
// Verify a Equihash proof against a headernonce + personalization string.
// indices:   proof array of PROOFSIZE u32 indices (unsorted; duplicates checked internally)
// headernonce: the combined header+nonce buffer, exactly HEADERNONCELEN bytes
// headerlen: must equal HEADERNONCELEN (= 140 by default) or returns POW_HEADER_LENGTH
// personal:  personalization prefix string, max 8 bytes; padded to 16 bytes
//            with LE-encoded WN (bytes 8-11) and WK (bytes 12-15)
// Returns: POW_OK (0) on success, non-zero verify_code on failure
int verify(u32 indices[PROOFSIZE], const char *headernonce, const u32 headerlen, const char *personal);
```

**Internal helpers also defined (not inline) in `equi.h`:**

```c
void setheader(blake2b_state *ctx, const char *headernonce, const char *personal);
void genhash(const blake2b_state *ctx, u32 idx, uchar *hash);
int  verifyrec(const blake2b_state *ctx, u32 *indices, uchar *hash, int r);
bool duped(proof prf);
int  compu32(const void *pa, const void *pb);
```

**Return codes (enum `verify_code` in `equi.h`):**

```c
enum verify_code {
    POW_OK,           // 0 — valid proof
    POW_HEADER_LENGTH,// 1 — headerlen != HEADERNONCELEN
    POW_DUPLICATE,    // 2 — duplicate index in proof
    POW_OUT_OF_ORDER, // 3 — indices not in increasing order within each subtree
    POW_NONZERO_XOR   // 4 — Wagner XOR condition not satisfied
};
const char *errstr[] = { "OK", "wrong header length", "duplicate index",
                         "indices out of order", "nonzero xor" };
// errstr[] is defined (not declared) in equi.h → ODR hazard, same as verify()
```

**Preparing the headernonce buffer for verify():**

```c
char headernonce[HEADERNONCELEN];   // HEADERNONCELEN = 140
u32 hdrlen = strlen(header);
memcpy(headernonce, header, hdrlen);
memset(headernonce + hdrlen, 0, sizeof(headernonce) - hdrlen);
// nonce lives at bytes [27*4 .. 27*4+3] (word 27) for the solver:
((u32 *)headernonce)[27] = htole32(nonce);
// NOTE: the standalone verify tool uses word index 32 for nonce; the solver uses 27.
// For driver benchmarking, use the solver's convention (word 27) and pass the same
// headernonce buffer directly to verify().
```

---

### Solve entry (CPU miner)

The solver is a C++ struct defined entirely in `equi_miner.h` (include + compile one TU only).
The driver must spawn threads manually using the `worker()` function.

```cpp
// 1. Construct solver (allocates ~145MB at WN=200,WK=9)
equi eq(nthreads);   // equi(const u32 n_threads) — also calls pthread_barrier_init

// 2. Set header + nonce (resets internal counters; call before each solve)
//    personalprefix: optional, defaults to "ZcashPoW" if NULL
eq.setheadernonce(const char *headernonce, const u32 len,
                  const char *personalprefix = 0);

// 3. Spawn nthreads POSIX threads, each running worker():
//    thread_ctx: { u32 id; pthread_t thread; equi *eq; }
thread_ctx tc[nthreads];
for (u32 t = 0; t < nthreads; t++) {
    tc[t].id = t; tc[t].eq = &eq;
    pthread_create(&tc[t].thread, NULL, worker, (void *)&tc[t]);
}
for (u32 t = 0; t < nthreads; t++)
    pthread_join(tc[t].thread, NULL);

// 4. Read solutions
u32 nsols = min((u32)MAXSOLS, (u32)eq.nsols);   // MAXSOLS = 8
// eq.sols[i] is proof[i] — a u32[PROOFSIZE] array of raw indices
for (u32 i = 0; i < nsols; i++) {
    u32 *sol = eq.sols[i];  // u32[PROOFSIZE]
    // pass directly to verify():
    int rc = verify(sol, headernonce, HEADERNONCELEN, "ZcashPoW");
}
```

---

### Proof representation + size in bytes

At default parameters (WN=200, WK=9):

```
typedef u32 proof[PROOFSIZE];   // from equi.h
                                // PROOFSIZE = 2^WK = 512
                                // u32 = uint32_t (always, regardless of WN/WK)
```

- In-memory proof (as stored in `eq.sols[]`): `u32[512]` = **2048 bytes**
- Compressed/wire-format proof (Zcash encoding): `PROOFSIZE * (DIGITBITS+1) / 8` = `512 * 21 / 8` = **1344 bytes**
  - Packed bit-field encoding, 21 bits per index, computed by `compress_solution()` in `equi_miner.cpp`
- For the benchmark driver, measure **verify of the uncompressed in-memory proof** (2048 bytes);
  record `proof_bytes=2048` in the CSV (or 1344 if benchmarking the compressed-verify path).

At smaller test parameters (WN=48, WK=5):

```
PROOFSIZE = 2^5 = 32
DIGITBITS = 48/6 = 8
In-memory:   u32[32]  = 128 bytes
Compressed:  32*9/8   = 36 bytes
```

---

### Build notes: CPU solver + verifier (no GPU required)

All sources self-contained. Bundled blake2b in `blake/blake2b.cpp`.
**GCC 13 compatibility patch required** to `blake/blake2.h` (see below).

#### GCC 13 patch to blake/blake2.h (already applied to third_party/equihash)

GCC 13 rejects arrays of over-aligned types where `sizeof(element)` is not a multiple of
the alignment. The original `blake2.h` places `ALIGN(64)` on `blake2s_state` and `blake2b_state`
inside a `#pragma pack(1)` block, making their `sizeof` not a multiple of 64. This causes:

```
error: size of array element is not a multiple of its alignment
```

Fix applied:
1. Moved `#pragma pack(pop)` to cover only the param structs (`blake2s_param`, `blake2b_param`).
2. Removed `ALIGN(64)` from state structs (`blake2s_state`, `blake2b_state`) — equihash does
   not require 64-byte alignment on these types; only the param structs need tight packing.
3. Removed `ALIGN(64)` from the parallel-hash wrapper structs (`blake2sp_state`, `blake2bp_state`)
   which are unused by the equihash miner.

#### Build equi1 CPU solver (WN=200, WK=9, single-threaded, no atomics):

```bash
cd third_party/equihash
g++ -march=native -m64 -std=c++11 \
    -Wall -Wno-deprecated-declarations \
    -D_POSIX_C_SOURCE=200112L -O3 -pthread \
    equi_miner.cpp blake/blake2b.cpp -o equi1
```

Or via Makefile: `cd third_party/equihash && make equi1`

Note: two `-Warray-bounds` warnings about `slot->bytes[4]` are emitted but are benign
(intentional pointer arithmetic in the original code). Build succeeds.

#### Build verify CLI tool (WN=200, WK=9):

```bash
cd third_party/equihash
g++ -g equi.c blake/blake2b.cpp -o verify
```

Or via Makefile: `cd third_party/equihash && make verify`

#### Build for smaller test parameters (WN=48, WK=5):

```bash
cd third_party/equihash
g++ -march=native -m64 -std=c++11 \
    -Wall -Wno-deprecated-declarations \
    -D_POSIX_C_SOURCE=200112L -O3 -pthread \
    -DWN=48 -DWK=5 -DRESTBITS=4 \
    equi_miner.cpp blake/blake2b.cpp -o eq4851
```

Or via Makefile: `cd third_party/equihash && make eq4851`

#### Driver-only verify (no solver, no equi_miner.h):

For the benchmark driver that only calls `verify()`:

```bash
g++ -std=c++11 -O3 \
    -I third_party/equihash \
    driver.cpp \
    third_party/equihash/blake/blake2b.cpp \
    -o equihash_driver
```

The driver's `.cpp` must `#include "equi.h"` (in exactly one TU) — `verify()` and all its
dependencies are defined (not declared) in that header.

#### Verified working (proof of build + run):

```
$ cd third_party/equihash

# Build (exits 0, two benign -Warray-bounds warnings only):
$ g++ -march=native -m64 -std=c++11 -Wall -Wno-deprecated-declarations \
      -D_POSIX_C_SOURCE=200112L -O3 -pthread \
      equi_miner.cpp blake/blake2b.cpp -o equi1

$ g++ -g equi.c blake/blake2b.cpp -o verify

# Solve nonce 0, show solutions:
$ ./equi1 -h "" -n 0 -t 1 -s
Looking for wagner-tree on ZcashPoW("0x...",0) with 10 20-bit digits and 1 threads
Using 2^10 buckets, 145MB of memory, and 1-way blake2b
Digit 0 b0 h0
...
Solution 58d 180768 13aa2 163011 a92c 16b3a 302e3 129de5 ...
Solution 35c 12d31f a2216 cbc99 7077 824a4 7659d 1d9f67 ...
2 solutions
2 total solutions

# Round-trip verify (pipe solver output through verify tool):
$ ./equi1 -h "" -n 0 -t 1 -s 2>/dev/null | grep ^Sol | ./verify -h "" -n 0
Verifying size 512 proof for ZcashPoW("",0)
Verified
Verified
```

---

### Driver integration notes for Task 11

1. **ODR constraint (critical):** `equi.h` defines `verify()`, `setheader()`, `verifyrec()`,
   `genhash()`, `duped()`, `compu32()`, and `errstr[]` directly in the header — none are
   `inline` or `static`. Include `equi.h` in **exactly one** `.cpp` TU. Do not include it in
   any header shared across TUs.

2. **Getting a proof to verify:** Call the full solver (equi class + worker threads), then read
   `eq.sols[i]` which is a `u32[PROOFSIZE]`. Pass directly to `verify(sol, headernonce,
   HEADERNONCELEN, personal)`. There is no "generate a known-good proof without solving" shortcut;
   the driver must run a full solve pass first to obtain at least one valid proof, then time
   repeated verify calls on that proof.

3. **Proof byte size for CSV:** `proof_bytes = PROOFSIZE * sizeof(u32) = 512 * 4 = 2048`.
   Use 2048 in the CSV `proof_bytes` column (uncompressed in-memory format that `verify()` takes).

4. **Memory:** The `equi` constructor allocates ~145MB heap at WN=200,WK=9 (via `hta.alloctrees()`).
   This is the solver's working memory — not needed for verify-only. The verify function itself
   uses O(WK * WN/8) stack/recursion space (≈ a few KB at defaults).

5. **Nonce convention:** The solver places the nonce at `((u32*)headernonce)[27]` (word index 27).
   The standalone `verify` CLI tool uses word index 32. For the driver, use the solver's convention
   (word 27) and pass the same buffer to `verify()` — this is the consistent path.

6. **No GPU code in CPU build:** `equi_miner.cpp` and `equi.c` are pure C/C++. The `.cu` files
   are separate. Do not reference them or link CUDA libraries in the driver.

7. **Parallelism:** `equi1` (no `-DATOMIC`) is single-threaded only. Multi-threaded use requires
   `-DATOMIC` (builds `equi`). For benchmarking, single-thread (`equi1`) is fine; solve time
   is not part of the timed verify path.
