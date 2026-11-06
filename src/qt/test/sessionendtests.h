// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_TEST_SESSIONENDTESTS_H
#define QUICKSILVER_QT_TEST_SESSIONENDTESTS_H

#include <QObject>
#include <QTest>

//! F-591. See qt/sessionend.h.
class SessionEndTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void lostSessionManagerRequestsOneQuit();
    void repeatedLossStillQuitsOnce();
    void emptyQuitActionIsHarmless();
    void installedIceHandlerReplacesTheExitingDefault();
    void installedIceHandlerRequestsTheQuit();
};

#endif // QUICKSILVER_QT_TEST_SESSIONENDTESTS_H
