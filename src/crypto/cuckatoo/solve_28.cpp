// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Lean CPU Cuckatoo solver compiled at EDGEBITS=28, isolated in its own
// namespace. Mining only — never on the validation path. This is the per-tx
// PoW graph size for publictest/main. EDGEBITS-28 lean solving is memory-heavy and
// slow on CPU (~hundreds of MB working set); it is a reference / low-volume
// FALLBACK. Production per-tx grinding is GPU-class (the actual users are
// GPU-running agents) — see the doc/design/transaction-pow.md.
//
// Exposes both entry points at this graph size: Solve28 (fixed PREPOW_BYTES block
// pre-pow) and Solve28Bytes (variable-length per-tx pre-image). Block PoW and
// per-tx PoW share EDGEBITS=28, so a single E28 solver serves both.

#include <crypto/cuckatoo/cuckatoo.h>
#include <crypto/cuckatoo/vendor_prelude_solve.h>

#define EDGEBITS 28
#define PROOFSIZE 42
#define NSIPHASH 1
#define ATOMIC
#define SQUASH_OUTPUT 1
namespace cuckatoo_solve_e28 {
#include "vendor/lean.cpp"
}
#undef EDGEBITS
#undef PROOFSIZE
#undef NSIPHASH
#undef ATOMIC
#undef SQUASH_OUTPUT

#include <cstring>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

namespace cuckatoo {

// Fixed-width (PREPOW_BYTES) BLOCK pre-pow solver at EDGEBITS=28. Block PoW shares the per-tx
// graph size (one E28 solver serves both); block difficulty comes from the target
// (cycles-per-block), not a larger graph. Mining only — CPU fallback for the block
// PoW under -allowcpumining; the GPU path (the graphics helper, E28) is primary.
bool Solve28(const std::array<unsigned char, PREPOW_BYTES>& prepow, uint32_t start_nonce,
             uint32_t max_attempts, Cycle& out, uint32_t& out_nonce,
             const SolverProgressCallback& progress,
             const SolverCancelCallback& cancel)
{
    namespace E = cuckatoo_solve_e28;
    E::SolverParams params;
    E::fill_default_params(&params);
    params.mutate_nonce = 1;
    // Use all hardware threads for 28-edge-bit graphs.
    const unsigned hw = std::thread::hardware_concurrency();
    params.nthreads = hw == 0 ? 1 : hw;

    std::unique_ptr<E::SolverCtx, decltype(&E::destroy_solver_ctx)> ctx{
        E::create_solver_ctx(&params), E::destroy_solver_ctx};
    char hdr[PREPOW_BYTES];
    std::memcpy(hdr, prepow.data(), sizeof(hdr));

    bool found = false;
    for (uint32_t i = 0; i < max_attempts && !found; ++i) {
        const uint32_t nonce = start_nonce + i;
        if (cancel && cancel()) break;  // shutdown: abandon the sweep between graphs
        if (progress) progress(nonce);
        E::SolverSolutions sols{};
        E::run_solver(ctx.get(), hdr, sizeof(hdr), nonce, /*range=*/1, &sols);
        if (sols.num_sols > 0) {
            for (int j = 0; j < cuckatoo::PROOFSIZE; ++j) {
                out[j] = static_cast<uint32_t>(sols.sols[0].proof[j]);
            }
            out_nonce = nonce;
            found = true;
        }
    }
    return found;
}

bool Solve28Bytes(const unsigned char* prepow, size_t len, uint32_t start_nonce,
                  uint32_t max_attempts, Cycle& out, uint32_t& out_nonce,
             const SolverProgressCallback& progress,
             const SolverCancelCallback& cancel)
{
    namespace E = cuckatoo_solve_e28;
    E::SolverParams params;
    E::fill_default_params(&params);
    params.mutate_nonce = 0;  // we place the nonce ourselves (alignment-safe for any len)
    // Use all hardware threads for 28-edge-bit graphs.
    const unsigned hw = std::thread::hardware_concurrency();
    params.nthreads = hw == 0 ? 1 : hw;

    // The nonce goes in buf[len-4..len-1] and run_solver takes a uint32_t length.
    // CuckatooSolveBytes refuses such a buffer first; this keeps the entry point
    // safe on its own.
    if (len < 4 || len > std::numeric_limits<uint32_t>::max()) return false;
    std::unique_ptr<E::SolverCtx, decltype(&E::destroy_solver_ctx)> ctx{
        E::create_solver_ctx(&params), E::destroy_solver_ctx};
    std::vector<char> buf(prepow, prepow + len);

    bool found = false;
    for (uint32_t i = 0; i < max_attempts && !found; ++i) {
        const uint32_t nonce = start_nonce + i;
        if (cancel && cancel()) break;  // shutdown: abandon the sweep between graphs
        if (progress) progress(nonce);
        buf[len - 4] = (char)(nonce & 0xff);
        buf[len - 3] = (char)((nonce >> 8) & 0xff);
        buf[len - 2] = (char)((nonce >> 16) & 0xff);
        buf[len - 1] = (char)((nonce >> 24) & 0xff);
        E::SolverSolutions sols{};
        E::run_solver(ctx.get(), buf.data(), (E::u32)len, nonce, /*range=*/1, &sols);
        if (sols.num_sols > 0) {
            for (int j = 0; j < cuckatoo::PROOFSIZE; ++j) {
                out[j] = static_cast<uint32_t>(sols.sols[0].proof[j]);
            }
            out_nonce = nonce;
            found = true;
        }
    }
    return found;
}

} // namespace cuckatoo
