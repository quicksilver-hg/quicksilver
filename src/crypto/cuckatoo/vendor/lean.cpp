// Cuckatoo Cycle, a memory-hard proof-of-work
// Copyright (c) 2013-2023 John Tromp

#include "lean.hpp"
#include <thread>
using SolverCtx = cuckoo_ctx;

static void run_solver(SolverCtx* ctx,
                               char* header,
                               int header_length,
                               u32 nonce,
                               u32 range,
                               SolverSolutions *solutions
                               )
{
  u64 time0, time1;
  u32 timems;
  u32 sumnsols = 0;
  thread_ctx *threads = new thread_ctx[ctx->nthreads];
  assert(threads);
  for (u32 r = 0; r < range; r++) {
    time0 = timestamp();
    ctx->setheadernonce(header, header_length, nonce + r);
    print_log("nonce %d k0 k1 k2 k3 %llx %llx %llx %llx\n", nonce+r, ctx->sip_keys.k0, ctx->sip_keys.k1, ctx->sip_keys.k2, ctx->sip_keys.k3);
    ctx->barry.clear();
    for (u32 t = 0; t < ctx->nthreads; t++) {
      threads[t].id = t;
      threads[t].ctx = ctx;
      threads[t].thread = std::thread(worker, (void *)&threads[t]);
    }
    for (u32 t = 0; t < ctx->nthreads; t++) {
      threads[t].thread.join();
    }
    time1 = timestamp();
    timems = (time1 - time0) / 1000000;
    print_log("Time: %d ms\n", timems);
    for (unsigned s = 0; s < ctx->nsols; s++) {
      print_log("Solution");
      for (int j = 0; j < PROOFSIZE; j++)
        print_log(" %jx", (uintmax_t)ctx->sols[s][j]);
      print_log("\n");
      // Quicksilver: `sumnsols += ctx->nsols` used to live inside this loop, so the
      // write index advanced 0, N+1, 2N+2, ... instead of 0, 1, 2, ... Any solve
      // returning 3+ cycles therefore wrote Solution structs past the end of
      // `solutions`, which is a STACK local in Solve19Bytes -- overwriting the saved
      // rbp/rip with sorted proof nonces. The bound below is belt-and-braces: the
      // caller's array holds MAX_SOLS entries and nothing may write beyond it.
      if (solutions != NULL && sumnsols + s < MAX_SOLS) {
        solutions->edge_bits = EDGEBITS;
        solutions->num_sols++;
        solutions->sols[sumnsols+s].nonce = nonce + r;
        for (u32 i = 0; i < PROOFSIZE; i++)
          solutions->sols[sumnsols+s].proof[i] = (u64) ctx->sols[s][i];
      }
      int pow_rc = verify(ctx->sols[s], &ctx->sip_keys);
      if (pow_rc == POW_OK) {
        print_log("Verified with cyclehash ");
        unsigned char cyclehash[32];
        blake2b((void *)cyclehash, sizeof(cyclehash), (const void *)ctx->sols[s], sizeof(ctx->sols[0]), 0, 0);
        for (int i=0; i<32; i++)
          print_log("%02x", cyclehash[i]);
        print_log("\n");
      } else {
        print_log("FAILED due to %s\n", errstr[pow_rc]);
      }
    }
    sumnsols += ctx->nsols;  // Quicksilver: once per range iteration, NOT per solution
  }
  delete[] threads;
  print_log("%d total solutions\n", sumnsols);
}

static SolverCtx* create_solver_ctx(SolverParams* params) {
  if (params->nthreads == 0) params->nthreads = 1;
  if (params->ntrims == 0) params->ntrims = EDGEBITS > 30 ? 96 : 68;

  SolverCtx* ctx = new SolverCtx(params->nthreads,
                                 params->ntrims,
                                 MAXSOLS,
                                 params->mutate_nonce);
  return ctx;
}

static void destroy_solver_ctx(SolverCtx* ctx) {
  delete ctx;
}

static void fill_default_params(SolverParams* params) {
  params->nthreads = 1;
  params->ntrims   = 8 * (PART_BITS+3) * (PART_BITS+4);
  params->mutate_nonce = 0;
}
