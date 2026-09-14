// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_TEST_INTROTESTS_H
#define QUICKSILVER_QT_TEST_INTROTESTS_H

#include <QObject>
#include <QTest>

/** Tests for the first screen a new desktop user ever sees. */
class IntroTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void explanationLeavesNoPlaceholderAndNoInheritedYear();
    void explanationDoesNotPromiseADownloadOfHistoryThatMayNotExist();
    void pruneSuffixReadsGrammaticallyWithNoCatalogue();
    void freeSpaceLabelReadsGrammaticallyWithNoCatalogue();
    void datadirExistenceIsNotTheFirstRunQuestion();
    void chainDataIsFoundUnderTheNetworkSubdirectoryOnly();
    void pruneChoiceIsWithheldFromADataDirThatAlreadyHoldsAChain();
};

#endif // QUICKSILVER_QT_TEST_INTROTESTS_H
