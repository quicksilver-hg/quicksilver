// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_TRANSACTIONDESC_H
#define QUICKSILVER_QT_TRANSACTIONDESC_H

#include <qt/quicksilverunits.h>

#include <QObject>
#include <QString>

class TransactionRecord;

namespace interfaces {
class Node;
class Vault;
struct VaultTx;
struct VaultTxStatus;
}

/** Provide a human-readable extended HTML description of a transaction.
 */
class TransactionDesc: public QObject
{
    Q_OBJECT

public:
    static QString toHTML(interfaces::Node& node, interfaces::Vault& vault, TransactionRecord* rec, QuicksilverUnit unit);

private:
    TransactionDesc() = default;

    static QString FormatTxStatus(const interfaces::VaultTxStatus& status, bool inRelayPool);
};

#endif // QUICKSILVER_QT_TRANSACTIONDESC_H
