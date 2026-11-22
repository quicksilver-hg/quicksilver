// Copyright (c) 2023 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_TEST_UTIL_COINS_H
#define QUICKSILVER_TEST_UTIL_COINS_H

#include <primitives/transaction.h>

class CCoinsViewCache;
class FastRandomContext;

/**
 * Create a Coin with platform-dependent DynamicMemoryUsage and add it to the view.
 * @param[in,out] coins_view  The coins view cache to add the new coin to.
 * @returns the COutPoint of the created coin.
 */
COutPoint AddTestCoin(FastRandomContext& rng, CCoinsViewCache& coins_view);

#endif // QUICKSILVER_TEST_UTIL_COINS_H
