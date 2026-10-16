// Copyright (c) 2011-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_QUICKSILVERUNITS_H
#define QUICKSILVER_QT_QUICKSILVERUNITS_H

#include <consensus/amount.h>

#include <QAbstractListModel>
#include <QDataStream>
#include <QString>

/** Quicksilver unit definitions. Encapsulates parsing and formatting
   and serves as list model for drop-down selection boxes.
*/
class QuicksilverUnits: public QAbstractListModel
{
    Q_OBJECT

public:
    explicit QuicksilverUnits(QObject *parent);

    /** Quicksilver units. The numeric mapping is serialized by QDataStream. */
    enum class Unit {
        HG = 0,
        HGS = 1
    };
    Q_ENUM(Unit)

    //! NEVER is separator-free, for copy, export and URIs.
    //! ALWAYS groups thousands with commas, from four digits up.
    enum class SeparatorStyle
    {
        NEVER,
        ALWAYS
    };

    //! @name Static API
    //! Unit conversion and formatting
    ///@{

    //! Get list of units, for drop-down box
    static QList<Unit> availableUnits();
    //! Long name
    static QString longName(Unit unit);
    //! Short name. Codes only: Hg and HgS.
    static QString shortName(Unit unit);
    //! Name shown in unit choices. Amount labels and keys continue to use the codes.
    static QString choiceName(Unit unit);
    //! What follows a displayed amount. Hg stays the code. Cinnabar is U+1F713.
    static QString displaySuffix(Unit unit);
    //! Longer description
    static QString description(Unit unit);
    //! Number of cinnabar (1e-8) per unit
    static qint64 factor(Unit unit);
    //! Number of decimals left
    static int decimals(Unit unit);
    //! Format as string
    static QString format(Unit unit, const CAmount& amount, bool plussign = false, SeparatorStyle separators = SeparatorStyle::ALWAYS, bool justify = false);
    //! Format as string (with unit)
    static QString formatWithUnit(Unit unit, const CAmount& amount, bool plussign = false, SeparatorStyle separators = SeparatorStyle::ALWAYS);
    //! Format as HTML string (with unit)
    static QString formatHtmlWithUnit(Unit unit, const CAmount& amount, bool plussign = false, SeparatorStyle separators = SeparatorStyle::ALWAYS);
    //! Format an inline HTML amount and suffix with privacy masking, if set.
    static QString formatInlineHtmlWithPrivacy(Unit unit, const CAmount& amount, SeparatorStyle separators, bool privacy);
    //! Format as string (with unit) of fixed length to preserve privacy, if it is set.
    static QString formatWithPrivacy(Unit unit, const CAmount& amount, SeparatorStyle separators, bool privacy);
    //! Format an inline amount with privacy masking, without column padding.
    static QString formatInlineWithPrivacy(Unit unit, const CAmount& amount, SeparatorStyle separators, bool privacy);
    //! The same inline amount and privacy mask, without the unit, for a figure whose unit is shown beside it.
    static QString formatInlineValueWithPrivacy(Unit unit, const CAmount& amount, SeparatorStyle separators, bool privacy);
    //! Parse string to coin amount
    static bool parse(Unit unit, const QString& value, CAmount* val_out);
    //! Gets title for amount column including current display unit if optionsModel reference available */
    static QString getAmountColumnTitle(Unit unit);
    ///@}

    //! @name AbstractListModel implementation
    //! List model for unit drop-down selection box.
    ///@{
    enum RoleIndex {
        /** Unit identifier */
        UnitRole = Qt::UserRole
    };
    int rowCount(const QModelIndex &parent) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    ///@}

    //! Return maximum number of base units (cinnabar)
    static CAmount maxMoney();

private:
    QList<Unit> unitlist;
};
typedef QuicksilverUnits::Unit QuicksilverUnit;

QDataStream& operator<<(QDataStream& out, const QuicksilverUnit& unit);
QDataStream& operator>>(QDataStream& in, QuicksilverUnit& unit);

#endif // QUICKSILVER_QT_QUICKSILVERUNITS_H
