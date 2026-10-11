// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/ledgerrows.h>

#include <qt/quicksilverstyle.h>
#include <qt/transactionrecord.h>
#include <qt/transactiontablemodel.h>

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QFontDatabase>
#include <QHash>
#include <QLocale>
#include <QPainter>
#include <QPixmap>
#include <QVariant>

using QuicksilverStyle::Color;
using QuicksilverStyle::Token;

namespace {
QString tr(const char* text) { return QCoreApplication::translate("LedgerRows", text); }

int Status(const QModelIndex& index) { return index.data(TransactionTableModel::StatusRole).toInt(); }

Token StateTone(int state)
{
    switch (state) {
    case TransactionStatus::Unconfirmed: return Token::StatePending;
    case TransactionStatus::Confirming:
    case TransactionStatus::Confirmed: return Token::StatusGood;
    // Not spendable yet, and nothing is wrong: a caution, not an error.
    case TransactionStatus::Immature: return Token::Warning;
    default: return Token::CinnabarSoft;
    }
}

QPixmap Dot(Token tone)
{
    static QHash<int, QPixmap> cache;
    const auto key = static_cast<int>(tone);
    auto found = cache.constFind(key);
    if (found != cache.constEnd()) return *found;
    QPixmap pixmap(QSize(14, 14));
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Color(tone));
    painter.drawEllipse(QRectF(3.5, 3.5, 7, 7));
    painter.end();
    cache.insert(key, pixmap);
    return pixmap;
}

QString FriendlyDate(const QDateTime& when)
{
    if (!when.isValid()) return {};
    const QLocale locale;
    const QDate day = when.date();
    const QDate today = QDate::currentDate();
    const QString time = locale.toString(when.time(), QStringLiteral("HH:mm"));
    if (day == today) return tr("Today") + QLatin1Char(' ') + time;
    if (day == today.addDays(-1)) return tr("Yesterday") + QLatin1Char(' ') + time;
    const QString format = day.year() == today.year() ? QStringLiteral("d MMM") : QStringLiteral("d MMM yyyy");
    return locale.toString(day, format) + QLatin1Char(' ') + time;
}

//! The label when there is one; otherwise the address, which still tells
//! the rows apart. A mining reward has neither worth showing.
QString Counterparty(const QModelIndex& index)
{
    QString label = index.data(TransactionTableModel::LabelRole).toString();
    if (!label.isEmpty()) return label;
    if (index.data(TransactionTableModel::TypeRole).toInt() == TransactionRecord::Generated) return QStringLiteral("—");
    const QString address = index.data(TransactionTableModel::AddressRole).toString();
    return address.isEmpty() ? QStringLiteral("—") : address;
}
} // namespace

LedgerRows::LedgerRows(Tooltips tooltips, QObject* parent)
    : QIdentityProxyModel(parent), m_tooltips(tooltips) {}

void LedgerRows::setDisplay(QuicksilverUnit unit, bool privacy)
{
    m_unit = unit;
    m_privacy = privacy;
    if (rowCount() > 0) Q_EMIT dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
    if (columnCount() > 0) Q_EMIT headerDataChanged(Qt::Horizontal, 0, columnCount() - 1);
}

QVariant LedgerRows::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal) return QIdentityProxyModel::headerData(section, orientation, role);
    if (role == Qt::DisplayRole) {
        switch (section) {
        case TransactionTableModel::Status: return tr("STATE");
        case TransactionTableModel::Date: return tr("DATE");
        case TransactionTableModel::Type: return tr("TYPE");
        case TransactionTableModel::ToAddress: return tr("LABEL");
        case TransactionTableModel::Amount: return QString{tr("AMOUNT") + QStringLiteral("  ") + QuicksilverUnits::shortName(m_unit)};
        }
    }
    if (role == Qt::TextAlignmentRole) {
        return QVariant(section == TransactionTableModel::Amount ? (Qt::AlignRight | Qt::AlignVCenter) : (Qt::AlignLeft | Qt::AlignVCenter));
    }
    if (role == Qt::ToolTipRole) return QVariant();
    return QIdentityProxyModel::headerData(section, orientation, role);
}

QVariant LedgerRows::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) return {};
    const int column = index.column();
    if (role == Qt::DisplayRole) {
        switch (column) {
        case TransactionTableModel::Status: return TransactionTableModel::stateWord(Status(index));
        case TransactionTableModel::Date: return FriendlyDate(index.data(TransactionTableModel::DateRole).toDateTime());
        case TransactionTableModel::Type: return TransactionTableModel::typeWord(index.data(TransactionTableModel::TypeRole).toInt());
        case TransactionTableModel::ToAddress: return Counterparty(index);
        case TransactionTableModel::Amount: return TransactionTableModel::signedAmount(m_unit, index.data(TransactionTableModel::AmountRole).toLongLong(), m_privacy);
        }
    }
    if (role == Qt::DecorationRole) {
        if (column == TransactionTableModel::Date) return Dot(StateTone(Status(index)));
        return {};
    }
    if (role == Qt::ForegroundRole) {
        switch (column) {
        case TransactionTableModel::Status: {
            const int state = Status(index);
            return Color(state == TransactionStatus::Unconfirmed || state == TransactionStatus::Immature ? StateTone(state) : Token::SilverMuted);
        }
        case TransactionTableModel::ToAddress:
            return Color(index.data(TransactionTableModel::LabelRole).toString().isEmpty() ? Token::SilverMuted : Token::Silver);
        case TransactionTableModel::Amount:
            return Color(index.data(TransactionTableModel::AmountRole).toLongLong() < 0 ? Token::CinnabarSoft : Token::SilverHi);
        default:
            return Color(Token::Silver);
        }
    }
    if (role == Qt::FontRole && column == TransactionTableModel::Amount) return QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (role == Qt::TextAlignmentRole) {
        return QVariant(column == TransactionTableModel::Amount ? (Qt::AlignRight | Qt::AlignVCenter) : (Qt::AlignLeft | Qt::AlignVCenter));
    }
    if (role == Qt::ToolTipRole && m_tooltips == Tooltips::LabelOnly && column != TransactionTableModel::ToAddress) return {};
    return QIdentityProxyModel::data(index, role);
}
