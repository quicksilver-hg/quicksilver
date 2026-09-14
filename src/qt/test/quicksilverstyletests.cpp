// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/quicksilverstyletests.h>

#include <qt/platformstyle.h>
#include <qt/quicksilverstyle.h>

#include <QApplication>
#include <QColor>
#include <QPalette>

#include <memory>

void QuicksilverStyleTests::paletteTokens()
{
    QCOMPARE(QuicksilverStyle::Color(QuicksilverStyle::Token::Base), QColor(QStringLiteral("#101214")));
    QCOMPARE(QuicksilverStyle::Color(QuicksilverStyle::Token::Surface), QColor(QStringLiteral("#171b1f")));
    QCOMPARE(QuicksilverStyle::Color(QuicksilverStyle::Token::SurfaceRaised), QColor(QStringLiteral("#20262b")));
    QCOMPARE(QuicksilverStyle::Color(QuicksilverStyle::Token::Rail), QColor(QStringLiteral("#0b0d0f")));
    QCOMPARE(QuicksilverStyle::Color(QuicksilverStyle::Token::Cinnabar), QColor(QStringLiteral("#d84b3e")));
    QCOMPARE(QuicksilverStyle::Color(QuicksilverStyle::Token::CinnabarBright), QColor(QStringLiteral("#ff7a6c")));
    QCOMPARE(QuicksilverStyle::Color(QuicksilverStyle::Token::CinnabarDim), QColor(QStringLiteral("#6b3431")));
    QCOMPARE(QuicksilverStyle::Color(QuicksilverStyle::Token::CinnabarTrace), QColor(QStringLiteral("#2d2222")));
    QCOMPARE(QuicksilverStyle::Color(QuicksilverStyle::Token::Silver), QColor(QStringLiteral("#ccd5dc")));
    QCOMPARE(QuicksilverStyle::Color(QuicksilverStyle::Token::SilverHi), QColor(QStringLiteral("#f5f7f9")));
    QCOMPARE(QuicksilverStyle::Color(QuicksilverStyle::Token::SilverMuted), QColor(QStringLiteral("#8e9aa3")));
    QCOMPARE(QuicksilverStyle::Color(QuicksilverStyle::Token::Teal), QColor(QStringLiteral("#9bd7c8")));
    QCOMPARE(QuicksilverStyle::Color(QuicksilverStyle::Token::Amber), QColor(QStringLiteral("#f1cf7a")));
    QCOMPARE(QuicksilverStyle::Color(QuicksilverStyle::Token::Violet), QColor(QStringLiteral("#c3ace8")));
}

void QuicksilverStyleTests::applySetsGlobalPaletteAndStylesheet()
{
    QuicksilverStyle::Apply(*qApp);

    QCOMPARE(qApp->property("quicksilverBaseStyle").toString(), QStringLiteral("Fusion"));
    QCOMPARE(qApp->palette().color(QPalette::Window), QColor(QStringLiteral("#101214")));
    QCOMPARE(qApp->palette().color(QPalette::WindowText), QColor(QStringLiteral("#ccd5dc")));
    QCOMPARE(qApp->palette().color(QPalette::Base), QColor(QStringLiteral("#171b1f")));
    QCOMPARE(qApp->palette().color(QPalette::Highlight), QColor(QStringLiteral("#d84b3e")));
    QVERIFY(qApp->styleSheet().contains(QStringLiteral("QToolBar#primaryCommandRail")));
    QVERIFY(qApp->styleSheet().contains(QStringLiteral("QFrame#receiveRequestPanel")));
    QVERIFY(qApp->styleSheet().contains(QStringLiteral("QWidget#SendCoinsEntry")));
    QVERIFY(qApp->styleSheet().contains(QStringLiteral("QTabWidget::pane")));
    QVERIFY(qApp->styleSheet().contains(QStringLiteral("QTextEdit")));
    QVERIFY(qApp->styleSheet().contains(QStringLiteral("QAbstractItemView")));
    QVERIFY(qApp->styleSheet().contains(QStringLiteral("#ff7a6c")));

    const std::unique_ptr<const PlatformStyle> windows_style{PlatformStyle::instantiate(QStringLiteral("windows"))};
    QVERIFY(windows_style);
    QCOMPARE(windows_style->SingleColor(), QColor(QStringLiteral("#f5f7f9")));
}
