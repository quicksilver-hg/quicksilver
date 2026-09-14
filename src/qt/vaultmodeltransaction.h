// Copyright (c) 2011-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_VAULTMODELTRANSACTION_H
#define QUICKSILVER_QT_VAULTMODELTRANSACTION_H

#include <primitives/transaction.h>
#include <qt/sendcoinsrecipient.h>

#include <consensus/amount.h>

#include <QObject>

class SendCoinsRecipient;

/** Data model for a vaultmodel transaction. */
class VaultModelTransaction
{
public:
    explicit VaultModelTransaction(const QList<SendCoinsRecipient> &recipients);

    QList<SendCoinsRecipient> getRecipients() const;

    CTransactionRef& getWtx();
    void setWtx(const CTransactionRef&);

    CAmount getTotalTransactionAmount() const;

private:
    QList<SendCoinsRecipient> recipients;
    CTransactionRef wtx;
};

#endif // QUICKSILVER_QT_VAULTMODELTRANSACTION_H
