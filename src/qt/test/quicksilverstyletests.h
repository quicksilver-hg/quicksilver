// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_TEST_QUICKSILVERSTYLETESTS_H
#define QUICKSILVER_QT_TEST_QUICKSILVERSTYLETESTS_H

#include <QObject>
#include <QTest>

class QuicksilverStyleTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void paletteTokens();
    void applySetsGlobalPaletteAndStylesheet();
};

#endif // QUICKSILVER_QT_TEST_QUICKSILVERSTYLETESTS_H
