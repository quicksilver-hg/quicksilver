// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/transactiondesc.h>

#include <qt/quicksilverunits.h>
#include <qt/guiutil.h>
#include <qt/transactionrecord.h>

#include <common/system.h>
#include <consensus/consensus.h>
#include <interfaces/node.h>
#include <interfaces/vault.h>
#include <key_io.h>
#include <logging.h>
#include <policy/policy.h>
#include <validation.h>
#include <vault/types.h>

#include <stdint.h>
#include <cassert>
#include <string>

#include <QLatin1String>

using vault::ISMINE_SPENDABLE;
using vault::isminetype;

QString TransactionDesc::FormatTxStatus(const interfaces::VaultTxStatus& status, bool inRelayPool)
{
    int depth = status.depth_in_main_chain;
    if (depth < 0) {
        /*: Text explaining the current status of a transaction, shown in the
            status field of the details window for this transaction. This status
            represents an unconfirmed transaction that conflicts with a confirmed
            transaction. */
        return tr("conflicted with a transaction with %1 confirmations").arg(-depth);
    } else if (depth == 0) {
        QString s;
        if (inRelayPool) {
            /*: Text explaining the current status of a transaction, shown in the
                status field of the details window for this transaction. This status
                represents an unconfirmed transaction that is in the memory pool. */
            s = tr("0/unconfirmed, in memory pool");
        } else {
            /*: Text explaining the current status of a transaction, shown in the
                status field of the details window for this transaction. This status
                represents an unconfirmed transaction that is not in the memory pool. */
            s = tr("0/unconfirmed, not in memory pool");
        }
        if (status.is_abandoned) {
            /*: Text explaining the current status of a transaction, shown in the
                status field of the details window for this transaction. This
                status represents an abandoned transaction. */
            s += QLatin1String(", ") + tr("abandoned");
        }
        return s;
    } else if (depth < 6) {
        /*: Text explaining the current status of a transaction, shown in the
            status field of the details window for this transaction. This
            status represents a transaction confirmed in at least one block,
            but less than 6 blocks. */
        return tr("%1/unconfirmed").arg(depth);
    } else {
        /*: Text explaining the current status of a transaction, shown in the
            status field of the details window for this transaction. This status
            represents a transaction confirmed in 6 or more blocks. */
        return tr("%1 confirmations").arg(depth);
    }
}

QString TransactionDesc::toHTML(interfaces::Node& node, interfaces::Vault& vault, TransactionRecord* rec, QuicksilverUnit unit)
{
    int numBlocks;
    interfaces::VaultTxStatus status;
    interfaces::VaultOrderForm orderForm;
    bool inRelayPool;
    interfaces::VaultTx wtx;

    QString strHTML;

    strHTML.reserve(4000);
    strHTML += "<html><font face='verdana, arial, helvetica, sans-serif'>";
    if (!vault.tryGetVaultTxDetails(rec->hash, wtx, status, orderForm, inRelayPool, numBlocks) || !wtx.tx) {
        strHTML += "<b>" + tr("Status") + ":</b> " + tr("Waiting for vault header tip");
        strHTML += "<br>";
        strHTML += "<b>" + tr("Transaction ID") + ":</b> " + QString::fromStdString(rec->hash.ToString());
        strHTML += "</font></html>";
        return strHTML;
    }

    int64_t nTime = wtx.time;
    CAmount nCredit = wtx.credit;
    CAmount nDebit = wtx.debit;
    CAmount nNet = nCredit - nDebit;

    strHTML += "<b>" + tr("Status") + ":</b> " + FormatTxStatus(status, inRelayPool);
    strHTML += "<br>";

    strHTML += "<b>" + tr("Date") + ":</b> " + (nTime ? GUIUtil::dateTimeStr(nTime) : "") + "<br>";

    //
    // From
    //
    if (wtx.is_coinbase)
    {
        strHTML += "<b>" + tr("Source") + ":</b> " + tr("Generated") + "<br>";
    }
    else
    {
        // Offline transaction
        if (nNet > 0)
        {
            // Credit
            CTxDestination address = DecodeDestination(rec->address);
            if (IsValidDestination(address)) {
                std::string name;
                isminetype ismine;
                if (vault.getAddress(address, &name, &ismine, /* purpose= */ nullptr))
                {
                    strHTML += "<b>" + tr("From") + ":</b> " + tr("unknown") + "<br>";
                    strHTML += "<b>" + tr("To") + ":</b> ";
                    strHTML += GUIUtil::HtmlEscape(rec->address);
                    if (!name.empty())
                        strHTML += " (" + tr("own address") + ", " + tr("label") + ": " + GUIUtil::HtmlEscape(name) + ")";
                    else
                        strHTML += " (" + tr("own address") + ")";
                    strHTML += "<br>";
                }
            }
        }
    }

    //
    // To
    //
    if (wtx.value_map.count("to") && !wtx.value_map["to"].empty())
    {
        // Online transaction
        std::string strAddress = wtx.value_map["to"];
        strHTML += "<b>" + tr("To") + ":</b> ";
        CTxDestination dest = DecodeDestination(strAddress);
        std::string name;
        if (vault.getAddress(
                dest, &name, /* is_mine= */ nullptr, /* purpose= */ nullptr) && !name.empty())
            strHTML += GUIUtil::HtmlEscape(name) + " ";
        strHTML += GUIUtil::HtmlEscape(strAddress) + "<br>";
    }

    //
    // Amount
    //
    if (wtx.is_coinbase && nCredit == 0)
    {
        //
        // Coinbase
        //
        CAmount nUnmatured = 0;
        for (const CTxOut& txout : wtx.tx->vout)
            nUnmatured += vault.getCredit(txout, ISMINE_SPENDABLE);
        strHTML += "<b>" + tr("Credit") + ":</b> ";
        // Singular and plural as separate strings rather than the "%n block(s)" idiom,
        // for the reason set out in qt/maturity.cpp: no app catalogue is installed, so
        // %n is never resolved and the literal "(s)" ships.
        const QString matures = status.blocks_to_maturity == 1
                                    ? tr("matures in 1 more block")
                                    : tr("matures in %1 more blocks").arg(status.blocks_to_maturity);
        if (status.is_in_main_chain)
            strHTML += QuicksilverUnits::formatHtmlWithUnit(unit, nUnmatured)+ " (" + matures + ")";
        else
            strHTML += "(" + tr("not accepted") + ")";
        strHTML += "<br>";
    }
    else if (nNet > 0)
    {
        //
        // Credit
        //
        strHTML += "<b>" + tr("Credit") + ":</b> " + QuicksilverUnits::formatHtmlWithUnit(unit, nNet) + "<br>";
    }
    else
    {
        isminetype fAllFromMe = ISMINE_SPENDABLE;
        for (const isminetype mine : wtx.txin_is_mine)
        {
            if(fAllFromMe > mine) fAllFromMe = mine;
        }

        isminetype fAllToMe = ISMINE_SPENDABLE;
        for (const isminetype mine : wtx.txout_is_mine)
        {
            if(fAllToMe > mine) fAllToMe = mine;
        }

        if (fAllFromMe)
        {
            //
            // Debit
            //
            auto mine = wtx.txout_is_mine.begin();
            for (const CTxOut& txout : wtx.tx->vout)
            {
                // Ignore change
                isminetype toSelf = *(mine++);
                if ((toSelf == ISMINE_SPENDABLE) && (fAllFromMe == ISMINE_SPENDABLE))
                    continue;

                if (!wtx.value_map.count("to") || wtx.value_map["to"].empty())
                {
                    // Offline transaction
                    CTxDestination address;
                    if (ExtractDestination(txout.scriptPubKey, address))
                    {
                        strHTML += "<b>" + tr("To") + ":</b> ";
                        std::string name;
                        if (vault.getAddress(
                                address, &name, /* is_mine= */ nullptr, /* purpose= */ nullptr) && !name.empty())
                            strHTML += GUIUtil::HtmlEscape(name) + " ";
                        strHTML += GUIUtil::HtmlEscape(EncodeDestination(address));
                        if(toSelf == ISMINE_SPENDABLE)
                            strHTML += " (" + tr("own address") + ")";
                        strHTML += "<br>";
                    }
                }

                strHTML += "<b>" + tr("Debit") + ":</b> " + QuicksilverUnits::formatHtmlWithUnit(unit, -txout.nValue) + "<br>";
                if(toSelf)
                    strHTML += "<b>" + tr("Credit") + ":</b> " + QuicksilverUnits::formatHtmlWithUnit(unit, txout.nValue) + "<br>";
            }

            if (fAllToMe)
            {
                // Payment to self
                CAmount nChange = wtx.change;
                CAmount nValue = nCredit - nChange;
                strHTML += "<b>" + tr("Total debit") + ":</b> " + QuicksilverUnits::formatHtmlWithUnit(unit, -nValue) + "<br>";
                strHTML += "<b>" + tr("Total credit") + ":</b> " + QuicksilverUnits::formatHtmlWithUnit(unit, nValue) + "<br>";
            }

        }
        else
        {
            //
            // Mixed debit transaction
            //
            auto mine = wtx.txin_is_mine.begin();
            for (const CTxIn& txin : wtx.tx->vin) {
                if (*(mine++)) {
                    strHTML += "<b>" + tr("Debit") + ":</b> " + QuicksilverUnits::formatHtmlWithUnit(unit, -vault.getDebit(txin, ISMINE_SPENDABLE)) + "<br>";
                }
            }
            mine = wtx.txout_is_mine.begin();
            for (const CTxOut& txout : wtx.tx->vout) {
                if (*(mine++)) {
                    strHTML += "<b>" + tr("Credit") + ":</b> " + QuicksilverUnits::formatHtmlWithUnit(unit, vault.getCredit(txout, ISMINE_SPENDABLE)) + "<br>";
                }
            }
        }
    }

    strHTML += "<b>" + tr("Net amount") + ":</b> " + QuicksilverUnits::formatHtmlWithUnit(unit, nNet, true) + "<br>";

    //
    // Message
    //
    if (wtx.value_map.count("message") && !wtx.value_map["message"].empty())
        strHTML += "<br><b>" + tr("Message") + ":</b><br>" + GUIUtil::HtmlEscape(wtx.value_map["message"], true) + "<br>";
    if (wtx.value_map.count("comment") && !wtx.value_map["comment"].empty())
        strHTML += "<br><b>" + tr("Comment") + ":</b><br>" + GUIUtil::HtmlEscape(wtx.value_map["comment"], true) + "<br>";

    strHTML += "<b>" + tr("Transaction ID") + ":</b> " + rec->getTxHash() + "<br>";
    strHTML += "<b>" + tr("Transaction total size") + ":</b> " + QString::number(wtx.tx->GetTotalSize()) + " bytes<br>";
    strHTML += "<b>" + tr("Transaction virtual size") + ":</b> " + QString::number(GetVirtualTransactionSize(*wtx.tx)) + " bytes<br>";
    strHTML += "<b>" + tr("Output index") + ":</b> " + QString::number(rec->getOutputIndex()) + "<br>";

    // Message from normal quicksilver:URI (quicksilver:123...?message=example)
    for (const std::pair<std::string, std::string>& r : orderForm) {
        if (r.first == "Message")
            strHTML += "<br><b>" + tr("Message") + ":</b><br>" + GUIUtil::HtmlEscape(r.second, true) + "<br>";
    }

    if (wtx.is_coinbase)
    {
        quint32 numBlocksToMaturity = COINBASE_MATURITY +  1;
        strHTML += "<br>" + tr("Generated coins must mature %1 blocks before they can be spent. When you generated this block, it was broadcast to the network to be added to the block chain. If it fails to get into the chain, its state will change to \"not accepted\" and it won't be spendable. This may occasionally happen if another node generates a block within a few seconds of yours.").arg(QString::number(numBlocksToMaturity)) + "<br>";
    }

    //
    // Debug view
    //
    if (node.getLogCategories() != HgLog::NONE)
    {
        strHTML += "<hr><br>" + tr("Debug information") + "<br><br>";
        for (const CTxIn& txin : wtx.tx->vin)
            if(vault.txinIsMine(txin))
                strHTML += "<b>" + tr("Debit") + ":</b> " + QuicksilverUnits::formatHtmlWithUnit(unit, -vault.getDebit(txin, ISMINE_SPENDABLE)) + "<br>";
        for (const CTxOut& txout : wtx.tx->vout)
            if(vault.txoutIsMine(txout))
                strHTML += "<b>" + tr("Credit") + ":</b> " + QuicksilverUnits::formatHtmlWithUnit(unit, vault.getCredit(txout, ISMINE_SPENDABLE)) + "<br>";

        strHTML += "<br><b>" + tr("Transaction") + ":</b><br>";
        strHTML += GUIUtil::HtmlEscape(wtx.tx->ToString(), true);

        strHTML += "<br><b>" + tr("Inputs") + ":</b>";
        strHTML += "<ul>";

        for (const CTxIn& txin : wtx.tx->vin)
        {
            COutPoint prevout = txin.prevout;

            if (auto prev{node.getUnspentOutput(prevout)}) {
                {
                    strHTML += "<li>";
                    const CTxOut& vout = prev->out;
                    CTxDestination address;
                    if (ExtractDestination(vout.scriptPubKey, address))
                    {
                        std::string name;
                        if (vault.getAddress(address, &name, /* is_mine= */ nullptr, /* purpose= */ nullptr) && !name.empty())
                            strHTML += GUIUtil::HtmlEscape(name) + " ";
                        strHTML += QString::fromStdString(EncodeDestination(address));
                    }
                    strHTML = strHTML + " " + tr("Amount") + "=" + QuicksilverUnits::formatHtmlWithUnit(unit, vout.nValue);
                    strHTML = strHTML + " IsMine=" + (vault.txoutIsMine(vout) & ISMINE_SPENDABLE ? tr("true") : tr("false")) + "</li>";
                }
            }
        }

        strHTML += "</ul>";
    }

    strHTML += "</font></html>";
    return strHTML;
}
