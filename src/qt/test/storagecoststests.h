// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_TEST_STORAGECOSTSTESTS_H
#define QUICKSILVER_QT_TEST_STORAGECOSTSTESTS_H

#include <QObject>
#include <QTest>

/** Tests for the storage figures the desktop quotes before a user accepts consensus. */
class StorageCostsTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void launchPageQuotesTheDerivedArchivalTotal();
    void consensusReviewQuotesTheDerivedArchivalTotal();
    void bothScreensAgreeWithIntroOnTheArchivalTotal();
};

#endif // QUICKSILVER_QT_TEST_STORAGECOSTSTESTS_H
