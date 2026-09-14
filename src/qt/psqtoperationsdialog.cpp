// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/psqtoperationsdialog.h>

#include <common/messages.h>
#include <core_io.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <node/psqt.h>
#include <node/types.h>
#include <policy/policy.h>
#include <qt/quicksilverunits.h>
#include <qt/forms/ui_psqtoperationsdialog.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <util/fs.h>
#include <util/strencodings.h>

#include <cassert>
#include <fstream>
#include <iostream>
#include <string>

#include <QPointer>

using common::TransactionErrorString;
using node::AnalyzePSQT;
using node::PSQTAnalysis;
using node::TransactionError;

PSQTOperationsDialog::PSQTOperationsDialog(
    QWidget* parent, VaultModel* vault_model, ClientModel* client_model) : QDialog(parent, GUIUtil::dialog_flags),
                                                                             m_ui(new Ui::PSQTOperationsDialog),
                                                                             m_vault_model(vault_model),
                                                                             m_client_model(client_model)
{
    m_ui->setupUi(this);

    connect(m_ui->signTransactionButton, &QPushButton::clicked, this, &PSQTOperationsDialog::signTransaction);
    connect(m_ui->broadcastTransactionButton, &QPushButton::clicked, this, &PSQTOperationsDialog::broadcastTransaction);
    connect(m_ui->copyToClipboardButton, &QPushButton::clicked, this, &PSQTOperationsDialog::copyToClipboard);
    connect(m_ui->saveButton, &QPushButton::clicked, this, &PSQTOperationsDialog::saveTransaction);

    connect(m_ui->closeButton, &QPushButton::clicked, this, &PSQTOperationsDialog::close);

    m_ui->signTransactionButton->setEnabled(false);
    m_ui->broadcastTransactionButton->setEnabled(false);
}

PSQTOperationsDialog::~PSQTOperationsDialog()
{
    delete m_ui;
}

void PSQTOperationsDialog::openWithPSQT(PartiallySignedQuicksilverTransaction psqtx)
{
    m_transaction_data = psqtx;

    bool complete = FinalizePSQT(psqtx); // Make sure all existing signatures are fully combined before checking for completeness.
    if (m_vault_model) {
        size_t n_could_sign;
        const auto err{m_vault_model->vault().fillPSQT(SIGHASH_ALL, /*sign=*/false, /*bip32derivs=*/true, &n_could_sign, m_transaction_data, complete)};
        if (err) {
            showStatus(tr("Failed to load transaction: %1")
                           .arg(QString::fromStdString(PSQTErrorString(*err).translated)),
                       StatusLevel::ERR);
            return;
        }
        m_ui->signTransactionButton->setEnabled(!complete && !m_vault_model->vault().privateKeysDisabled() && n_could_sign > 0);
    } else {
        m_ui->signTransactionButton->setEnabled(false);
    }

    m_ui->broadcastTransactionButton->setEnabled(complete);

    updateTransactionDisplay();
}

void PSQTOperationsDialog::signTransaction()
{
    QPointer<PSQTOperationsDialog> self(this);
    m_vault_model->requestUnlock([this, self](std::shared_ptr<VaultModel::UnlockContext> ctx) {
        if (!self) return;

        bool complete;
        size_t n_signed;
        const auto err{m_vault_model->vault().fillPSQT(SIGHASH_DEFAULT, /*sign=*/true, /*bip32derivs=*/true, &n_signed, m_transaction_data, complete)};

        if (err) {
            showStatus(tr("Failed to sign transaction: %1")
                .arg(QString::fromStdString(PSQTErrorString(*err).translated)), StatusLevel::ERR);
            return;
        }

        updateTransactionDisplay();

        if (!complete && (!ctx || !ctx->isValid())) {
            showStatus(tr("Cannot sign inputs while vault is locked."), StatusLevel::WARN);
        } else if (!complete && n_signed < 1) {
            showStatus(tr("Could not sign any more inputs."), StatusLevel::WARN);
        } else if (!complete) {
            showStatus(tr("Signed %1 inputs, but more signatures are still required.").arg(n_signed),
                StatusLevel::INFO);
        } else {
            showStatus(tr("Signed transaction successfully. Transaction is ready to broadcast."),
                StatusLevel::INFO);
            m_ui->broadcastTransactionButton->setEnabled(true);
        }
    });
}

void PSQTOperationsDialog::broadcastTransaction()
{
    CMutableTransaction mtx;
    if (!FinalizeAndExtractPSQT(m_transaction_data, mtx)) {
        // This is never expected to fail unless we were given a malformed PSQT
        // (e.g. with an invalid signature.)
        showStatus(tr("Unknown error processing transaction."), StatusLevel::ERR);
        return;
    }

    CTransactionRef tx = MakeTransactionRef(mtx);
    std::string err_string;
    TransactionError error =
        m_client_model->node().broadcastTransaction(tx, err_string);

    if (error == TransactionError::OK) {
        showStatus(tr("Transaction broadcast successfully! Transaction ID: %1")
            .arg(QString::fromStdString(tx->GetHash().GetHex())), StatusLevel::INFO);
    } else {
        showStatus(tr("Transaction broadcast failed: %1")
            .arg(QString::fromStdString(TransactionErrorString(error).translated)), StatusLevel::ERR);
    }
}

void PSQTOperationsDialog::copyToClipboard() {
    DataStream ssTx{};
    ssTx << m_transaction_data;
    GUIUtil::setClipboard(EncodeBase64(ssTx.str()).c_str());
    showStatus(tr("PSQT copied to clipboard."), StatusLevel::INFO);
}

void PSQTOperationsDialog::saveTransaction() {
    DataStream ssTx{};
    ssTx << m_transaction_data;

    QString filename_suggestion = "";
    bool first = true;
    for (const CTxOut& out : m_transaction_data.tx->vout) {
        if (!first) {
            filename_suggestion.append("-");
        }
        CTxDestination address;
        ExtractDestination(out.scriptPubKey, address);
        QString amount = QuicksilverUnits::format(m_client_model->getOptionsModel()->getDisplayUnit(), out.nValue);
        QString address_str = QString::fromStdString(EncodeDestination(address));
        filename_suggestion.append(address_str + "-" + amount);
        first = false;
    }
    filename_suggestion.append(".psqt");
    QPointer<PSQTOperationsDialog> self{this};
    GUIUtil::getSaveFileName(this,
        tr("Save Transaction Data"), filename_suggestion,
        //: Expanded name of the binary PSQT file format.
        tr("Partially Signed Transaction (Binary)") + QLatin1String(" (*.psqt)"),
        [self, payload = ssTx.str()](const QString& filename) {
            if (!self || filename.isEmpty()) return;
            std::ofstream out{filename.toLocal8Bit().data(), std::ofstream::out | std::ofstream::binary};
            out << payload;
            out.close();
            self->showStatus(self->tr("PSQT saved to disk."), StatusLevel::INFO);
        });
}

void PSQTOperationsDialog::updateTransactionDisplay() {
    m_ui->transactionDescription->setText(renderTransaction(m_transaction_data));
    showTransactionStatus(m_transaction_data);
}

QString PSQTOperationsDialog::renderTransaction(const PartiallySignedQuicksilverTransaction &psqtx)
{
    QString tx_description;
    QLatin1String bullet_point(" * ");
    CAmount totalAmount = 0;
    for (const CTxOut& out : psqtx.tx->vout) {
        CTxDestination address;
        ExtractDestination(out.scriptPubKey, address);
        totalAmount += out.nValue;
        tx_description.append(bullet_point).append(tr("Sends %1 to %2")
            .arg(QuicksilverUnits::formatWithUnit(QuicksilverUnit::HG, out.nValue))
            .arg(QString::fromStdString(EncodeDestination(address))));
        // Check if the address is one of ours
        if (m_vault_model != nullptr && m_vault_model->vault().txoutIsMine(out)) tx_description.append(" (" + tr("own address") + ")");
        tx_description.append("<br>");
    }

    PSQTAnalysis analysis = AnalyzePSQT(psqtx);
    if (!analysis.value_delta.has_value()) {
        // This happens if the transaction is missing input UTXO information.
        tx_description.append(bullet_point).append(tr("Unable to calculate total transaction amount."));
    } else if (*analysis.value_delta != 0) {
        tx_description.append(bullet_point).append(tr("Unsupported input/output value mismatch."));
    } else {
        // add total amount in all subdivision units
        tx_description.append("<hr />");
        QStringList alternativeUnits;
        for (const QuicksilverUnits::Unit u : QuicksilverUnits::availableUnits())
        {
            if(u != m_client_model->getOptionsModel()->getDisplayUnit()) {
                alternativeUnits.append(QuicksilverUnits::formatHtmlWithUnit(u, totalAmount));
            }
        }
        tx_description.append(QString("<b>%1</b>: <b>%2</b>").arg(tr("Total Amount"))
            .arg(QuicksilverUnits::formatHtmlWithUnit(m_client_model->getOptionsModel()->getDisplayUnit(), totalAmount)));
        tx_description.append(QString("<br /><span style='font-size:10pt; font-weight:normal;'>(=%1)</span>")
            .arg(alternativeUnits.join(" " + tr("or") + " ")));
    }

    size_t num_unsigned = CountPSQTUnsignedInputs(psqtx);
    if (num_unsigned > 0) {
        tx_description.append("<br><br>");
        tx_description.append(tr("Transaction has %1 unsigned inputs.").arg(QString::number(num_unsigned)));
    }

    return tx_description;
}

void PSQTOperationsDialog::showStatus(const QString &msg, StatusLevel level) {
    m_ui->statusBar->setText(msg);
    switch (level) {
        case StatusLevel::INFO: {
            m_ui->statusBar->setStyleSheet("QLabel { background-color : lightgreen }");
            break;
        }
        case StatusLevel::WARN: {
            m_ui->statusBar->setStyleSheet("QLabel { background-color : orange }");
            break;
        }
        case StatusLevel::ERR: {
            m_ui->statusBar->setStyleSheet("QLabel { background-color : red }");
            break;
        }
    }
    m_ui->statusBar->show();
}

size_t PSQTOperationsDialog::couldSignInputs(const PartiallySignedQuicksilverTransaction &psqtx) {
    if (!m_vault_model) {
        return 0;
    }

    size_t n_signed;
    bool complete;
    const auto err{m_vault_model->vault().fillPSQT(SIGHASH_ALL, /*sign=*/false, /*bip32derivs=*/false, &n_signed, m_transaction_data, complete)};

    if (err) {
        return 0;
    }
    return n_signed;
}

void PSQTOperationsDialog::showTransactionStatus(const PartiallySignedQuicksilverTransaction &psqtx) {
    PSQTAnalysis analysis = AnalyzePSQT(psqtx);
    size_t n_could_sign = couldSignInputs(psqtx);

    switch (analysis.next) {
        case PSQTRole::UPDATER: {
            showStatus(tr("Transaction is missing some information about inputs."), StatusLevel::WARN);
            break;
        }
        case PSQTRole::SIGNER: {
            QString need_sig_text = tr("Transaction still needs signature(s).");
            StatusLevel level = StatusLevel::INFO;
            if (!m_vault_model) {
                need_sig_text += " " + tr("(But no vault is loaded.)");
                level = StatusLevel::WARN;
            } else if (m_vault_model->vault().privateKeysDisabled()) {
                need_sig_text += " " + tr("(But this vault cannot sign transactions.)");
                level = StatusLevel::WARN;
            } else if (n_could_sign < 1) {
                need_sig_text += " " + tr("(But this vault does not have the right keys.)"); // XXX wording
                level = StatusLevel::WARN;
            }
            showStatus(need_sig_text, level);
            break;
        }
        case PSQTRole::FINALIZER:
        case PSQTRole::EXTRACTOR: {
            showStatus(tr("Transaction is fully signed and ready for broadcast."), StatusLevel::INFO);
            break;
        }
        default: {
            showStatus(tr("Transaction status is unknown."), StatusLevel::ERR);
            break;
        }
    }
}
