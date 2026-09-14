// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/cuckatoo/cuckatoo.h>

#include <cstring>

#include "vendor/blake2.h"           // self-guarded extern "C"
#include <compat/endian.h>           // htole64_internal — the tree's single portable endian impl

namespace cuckatoo {

Keys CuckatooSetHeader(const unsigned char* header, uint32_t headerlen)
{
    // Mirrors the vendored setheader()/siphash_keys::setkeys():
    // blake2b(header) -> 32-byte key -> four little-endian u64 siphash keys.
    // We inline the four reads instead of pulling siphash.hpp (whose member
    // definitions are non-inline) into global scope.
    unsigned char hdrkey[32];
    blake2b(hdrkey, sizeof(hdrkey), header, headerlen, nullptr, 0);

    uint64_t w[4];
    std::memcpy(w, hdrkey, sizeof(w));  // avoid aliasing UB from a raw cast
    return Keys{htole64_internal(w[0]), htole64_internal(w[1]), htole64_internal(w[2]), htole64_internal(w[3])};
}

} // namespace cuckatoo
