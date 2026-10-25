// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/quicksilverstyletests.h>

#include <qt/platformstyle.h>
#include <qt/quicksilverstyle.h>

#include <QApplication>
#include <QColor>
#include <QDialogButtonBox>
#include <QFile>
#include <QImage>
#include <QPalette>
#include <QProxyStyle>
#include <QPushButton>
#include <QStyleFactory>

#include <algorithm>
#include <cmath>
#include <memory>

void QuicksilverStyleTests::paletteTokens()
{
    QCOMPARE(QuicksilverStyle::Color(QuicksilverStyle::Token::Base), QColor(QStringLiteral("#101214")));
    QCOMPARE(QuicksilverStyle::Color(QuicksilverStyle::Token::Surface), QColor(QStringLiteral("#171b1f")));
    QCOMPARE(QuicksilverStyle::Color(QuicksilverStyle::Token::SurfaceRaised), QColor(QStringLiteral("#20262b")));
    QCOMPARE(QuicksilverStyle::Color(QuicksilverStyle::Token::Rail), QColor(QStringLiteral("#0b0d0f")));
    QCOMPARE(QuicksilverStyle::Color(QuicksilverStyle::Token::Vermilion), QColor(QStringLiteral("#d84b3e")));
    QCOMPARE(QuicksilverStyle::Color(QuicksilverStyle::Token::CinnabarBright), QColor(QStringLiteral("#ff7a6c")));
    QCOMPARE(QuicksilverStyle::Color(QuicksilverStyle::Token::CinnabarFill), QColor(QStringLiteral("#6b3431")));
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
    // The harness must start every class with the style the application ships.
    QCOMPARE(qApp->property("quicksilverBaseStyle").toString(), QStringLiteral("Fusion"));
    QuicksilverStyle::Apply(*qApp);

    QCOMPARE(qApp->property("quicksilverBaseStyle").toString(), QStringLiteral("Fusion"));
    QCOMPARE(qApp->palette().color(QPalette::Window), QColor(QStringLiteral("#101214")));
    QCOMPARE(qApp->palette().color(QPalette::WindowText), QColor(QStringLiteral("#ccd5dc")));
    QCOMPARE(qApp->palette().color(QPalette::Base), QColor(QStringLiteral("#171b1f")));
    QCOMPARE(qApp->palette().color(QPalette::Highlight), QColor(QStringLiteral("#6b3431")));
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

    const QString gui_path = QStringLiteral(QS_QT_SOURCE_DIR "/quicksilvergui.cpp");
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
QColor ButtonFill(const QString& button_class, bool enabled, const QString& object_name = QString())
{
    QPushButton button(QStringLiteral("Transfer"));
    if (!button_class.isEmpty()) button.setProperty("class", button_class);
    if (!object_name.isEmpty()) button.setObjectName(object_name);
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

namespace {
//! WCAG 2 contrast ratio between two opaque colours.
double Contrast(const QColor& a, const QColor& b)
{
    const auto luminance = [](const QColor& c) {
        const auto channel = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
        return 0.2126 * channel(c.redF()) + 0.7152 * channel(c.greenF()) + 0.0722 * channel(c.blueF());
    };
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}
} // namespace

void QuicksilverStyleTests::redCommandsAreFilledWithCinnabar()
{
    // F-477: a red command has the cinnabar fill of a selected row, not the
    // vermilion accent, which carries white text at only 3.9:1.
    QuicksilverStyle::Apply(*qApp);
    const QColor fill = QuicksilverStyle::Color(QuicksilverStyle::Token::CinnabarFill);
    QCOMPARE(ButtonFill(QStringLiteral("primaryActionButton"), true), fill);
    QCOMPARE(ButtonFill(QString(), true, QStringLiteral("sendButton")), fill);
    for (const char* name : {"consensusContinueButton", "startMiningButton", "agentAllotmentCreateButton"}) {
        QCOMPARE(ButtonFill(QString(), true, QString::fromLatin1(name)), fill);
    }
    const QColor text = QuicksilverStyle::Color(QuicksilverStyle::Token::SilverHi);
    QVERIFY(Contrast(text, fill) >= 4.5);
    QVERIFY(Contrast(text, QuicksilverStyle::Color(QuicksilverStyle::Token::Vermilion)) < 4.5);
}
