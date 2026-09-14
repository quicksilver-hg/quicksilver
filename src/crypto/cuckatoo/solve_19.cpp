// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Lean CPU Cuckatoo solver compiled at EDGEBITS=19, isolated in its own
// namespace. Mining only — never on the validation path.

#include <crypto/cuckatoo/cuckatoo.h>             // public API (before the macros)
#include <crypto/cuckatoo/vendor_prelude_solve.h> // system headers, global

#define EDGEBITS 19
#define PROOFSIZE 42
#define NSIPHASH 1        // scalar siphash (compiles out the AVX paths)
#define ATOMIC            // atomic edge bitmap (matches the validated lean19x1 build)
#define SQUASH_OUTPUT 1   // silence the solver's stdout chatter
#define CUCKATOO_NO_MAIN  // drop the standalone CLI main()
namespace cuckatoo_solve_e19 {
#include "vendor/lean.cpp"  // run_solver/create_solver_ctx/... + the whole solver chain
}
#undef EDGEBITS
#undef PROOFSIZE
#undef NSIPHASH
#undef ATOMIC
#undef SQUASH_OUTPUT
#undef CUCKATOO_NO_MAIN

#include <cstring>
#include <thread>
#include <vector>

namespace cuckatoo {

bool Solve19(const std::array<unsigned char, PREPOW_BYTES>& prepow, uint32_t start_nonce,
             uint32_t max_attempts, Cycle& out, uint32_t& out_nonce,
             const SolverProgressCallback& progress,
             const SolverCancelCallback& cancel)
{
    namespace E = cuckatoo_solve_e19;
    E::SolverParams params;
    E::fill_default_params(&params);
    params.mutate_nonce = 1;  // sweep nonces, written into the header's nNonce slot
    const unsigned hw = std::thread::hardware_concurrency();
    params.nthreads = hw == 0 ? 1 : (hw < 4 ? hw : 4);

    E::SolverCtx* ctx = E::create_solver_ctx(&params);  // created once, reused across the sweep
    char hdr[PREPOW_BYTES];
    std::memcpy(hdr, prepow.data(), sizeof(hdr));

    bool found = false;
    for (uint32_t i = 0; i < max_attempts && !found; ++i) {
        const uint32_t nonce = start_nonce + i;
        if (cancel && cancel()) break;  // shutdown: abandon the sweep between graphs
        if (progress) progress(nonce);
        E::SolverSolutions sols;
        std::memset(&sols, 0, sizeof(sols));
        E::run_solver(ctx, hdr, sizeof(hdr), nonce, /*range=*/1, &sols, nullptr);
        if (sols.num_sols > 0) {
            for (int j = 0; j < cuckatoo::PROOFSIZE; ++j) {
                out[j] = static_cast<uint32_t>(sols.sols[0].proof[j]);
            }
            out_nonce = nonce;
            found = true;
        }
    }
    E::destroy_solver_ctx(ctx);
    return found;
}

bool Solve19Bytes(const unsigned char* prepow, size_t len, uint32_t start_nonce,
                  uint32_t max_attempts, Cycle& out, uint32_t& out_nonce,
                  const SolverProgressCallback& progress,
                  const SolverCancelCallback& cancel)
{
    namespace E = cuckatoo_solve_e19;
    E::SolverParams params;
    E::fill_default_params(&params);
    params.mutate_nonce = 0;  // we place the nonce ourselves (alignment-safe for any len)
    const unsigned hw = std::thread::hardware_concurrency();
    params.nthreads = hw == 0 ? 1 : (hw < 4 ? hw : 4);

    E::SolverCtx* ctx = E::create_solver_ctx(&params);
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
        E::SolverSolutions sols;
        std::memset(&sols, 0, sizeof(sols));
        E::run_solver(ctx, buf.data(), (E::u32)len, nonce, /*range=*/1, &sols, nullptr);
        if (sols.num_sols > 0) {
            for (int j = 0; j < cuckatoo::PROOFSIZE; ++j) {
                out[j] = static_cast<uint32_t>(sols.sols[0].proof[j]);
            }
            out_nonce = nonce;
            found = true;
        }
    }
    E::destroy_solver_ctx(ctx);
    return found;
}

} // namespace cuckatoo
