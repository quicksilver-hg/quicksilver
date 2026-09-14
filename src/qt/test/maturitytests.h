// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_TEST_MATURITYTESTS_H
#define QUICKSILVER_QT_TEST_MATURITYTESTS_H

#include <QObject>
#include <QTest>

class MaturityTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void nothingMaturing();
    void wholeAndPartialHours();
    void roundsToMinutesUnderAnHour();
    void fullMaturityAtShippedParameters();
};

#endif // QUICKSILVER_QT_TEST_MATURITYTESTS_H
