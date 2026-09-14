// Copyright (c) 2012-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/bloom.h>

#include <hash.h>
#include <random.h>
#include <span.h>
#include <util/fastrange.h>

#include <algorithm>
#include <cmath>
#include <vector>

CRollingBloomFilter::CRollingBloomFilter(const unsigned int nElements, const double fpRate)
{
    double logFpRate = log(fpRate);
    /* The optimal number of hash functions is log(fpRate) / log(0.5), but
     * restrict it to the range 1-50. */
    nHashFuncs = std::max(1, std::min((int)round(logFpRate / log(0.5)), 50));
    /* In this rolling bloom filter, we'll store between 2 and 3 generations of nElements / 2 entries. */
    nEntriesPerGeneration = (nElements + 1) / 2;
    uint32_t nMaxElements = nEntriesPerGeneration * 3;
    /* The maximum fpRate = pow(1.0 - exp(-nHashFuncs * nMaxElements / nFilterBits), nHashFuncs)
     * =>          pow(fpRate, 1.0 / nHashFuncs) = 1.0 - exp(-nHashFuncs * nMaxElements / nFilterBits)
     * =>          1.0 - pow(fpRate, 1.0 / nHashFuncs) = exp(-nHashFuncs * nMaxElements / nFilterBits)
     * =>          log(1.0 - pow(fpRate, 1.0 / nHashFuncs)) = -nHashFuncs * nMaxElements / nFilterBits
     * =>          nFilterBits = -nHashFuncs * nMaxElements / log(1.0 - pow(fpRate, 1.0 / nHashFuncs))
     * =>          nFilterBits = -nHashFuncs * nMaxElements / log(1.0 - exp(logFpRate / nHashFuncs))
     */
    uint32_t nFilterBits = (uint32_t)ceil(-1.0 * nHashFuncs * nMaxElements / log(1.0 - exp(logFpRate / nHashFuncs)));
    data.clear();
    /* For each data element we need to store 2 bits. If both bits are 0, the
     * bit is treated as unset. If the bits are (01), (10), or (11), the bit is
     * treated as set in generation 1, 2, or 3 respectively.
     * These bits are stored in separate integers: position P corresponds to bit
     * (P & 63) of the integers data[(P >> 6) * 2] and data[(P >> 6) * 2 + 1]. */
    data.resize(((nFilterBits + 63) / 64) << 1);
    reset();
}

/* Seeds MurmurHash3 per hash function; 0xFBA4C795 is the stride between seeds. */
static inline uint32_t RollingBloomHash(unsigned int nHashNum, uint32_t nTweak, Span<const unsigned char> vDataToHash)
{
    return MurmurHash3(nHashNum * 0xFBA4C795 + nTweak, vDataToHash);
}

void CRollingBloomFilter::insert(Span<const unsigned char> vKey)
{
    if (nEntriesThisGeneration == nEntriesPerGeneration) {
        nEntriesThisGeneration = 0;
        nGeneration++;
        if (nGeneration == 4) {
            nGeneration = 1;
        }
        uint64_t nGenerationMask1 = 0 - (uint64_t)(nGeneration & 1);
        uint64_t nGenerationMask2 = 0 - (uint64_t)(nGeneration >> 1);
        /* Wipe old entries that used this generation number. */
        for (uint32_t p = 0; p < data.size(); p += 2) {
            uint64_t p1 = data[p], p2 = data[p + 1];
            uint64_t mask = (p1 ^ nGenerationMask1) | (p2 ^ nGenerationMask2);
            data[p] = p1 & mask;
            data[p + 1] = p2 & mask;
        }
    }
    nEntriesThisGeneration++;

    for (int n = 0; n < nHashFuncs; n++) {
        uint32_t h = RollingBloomHash(n, nTweak, vKey);
        int bit = h & 0x3F;
        /* FastMod works with the upper bits of h, so it is safe to ignore that the lower bits of h are already used for bit. */
        uint32_t pos = FastRange32(h, data.size());
        /* The lowest bit of pos is ignored, and set to zero for the first bit, and to one for the second. */
        data[pos & ~1U] = (data[pos & ~1U] & ~(uint64_t{1} << bit)) | (uint64_t(nGeneration & 1)) << bit;
        data[pos | 1] = (data[pos | 1] & ~(uint64_t{1} << bit)) | (uint64_t(nGeneration >> 1)) << bit;
    }
}

bool CRollingBloomFilter::contains(Span<const unsigned char> vKey) const
{
    for (int n = 0; n < nHashFuncs; n++) {
        uint32_t h = RollingBloomHash(n, nTweak, vKey);
        int bit = h & 0x3F;
        uint32_t pos = FastRange32(h, data.size());
        /* If the relevant bit is not set in either data[pos & ~1] or data[pos | 1], the filter does not contain vKey */
        if (!(((data[pos & ~1U] | data[pos | 1]) >> bit) & 1)) {
            return false;
        }
    }
    return true;
}

void CRollingBloomFilter::reset()
{
    nTweak = FastRandomContext().rand<unsigned int>();
    nEntriesThisGeneration = 0;
    nGeneration = 1;
    std::fill(data.begin(), data.end(), 0);
}
