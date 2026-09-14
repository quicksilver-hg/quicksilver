// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Calibration-only registry of single-graph Cuckatoo solvers, one per EDGEBITS.
// NEVER linked into node binaries — this exists to measure solve cost across
// graph sizes for the transaction-cost flag day. See
// doc/design/chain-growth.md and tools/calibration/.
#ifndef QUICKSILVER_CRYPTO_CUCKATOO_BENCH_REGISTRY_H
#define QUICKSILVER_CRYPTO_CUCKATOO_BENCH_REGISTRY_H

#include <crypto/cuckatoo/cuckatoo.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cuckatoo {
namespace bench {

//! One EDGEBITS instantiation of the lean solver, as four C function pointers.
//! The context is created once and reused across graphs, matching how a real
//! grind works — creating it per graph would put a ~32 MB allocation at E28
//! inside the measured interval and corrupt the timing.
struct SolverVTable {
    //! Allocate a solver context using `nthreads` worker threads. Never null on success.
    void* (*create)(unsigned nthreads);
    //! Release a context from `create`. Safe on null.
    void (*destroy)(void* ctx);
    //! Search exactly ONE graph: the one keyed by `prepow` with `nonce` written
    //! as the trailing 4 little-endian bytes. Returns true iff a 42-cycle was
    //! found, filling `out`. Never searches a second graph.
    bool (*solve_one)(void* ctx, const unsigned char* prepow, size_t len,
                      uint32_t nonce, Cycle& out);
    //! Check `cycle` against the graph keyed by `prepow` (with the nonce already
    //! placed at the tail), using the vendored verifier compiled at this same
    //! EDGEBITS. Exists because the consensus verifier — cuckatoo::CuckatooVerify
    //! — dispatches over {19, 29} only, so it cannot check the swept sizes; and
    //! teaching it to would be a consensus change, which Stage 1 forbids. The
    //! bench harness is tied to consensus at E19 instead, where both verifiers
    //! exist and a graph is cheap. See cuckatoo_bench_solve_tests.cpp.
    bool (*verify_one)(const Cycle& cycle, const unsigned char* prepow, size_t len);
};

//! Register `vt` for `edgebits`. Returns true on success, false if already taken.
//! Called from static initialisers in the generated per-size translation units.
bool Register(uint8_t edgebits, const SolverVTable& vt);

//! The vtable for `edgebits`, or nullptr if no solver was built for that size.
const SolverVTable* Lookup(uint8_t edgebits);

//! Every registered edgebits value, ascending.
std::vector<uint8_t> Sizes();

} // namespace bench
} // namespace cuckatoo

#endif // QUICKSILVER_CRYPTO_CUCKATOO_BENCH_REGISTRY_H
