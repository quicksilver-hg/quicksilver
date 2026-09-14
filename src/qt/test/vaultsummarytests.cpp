// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/vaultsummarytests.h>

#include <consensus/amount.h>
#include <interfaces/vault.h>
#include <qt/quicksilverunits.h>
#include <qt/vaultsummary.h>

#include <QStringList>

using qsvaultsummary::FormatHoldings;

namespace {
constexpr QuicksilverUnit HG{QuicksilverUnit::HG};
} // namespace

void VaultSummaryTests::spendableOnlyReadsAsOneSegment()
{
    interfaces::VaultBalances balances;
    balances.balance = 2 * COIN;

    const QStringList holdings = FormatHoldings(balances, HG, /*privacy=*/false);
    QCOMPARE(holdings.size(), 1);
    QCOMPARE(holdings.at(0), QString("Balance: 2.00000000 Hg"));
}

void VaultSummaryTests::freshlyPaidVaultIsNotReportedEmpty()
{
    // The first-run case this guards: the vault has been paid, the credit is still in
    // the relay pool, and the spendable balance is genuinely zero. Rendering only the
    // spendable figure told that user they held nothing.
    interfaces::VaultBalances balances;
    balances.unconfirmed_balance = 3 * COIN / 2;

    const QStringList holdings = FormatHoldings(balances, HG, /*privacy=*/false);
    QCOMPARE(holdings.size(), 2);
    QCOMPARE(holdings.at(0), QString("Balance: 0.00000000 Hg"));
    QCOMPARE(holdings.at(1), QString("Pending: 1.50000000 Hg"));
}

void VaultSummaryTests::immatureAndDelegatedAppearOnlyWhenHeld()
{
    interfaces::VaultBalances none;
    none.balance = COIN;
    QCOMPARE(FormatHoldings(none, HG, /*privacy=*/false).size(), 1);

    interfaces::VaultBalances all;
    all.balance = COIN;
    all.unconfirmed_balance = 2 * COIN;
    all.immature_balance = 3 * COIN;
    all.delegated_balance = 4 * COIN;

    const QStringList holdings = FormatHoldings(all, HG, /*privacy=*/false);
    QCOMPARE(holdings, QStringList({QString("Balance: 1.00000000 Hg"),
                                    QString("Pending: 2.00000000 Hg"),
                                    QString("Immature: 3.00000000 Hg"),
                                    QString("Delegated: 4.00000000 Hg")}));
}

void VaultSummaryTests::thousandsSeparatorSurvivesTrimming()
{
    // The fixed-width padding these segments drop is itself threaded with thin-space
    // group separators, so a trim that reached past the padding would eat the
    // separator that belongs to the number.
    interfaces::VaultBalances balances;
    balances.balance = 1234 * COIN;

    const QStringList holdings = FormatHoldings(balances, HG, /*privacy=*/false);
    QCOMPARE(holdings.size(), 1);
    QCOMPARE(holdings.at(0), QStringLiteral("Balance: 1") + QChar(THIN_SP_CP) + QStringLiteral("234.00000000 Hg"));
}

void VaultSummaryTests::everyHoldingIsMaskedUnderPrivacy()
{
    // A segment that is added but not masked would leak the amount the privacy
    // toggle exists to hide, so assert on every one of them.
    interfaces::VaultBalances balances;
    balances.balance = COIN;
    balances.unconfirmed_balance = 2 * COIN;
    balances.immature_balance = 3 * COIN;
    balances.delegated_balance = 4 * COIN;

    const QStringList holdings = FormatHoldings(balances, HG, /*privacy=*/true);
    QCOMPARE(holdings.size(), 4);
    for (const QString& holding : holdings) {
        QVERIFY2(!holding.contains(QStringLiteral("00000000")),
                 qPrintable(QStringLiteral("unmasked amount in: %1").arg(holding)));
    }
}
