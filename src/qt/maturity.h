// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_MATURITY_H
#define QUICKSILVER_QT_MATURITY_H

#include <consensus/amount.h>
#include <qt/quicksilverunits.h>

#include <QString>

#include <cstdint>
#include <utility>
#include <vector>

namespace qsmaturity {

/**
 * Human-readable "spendable in about ..." for a coinbase that matures in
 * `blocks_remaining` blocks at `target_spacing_seconds` per block. Returns an
 * empty QString when nothing is maturing.
 *
 * Coinbase maturity is 100 blocks against a 5-minute target, so a freshly mined
 * coin is spendable roughly 8-9 hours later. Showing only an amount under a
 * "Maturing" label leaves a first-run miner with no idea whether that is minutes
 * or days, which is the kind of silence that reads as a broken application.
 */
QString FormatMaturityCountdown(int blocks_remaining, int64_t target_spacing_seconds);

/** The two ends of a vault's maturing balance, in blocks remaining. */
struct MaturingSummary {
    int soonest{0};             //!< blocks until the first maturing output frees up; 0 when none is maturing
    CAmount soonest_amount{0};  //!< the sum of every output that frees up at `soonest`
    int latest{0};              //!< blocks until the last maturing output frees up
};

/**
 * Summarize `(blocks_remaining, amount)` pairs, one per maturing row. Rows with
 * nothing left to wait (blocks_remaining <= 0) are ignored. Several rows can share
 * the soonest height, so the soonest amount is their sum, not any one of them.
 */
MaturingSummary SummarizeMaturing(const std::vector<std::pair<int, CAmount>>& rows);

/**
 * The overview's hint under "Maturing". A miner holds many coinbases one block
 * apart, so the soonest one alone is ~1% of the balance shown above it and is
 * always about one block away (F-432). When everything matures at one height the
 * single FormatMaturityCountdown text is true of the whole balance and is kept;
 * otherwise this says how much frees up first and when all of it does.
 * `soonest_amount` is already formatted, so privacy masking stays the caller's job.
 */
QString FormatMaturingHint(int soonest, const QString& soonest_amount, int latest, int64_t target_spacing_seconds);

/** Format the overview hint from its amounts, with inline spacing and privacy masking. */
QString FormatMaturingHint(const MaturingSummary& summary, QuicksilverUnit unit,
                           QuicksilverUnits::SeparatorStyle separators, bool privacy,
                           int64_t target_spacing_seconds);

} // namespace qsmaturity

#endif // QUICKSILVER_QT_MATURITY_H
