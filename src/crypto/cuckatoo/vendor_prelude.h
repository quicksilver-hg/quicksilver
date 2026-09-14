// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Global-scope pre-includes for the per-EDGEBITS translation units.
//
// The vendored cuckatoo.h has no include guard and pulls system headers plus
// the project headers blake2.h / siphash.hpp. We include all of those at GLOBAL
// scope here so that when cuckatoo.h is subsequently #included INSIDE a
// per-EDGEBITS namespace, every one of those includes is a no-op (guard / pragma
// once already satisfied) and cuckatoo.h emits ONLY its own EDGEBITS-dependent
// symbols into the namespace. siphash_keys thus stays a single global type.
//
// Include this at global scope, before opening the per-EDGEBITS namespace.
#ifndef QUICKSILVER_CRYPTO_CUCKATOO_VENDOR_PRELUDE_H
#define QUICKSILVER_CRYPTO_CUCKATOO_VENDOR_PRELUDE_H

// Exactly the system headers cuckatoo.h includes (matched by name so guards align).
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <bit>       // std::countl_zero for SIZEMASK (portable __builtin_clz replacement)
#include <chrono>
#include <ctime>

#include "vendor/blake2.h"     // extern "C" blake2b decls
#include "vendor/siphash.hpp"  // global ::siphash_keys (inline methods)

#endif // QUICKSILVER_CRYPTO_CUCKATOO_VENDOR_PRELUDE_H
