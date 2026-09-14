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
namespace {
QString Amount(QuicksilverUnit unit, const CAmount& amount, bool privacy)
{
    // formatWithPrivacy() right-justifies to a fixed width so the Home HUD's stacked
    // labels line up, and the ALWAYS separator style then threads thin spaces through
    // that padding. Inline in a sentence the padding is just a ragged gap, so drop it.
    return QuicksilverUnits::formatWithPrivacy(unit, amount, QuicksilverUnits::SeparatorStyle::ALWAYS, privacy).trimmed();
}
} // namespace

QStringList FormatHoldings(const interfaces::VaultBalances& balances, QuicksilverUnit unit, bool privacy)
{
    // The labels match the Home HUD ("Pending", "Immature", "Delegated") so the same
    // holding is not called two different things on two screens.
    QStringList holdings;
    holdings << QObject::tr("Balance: %1").arg(Amount(unit, balances.balance, privacy));
    if (balances.unconfirmed_balance != 0) {
        holdings << QObject::tr("Pending: %1").arg(Amount(unit, balances.unconfirmed_balance, privacy));
    }
    if (balances.immature_balance != 0) {
        holdings << QObject::tr("Immature: %1").arg(Amount(unit, balances.immature_balance, privacy));
    }
    if (balances.delegated_balance != 0) {
        holdings << QObject::tr("Delegated: %1").arg(Amount(unit, balances.delegated_balance, privacy));
    }
    return holdings;
}

} // namespace qsvaultsummary
