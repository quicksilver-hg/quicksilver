// Copyright (c) 2011-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/vaultmodeltransaction.h>

VaultModelTransaction::VaultModelTransaction(const QList<SendCoinsRecipient>& _recipients)
    : recipients(_recipients)
{
}

QList<SendCoinsRecipient> VaultModelTransaction::getRecipients() const
{
    return recipients;
}

CTransactionRef& VaultModelTransaction::getWtx()
{
    return wtx;
}

void VaultModelTransaction::setWtx(const CTransactionRef& newTx)
{
    wtx = newTx;
}

CAmount VaultModelTransaction::getTotalTransactionAmount() const
{
    CAmount totalTransactionAmount = 0;
    for (const SendCoinsRecipient &rcp : recipients)
    {
        totalTransactionAmount += rcp.amount;
    }
    return totalTransactionAmount;
}
