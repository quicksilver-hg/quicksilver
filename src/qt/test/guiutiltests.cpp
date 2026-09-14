// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/guiutiltests.h>

#include <qt/guiutil.h>

#include <QRegularExpression>
#include <QString>

//! Phase 6 S3. formatNiceTimeOffset() feeds the status-bar tooltip
//! (quicksilvergui.cpp) and the sync overlay's "time left" (modaloverlay.cpp), so
//! whatever it returns is read by every user on every start-up. It used to compose
//! that text with Qt's "%n unit(s)" numerus idiom, which only a translation
//! catalogue resolves -- and this application installs none of its own (see
//! initTranslations() in qt/quicksilver.cpp). The tooltip therefore read
//! "3 minute(s)" for everyone, in every locale.
void GUIUtilTests::timeOffsetReadsGrammaticallyWithNoCatalogue()
{
    // One value from every branch, plus both sides of each boundary.
    const qint64 samples[] = {0, 1, 45, 59, 60, 61, 3600, 7199, 7200, 10800,
                              172799, 172800, 259200, 1209599, 1209600, 1814400,
                              31556951, 31556952, 33371352, 63718704, 315569520};
    for (const qint64 secs : samples) {
        const QString text = GUIUtil::formatNiceTimeOffset(secs);
        QVERIFY2(!text.isEmpty(), qPrintable(QString::number(secs)));
        // The tell of an unresolved numerus form.
        QVERIFY2(!text.contains(QStringLiteral("(s)")), qPrintable(text));
        QVERIFY2(!text.contains(QStringLiteral("%n")), qPrintable(text));
        // And every placeholder must have been consumed: QString::arg leaves an
        // unmatched marker in place rather than failing.
        QVERIFY2(!text.contains(QRegularExpression(QStringLiteral("%[0-9]"))), qPrintable(text));
    }
}

//! A count of one is the case the numerus idiom existed to get right, so it is the
//! case worth pinning: "1 seconds" is as wrong as "1 second(s)".
//!
//! Only seconds, minutes and years can actually reach a count of one. The unit
//! thresholds are deliberately 2x -- the hour branch does not begin until 7200s --
//! so hours, days and the standalone week branch can never render fewer than two.
//! Their singular forms are kept for the day a threshold changes; the trailing week
//! of the year branch does reach one and is covered by timeOffsetPairsYearsWithWeeks.
void GUIUtilTests::timeOffsetSingularWhereReachable()
{
    QCOMPARE(GUIUtil::formatNiceTimeOffset(1), QStringLiteral("1 second"));
    QCOMPARE(GUIUtil::formatNiceTimeOffset(60), QStringLiteral("1 minute"));
    QCOMPARE(GUIUtil::formatNiceTimeOffset(119), QStringLiteral("1 minute"));
}

void GUIUtilTests::timeOffsetPluralAtEveryUnit()
{
    QCOMPARE(GUIUtil::formatNiceTimeOffset(0), QStringLiteral("0 seconds"));
    QCOMPARE(GUIUtil::formatNiceTimeOffset(45), QStringLiteral("45 seconds"));
    QCOMPARE(GUIUtil::formatNiceTimeOffset(600), QStringLiteral("10 minutes"));
    QCOMPARE(GUIUtil::formatNiceTimeOffset(10800), QStringLiteral("3 hours"));
    QCOMPARE(GUIUtil::formatNiceTimeOffset(259200), QStringLiteral("3 days"));
    QCOMPARE(GUIUtil::formatNiceTimeOffset(1814400), QStringLiteral("3 weeks"));
}

//! The year branch is the only optionally composed string, and the only place a
//! count of one reaches the week form.
void GUIUtilTests::timeOffsetPairsYearsWithWeeks()
{
    // Suppress a zero-valued suffix throughout the first partial week.
    QCOMPARE(GUIUtil::formatNiceTimeOffset(31556952), QStringLiteral("1 year"));
    QCOMPARE(GUIUtil::formatNiceTimeOffset(31556952 + 604799), QStringLiteral("1 year"));
    // 1 year + 3 weeks.
    QCOMPARE(GUIUtil::formatNiceTimeOffset(31556952 + 3 * 604800),
             QStringLiteral("1 year and 3 weeks"));
    QCOMPARE(GUIUtil::formatNiceTimeOffset(2 * 31556952), QStringLiteral("2 years"));
    // 2 years + 1 week: both halves inflect independently.
    QCOMPARE(GUIUtil::formatNiceTimeOffset(2 * 31556952 + 604800),
             QStringLiteral("2 years and 1 week"));
}

//! Each branch is picked by a strict `<`, so an off-by-one in a threshold silently
//! changes the unit rather than failing. Pin both sides of every boundary.
void GUIUtilTests::timeOffsetBoundariesPickTheIntendedUnit()
{
    QCOMPARE(GUIUtil::formatNiceTimeOffset(59), QStringLiteral("59 seconds"));
    QCOMPARE(GUIUtil::formatNiceTimeOffset(60), QStringLiteral("1 minute"));

    QCOMPARE(GUIUtil::formatNiceTimeOffset(7199), QStringLiteral("119 minutes"));
    QCOMPARE(GUIUtil::formatNiceTimeOffset(7200), QStringLiteral("2 hours"));

    QCOMPARE(GUIUtil::formatNiceTimeOffset(172799), QStringLiteral("47 hours"));
    QCOMPARE(GUIUtil::formatNiceTimeOffset(172800), QStringLiteral("2 days"));

    QCOMPARE(GUIUtil::formatNiceTimeOffset(1209599), QStringLiteral("13 days"));
    QCOMPARE(GUIUtil::formatNiceTimeOffset(1209600), QStringLiteral("2 weeks"));

    QCOMPARE(GUIUtil::formatNiceTimeOffset(31556951), QStringLiteral("52 weeks"));
    QCOMPARE(GUIUtil::formatNiceTimeOffset(31556952), QStringLiteral("1 year"));
}
