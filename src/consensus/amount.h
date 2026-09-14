// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_CONSENSUS_AMOUNT_H
#define QUICKSILVER_CONSENSUS_AMOUNT_H

#include <cstdint>
#include <string>

/** Amount in cinnabar (Can be negative) */
typedef int64_t CAmount;

/** The amount of cinnabar in one coin. */
static constexpr CAmount COIN = 100000000;

inline const std::string CURRENCY_UNIT{"Hg"}; // One formatted unit (Quicksilver; Hg = mercury)

/** Quicksilver: NOT an economic supply cap. Quicksilver has no hard cap — a
 *  perpetual ~1-coin tail subsidy plus the Frame-B mint C issue coins forever
 *  (economics spec §7), so cumulative supply grows without bound and passes the
 *  old 21M mark within ~10 years. This constant is purely a per-value OVERFLOW
 *  GUARD: MoneyRange bounds every individual CAmount so that summing values
 *  cannot overflow int64 (CAmount maxes at INT64_MAX ≈ 92.2e9 COIN). 1e9 COIN is
 *  ~3000 years above realistic supply at maximum emission and leaves 2*MAX_MONEY
 *  (2e17) << INT64_MAX (9.22e18); overflow safety at this height relies on the
 *  feeless invariant (fees == 0, so no large multi-term value sum exists — see
 *  the MAX_MONEY reconciliation design). It remains consensus-critical: changing
 *  it changes which values are valid. */
static constexpr CAmount MAX_MONEY = 1'000'000'000 * COIN;
inline bool MoneyRange(const CAmount& nValue) { return (nValue >= 0 && nValue <= MAX_MONEY); }

#endif // QUICKSILVER_CONSENSUS_AMOUNT_H
