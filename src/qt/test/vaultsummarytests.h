// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_TEST_VAULTSUMMARYTESTS_H
#define QUICKSILVER_QT_TEST_VAULTSUMMARYTESTS_H

#include <QObject>
#include <QTest>

class VaultSummaryTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void spendableOnlyReadsAsOneSegment();
    void freshlyPaidVaultIsNotReportedEmpty();
    void immatureAndDelegatedAppearOnlyWhenHeld();
    void thousandsSeparatorSurvivesTrimming();
    void everyHoldingIsMaskedUnderPrivacy();
};

#endif // QUICKSILVER_QT_TEST_VAULTSUMMARYTESTS_H
