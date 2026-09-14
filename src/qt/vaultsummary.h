// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_VAULTSUMMARY_H
#define QUICKSILVER_QT_VAULTSUMMARY_H

#include <qt/quicksilverunits.h>

#include <QStringList>

namespace interfaces {
struct VaultBalances;
} // namespace interfaces

namespace qsvaultsummary {

/**
 * Labelled holdings segments for the launch card, one per non-zero holding.
 *
 * `VaultBalances::balance` is the spendable figure alone: it excludes coin that is
 * pending, immature or delegated to an agent. Rendering it by itself tells a vault
 * that has just been paid it holds `0.00000000 Hg` while the Ledger shows the
 * credit, which on a chain whose block target is five minutes is the first thing a
 * new user sees. So every holding `balance` leaves out gets its own segment.
 *
 * Zero holdings are omitted rather than shown as zero, matching the Home HUD: a
 * vault that has never mined or funded an agent should not have to read lines
 * explaining concepts it does not use.
 */
QStringList FormatHoldings(const interfaces::VaultBalances& balances, QuicksilverUnit unit, bool privacy);

} // namespace qsvaultsummary

#endif // QUICKSILVER_QT_VAULTSUMMARY_H
