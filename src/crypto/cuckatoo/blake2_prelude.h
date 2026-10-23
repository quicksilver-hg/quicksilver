// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Owned include of the vendored blake2.h. Every first-party site that needs
// blake2b goes through this header, so the vendored file has one entry point.
// Do not include vendor/blake2.h directly.
#ifndef QUICKSILVER_CRYPTO_CUCKATOO_BLAKE2_PRELUDE_H
#define QUICKSILVER_CRYPTO_CUCKATOO_BLAKE2_PRELUDE_H

#include "vendor/blake2.h"     // extern "C" blake2b decls

#endif // QUICKSILVER_CRYPTO_CUCKATOO_BLAKE2_PRELUDE_H
