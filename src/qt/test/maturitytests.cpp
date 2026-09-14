// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/maturitytests.h>

#include <qt/maturity.h>

using qsmaturity::FormatMaturityCountdown;

void MaturityTests::nothingMaturing()
{
    QCOMPARE(FormatMaturityCountdown(0, 300), QString());
    QCOMPARE(FormatMaturityCountdown(-1, 300), QString());
}

void MaturityTests::roundsToMinutesUnderAnHour()
{
    // 6 blocks x 300 s = 1800 s = 30 minutes.
    QCOMPARE(FormatMaturityCountdown(6, 300), QString("spendable in about 30 minutes"));
    // 1 block: singular.
    QCOMPARE(FormatMaturityCountdown(1, 300), QString("spendable in about 5 minutes"));
}

void MaturityTests::wholeAndPartialHours()
{
    // 12 blocks x 300 s = 3600 s = 1 hour exactly.
    QCOMPARE(FormatMaturityCountdown(12, 300), QString("spendable in about 1 hour"));
    // 30 blocks x 300 s = 9000 s = 2.5 h, rounded to the nearest hour = 3.
    QCOMPARE(FormatMaturityCountdown(30, 300), QString("spendable in about 3 hours"));
}

void MaturityTests::fullMaturityAtShippedParameters()
{
    // COINBASE_MATURITY is 100 and nPowTargetSpacing is 300: 30000 s = 8.33 h.
    QCOMPARE(FormatMaturityCountdown(100, 300), QString("spendable in about 8 hours"));
}
