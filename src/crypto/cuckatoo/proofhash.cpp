// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/cuckatoo/cuckatoo.h>

#include <span.h>

#include "blake2_prelude.h"  // self-guarded extern "C"; owns MSVC C4804

namespace cuckatoo {

uint256 CuckatooProofHash(const Cycle& cycle)
{
    // Consensus encoding: the 42 edges as little-endian uint32, in order.
    unsigned char buf[PROOFSIZE * 4];
    for (int i = 0; i < PROOFSIZE; ++i) {
        const uint32_t e = cycle[i];
        buf[i * 4 + 0] = static_cast<unsigned char>(e & 0xff);
        buf[i * 4 + 1] = static_cast<unsigned char>((e >> 8) & 0xff);
        buf[i * 4 + 2] = static_cast<unsigned char>((e >> 16) & 0xff);
        buf[i * 4 + 3] = static_cast<unsigned char>((e >> 24) & 0xff);
    }
    unsigned char out[32];
    blake2b(out, 32, buf, sizeof(buf), nullptr, 0);
    return uint256(Span<const unsigned char>(out, 32));
}

} // namespace cuckatoo
