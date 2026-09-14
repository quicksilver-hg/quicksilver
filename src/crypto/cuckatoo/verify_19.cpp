// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Cuckatoo verify compiled at EDGEBITS=19, isolated in its own namespace.

#include <crypto/cuckatoo/cuckatoo.h>     // public API (cuckatoo::PROOFSIZE constexpr) — before the macro
#include <crypto/cuckatoo/vendor_prelude.h>

#define EDGEBITS 19
#define PROOFSIZE 42
namespace cuckatoo_e19 {
#include "vendor/cuckatoo.h"  // emits verify(), word_t, POW_OK, ... into this namespace
}
#undef EDGEBITS
#undef PROOFSIZE

namespace cuckatoo {

bool Verify19(const Cycle& cycle, const Keys& keys)
{
    ::siphash_keys sk;
    sk.k0 = keys.k0; sk.k1 = keys.k1; sk.k2 = keys.k2; sk.k3 = keys.k3;
    cuckatoo_e19::word_t edges[cuckatoo::PROOFSIZE];
    for (int i = 0; i < cuckatoo::PROOFSIZE; ++i) edges[i] = cycle[i];
    return cuckatoo_e19::verify(edges, &sk) == cuckatoo_e19::POW_OK;
}

} // namespace cuckatoo
