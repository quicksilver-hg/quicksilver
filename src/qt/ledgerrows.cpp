// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/ledgerrows.h>

#include <qt/guiutil.h>
#include <qt/quicksilverstyle.h>
#include <qt/transactionrecord.h>
#include <qt/transactiontablemodel.h>

#include <QAbstractItemView>
#include <QApplication>
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QFontDatabase>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QTextFormat>
#include <QTextLayout>
#include <QHash>
#include <QLocale>
#include <QPainter>
#include <QPixmap>
#include <QVariant>

using QuicksilverStyle::Color;
using QuicksilverStyle::Token;

namespace {
QString tr(const char* text) { return QCoreApplication::translate("LedgerRows", text); }

class AmountSuffixDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QString text = opt.text;
        const QString sign = QuicksilverUnits::displaySuffix(QuicksilverUnit::HGS);
        const int at = text.lastIndexOf(sign);
        if (at < 0) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        opt.text.clear();
        QStyle* style = opt.widget ? opt.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

        const QRect text_rect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, opt.widget);
        const QColor body = opt.palette.color(opt.state & QStyle::State_Selected ? QPalette::HighlightedText : QPalette::Text);
        QTextCharFormat plain;
        plain.setForeground(body);
        QTextCharFormat accent;
        accent.setForeground(Color(Token::Vermilion));
        QTextLayout layout(text, opt.font);
        QVector<QTextLayout::FormatRange> ranges(2);
        ranges[0].start = 0;
        ranges[0].length = at;
        ranges[0].format = plain;
        ranges[1].start = at;
        ranges[1].length = text.size() - at;
        ranges[1].format = accent;
        layout.setFormats(ranges);
        layout.beginLayout();
        QTextLine line = layout.createLine();
        line.setLineWidth(text_rect.width());
        layout.endLayout();
        qreal x = text_rect.left();
        const qreal used = line.naturalTextWidth();
        if (opt.displayAlignment & Qt::AlignRight) x = text_rect.right() - used + 1;
        else if (opt.displayAlignment & Qt::AlignHCenter) x = text_rect.left() + (text_rect.width() - used) / 2.0;
        const qreal y = text_rect.top() + (text_rect.height() - line.height()) / 2.0;
        painter->save();
        painter->setClipRect(opt.rect);
        layout.draw(painter, QPointF(x, y));
        painter->restore();
    }
};

} // namespace

void ColorCinnabarAmountSuffix(QAbstractItemView* view)
{
    if (!view) return;
    view->setItemDelegateForColumn(TransactionTableModel::Amount, new AmountSuffixDelegate(view));
}

namespace {

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
    if (role == Qt::FontRole && column == TransactionTableModel::Amount) {
        return GUIUtil::WithUnitGlyph(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    }
    if (role == Qt::TextAlignmentRole) {
        return QVariant(column == TransactionTableModel::Amount ? (Qt::AlignRight | Qt::AlignVCenter) : (Qt::AlignLeft | Qt::AlignVCenter));
    }
    if (role == Qt::ToolTipRole && m_tooltips == Tooltips::LabelOnly) {
        if (column != TransactionTableModel::ToAddress) return {};
        // The cell elides. The tooltip is the full text the cell is showing
        // (the label, or the address when there is no label), not the
        // transaction-status tooltip the model would otherwise supply.
        return Counterparty(index);
    }
    return QIdentityProxyModel::data(index, role);
}
