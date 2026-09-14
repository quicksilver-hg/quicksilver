// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Cuckatoo verify compiled at EDGEBITS=28, isolated in its own namespace.
// 28 is the shipped graph size for BOTH block PoW and per-tx PoW on publictest
// and main, so this one verifier serves both proofs.

#include <crypto/cuckatoo/cuckatoo.h>
#include <crypto/cuckatoo/vendor_prelude.h>

#define EDGEBITS 28
#define PROOFSIZE 42
namespace cuckatoo_e28 {
#include "vendor/cuckatoo.h"
}
#undef EDGEBITS
#undef PROOFSIZE

namespace cuckatoo {

bool Verify28(const Cycle& cycle, const Keys& keys)
{
    ::siphash_keys sk;
    sk.k0 = keys.k0; sk.k1 = keys.k1; sk.k2 = keys.k2; sk.k3 = keys.k3;
    cuckatoo_e28::word_t edges[cuckatoo::PROOFSIZE];
    for (int i = 0; i < cuckatoo::PROOFSIZE; ++i) edges[i] = cycle[i];
    return cuckatoo_e28::verify(edges, &sk) == cuckatoo_e28::POW_OK;
}

} // namespace cuckatoo
