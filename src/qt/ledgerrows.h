// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_LEDGERROWS_H
#define QUICKSILVER_QT_LEDGERROWS_H

#include <qt/quicksilverunits.h>

#include <QIdentityProxyModel>

//! How the ledger reads the transaction table, on Home and on the Ledger page:
//! a state word instead of the status icon, a friendly date with the state dot,
//! a plain type word, the label, and a signed amount without the unconfirmed
//! brackets.
class LedgerRows : public QIdentityProxyModel
{
public:
    //! Home shows a tooltip only on the label; the Ledger page keeps the
    //! model's tooltips, which its legend points to for a row's details.
    enum class Tooltips { LabelOnly, All };

    explicit LedgerRows(Tooltips tooltips, QObject* parent);

    void setDisplay(QuicksilverUnit unit, bool privacy);

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    QVariant data(const QModelIndex& index, int role) const override;

private:
    const Tooltips m_tooltips;
    QuicksilverUnit m_unit{QuicksilverUnit::HG};
    bool m_privacy{false};
};

#endif // QUICKSILVER_QT_LEDGERROWS_H
