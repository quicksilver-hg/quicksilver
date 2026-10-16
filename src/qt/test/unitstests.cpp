// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/unitstests.h>

#include <consensus/amount.h>
#include <qt/guiutil.h>
#include <qt/quicksilveramountfield.h>
#include <qt/quicksilvergui.h>
#include <qt/quicksilverstyle.h>
#include <qt/quicksilverunits.h>
#include <qt/platformstyle.h>
#include <qt/test/util.h>

#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QChar>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QRawFont>
#include <QRegularExpression>
#include <QString>
#include <QTextLayout>
#include <QValidator>

namespace {
using Unit = QuicksilverUnit;
using Style = QuicksilverUnits::SeparatorStyle;

QString Grouped(Unit unit, CAmount amount, bool plus = false)
{
    return QuicksilverUnits::format(unit, amount, plus, Style::ALWAYS);
}

bool Parsed(Unit unit, const QString& text, CAmount& out)
{
    return QuicksilverUnits::parse(unit, text, &out);
}

QValidator::State FieldState(QAbstractSpinBox* spin, const QString& typed)
{
    QString text = typed;
    int pos = text.size();
    return spin->validate(text, pos);
}

quint16 ReadBe16(const QByteArray& bytes, int offset)
{
    const unsigned char* p = reinterpret_cast<const unsigned char*>(bytes.constData() + offset);
    return static_cast<quint16>((p[0] << 8) | p[1]);
}

quint32 ReadBe32(const QByteArray& bytes, int offset)
{
    const unsigned char* p = reinterpret_cast<const unsigned char*>(bytes.constData() + offset);
    return (quint32{p[0]} << 24) | (quint32{p[1]} << 16) | (quint32{p[2]} << 8) | quint32{p[3]};
}
} // namespace

void UnitsTests::formatGroupsThousandsWithCommas()
{
    // Four digits group. The old SI threshold left 1000 ungrouped.
    QCOMPARE(Grouped(Unit::HG, 1000 * COIN), QStringLiteral("1,000.00000000"));
    QCOMPARE(Grouped(Unit::HG, 1234 * COIN), QStringLiteral("1,234.00000000"));
    QCOMPARE(Grouped(Unit::HG, 999 * COIN), QStringLiteral("999.00000000"));
    QCOMPARE(Grouped(Unit::HG, 12345 * COIN), QStringLiteral("12,345.00000000"));
    QCOMPARE(Grouped(Unit::HG, 1000000 * COIN), QStringLiteral("1,000,000.00000000"));
    QCOMPARE(Grouped(Unit::HG, MAX_MONEY), QStringLiteral("1,000,000,000.00000000"));
    QCOMPARE(Grouped(Unit::HGS, 1000), QStringLiteral("1,000"));
    QCOMPARE(Grouped(Unit::HGS, 1234), QStringLiteral("1,234"));
    QCOMPARE(Grouped(Unit::HGS, 1000000), QStringLiteral("1,000,000"));
    QCOMPARE(Grouped(Unit::HGS, MAX_MONEY), QStringLiteral("100,000,000,000,000,000"));
    QCOMPARE(Grouped(Unit::HG, 0), QStringLiteral("0.00000000"));
    QCOMPARE(Grouped(Unit::HGS, 0), QStringLiteral("0"));
    QCOMPARE(Grouped(Unit::HG, 1), QStringLiteral("0.00000001"));
    QCOMPARE(Grouped(Unit::HGS, 1), QStringLiteral("1"));
    QCOMPARE(Grouped(Unit::HG, 999), QStringLiteral("0.00000999"));

    // Justify pads with spaces. Trimming must not leave a comma from the padding.
    const QString justified = QuicksilverUnits::format(Unit::HG, 1234 * COIN, false, Style::ALWAYS, true).trimmed();
    QCOMPARE(justified, QStringLiteral("1,234.00000000"));
    const QString masked = QuicksilverUnits::formatWithPrivacy(Unit::HG, 1234 * COIN, Style::ALWAYS, true);
    QVERIFY(!masked.contains(QRegularExpression(QStringLiteral("[0-9]"))));
    QVERIFY(masked.contains(QLatin1Char('#')));
    QVERIFY(!masked.contains(QLatin1Char(',')));
    QCOMPARE(masked.trimmed(), QStringLiteral("#.########Hg"));
}

void UnitsTests::formatNeverStaysSeparatorFree()
{
    QCOMPARE(QuicksilverUnits::format(Unit::HG, 12345 * COIN, false, Style::NEVER), QStringLiteral("12345.00000000"));
    QCOMPARE(QuicksilverUnits::format(Unit::HGS, 12345, false, Style::NEVER), QStringLiteral("12345"));
    QCOMPARE(QuicksilverUnits::format(Unit::HG, MAX_MONEY, false, Style::NEVER), QStringLiteral("1000000000.00000000"));
    QVERIFY(!QuicksilverUnits::format(Unit::HGS, MAX_MONEY, false, Style::NEVER).contains(QLatin1Char(',')));
}

void UnitsTests::parseAcceptsWholeThousandsGroupsAndSpaces()
{
    const struct {
        const char* text;
        CAmount expected;
    } accepted[]{
        {"40,000", 40000 * COIN},
        {"40,000.5", 40000 * COIN + COIN / 2},
        {"1,234,567.5", 1234567 * COIN + COIN / 2},
        {"999", 999 * COIN},
        {"1, 000", 1000 * COIN},
        {" 1 000 ", 1000 * COIN},
    };
    for (const auto& sample : accepted) {
        CAmount parsed{0};
        QVERIFY2(Parsed(Unit::HG, QString::fromLatin1(sample.text), parsed), sample.text);
        QCOMPARE(parsed, sample.expected);
    }

    CAmount thin_parsed{0};
    const QString thin = QStringLiteral("1") + QChar(0x2009) + QStringLiteral("000");
    QVERIFY(Parsed(Unit::HG, thin, thin_parsed));
    QCOMPARE(thin_parsed, 1000 * COIN);

    CAmount nbsp_parsed{0};
    const QString nbsp = QStringLiteral("1") + QChar(0x00A0) + QStringLiteral("000");
    QVERIFY(Parsed(Unit::HG, nbsp, nbsp_parsed));
    QCOMPARE(nbsp_parsed, 1000 * COIN);

    // `1,2,3` is not a thousands group. It used to parse as 123 because every
    // comma was ignored. A comma is accepted only in whole groups of three.
    const char* refused[]{
        "1,5",
        "1,2,3",
        "1,23",
        "1234,567",
        "12,34,567",
        ",000",
        "40,000,",
        "1,,000",
        ",",
        "40.000,5",
        "1.2.3",
        "1.000000000",
        "10000000000.0",
    };
    for (const char* text : refused) {
        CAmount rejected{0};
        QVERIFY2(!Parsed(Unit::HG, QString::fromLatin1(text), rejected), text);
    }
    CAmount rejected{0};
    QVERIFY(!Parsed(Unit::HG, QString(), rejected));
}

void UnitsTests::amountFieldKeepsAnIncompleteGroup()
{
    QuicksilverAmountField field;
    field.setDisplayUnit(Unit::HG);
    QAbstractSpinBox* spin = field.findChild<QAbstractSpinBox*>();
    QVERIFY(spin != nullptr);
    QLineEdit* edit = spin->findChild<QLineEdit*>();
    QVERIFY(edit != nullptr);

    QCOMPARE(static_cast<int>(FieldState(spin, QStringLiteral("40,0"))), static_cast<int>(QValidator::Intermediate));
    QCOMPARE(static_cast<int>(FieldState(spin, QStringLiteral("40,00"))), static_cast<int>(QValidator::Intermediate));

    edit->setText(QStringLiteral("40,00"));
    bool valid = true;
    field.value(&valid);
    QVERIFY(!valid);

    edit->setText(QStringLiteral("1,5"));
    QString typed = QStringLiteral("1,5");
    spin->fixup(typed);
    QCOMPARE(typed, QStringLiteral("1,5"));
    QCOMPARE(edit->text(), QStringLiteral("1,5"));

    QCOMPARE(static_cast<int>(FieldState(spin, QStringLiteral("abc"))), static_cast<int>(QValidator::Invalid));
}

void UnitsTests::commaKeyTypesAComma()
{
    // The field's focus proxy is the spin box, and that box receives the key.
    QuicksilverAmountField thousands;
    thousands.setDisplayUnit(Unit::HG);
    QAbstractSpinBox* thousands_box = qobject_cast<QAbstractSpinBox*>(thousands.focusProxy());
    QVERIFY(thousands_box != nullptr);
    QTest::keyClicks(thousands_box, QStringLiteral("40"));
    QTest::keyClick(thousands_box, Qt::Key_Comma);
    QTest::keyClicks(thousands_box, QStringLiteral("000"));
    const QString thousands_text = thousands_box->text();

    QuicksilverAmountField malformed;
    malformed.setDisplayUnit(Unit::HG);
    QAbstractSpinBox* malformed_box = qobject_cast<QAbstractSpinBox*>(malformed.focusProxy());
    QVERIFY(malformed_box != nullptr);
    QTest::keyClicks(malformed_box, QStringLiteral("1"));
    QTest::keyClick(malformed_box, Qt::Key_Comma);
    QTest::keyClicks(malformed_box, QStringLiteral("5"));
    const QString malformed_text = malformed_box->text();

    QuicksilverAmountField decimal;
    decimal.setDisplayUnit(Unit::HG);
    QAbstractSpinBox* decimal_box = qobject_cast<QAbstractSpinBox*>(decimal.focusProxy());
    QVERIFY(decimal_box != nullptr);
    QTest::keyClicks(decimal_box, QStringLiteral("1.5"));
    const QString decimal_text = decimal_box->text();

    QVERIFY2(thousands_text.contains(QStringLiteral("40,000")), qPrintable(thousands_text));
    bool thousands_valid = false;
    QCOMPARE(thousands.value(&thousands_valid), 40000 * COIN);
    QVERIFY(thousands_valid);

    QCOMPARE(malformed_text, QStringLiteral("1,5"));
    bool malformed_valid = true;
    malformed.value(&malformed_valid);
    QVERIFY(!malformed_valid);
    QVERIFY(!malformed.validate());

    QCOMPARE(decimal_text, QStringLiteral("1.5"));
    bool decimal_valid = false;
    QCOMPARE(decimal.value(&decimal_valid), COIN + COIN / 2);
    QVERIFY(decimal_valid);
}

void UnitsTests::formatRoundTripsThroughParse()
{
    const CAmount values[]{0, 1, 999, 1000, 1234, 1000000, 1000 * COIN, 1234 * COIN, 1000000 * COIN, MAX_MONEY};
    // STANDARD was removed: grouping from four digits made it identical to ALWAYS.
    const Style styles[]{Style::NEVER, Style::ALWAYS};
    const Unit units[]{Unit::HG, Unit::HGS};
    for (const Unit unit : units) {
        for (const Style style : styles) {
            for (const CAmount amount : values) {
                const QString text = QuicksilverUnits::format(unit, amount, false, style);
                CAmount parsed{0};
                QVERIFY2(QuicksilverUnits::parse(unit, text, &parsed), qPrintable(text));
                QCOMPARE(parsed, amount);
            }
        }
    }
}

void UnitsTests::htmlAmountKeepsNowrapWithoutAThinSpace()
{
    const QString html = QuicksilverUnits::formatHtmlWithUnit(Unit::HG, 12345 * COIN);
    QVERIFY(html.contains(QStringLiteral("12,345.00000000")));
    QVERIFY(html.contains(QStringLiteral("white-space: nowrap")));
    QVERIFY(!html.contains(QChar(0x2009)));
    QVERIFY(!html.contains(QStringLiteral("font-size: 6pt")));
}

QString CinnabarSign()
{
    const char32_t sign = 0x1F713;
    return QString::fromUcs4(&sign, 1);
}

void UnitsTests::displaySuffixFollowsTheValueWithNoSpace()
{
    const CAmount amount = 401238 * COIN + 63847287;
    QCOMPARE(QuicksilverUnits::formatWithUnit(Unit::HG, amount), QStringLiteral("401,238.63847287Hg"));
    QCOMPARE(QuicksilverUnits::formatWithUnit(Unit::HGS, amount), QStringLiteral("40,123,863,847,287") + CinnabarSign());
    QCOMPARE(QuicksilverUnits::formatWithUnit(Unit::HG, -amount), QStringLiteral("-401,238.63847287Hg"));
    QCOMPARE(QuicksilverUnits::formatWithUnit(Unit::HGS, amount, true), QStringLiteral("+40,123,863,847,287") + CinnabarSign());

    const QString masked = QuicksilverUnits::formatWithPrivacy(Unit::HGS, amount, Style::ALWAYS, true);
    QVERIFY(masked.endsWith(CinnabarSign()));
    QVERIFY(masked.at(masked.size() - 1 - CinnabarSign().size()) != QLatin1Char(' '));
    QCOMPARE(masked.trimmed(), QStringLiteral("#") + CinnabarSign());
}

void UnitsTests::htmlColoursOnlyTheCinnabarSuffix()
{
    const CAmount amount = 401238 * COIN + 63847287;
    const QString html = QuicksilverUnits::formatHtmlWithUnit(Unit::HGS, amount);
    const QString color = QuicksilverStyle::Color(QuicksilverStyle::Token::Vermilion).name();
    const QString number = QStringLiteral("40,123,863,847,287");
    QVERIFY(html.contains(QStringLiteral("white-space: nowrap")));
    QVERIFY(html.contains(number));
    QVERIFY(html.contains(CinnabarSign()));
    QVERIFY(html.contains(color));
    const int number_at = html.indexOf(number);
    const int color_at = html.indexOf(color);
    QVERIFY(number_at >= 0);
    QVERIFY(color_at > number_at);
    // QColor::name() is the hex, which sits after the `color:` keyword.
    // The number is uncoloured when that keyword itself follows the number.
    QVERIFY(!html.left(number_at).contains(QStringLiteral("color:")));
    QVERIFY(html.mid(html.indexOf(QStringLiteral("color:"))).contains(CinnabarSign()));
    const QString quicksilver = QuicksilverUnits::formatHtmlWithUnit(Unit::HG, amount);
    QVERIFY(quicksilver.contains(QStringLiteral("401,238.63847287Hg")));
    QVERIFY(!quicksilver.contains(CinnabarSign()));
}

void UnitsTests::namesStayTheCodes()
{
    QCOMPARE(QuicksilverUnits::shortName(Unit::HG), QStringLiteral("Hg"));
    QCOMPARE(QuicksilverUnits::shortName(Unit::HGS), QStringLiteral("HgS"));
    QCOMPARE(QuicksilverUnits::longName(Unit::HG), QStringLiteral("Hg"));
    QCOMPARE(QuicksilverUnits::longName(Unit::HGS), QStringLiteral("HgS"));
    QCOMPARE(QuicksilverUnits::choiceName(Unit::HG), QStringLiteral("Hg"));
    QCOMPARE(QuicksilverUnits::choiceName(Unit::HGS), QString::fromUtf8("HgS (\xF0\x9F\x9C\x93)"));
    QCOMPARE(QuicksilverUnits::getAmountColumnTitle(Unit::HGS), QStringLiteral("Amount (HgS)"));
    QuicksilverUnits units{nullptr};
    const QModelIndex quicksilver = units.index(0);
    const QModelIndex cinnabar = units.index(1);
    QCOMPARE(units.data(quicksilver, Qt::DisplayRole).toString(), QStringLiteral("Hg"));
    QCOMPARE(units.data(quicksilver, Qt::EditRole).toString(), QStringLiteral("Hg"));
    QCOMPARE(units.data(cinnabar, Qt::DisplayRole).toString(), QString::fromUtf8("HgS (\xF0\x9F\x9C\x93)"));
    QCOMPARE(units.data(cinnabar, Qt::EditRole).toString(), QString::fromUtf8("HgS (\xF0\x9F\x9C\x93)"));
    QCOMPARE(units.data(cinnabar, Qt::ToolTipRole).toString(), QStringLiteral("cinnabar (0.00000001 Quicksilver)"));
    QVERIFY(!units.data(cinnabar, Qt::ToolTipRole).toString().contains(CinnabarSign()));

    const QFont previous = QApplication::font();
    GUIUtil::LoadUnitGlyph();
    QuicksilverAmountField field;
    field.setDisplayUnit(Unit::HGS);
    QComboBox* combo = field.findChild<QComboBox*>();
    QVERIFY(combo);
    QCOMPARE(combo->currentText(), QString::fromUtf8("HgS (\xF0\x9F\x9C\x93)"));
    QVERIFY(combo->sizeHint().width() >= QFontMetricsF(combo->font()).horizontalAdvance(QString::fromUtf8("HgS (\xF0\x9F\x9C\x93)")));

    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    UnitDisplayStatusBarControl status_control(platform_style.get());
    QMenu* menu = status_control.findChild<QMenu*>();
    QVERIFY(menu);
    QCOMPARE(menu->actions().size(), 2);
    QCOMPARE(menu->actions().at(0)->text(), QStringLiteral("Hg"));
    QCOMPARE(menu->actions().at(1)->text(), QString::fromUtf8("HgS (\xF0\x9F\x9C\x93)"));
    QVERIFY(QMetaObject::invokeMethod(&status_control, "updateDisplayUnit", Qt::DirectConnection,
                                      Q_ARG(QuicksilverUnit, Unit::HGS)));
    QCOMPARE(status_control.text(), QStringLiteral("HgS"));
    QApplication::setFont(previous);
}

void UnitsTests::cinnabarGlyphUsesTheBundledFace()
{
    const QFont previous = QApplication::font();
    GUIUtil::LoadUnitGlyph();
    const QString bundled = GUIUtil::UnitGlyphFamily();
    QCOMPARE(bundled, QStringLiteral("Quicksilver Unit Glyphs"));

    const QFontMetricsF metrics(QApplication::font());
    const QRectF ink = metrics.tightBoundingRect(CinnabarSign());
    const qreal advance = metrics.horizontalAdvance(CinnabarSign());
    qInfo() << "Cinnabar choice overlap:" << "left" << ink.left() << "right" << ink.right()
            << "advance" << advance << "space" << metrics.horizontalAdvance(QStringLiteral(" "));
    QVERIFY(ink.left() >= 0);
    QVERIFY(ink.right() <= advance);

    QLabel label(QStringLiteral("1") + CinnabarSign());
    label.setFont(QApplication::font());
    QTextLayout layout(label.text(), label.font());
    layout.beginLayout();
    QTextLine line = layout.createLine();
    line.setLineWidth(1000);
    layout.endLayout();
    const QList<QGlyphRun> runs = layout.glyphRuns(1, label.text().size() - 1);
    QVERIFY2(!runs.isEmpty(), "the cinnabar sign produced no glyph run");
    const QGlyphRun& run = runs.first();
    QCOMPARE(run.rawFont().familyName(), bundled);
    QVERIFY(!run.glyphIndexes().isEmpty());
    QVERIFY(run.glyphIndexes().first() != 0);
    QVERIFY(!run.rawFont().boundingRect(run.glyphIndexes().first()).isEmpty());
    const QList<QGlyphRun> digits = layout.glyphRuns(0, 1);
    QVERIFY(!digits.isEmpty());
    QVERIFY(digits.first().rawFont().familyName() != bundled);
    QApplication::setFont(previous);

    const QString dir = qEnvironmentVariable("QS_UNIT_DISPLAY_SCREENSHOTS");
    if (dir.isEmpty()) return;
    QVERIFY(QDir().mkpath(dir));

    const RestoreApplicationStyle restore;
    const QFont capture_previous = QApplication::font();
    QuicksilverStyle::Apply(*qApp);
    GUIUtil::LoadUnitGlyph();

    QuicksilverAmountField field;
    field.setDisplayUnit(Unit::HGS);
    field.resize(field.sizeHint());
    field.show();
    QVERIFY(QTest::qWaitForWindowExposed(&field));
    QCoreApplication::processEvents();
    QComboBox* combo = field.findChild<QComboBox*>();
    QVERIFY(combo);
    QVERIFY(combo->grab().save(QDir(dir).filePath(QStringLiteral("amount-combobox-closed.png"))));

    combo->showPopup();
    QCoreApplication::processEvents();
    QWidget* popup = combo->view()->window();
    QVERIFY(popup && popup->isVisible());
    QVERIFY(popup->grab().save(QDir(dir).filePath(QStringLiteral("amount-combobox-popup.png"))));
    combo->hidePopup();

    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    UnitDisplayStatusBarControl status_control(platform_style.get());
    status_control.show();
    QVERIFY(QTest::qWaitForWindowExposed(&status_control));
    QMenu* menu = status_control.findChild<QMenu*>();
    QVERIFY(menu);
    menu->popup(status_control.mapToGlobal(QPoint(0, 0)));
    QCoreApplication::processEvents();
    QVERIFY(menu->isVisible());
    QVERIFY(menu->grab().save(QDir(dir).filePath(QStringLiteral("status-bar-unit-menu.png"))));
    menu->hide();
    QApplication::setFont(capture_previous);
}

void UnitsTests::unitGlyphCmapHasWindowsBmpEncoding()
{
    // Windows' font loader refuses a face without a (3,1) cmap subtable (F-489).
    QFile face{QStringLiteral(":/fonts/unitglyph")};
    QVERIFY(face.open(QIODevice::ReadOnly));
    const QByteArray bytes = face.readAll();
    QVERIFY(bytes.size() >= 12);
    const int num_tables = ReadBe16(bytes, 4);
    QVERIFY(num_tables > 0);
    QVERIFY(bytes.size() >= 12 + num_tables * 16);

    bool saw_cmap{false};
    quint32 cmap_offset{0};
    quint32 cmap_length{0};
    for (int i = 0; i < num_tables; ++i) {
        const int record = 12 + i * 16;
        if (QByteArray::fromRawData(bytes.constData() + record, 4) != QByteArrayLiteral("cmap")) continue;
        saw_cmap = true;
        cmap_offset = ReadBe32(bytes, record + 8);
        cmap_length = ReadBe32(bytes, record + 12);
        break;
    }
    QVERIFY2(saw_cmap, "sfnt directory has no cmap table");
    const quint32 file_size = static_cast<quint32>(bytes.size());
    QVERIFY(cmap_offset <= file_size);
    QVERIFY(cmap_length >= 4);
    QVERIFY(cmap_length <= file_size - cmap_offset);

    const int cmap_at = static_cast<int>(cmap_offset);
    const quint16 num_encodings = ReadBe16(bytes, cmap_at + 2);
    const quint32 records_bytes = 4u + static_cast<quint32>(num_encodings) * 8u;
    QVERIFY(records_bytes <= cmap_length);

    bool found{false};
    for (quint16 i = 0; i < num_encodings; ++i) {
        const int record = cmap_at + 4 + static_cast<int>(i) * 8;
        if (ReadBe16(bytes, record) == 3 && ReadBe16(bytes, record + 2) == 1) {
            found = true;
            break;
        }
    }
    QVERIFY2(found, "no cmap encoding record with platform 3 and encoding 1");
}
