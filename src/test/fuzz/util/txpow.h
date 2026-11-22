// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_TEST_FUZZ_UTIL_TXPOW_H
#define QUICKSILVER_TEST_FUZZ_UTIL_TXPOW_H

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <crypto/cuckatoo/cuckatoo.h>
#include <pow.h>
#include <primitives/transaction.h>

// Build an anchored proof under sandbox's -txpownocycle option. The cycle still
// has to meet the hash target, and the anchor must precede the transaction ID.
inline void ProveFuzzTxPow(CMutableTransaction& tx, const CBlockIndex* tip)
{
    tx.nAnchorHeight = tip->nHeight;
    const uint256 target{GetTxPowTarget(Params().GetConsensus(), tip, CTransaction(tx))};
    for (uint32_t i = 0; i < tx.nCycle.size(); ++i) tx.nCycle[i] = i + 1;
    while (UintToArith256(cuckatoo::CuckatooProofHash(tx.nCycle)) > UintToArith256(target)) {
        tx.nCycle[0] += static_cast<uint32_t>(tx.nCycle.size());
    }
}

#endif // QUICKSILVER_TEST_FUZZ_UTIL_TXPOW_H
