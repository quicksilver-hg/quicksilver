// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_TEST_GUIUTILTESTS_H
#define QUICKSILVER_QT_TEST_GUIUTILTESTS_H

#include <QObject>
#include <QTest>

class GUIUtilTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void timeOffsetReadsGrammaticallyWithNoCatalogue();
    void timeOffsetSingularWhereReachable();
    void timeOffsetPluralAtEveryUnit();
    void timeOffsetPairsYearsWithWeeks();
    void timeOffsetBoundariesPickTheIntendedUnit();
};

#endif // QUICKSILVER_QT_TEST_GUIUTILTESTS_H
