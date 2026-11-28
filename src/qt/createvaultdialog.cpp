// Copyright (c) 2019-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/benchpanel.h>
#include <qt/createvaultdialog.h>
#include <qt/forms/ui_createvaultdialog.h>

#include <qt/guiutil.h>

#include <QLabel>
#include <QPushButton>

CreateVaultDialog::CreateVaultDialog(QWidget* parent) :
    QDialog(parent, GUIUtil::dialog_flags),
    ui(new Ui::CreateVaultDialog)
{
    ui->setupUi(this);
    // The page grammar: the form in one titled panel, the dialog's own buttons
    // below it, quiet commands.
    BenchPanel::Adopt(this, ui->buttonBox, QStringLiteral("createVaultPanel"), tr("Create vault"));
    for (QLabel* note : {ui->label_description, ui->label_subdescription}) {
        note->setProperty("class", QStringLiteral("benchNote"));
    }
    ui->vault_name_label->setProperty("class", QStringLiteral("benchKey"));
    for (QPushButton* button : findChildren<QPushButton*>()) {
        button->setProperty("class", QStringLiteral("benchQuiet"));
    }
    ui->buttonBox->button(QDialogButtonBox::Ok)->setText(tr("Create"));
    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);
    ui->vault_name_line_edit->setFocus(Qt::ActiveWindowFocusReason);

    connect(ui->vault_name_line_edit, &QLineEdit::textChanged, this, [this] {
        const QString name = vaultName();
        ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(
            !name.isEmpty() && name != QStringLiteral(".") && name != QStringLiteral("..") &&
            !GUIUtil::QStringToPath(name).has_root_path() && !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\')));
    });
}

CreateVaultDialog::~CreateVaultDialog()
{
    delete ui;
}

QString CreateVaultDialog::vaultName() const
{
    return ui->vault_name_line_edit->text().trimmed();
}

bool CreateVaultDialog::isEncryptVaultChecked() const
{
    return ui->encrypt_vault_checkbox->isChecked();
}
