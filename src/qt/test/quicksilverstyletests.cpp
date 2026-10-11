// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/quicksilverstyletests.h>

#include <qt/platformstyle.h>
#include <qt/quicksilverstyle.h>

#include <QApplication>
#include <QColor>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPalette>
#include <QProxyStyle>
#include <QPushButton>
#include <QStyleFactory>

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
    // Page sections are Bench panels; a section a form lays out sits flat
    // inside one.
    QVERIFY(qApp->styleSheet().contains(QStringLiteral("QFrame[benchPanel=\"true\"]")));
    QVERIFY(qApp->styleSheet().contains(QStringLiteral("QFrame[benchBody=\"true\"]")));
    QVERIFY(qApp->styleSheet().contains(QStringLiteral("QWidget#SendCoinsEntry")));
    QVERIFY(qApp->styleSheet().contains(QStringLiteral("QTabWidget::pane")));
    QVERIFY(qApp->styleSheet().contains(QStringLiteral("QTextEdit")));
    QVERIFY(qApp->styleSheet().contains(QStringLiteral("QAbstractItemView")));
    QVERIFY(qApp->styleSheet().contains(QStringLiteral("#ff7a6c")));

    const std::unique_ptr<const PlatformStyle> windows_style{PlatformStyle::instantiate(QStringLiteral("windows"))};
    QVERIFY(windows_style);
    QCOMPARE(windows_style->SingleColor(), QColor(QStringLiteral("#f5f7f9")));
}

void QuicksilverStyleTests::benchPaletteTokens()
{
    // Concept colours for roles the existing palette does not name. Nearby
    // existing tokens stay as they are; these are the ones the sheet is built from.
    QCOMPARE(QuicksilverStyle::Color(QuicksilverStyle::Token::BenchSurface), QColor(QStringLiteral("#15181c")));
    QCOMPARE(QuicksilverStyle::Color(QuicksilverStyle::Token::BenchSurfaceRaised), QColor(QStringLiteral("#191d22")));
    QCOMPARE(QuicksilverStyle::Color(QuicksilverStyle::Token::BenchTop), QColor(QStringLiteral("#14171b")));
    QCOMPARE(QuicksilverStyle::Color(QuicksilverStyle::Token::Hairline), QColor(QStringLiteral("#252a30")));
    QCOMPARE(QuicksilverStyle::Color(QuicksilverStyle::Token::StatusGood), QColor(QStringLiteral("#6cc4a1")));
    QCOMPARE(QuicksilverStyle::Color(QuicksilverStyle::Token::TickerText), QColor(QStringLiteral("#c9d0d6")));
}

void QuicksilverStyleTests::benchSheetRules()
{
    QuicksilverStyle::Apply(*qApp);
    const QString sheet = qApp->styleSheet();
    QVERIFY(sheet.contains(QStringLiteral("QToolBar#primaryCommandRail")));
    const int rail_buttons = sheet.indexOf(QStringLiteral("QToolBar#primaryCommandRail QToolButton {"));
    QVERIFY(rail_buttons >= 0);
    const QString rail_rule = sheet.mid(rail_buttons, 400);
    QVERIFY(rail_rule.contains(QStringLiteral("border-left: 2px solid")));
    QVERIFY(rail_rule.contains(QStringLiteral("padding-left: 16px")));
    QVERIFY(rail_rule.contains(QStringLiteral("min-height: 38px")));
    QVERIFY(sheet.contains(QStringLiteral("QFrame#benchTopBar")));
    QVERIFY(sheet.contains(QStringLiteral("QFrame#benchPanel")));
    QVERIFY(sheet.contains(QStringLiteral("QFrame#benchStatusStrip")));
    QVERIFY(!sheet.contains(QStringLiteral("#FF8000"), Qt::CaseInsensitive));

    // __FILE__ is remapped relative to src/, and ctest's working directory is the
    // build tree, so the sibling path is not always openable as written.
    QString gui_path = QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath(QStringLiteral("../quicksilvergui.cpp"));
    if (!QFileInfo::exists(gui_path)) {
        QDir dir(QDir::current());
        for (int i = 0; i < 8 && !QFileInfo::exists(gui_path); ++i) {
            const QString beside = dir.filePath(QStringLiteral("qt/quicksilvergui.cpp"));
            const QString under_src = dir.filePath(QStringLiteral("src/qt/quicksilvergui.cpp"));
            if (QFileInfo::exists(beside)) gui_path = beside;
            else if (QFileInfo::exists(under_src)) gui_path = under_src;
            else if (!dir.cdUp()) break;
        }
    }
    QFile gui_source(gui_path);
    QVERIFY2(gui_source.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(gui_path));
    const QString source = QString::fromUtf8(gui_source.readAll());
    QVERIFY(!source.contains(QStringLiteral("#FF8000"), Qt::CaseInsensitive));
}

namespace {
//! Answers the way the GNOME platform theme does under GNOME and Cinnamon.
class StockIconStyle : public QProxyStyle
{
public:
    StockIconStyle() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}
    int styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget, QStyleHintReturn* data) const override
    {
        if (hint == SH_DialogButtonBox_ButtonsHaveIcons) return 1;
        return QProxyStyle::styleHint(hint, option, widget, data);
    }
};

QIcon OkIcon(QStyle* style)
{
    QDialogButtonBox box;
    box.setStyle(style);
    box.setStandardButtons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    return box.button(QDialogButtonBox::Ok)->icon();
}
} // namespace

void QuicksilverStyleTests::dialogButtonsCarryNoStockIcon()
{
    // A desktop theme can ask for a stock icon on every dialog button; the
    // offscreen platform never does, so the theme is stood in for here.
    std::unique_ptr<QStyle> themed{new StockIconStyle};
    QVERIFY(!OkIcon(themed.get()).isNull());

    std::unique_ptr<QStyle> bench{QuicksilverStyle::WithoutDialogButtonIcons(new StockIconStyle)};
    QCOMPARE(bench->styleHint(QStyle::SH_DialogButtonBox_ButtonsHaveIcons), 0);
    QVERIFY(OkIcon(bench.get()).isNull());

    // The application style is the wrapped one, and keeps the base style's
    // name so it can be recreated by name. qApp->style() answers with the
    // stylesheet's wrapper, which owns the installed style when a sheet is
    // already set, so the installed style is found among the application's
    // descendants.
    QuicksilverStyle::Apply(*qApp);
    const QStyle* installed = qApp->findChild<QProxyStyle*>();
    QVERIFY(installed);
    QCOMPARE(installed->objectName(), QStringLiteral("fusion"));
    QCOMPARE(installed->styleHint(QStyle::SH_DialogButtonBox_ButtonsHaveIcons), 0);
}

namespace {
//! The fill a button paints just inside its left border, clear of its text.
QColor ButtonFill(const QString& button_class, bool enabled)
{
    QPushButton button(QStringLiteral("Transfer"));
    if (!button_class.isEmpty()) button.setProperty("class", button_class);
    button.setEnabled(enabled);
    button.resize(120, 30);
    button.ensurePolished();
    const QImage image = button.grab().toImage();
    return image.pixelColor(4, image.height() / 2);
}
} // namespace

void QuicksilverStyleTests::disabledPrimaryButtonLooksDisabled()
{
    QuicksilverStyle::Apply(*qApp);
    const QString primary = QStringLiteral("primaryActionButton");
    QVERIFY(ButtonFill(primary, true) != ButtonFill(QString(), true));
    QCOMPARE(ButtonFill(primary, false), ButtonFill(QString(), false));
}
