// Copyright (c) 2011-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/quicksilverunits.h>

#include <qt/quicksilverstyle.h>

#include <consensus/amount.h>

#include <QRegularExpression>
#include <QStringList>

#include <cassert>

static constexpr auto MAX_DIGITS_HG = 16;

QuicksilverUnits::QuicksilverUnits(QObject *parent):
        QAbstractListModel(parent),
        unitlist(availableUnits())
{
}

QList<QuicksilverUnit> QuicksilverUnits::availableUnits()
{
    QList<QuicksilverUnit> unitlist;
    unitlist.append(Unit::HG);
    unitlist.append(Unit::HGS);
    return unitlist;
}

QString QuicksilverUnits::longName(Unit unit)
{
    switch (unit) {
    case Unit::HG: return QString("Hg");
    case Unit::HGS: return QString("HgS"); // cinnabar ore's own formula, mercury(II) sulfide
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

QString QuicksilverUnits::shortName(Unit unit)
{
    switch (unit) {
    case Unit::HG: return longName(unit);
    case Unit::HGS: return QString("HgS");
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

QString QuicksilverUnits::choiceName(Unit unit)
{
    switch (unit) {
    case Unit::HG: return QStringLiteral("Hg");
    case Unit::HGS: return QString::fromUtf8("HgS (\xF0\x9F\x9C\x93)");
    }
    assert(false);
}

QString QuicksilverUnits::displaySuffix(Unit unit)
{
    switch (unit) {
    case Unit::HG: return QStringLiteral("Hg");
    case Unit::HGS: {
        const char32_t sign = 0x1F713;
        return QString::fromUcs4(&sign, 1);
    }
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

QString QuicksilverUnits::description(Unit unit)
{
    switch (unit) {
    case Unit::HG: return QString("Quicksilver");
    case Unit::HGS: return QString("cinnabar (0.00000001 Quicksilver)");
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

qint64 QuicksilverUnits::factor(Unit unit)
{
    switch (unit) {
    case Unit::HG: return 100'000'000;
    case Unit::HGS: return 1;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

int QuicksilverUnits::decimals(Unit unit)
{
    switch (unit) {
    case Unit::HG: return 8;
    case Unit::HGS: return 0;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

namespace {
//! Commas between significant digits. Padding is applied afterwards, so a
//! comma never lands in the spaces that trimmed() is expected to drop.
QString GroupDigits(QString digits)
{
    const int q_size = digits.size();
    for (int i = 3; i < q_size; i += 3) {
        digits.insert(q_size - i, QLatin1Char(','));
    }
    return digits;
}

int GroupedWidth(int digits)
{
    if (digits <= 0) return 0;
    return digits + (digits - 1) / 3;
}

//! Space, the thin space, and the no-break space. None of these is a decimal mark.
QString StripAmountSpaces(QString text)
{
    text.remove(QLatin1Char(' '));
    text.remove(QChar(0x2009));
    text.remove(QChar(0x00A0));
    return text;
}

//! Spaces, then grouping commas. Callers check the commas before this runs.
QString StripAmountSeparators(QString text)
{
    text = StripAmountSpaces(text);
    text.remove(QLatin1Char(','));
    return text;
}

QString HtmlWithUnitSuffix(QuicksilverUnits::Unit unit, const QString& amount_text)
{
    QString suffix = QuicksilverUnits::displaySuffix(unit).toHtmlEscaped();
    if (unit == QuicksilverUnits::Unit::HGS) {
        const QString color = QuicksilverStyle::Color(QuicksilverStyle::Token::Vermilion).name();
        suffix = QStringLiteral("<span style='color:%1;'>%2</span>").arg(color, suffix);
    }
    return QStringLiteral("<span style='white-space: nowrap;'>%1%2</span>").arg(amount_text.toHtmlEscaped(), suffix);
}
} // namespace

QString QuicksilverUnits::format(Unit unit, const CAmount& nIn, bool fPlus, SeparatorStyle separators, bool justify)
{
    // Note: not using straight sprintf here because we do NOT want
    // localized number formatting.
    qint64 n = (qint64)nIn;
    qint64 coin = factor(unit);
    int num_decimals = decimals(unit);
    qint64 n_abs = (n > 0 ? n : -n);
    qint64 quotient = n_abs / coin;
    const bool group = separators != SeparatorStyle::NEVER;
    QString quotient_str = QString::number(quotient);
    if (group) {
        quotient_str = GroupDigits(quotient_str);
    }
    if (justify) {
        const int digits = MAX_DIGITS_HG - num_decimals;
        const int width = group ? GroupedWidth(digits) : digits;
        quotient_str = quotient_str.rightJustified(width, QLatin1Char(' '));
    }

    if (n < 0)
        quotient_str.insert(0, '-');
    else if (fPlus && n > 0)
        quotient_str.insert(0, '+');

    if (num_decimals > 0) {
        qint64 remainder = n_abs % coin;
        QString remainder_str = QString::number(remainder).rightJustified(num_decimals, '0');
        return quotient_str + QString(".") + remainder_str;
    } else {
        return quotient_str;
    }
}


// NOTE: Using formatWithUnit in an HTML context risks wrapping
// quantities at a thousands comma. formatHtmlWithUnit keeps the amount
// on one line.

QString QuicksilverUnits::formatWithUnit(Unit unit, const CAmount& amount, bool plussign, SeparatorStyle separators)
{
    return format(unit, amount, plussign, separators) + displaySuffix(unit);
}

QString QuicksilverUnits::formatHtmlWithUnit(Unit unit, const CAmount& amount, bool plussign, SeparatorStyle separators)
{
    return HtmlWithUnitSuffix(unit, format(unit, amount, plussign, separators));
}

QString QuicksilverUnits::formatInlineHtmlWithPrivacy(Unit unit, const CAmount& amount, SeparatorStyle separators, bool privacy)
{
    assert(amount >= 0);
    const QString value = privacy ? format(unit, 0, false, separators, true).replace('0', '#')
                                  : format(unit, amount, false, separators, true);
    return HtmlWithUnitSuffix(unit, value.trimmed());
}

QString QuicksilverUnits::formatWithPrivacy(Unit unit, const CAmount& amount, SeparatorStyle separators, bool privacy)
{
    assert(amount >= 0);
    QString value;
    if (privacy) {
        value = format(unit, 0, false, separators, true).replace('0', '#');
    } else {
        value = format(unit, amount, false, separators, true);
    }
    return value + displaySuffix(unit);
}

QString QuicksilverUnits::formatInlineValueWithPrivacy(Unit unit, const CAmount& amount, SeparatorStyle separators, bool privacy)
{
    assert(amount >= 0);
    const QString value = privacy ? format(unit, 0, false, separators, true).replace('0', '#')
                                  : format(unit, amount, false, separators, true);
    return value.trimmed();
}

QString QuicksilverUnits::formatInlineWithPrivacy(Unit unit, const CAmount& amount, SeparatorStyle separators, bool privacy)
{
    // Keep the stacked labels' formatting and privacy mask, but remove the
    // column padding. Grouping commas sit between digits, so they survive.
    return formatWithPrivacy(unit, amount, separators, privacy).trimmed();
}

bool QuicksilverUnits::parse(Unit unit, const QString& value, CAmount* val_out)
{
    if (value.isEmpty()) {
        return false; // Refuse to parse invalid unit or empty string
    }
    int num_decimals = decimals(unit);

    // A comma after the decimal point is not a group separator. `40.000,5`
    // is refused rather than read as forty or as forty thousand.
    const int dot = value.indexOf(QLatin1Char('.'));
    if (dot >= 0 && value.indexOf(QLatin1Char(','), dot + 1) >= 0) {
        return false;
    }

    // Spaces are ignored, including a space inside a group (`1, 000`).
    // A comma is a thousands separator only when the whole part is groups
    // of three. `1,5` and `1,2,3` are refused rather than read as 15 and 123.
    const QString spaced = StripAmountSpaces(value);
    const int spaced_dot = spaced.indexOf(QLatin1Char('.'));
    const QString grouped = spaced_dot < 0 ? spaced : spaced.left(spaced_dot);
    if (grouped.contains(QLatin1Char(','))) {
        static const QRegularExpression groups(QStringLiteral("^[0-9]{1,3}(,[0-9]{3})+$"));
        if (!groups.match(grouped).hasMatch()) {
            return false;
        }
    }

    const QString cleaned = StripAmountSeparators(spaced);
    if (cleaned.isEmpty()) {
        return false;
    }
    QStringList parts = cleaned.split(QLatin1Char('.'));

    if(parts.size() > 2)
    {
        return false; // More than one dot
    }
    const QString& whole = parts[0];
    QString decimals;

    if(parts.size() > 1)
    {
        decimals = parts[1];
    }
    if(decimals.size() > num_decimals)
    {
        return false; // Exceeds max precision
    }
    bool ok = false;
    QString str = whole + decimals.leftJustified(num_decimals, '0');

    if(str.size() > 18)
    {
        return false; // Longer numbers will exceed 63 bits
    }
    CAmount retvalue(str.toLongLong(&ok));
    if(val_out)
    {
        *val_out = retvalue;
    }
    return ok;
}

QString QuicksilverUnits::getAmountColumnTitle(Unit unit)
{
    return QObject::tr("Amount") + " (" + shortName(unit) + ")";
}

int QuicksilverUnits::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return unitlist.size();
}

QVariant QuicksilverUnits::data(const QModelIndex &index, int role) const
{
    int row = index.row();
    if(row >= 0 && row < unitlist.size())
    {
        Unit unit = unitlist.at(row);
        switch(role)
        {
        case Qt::EditRole:
        case Qt::DisplayRole:
            return QVariant(choiceName(unit));
        case Qt::ToolTipRole:
            return QVariant(description(unit));
        case UnitRole:
            return QVariant::fromValue(unit);
        }
    }
    return QVariant();
}

CAmount QuicksilverUnits::maxMoney()
{
    return MAX_MONEY;
}

namespace {
qint8 ToQint8(QuicksilverUnit unit)
{
    switch (unit) {
    case QuicksilverUnit::HG: return 0;
    case QuicksilverUnit::HGS: return 1;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

QuicksilverUnit FromQint8(qint8 num)
{
    switch (num) {
    case 0: return QuicksilverUnit::HG;
    case 1: return QuicksilverUnit::HGS;
    default: return QuicksilverUnit::HGS;
    }
}
} // namespace

QDataStream& operator<<(QDataStream& out, const QuicksilverUnit& unit)
{
    return out << ToQint8(unit);
}

QDataStream& operator>>(QDataStream& in, QuicksilverUnit& unit)
{
    qint8 input;
    in >> input;
    unit = FromQint8(input);
    return in;
}
