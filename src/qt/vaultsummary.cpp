// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/vaultsummary.h>

#include <consensus/amount.h>
#include <interfaces/vault.h>
#include <qt/quicksilverunits.h>

#include <QObject>
#include <QString>

namespace qsvaultsummary {
QStringList FormatHoldings(const interfaces::VaultBalances& balances, QuicksilverUnit unit, bool privacy)
{
    // The labels match the Home HUD ("Pending", "Immature", "Delegated") so the same
    // holding is not called two different things on two screens.
    QStringList holdings;
    holdings << QObject::tr("Balance: %1").arg(QuicksilverUnits::formatInlineWithPrivacy(unit, balances.balance, QuicksilverUnits::SeparatorStyle::ALWAYS, privacy));
    if (balances.unconfirmed_balance != 0) {
        holdings << QObject::tr("Pending: %1").arg(QuicksilverUnits::formatInlineWithPrivacy(unit, balances.unconfirmed_balance, QuicksilverUnits::SeparatorStyle::ALWAYS, privacy));
    }
    if (balances.immature_balance != 0) {
        holdings << QObject::tr("Immature: %1").arg(QuicksilverUnits::formatInlineWithPrivacy(unit, balances.immature_balance, QuicksilverUnits::SeparatorStyle::ALWAYS, privacy));
    }
    if (balances.delegated_balance != 0) {
        holdings << QObject::tr("Delegated: %1").arg(QuicksilverUnits::formatInlineWithPrivacy(unit, balances.delegated_balance, QuicksilverUnits::SeparatorStyle::ALWAYS, privacy));
    }
    return holdings;
}

} // namespace qsvaultsummary
