// Copyright (c) 2011-2020 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_PSQTOPERATIONSDIALOG_H
#define QUICKSILVER_QT_PSQTOPERATIONSDIALOG_H

#include <QDialog>
#include <QString>

#include <psqt.h>
#include <qt/clientmodel.h>
#include <qt/vaultmodel.h>

namespace Ui {
class PSQTOperationsDialog;
}

/** Dialog showing transaction details. */
class PSQTOperationsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PSQTOperationsDialog(QWidget* parent, VaultModel* vaultModel, ClientModel* clientModel);
    ~PSQTOperationsDialog();

    void openWithPSQT(PartiallySignedQuicksilverTransaction psqtx);

public Q_SLOTS:
    void signTransaction();
    void broadcastTransaction();
    void copyToClipboard();
    void saveTransaction();

private:
    Ui::PSQTOperationsDialog* m_ui;
    PartiallySignedQuicksilverTransaction m_transaction_data;
    VaultModel* m_vault_model;
    ClientModel* m_client_model;

    enum class StatusLevel {
        INFO,
        WARN,
        ERR
    };

    size_t couldSignInputs(const PartiallySignedQuicksilverTransaction &psqtx);
    void updateTransactionDisplay();
    QString renderTransaction(const PartiallySignedQuicksilverTransaction &psqtx);
    void showStatus(const QString &msg, StatusLevel level);
    void showTransactionStatus(const PartiallySignedQuicksilverTransaction &psqtx);
};

#endif // QUICKSILVER_QT_PSQTOPERATIONSDIALOG_H
