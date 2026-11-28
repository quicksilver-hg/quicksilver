// Copyright (c) 2019-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_CREATEVAULTDIALOG_H
#define QUICKSILVER_QT_CREATEVAULTDIALOG_H

#include <QDialog>


namespace Ui {
    class CreateVaultDialog;
}

/** Dialog for creating vaults
 */
class CreateVaultDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CreateVaultDialog(QWidget* parent);
    virtual ~CreateVaultDialog();

    QString vaultName() const;
    bool isEncryptVaultChecked() const;

private:
    Ui::CreateVaultDialog *ui;
};

#endif // QUICKSILVER_QT_CREATEVAULTDIALOG_H
