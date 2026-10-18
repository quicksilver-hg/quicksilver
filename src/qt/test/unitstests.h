// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_TEST_UNITSTESTS_H
#define QUICKSILVER_QT_TEST_UNITSTESTS_H

#include <QObject>
#include <QTest>

class UnitsTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void formatGroupsThousandsWithCommas();
    void formatNeverStaysSeparatorFree();
    void parseAcceptsWholeThousandsGroupsAndSpaces();
    void amountFieldKeepsAnIncompleteGroup();
    void commaKeyTypesAComma();
    void formatRoundTripsThroughParse();
    void htmlAmountKeepsNowrapWithoutAThinSpace();
    void displaySuffixFollowsTheValueWithNoSpace();
    void htmlColoursOnlyTheCinnabarSuffix();
    void namesStayTheCodes();
    void cinnabarGlyphUsesTheBundledFace();
    void unitGlyphCmapHasWindowsBmpEncoding();
    void failedFontLoadsChangeNothing();
};

#endif // QUICKSILVER_QT_TEST_UNITSTESTS_H
