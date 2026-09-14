// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Global-scope pre-includes for the per-EDGEBITS SOLVER translation units.
//
// The solver chain (lean.cpp -> lean.hpp -> cuckatoo.h/graph.hpp/siphashxN.h/
// barrier.hpp/...) pulls a wider set of system headers than the verify path.
// We include every one of them at GLOBAL scope here so that when lean.cpp is
// #included INSIDE a per-EDGEBITS namespace, those system headers are guarded
// out and only the solver's own (EDGEBITS-dependent) symbols land in the
// namespace. Unlike the verify prelude, we do NOT pre-include the project
// headers (blake2.h/siphash.hpp): the solver wrapper drives run_solver() with
// raw header bytes and needs no global siphash_keys, so those stay namespaced.
#ifndef QUICKSILVER_CRYPTO_CUCKATOO_VENDOR_PRELUDE_SOLVE_H
#define QUICKSILVER_CRYPTO_CUCKATOO_VENDOR_PRELUDE_SOLVE_H

#include <assert.h>
#include <atomic>
#include <bit>       // std::countl_zero for SIZEMASK (portable __builtin_clz replacement)
#include <chrono>
#include <condition_variable>  // trim_barrier (ported from pthread to std threading for MSVC)
#include <ctime>
#include <errno.h>
#include <immintrin.h>
#include <mutex>               // trim_barrier
#include <new>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>              // run_solver worker threads (ported from pthread for MSVC)

#endif // QUICKSILVER_CRYPTO_CUCKATOO_VENDOR_PRELUDE_SOLVE_H
