// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Owned include of the vendored blake2.h. Every first-party site that needs
// blake2b goes through this header so the MSVC C4804 suppression lives in one
// place. Do not include vendor/blake2.h directly.
#ifndef QUICKSILVER_CRYPTO_CUCKATOO_BLAKE2_PRELUDE_H
#define QUICKSILVER_CRYPTO_CUCKATOO_BLAKE2_PRELUDE_H

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

#endif // QUICKSILVER_CRYPTO_CUCKATOO_BLAKE2_PRELUDE_H
