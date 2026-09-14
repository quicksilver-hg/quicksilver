// Copyright (c) 2011-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/receiverequestdialog.h>
#include <qt/forms/ui_receiverequestdialog.h>

#include <qt/quicksilverunits.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/qrimagewidget.h>
#include <qt/vaultmodel.h>

#include <QDialog>
#include <QString>

#include <quicksilver-build-config.h> // IWYU pragma: keep

ReceiveRequestDialog::ReceiveRequestDialog(QWidget* parent)
    : QDialog(parent, GUIUtil::dialog_flags),
      ui(new Ui::ReceiveRequestDialog)
{
    ui->setupUi(this);
    GUIUtil::handleCloseWindowShortcut(this);
}

ReceiveRequestDialog::~ReceiveRequestDialog()
{
    delete ui;
}

void ReceiveRequestDialog::setModel(VaultModel *_model)
{
    this->model = _model;

    if (_model)
        connect(_model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &ReceiveRequestDialog::updateDisplayUnit);

    // update the display unit if necessary
    update();
}

void ReceiveRequestDialog::setInfo(const SendCoinsRecipient &_info)
{
    this->info = _info;
    setWindowTitle(tr("Address request for %1").arg(info.label.isEmpty() ? info.address : info.label));
    QString uri = GUIUtil::formatQuicksilverURI(info);

#ifdef USE_QRCODE
    if (ui->qr_code->setQR(uri, info.address)) {
        connect(ui->btnSaveAs, &QPushButton::clicked, ui->qr_code, &QRImageWidget::saveImage);
    } else {
        ui->btnSaveAs->setEnabled(false);
    }
#else
    ui->btnSaveAs->hide();
    ui->qr_code->hide();
#endif

    ui->uri_content->setText("<a href=\"" + uri + "\">" + GUIUtil::HtmlEscape(uri) + "</a>");
    ui->address_content->setText(info.address);

    if (!info.amount) {
        ui->amount_tag->hide();
        ui->amount_content->hide();
    } // Amount is set in updateDisplayUnit() slot.
    updateDisplayUnit();

    if (!info.label.isEmpty()) {
        ui->label_content->setText(info.label);
    } else {
        ui->label_tag->hide();
        ui->label_content->hide();
    }

    if (!info.message.isEmpty()) {
        ui->message_content->setText(info.message);
    } else {
        ui->message_tag->hide();
        ui->message_content->hide();
    }

    if (!model->getVaultName().isEmpty()) {
        ui->vault_content->setText(model->getVaultName());
    } else {
        ui->vault_tag->hide();
        ui->vault_content->hide();
    }

    ui->btnVerify->setVisible(model->vault().hasExternalSigner());

    connect(ui->btnVerify, &QPushButton::clicked, [this] {
        model->displayAddress(info.address.toStdString());
    });
}

void ReceiveRequestDialog::updateDisplayUnit()
{
    if (!model) return;
    ui->amount_content->setText(QuicksilverUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), info.amount));
}

void ReceiveRequestDialog::on_btnCopyURI_clicked()
{
    GUIUtil::setClipboard(GUIUtil::formatQuicksilverURI(info));
}

void ReceiveRequestDialog::on_btnCopyAddress_clicked()
{
    GUIUtil::setClipboard(info.address);
}
