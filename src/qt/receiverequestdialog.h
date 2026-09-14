// Copyright (c) 2011-2020 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_RECEIVEREQUESTDIALOG_H
#define QUICKSILVER_QT_RECEIVEREQUESTDIALOG_H

#include <qt/sendcoinsrecipient.h>

#include <QDialog>

class VaultModel;

namespace Ui {
    class ReceiveRequestDialog;
}

class ReceiveRequestDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ReceiveRequestDialog(QWidget *parent = nullptr);
    ~ReceiveRequestDialog();

    void setModel(VaultModel *model);
    void setInfo(const SendCoinsRecipient &info);

private Q_SLOTS:
    void on_btnCopyURI_clicked();
    void on_btnCopyAddress_clicked();
    void updateDisplayUnit();

private:
    Ui::ReceiveRequestDialog *ui;
    VaultModel* model{nullptr};
    SendCoinsRecipient info;
};

#endif // QUICKSILVER_QT_RECEIVEREQUESTDIALOG_H
