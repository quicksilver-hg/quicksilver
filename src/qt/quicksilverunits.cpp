// Copyright (c) 2011-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/quicksilverunits.h>

#include <consensus/amount.h>

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

QString QuicksilverUnits::format(Unit unit, const CAmount& nIn, bool fPlus, SeparatorStyle separators, bool justify)
{
    // Note: not using straight sprintf here because we do NOT want
    // localized number formatting.
    qint64 n = (qint64)nIn;
    qint64 coin = factor(unit);
    int num_decimals = decimals(unit);
    qint64 n_abs = (n > 0 ? n : -n);
    qint64 quotient = n_abs / coin;
    QString quotient_str = QString::number(quotient);
    if (justify) {
        quotient_str = quotient_str.rightJustified(MAX_DIGITS_HG - num_decimals, ' ');
    }

    // Use SI-style thin space separators as these are locale independent and can't be
    // confused with the decimal marker.
    QChar thin_sp(THIN_SP_CP);
    int q_size = quotient_str.size();
    if (separators == SeparatorStyle::ALWAYS || (separators == SeparatorStyle::STANDARD && q_size > 4))
        for (int i = 3; i < q_size; i += 3)
            quotient_str.insert(q_size - i, thin_sp);

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
// quantities at the thousands separator. More subtly, it also results
// in a standard space rather than a thin space, due to a bug in Qt's
// XML whitespace canonicalisation
//
// Please take care to use formatHtmlWithUnit instead, when
// appropriate.

QString QuicksilverUnits::formatWithUnit(Unit unit, const CAmount& amount, bool plussign, SeparatorStyle separators)
{
    return format(unit, amount, plussign, separators) + QString(" ") + shortName(unit);
}

QString QuicksilverUnits::formatHtmlWithUnit(Unit unit, const CAmount& amount, bool plussign, SeparatorStyle separators)
{
    QString str(formatWithUnit(unit, amount, plussign, separators));
    str.replace(QChar(THIN_SP_CP), QString(THIN_SP_HTML));
    return QString("<span style='white-space: nowrap;'>%1</span>").arg(str);
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
    return value + QString(" ") + shortName(unit);
}

bool QuicksilverUnits::parse(Unit unit, const QString& value, CAmount* val_out)
{
    if (value.isEmpty()) {
        return false; // Refuse to parse invalid unit or empty string
    }
    int num_decimals = decimals(unit);

    // Ignore spaces and thin spaces when parsing
    QStringList parts = removeSpaces(value).split(".");

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
            return QVariant(longName(unit));
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
    default: return QuicksilverUnit::HG;
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
