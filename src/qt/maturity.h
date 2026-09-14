// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_MATURITY_H
#define QUICKSILVER_QT_MATURITY_H

#include <QString>

#include <cstdint>

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

} // namespace qsmaturity

#endif // QUICKSILVER_QT_MATURITY_H
