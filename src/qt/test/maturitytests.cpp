// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/maturitytests.h>

#include <qt/maturity.h>

#include <consensus/amount.h>

#include <utility>
#include <vector>

using qsmaturity::FormatMaturingHint;
using qsmaturity::FormatMaturityCountdown;
using qsmaturity::MaturingSummary;
using qsmaturity::SummarizeMaturing;

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

void MaturityTests::summaryOfNothingMaturing()
{
    const MaturingSummary none{SummarizeMaturing({})};
    QCOMPARE(none.soonest, 0);
    QCOMPARE(none.soonest_amount, CAmount{0});
    QCOMPARE(none.latest, 0);
    // A row that has already matured (0) is not maturing.
    const MaturingSummary matured{SummarizeMaturing({{0, 50 * COIN}})};
    QCOMPARE(matured.soonest, 0);
    QCOMPARE(matured.latest, 0);
    QCOMPARE(FormatMaturingHint(matured.soonest, QString("50.00 Hg"), matured.latest, 300), QString());
}

void MaturityTests::summarySumsEveryRowAtTheSoonestHeight()
{
    // Two rows free up at the soonest block: the amount is their sum, not either one.
    const MaturingSummary s{SummarizeMaturing({{40, 50 * COIN}, {3, 30 * COIN}, {3, 20 * COIN}, {7, 50 * COIN}})};
    QCOMPARE(s.soonest, 3);
    QCOMPARE(s.soonest_amount, 50 * COIN);
    QCOMPARE(s.latest, 40);
}

void MaturityTests::oneHeightKeepsTheSingleText()
{
    // Several coinbases, all one block away: the single text is true of the whole balance,
    // so it is unchanged and no amount is shown.
    const MaturingSummary s{SummarizeMaturing({{1, 50 * COIN}, {1, 49 * COIN}, {1, 48 * COIN}})};
    QCOMPARE(s.soonest, 1);
    QCOMPARE(s.latest, 1);
    QCOMPARE(FormatMaturingHint(s.soonest, QString("147.00 Hg"), s.latest, 300), QString("spendable in about 5 minutes"));
    // The first-run miner: one coinbase, 100 blocks away.
    const MaturingSummary first{SummarizeMaturing({{100, 50 * COIN}})};
    QCOMPARE(FormatMaturingHint(first.soonest, QString("50.00 Hg"), first.latest, 300), QString("spendable in about 8 hours"));
}

void MaturityTests::reportedCaseShowsNextAndAll()
{
    // The owner's overview: ~100 coinbases, one block apart, the soonest one block away.
    std::vector<std::pair<int, CAmount>> rows;
    for (int blocks = 1; blocks <= 99; ++blocks) rows.emplace_back(blocks, 50 * COIN);
    const MaturingSummary s{SummarizeMaturing(rows)};
    QCOMPARE(s.soonest, 1);
    QCOMPARE(s.soonest_amount, 50 * COIN);
    QCOMPARE(s.latest, 99);
    // 99 x 300 s = 29700 s = 8.25 h, rounded to the nearest hour = 8.
    QCOMPARE(FormatMaturingHint(s.soonest, QString("50.00000000 Hg"), s.latest, 300),
             QString::fromUtf8("next 50.00000000 Hg in about 5 minutes \u00b7 all in about 8 hours"));
}

void MaturityTests::singularAtBothEnds()
{
    // 1 block x 60 s = 1 minute; 60 blocks x 60 s = 1 hour.
    QCOMPARE(FormatMaturingHint(1, QString("50.00 Hg"), 60, 60),
             QString::fromUtf8("next 50.00 Hg in about 1 minute \u00b7 all in about 1 hour"));
    // 6 blocks x 300 s = 30 minutes; 12 blocks x 300 s = 1 hour.
    QCOMPARE(FormatMaturingHint(6, QString("50.00 Hg"), 12, 300),
             QString::fromUtf8("next 50.00 Hg in about 30 minutes \u00b7 all in about 1 hour"));
    // Both under an hour, and both over one.
    QCOMPARE(FormatMaturingHint(2, QString("50.00 Hg"), 6, 300),
             QString::fromUtf8("next 50.00 Hg in about 10 minutes \u00b7 all in about 30 minutes"));
    QCOMPARE(FormatMaturingHint(30, QString("50.00 Hg"), 100, 300),
             QString::fromUtf8("next 50.00 Hg in about 3 hours \u00b7 all in about 8 hours"));
}

void MaturityTests::amountStringPassesThroughUnchanged()
{
    // Privacy masking is the caller's job: whatever amount string comes in goes out verbatim.
    const QString masked{QString::fromUtf8("#.########### Hg")};
    QCOMPARE(FormatMaturingHint(1, masked, 99, 300),
             QString::fromUtf8("next #.########### Hg in about 5 minutes \u00b7 all in about 8 hours"));
}

void MaturityTests::reportedAmountUsesInlineFormatting()
{
    const MaturingSummary summary{1, 4'971'254'564, 99};
    QCOMPARE(FormatMaturingHint(summary, QuicksilverUnit::HG, QuicksilverUnits::SeparatorStyle::ALWAYS, false, 300),
             QString::fromUtf8("next 49.71254564 Hg in about 5 minutes \u00b7 all in about 8 hours"));
}

void MaturityTests::inlineThousandsKeepTheirSeparator()
{
    const MaturingSummary summary{1, 12'345 * COIN, 99};
    QCOMPARE(FormatMaturingHint(summary, QuicksilverUnit::HG, QuicksilverUnits::SeparatorStyle::ALWAYS, false, 300),
             QString::fromUtf8("next 12\u2009345.00000000 Hg in about 5 minutes \u00b7 all in about 8 hours"));
}

void MaturityTests::inlinePrivacyHasNoPadding()
{
    const MaturingSummary summary{1, 4'971'254'564, 99};
    QCOMPARE(FormatMaturingHint(summary, QuicksilverUnit::HG, QuicksilverUnits::SeparatorStyle::ALWAYS, true, 300),
             QString::fromUtf8("next #.######## Hg in about 5 minutes \u00b7 all in about 8 hours"));
    QCOMPARE(QuicksilverUnits::formatInlineWithPrivacy(QuicksilverUnit::HGS, summary.soonest_amount,
                                                     QuicksilverUnits::SeparatorStyle::ALWAYS, true),
             QString("# HgS"));
}
