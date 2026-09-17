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

// blake2.h's padding check is `1/(sizeof(param) == OUTBYTES)`, whose divisor is a
// bool in C++. MSVC calls that C4804 at /W1, on every TU that pulls the header in.
// It was a warning-only nuisance while the only consumers were quicksilver_cuckatoo
// TUs, which are not built /WX -- but test_quicksilver IS, so the first test TU to
// include this prelude turned a long-standing warning into a build failure under
// -DWERROR=ON (CI's Win64 job; a local WERROR=OFF build never sees it). The check
// itself is correct and belongs to the vendored file, so silence it here rather
// than widen the vendored delta or disable C4804 for the whole project.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4804)  // '/': unsafe use of type 'bool' in operation
#endif
#include "vendor/blake2.h"     // extern "C" blake2b decls
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include "vendor/siphash.hpp"  // global ::siphash_keys (inline methods)

#endif // QUICKSILVER_CRYPTO_CUCKATOO_VENDOR_PRELUDE_H
