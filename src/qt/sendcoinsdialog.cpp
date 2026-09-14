// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <quicksilver-build-config.h> // IWYU pragma: keep

#include <qt/sendcoinsdialog.h>
#include <qt/forms/ui_sendcoinsdialog.h>

#include <qt/addresstablemodel.h>
#include <qt/quicksilverunits.h>
#include <qt/clientmodel.h>
#include <qt/coincontroldialog.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/quicksilverstyle.h>
#include <qt/sendcoinsentry.h>

#include <chainparams.h>
#include <common/args.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <node/interface_ui.h>
#include <node/types.h>
#include <txrelaypool.h>
#include <validation.h>
#include <vault/coincontrol.h>
#include <vault/vault.h>

#include <chrono>
#include <fstream>
#include <memory>

#include <QDebug>
#include <QCheckBox>
#include <QFileInfo>
#include <QLocale>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSharedPointer>
#include <QShowEvent>
#include <QStringList>
#include <QTextDocument>
#include <QThread>

using common::PSQTError;
using vault::CCoinControl;

namespace {
struct PreparedSendResult {
    std::unique_ptr<VaultModelTransaction> transaction;
    VaultModel::SendCoinsReturn status;
    quint64 generation;
};

bool TxProofRequiresConfiguredGpuSolver()
{
    return Params().GetConsensus().nTxEdgeBits > 19;
}

QString ConfiguredGpuSolverPath()
{
    return QString::fromStdString(gArgs.GetArg("-cuckatoosolver", ""));
}

bool IsExecutableSolverFile(const QString& solver_path)
{
    const QFileInfo solver_info(solver_path.trimmed());
    return solver_info.exists() && solver_info.isFile() && solver_info.isExecutable();
}

bool g_startup_acceleration_warning_shown{false};
} // namespace

SendCoinsDialog::SendCoinsDialog(const PlatformStyle *_platformStyle, QWidget *parent) :
    QDialog(parent, GUIUtil::dialog_flags),
    ui(new Ui::SendCoinsDialog),
    m_coin_control(new CCoinControl),
    platformStyle(_platformStyle)
{
    ui->setupUi(this);
    m_gpu_state = gpu::GpuDetectionService::detectGpu(IsExecutableSolverFile(ConfiguredGpuSolverPath()));

    if (!_platformStyle->getImagesOnButtons()) {
        ui->addButton->setIcon(QIcon());
        ui->clearButton->setIcon(QIcon());
        ui->sendButton->setIcon(QIcon());
    } else {
        ui->addButton->setIcon(_platformStyle->ColorIcon(":/icons/add", QuicksilverStyle::Color(QuicksilverStyle::Token::Teal)));
        ui->clearButton->setIcon(_platformStyle->ColorIcon(":/icons/remove", QuicksilverStyle::Color(QuicksilverStyle::Token::CinnabarBright)));
        ui->sendButton->setIcon(_platformStyle->ColorIcon(":/icons/send", QuicksilverStyle::Color(QuicksilverStyle::Token::Amber)));
    }

    GUIUtil::setupAddressWidget(ui->lineEditCoinControlChange, this);
    ui->sendWorkProgressPanel->hide();
    ui->sendWorkProgressBar->setRange(0, 0);
    updateSendWorkDisclosure();

    addEntry();

    connect(ui->addButton, &QPushButton::clicked, this, &SendCoinsDialog::addEntry);
    connect(ui->clearButton, &QPushButton::clicked, this, &SendCoinsDialog::clear);

    // Coin Control
    connect(ui->pushButtonCoinControl, &QPushButton::clicked, this, &SendCoinsDialog::coinControlButtonClicked);
    connect(ui->checkBoxCoinControlChange, &QCheckBox::stateChanged, this, &SendCoinsDialog::coinControlChangeChecked);
    connect(ui->lineEditCoinControlChange, &QValidatedLineEdit::textEdited, this, &SendCoinsDialog::coinControlChangeEdited);

    // Coin Control: clipboard actions
    QAction *clipboardQuantityAction = new QAction(tr("Copy quantity"), this);
    QAction *clipboardAmountAction = new QAction(tr("Copy amount"), this);
    QAction *clipboardBytesAction = new QAction(tr("Copy bytes"), this);
    QAction *clipboardChangeAction = new QAction(tr("Copy change"), this);
    connect(clipboardQuantityAction, &QAction::triggered, this, &SendCoinsDialog::coinControlClipboardQuantity);
    connect(clipboardAmountAction, &QAction::triggered, this, &SendCoinsDialog::coinControlClipboardAmount);
    connect(clipboardBytesAction, &QAction::triggered, this, &SendCoinsDialog::coinControlClipboardBytes);
    connect(clipboardChangeAction, &QAction::triggered, this, &SendCoinsDialog::coinControlClipboardChange);
    ui->labelCoinControlQuantity->addAction(clipboardQuantityAction);
    ui->labelCoinControlAmount->addAction(clipboardAmountAction);
    ui->labelCoinControlBytes->addAction(clipboardBytesAction);
    ui->labelCoinControlChange->addAction(clipboardChangeAction);

    GUIUtil::ExceptionSafeConnect(ui->sendButton, &QPushButton::clicked, this, &SendCoinsDialog::sendButtonClicked);
    connect(ui->sendWorkCancelButton, &QPushButton::clicked, this, &SendCoinsDialog::cancelSendWork);
    connect(&m_send_work_update_timer, &QTimer::timeout, this, &SendCoinsDialog::updateSendWorkElapsed);
    m_send_work_update_timer.setInterval(1000);
}

void SendCoinsDialog::setClientModel(ClientModel *_clientModel)
{
    this->clientModel = _clientModel;

    if (_clientModel) {
        connect(_clientModel, &ClientModel::numBlocksChanged, this, &SendCoinsDialog::updateNumberOfBlocks);
    }
}

void SendCoinsDialog::setModel(VaultModel *_model)
{
    this->model = _model;
    m_gpu_state = gpu::GpuDetectionService::detectGpu(IsExecutableSolverFile(ConfiguredGpuSolverPath()));
    updateSendWorkDisclosure();

    if(_model && _model->getOptionsModel())
    {
        for(int i = 0; i < ui->entries->count(); ++i)
        {
            SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
            if(entry)
            {
                entry->setModel(_model);
            }
        }

        connect(_model, &VaultModel::balanceChanged, this, &SendCoinsDialog::setBalance);
        connect(_model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &SendCoinsDialog::refreshBalance);
        refreshBalance();

        // Coin Control
        connect(_model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &SendCoinsDialog::coinControlUpdateLabels);
        connect(_model->getOptionsModel(), &OptionsModel::coinControlFeaturesChanged, this, &SendCoinsDialog::coinControlFeatureChanged);
        ui->frameCoinControl->setVisible(_model->getOptionsModel()->getCoinControlFeatures());
        coinControlUpdateLabels();

        if (model->vault().hasExternalSigner()) {
            //: "device" usually means a hardware vault.
            ui->sendButton->setText(tr("Sign on device"));
            if (model->getOptionsModel()->hasSigner()) {
                ui->sendButton->setEnabled(true);
                ui->sendButton->setToolTip(tr("Connect your hardware signer first."));
            } else {
                ui->sendButton->setEnabled(false);
                //: "External signer" means using devices such as hardware vaults.
                ui->sendButton->setToolTip(tr("Set external signer script path in Options -> Vault"));
            }
        } else if (model->vault().privateKeysDisabled()) {
            ui->sendButton->setText(tr("Cr&eate Unsigned"));
            ui->sendButton->setToolTip(tr("Creates a Partially Signed Quicksilver Transaction (PSQT) for use with e.g. an offline %1 vault, or a PSQT-compatible hardware signer.").arg(CLIENT_NAME));
        }
    }
}

void SendCoinsDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    if (m_startup_warning_queued || g_startup_acceleration_warning_shown || !model) return;

    m_startup_warning_queued = true;
    QTimer::singleShot(0, this, [this] {
        m_startup_warning_queued = false;
        showStartupAccelerationWarningIfNeeded();
    });
}

SendCoinsDialog::~SendCoinsDialog()
{
    stopGpuSolverProbe();
    delete ui;
}

bool SendCoinsDialog::prepareTransactionForAsync(std::unique_ptr<VaultModelTransaction>& transaction, CCoinControl& coin_control)
{
    QList<SendCoinsRecipient> recipients;
    bool valid = true;

    for(int i = 0; i < ui->entries->count(); ++i)
    {
        SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
        if(entry)
        {
            if(entry->validate())
            {
                recipients.append(entry->getValue());
            }
            else if (valid)
            {
                ui->scrollArea->ensureWidgetVisible(entry);
                valid = false;
            }
        }
    }

    if(!valid || recipients.isEmpty())
    {
        return false;
    }

    fNewRecipientAllowed = false;

    coin_control = *m_coin_control;
    coin_control.m_allow_other_inputs = !coin_control.HasSelected();
    transaction = std::make_unique<VaultModelTransaction>(recipients);
    return true;
}

bool SendCoinsDialog::PrepareSendText(QString& question_string, QString& informative_text, QString& detailed_text) const
{
    assert(m_current_transaction);

    QStringList formatted;
    for (const SendCoinsRecipient &rcp : m_current_transaction->getRecipients())
    {
        // generate amount string with vault name in case of multivault
        QString amount = QuicksilverUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), rcp.amount);
        if (model->isMultivault()) {
            amount = tr("%1 from vault '%2'").arg(amount, GUIUtil::HtmlEscape(model->getVaultName()));
        }

        // generate address string
        QString address = rcp.address;

        QString recipientElement;

        {
            if(rcp.label.length() > 0) // label with address
            {
                recipientElement.append(tr("%1 to '%2'").arg(amount, GUIUtil::HtmlEscape(rcp.label)));
                recipientElement.append(QString(" (%1)").arg(address));
            }
            else // just address
            {
                recipientElement.append(tr("%1 to %2").arg(amount, address));
            }
        }
        formatted.append(recipientElement);
    }

    /*: Message displayed when attempting to create a transaction. Cautionary text to prompt the user to verify
        that the displayed transaction details represent the transaction the user intends to create. */
    question_string.append(tr("Do you want to create this transaction?"));
    question_string.append("<br /><span style='font-size:10pt;'>");
    if (model->vault().privateKeysDisabled() && !model->vault().hasExternalSigner()) {
        /*: Text to inform a user attempting to create a transaction of their current options. At this stage,
            a user can only create a PSQT. This string is displayed when private keys are disabled and an external
            signer is not available. */
        question_string.append(tr("Please, review your transaction proposal. This will produce a Partially Signed Quicksilver Transaction (PSQT) which you can save or copy and then sign with e.g. an offline %1 vault, or a PSQT-compatible hardware signer.").arg(CLIENT_NAME));
    } else if (model->getOptionsModel()->getEnablePSQTControls()) {
        /*: Text to inform a user attempting to create a transaction of their current options. At this stage,
            a user can send their transaction or create a PSQT. This string is displayed when both private keys
            and PSQT controls are enabled. */
        question_string.append(tr("Please, review your transaction. You can create and send this transaction or create a Partially Signed Quicksilver Transaction (PSQT), which you can save or copy and then sign with, e.g., an offline %1 vault, or a PSQT-compatible hardware signer.").arg(CLIENT_NAME));
    } else {
        /*: Text to prompt a user to review the details of the transaction they are attempting to send. */
        question_string.append(tr("Please, review your transaction."));
    }
    question_string.append("<br /><br />");
    question_string.append(tr("Sending uses proof-of-work. Nothing is deducted from the amount; the full amount arrives after this desktop solves the required work."));
    const QString graph_count = sendWorkGraphsConfirmationText();
    if (!graph_count.isEmpty()) {
        question_string.append("<br />");
        question_string.append(graph_count);
    }
    question_string.append("</span>%1");

    // add total amount in all subdivision units
    question_string.append("<hr />");
    const CAmount totalAmount = m_current_transaction->getTotalTransactionAmount();
    QStringList alternativeUnits;
    for (const QuicksilverUnit u : QuicksilverUnits::availableUnits()) {
        if(u != model->getOptionsModel()->getDisplayUnit())
            alternativeUnits.append(QuicksilverUnits::formatHtmlWithUnit(u, totalAmount));
    }
    question_string.append(QString("<b>%1</b>: <b>%2</b>").arg(tr("Total Amount"))
        .arg(QuicksilverUnits::formatHtmlWithUnit(model->getOptionsModel()->getDisplayUnit(), totalAmount)));
    question_string.append(QString("<br /><span style='font-size:10pt; font-weight:normal;'>(=%1)</span>")
        .arg(alternativeUnits.join(" " + tr("or") + " ")));

    if (formatted.size() > 1) {
        question_string = question_string.arg("");
        informative_text = tr("To review recipient list click \"Show Details…\"");
        detailed_text = formatted.join("\n\n");
    } else {
        question_string = question_string.arg("<br /><br />" + formatted.at(0));
    }

    return true;
}

void SendCoinsDialog::presentPSQT(PartiallySignedQuicksilverTransaction& psqtx)
{
    // Serialize the PSQT
    DataStream ssTx{};
    ssTx << psqtx;
    GUIUtil::setClipboard(EncodeBase64(ssTx.str()).c_str());
    const std::string serialized = ssTx.str();

    QString fileNameSuggestion;
    bool first = true;
    for (const SendCoinsRecipient &rcp : m_current_transaction->getRecipients()) {
        if (!first) {
            fileNameSuggestion.append(" - ");
        }
        QString labelOrAddress = rcp.label.isEmpty() ? rcp.address : rcp.label;
        QString amount = QuicksilverUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), rcp.amount);
        fileNameSuggestion.append(labelOrAddress + "-" + amount);
        first = false;
    }
    fileNameSuggestion.append(".psqt");

    auto* msgBox = new QMessageBox(this);
    //: Caption of "PSQT has been copied" messagebox
    msgBox->setText(tr("Unsigned Transaction", "PSQT copied"));
    msgBox->setInformativeText(tr("The PSQT has been copied to the clipboard. You can also save it."));
    msgBox->setStandardButtons(QMessageBox::Save | QMessageBox::Discard);
    msgBox->setDefaultButton(QMessageBox::Discard);
    msgBox->setObjectName("psqt_copied_message");
    GUIUtil::ShowModalMessageBoxAsynchronously(msgBox, [this, serialized, fileNameSuggestion](int result, QAbstractButton*) {
        if (result == QMessageBox::Save) {
            QPointer<SendCoinsDialog> self{this};
            GUIUtil::getSaveFileName(this,
                tr("Save Transaction Data"), fileNameSuggestion,
                //: Expanded name of the binary PSQT file format.
                tr("Partially Signed Transaction (Binary)") + QLatin1String(" (*.psqt)"),
                [self, serialized](const QString& filename) {
                    if (self && !filename.isEmpty()) {
                        std::ofstream out{filename.toLocal8Bit().data(), std::ofstream::out | std::ofstream::binary};
                        out << serialized;
                        out.close();
                        //: Popup message when a PSQT has been saved to a file
                        Q_EMIT self->message(self->tr("PSQT saved"), self->tr("PSQT saved to disk"), CClientUIInterface::MSG_INFORMATION);
                    }
                    if (self) self->finishSendFlow(false);
                });
            return;
        }
        finishSendFlow(false);
    });
}

bool SendCoinsDialog::signWithExternalSigner(PartiallySignedQuicksilverTransaction& psqtx, CMutableTransaction& mtx, bool& complete) {
    auto show_error = [this](const QString& object_name, const QString& title, const QString& text) {
        auto* box = new QMessageBox(QMessageBox::Critical, title, text, QMessageBox::Ok, this);
        box->setObjectName(object_name);
        GUIUtil::ShowModalDialogAsynchronously(box);
    };
    std::optional<PSQTError> err;
    try {
        err = model->vault().fillPSQT(SIGHASH_ALL, /*sign=*/true, /*bip32derivs=*/true, /*n_signed=*/nullptr, psqtx, complete);
    } catch (const std::runtime_error& e) {
        show_error(QStringLiteral("externalSignerSignFailed"), tr("Sign failed"), e.what());
        return false;
    }
    if (err == PSQTError::EXTERNAL_SIGNER_NOT_FOUND) {
        //: "External signer" means using devices such as hardware vaults.
        const QString msg = tr("External signer not found");
        show_error(QStringLiteral("externalSignerNotFound"), msg, msg);
        return false;
    }
    if (err == PSQTError::EXTERNAL_SIGNER_FAILED) {
        //: "External signer" means using devices such as hardware vaults.
        const QString msg = tr("External signer failure");
        show_error(QStringLiteral("externalSignerFailed"), msg, msg);
        return false;
    }
    if (err) {
        qWarning() << "Failed to sign PSQT";
        processSendCoinsReturn(VaultModel::TransactionCreationFailed);
        return false;
    }
    // fillPSQT does not always properly finalize
    complete = FinalizeAndExtractPSQT(psqtx, mtx);
    return true;
}

void SendCoinsDialog::finishSendFlow(bool send_failure)
{
    if (!send_failure) {
        accept();
        m_coin_control->UnSelectAll();
        coinControlUpdateLabels();
    }
    fNewRecipientAllowed = true;
    m_current_transaction.reset();
}

void SendCoinsDialog::confirmAndSendPreparedTransaction()
{
    assert(m_current_transaction);
    QString question_string, informative_text, detailed_text;
    if (!PrepareSendText(question_string, informative_text, detailed_text)) {
        fNewRecipientAllowed = true;
        m_current_transaction.reset();
        hideSendWorkProgress();
        return;
    }

    const QString confirmation = tr("Confirm transfer");
    const bool enable_send{!model->vault().privateKeysDisabled() || model->vault().hasExternalSigner()};
    const bool always_show_unsigned{model->getOptionsModel()->getEnablePSQTControls()};
    Q_EMIT prepareSendConfirmationReadyForTesting();
    auto confirmationDialog = new SendConfirmationDialog(confirmation, question_string, informative_text, detailed_text, SEND_CONFIRM_DELAY, enable_send, always_show_unsigned, this);
    connect(confirmationDialog, &QDialog::finished, this, &SendCoinsDialog::handleSendConfirmationFinished, Qt::QueuedConnection);
    GUIUtil::ShowModalDialogAsynchronously(confirmationDialog);
}

void SendCoinsDialog::handleSendConfirmationFinished(int retval)
{
    if (!m_current_transaction) return;
    if(retval != QMessageBox::Yes && retval != QMessageBox::Save)
    {
        fNewRecipientAllowed = true;
        m_current_transaction.reset();
        hideSendWorkProgress();
        return;
    }

    bool send_failure = false;
    if (retval == QMessageBox::Save) {
        CMutableTransaction mtx = CMutableTransaction{*(m_current_transaction->getWtx())};
        PartiallySignedQuicksilverTransaction psqtx(mtx);
        bool complete = false;
        const auto err{model->vault().fillPSQT(SIGHASH_ALL, /*sign=*/false, /*bip32derivs=*/true, /*n_signed=*/nullptr, psqtx, complete)};
        assert(!complete);
        assert(!err);
        presentPSQT(psqtx);
        return;
    }

    assert(!model->vault().privateKeysDisabled() || model->vault().hasExternalSigner());
    bool broadcast = true;
    if (model->vault().hasExternalSigner()) {
        CMutableTransaction mtx = CMutableTransaction{*(m_current_transaction->getWtx())};
        PartiallySignedQuicksilverTransaction psqtx(mtx);
        bool complete = false;
        const auto err{model->vault().fillPSQT(SIGHASH_ALL, /*sign=*/false, /*bip32derivs=*/true, /*n_signed=*/nullptr, psqtx, complete)};
        assert(!complete);
        assert(!err);
        send_failure = !signWithExternalSigner(psqtx, mtx, complete);
        broadcast = complete && !send_failure;
        if (!send_failure) {
            if (complete) {
                const CTransactionRef tx = MakeTransactionRef(mtx);
                m_current_transaction->setWtx(tx);
            } else {
                presentPSQT(psqtx);
                return;
            }
        }
    }

    if (broadcast) {
        const VaultModel::SendCoinsReturn status{model->sendCoins(*m_current_transaction)};
        if (status.status == VaultModel::OK) {
            Q_EMIT coinsSent(m_current_transaction->getWtx()->GetHash());
        } else {
            processSendCoinsReturn(status);
            send_failure = true;
        }
    }
    finishSendFlow(send_failure);
}

void SendCoinsDialog::finishSendPrepare(std::unique_ptr<VaultModelTransaction> transaction, const VaultModel::SendCoinsReturn& prepare_status, quint64 generation)
{
    if (!acceptSolveResult(generation)) {
        return;
    }

    setSendControlsEnabled(true);
    m_current_transaction = std::move(transaction);
    Q_EMIT sendPreparationFinishedForTesting(static_cast<int>(prepare_status.status), prepare_status.reason);
    processSendCoinsReturn(prepare_status);

    if(prepare_status.status != VaultModel::OK) {
        fNewRecipientAllowed = true;
        m_current_transaction.reset();
        failSendWorkProgress();
        return;
    }

    completeSendWorkProgress();
    confirmAndSendPreparedTransaction();
}

void SendCoinsDialog::setSendControlsEnabled(bool enabled)
{
    ui->entries->setEnabled(enabled);
    ui->addButton->setEnabled(enabled);
    ui->clearButton->setEnabled(enabled);
    ui->sendButton->setEnabled(enabled);
    ui->frameCoinControl->setEnabled(enabled);
}

void SendCoinsDialog::beginSendWorkProgress()
{
    m_send_work_started.restart();
    ui->sendWorkStatusLabel->setText(tr("Solving transfer proof-of-work"));
    ui->sendWorkGraphsLabel->setText(tr("Graphs tried: pending"));
    ui->sendWorkProgressBar->setRange(0, 0);
    ui->sendWorkProgressBar->setFormat(tr("Working"));
    ui->sendWorkCancelButton->setEnabled(true);
    ui->sendWorkProgressPanel->show();
    updateSendWorkElapsed();
    m_send_work_update_timer.start();
}

void SendCoinsDialog::completeSendWorkProgress()
{
    m_send_work_update_timer.stop();
    updateSendWorkElapsed();
    ui->sendWorkStatusLabel->setText(tr("Proof-of-work ready for review"));
    ui->sendWorkGraphsLabel->setText(sendWorkGraphsTriedText());
    ui->sendWorkProgressBar->setRange(0, 1);
    ui->sendWorkProgressBar->setValue(1);
    ui->sendWorkProgressBar->setFormat(tr("Ready"));
    ui->sendWorkCancelButton->setEnabled(false);
}

void SendCoinsDialog::failSendWorkProgress()
{
    m_send_work_update_timer.stop();
    updateSendWorkElapsed();
    ui->sendWorkStatusLabel->setText(tr("Proof-of-work was not completed"));
    ui->sendWorkGraphsLabel->setText(tr("Graphs tried: not completed"));
    ui->sendWorkProgressBar->setRange(0, 1);
    ui->sendWorkProgressBar->setValue(0);
    ui->sendWorkProgressBar->setFormat(tr("Stopped"));
    ui->sendWorkCancelButton->setEnabled(false);
    ui->sendWorkProgressPanel->show();
}

void SendCoinsDialog::hideSendWorkProgress()
{
    m_send_work_update_timer.stop();
    ui->sendWorkProgressPanel->hide();
}

void SendCoinsDialog::updateSendWorkElapsed()
{
    const qint64 elapsed_seconds = m_send_work_started.isValid() ? m_send_work_started.elapsed() / 1000 : 0;
    ui->sendWorkElapsedLabel->setText(tr("Elapsed: %1s").arg(elapsed_seconds));
}

void SendCoinsDialog::updateSendWorkDisclosure()
{
    refreshGpuSolverProbe(TxProofRequiresConfiguredGpuSolver(), ConfiguredGpuSolverPath());
    ui->sendWorkDisclosureLabel->setText(sendWorkResourceText());
}

void SendCoinsDialog::updateSendWorkGraphs(uint32_t nonce)
{
    ui->sendWorkGraphsLabel->setText(tr("Graphs tried: at least %1").arg(QLocale().toString(static_cast<quint64>(nonce) + 1)));
}

QString SendCoinsDialog::sendWorkGraphsTriedText() const
{
    if (!m_current_transaction || !m_current_transaction->getWtx()) {
        return tr("Graphs tried: pending");
    }
    const quint64 graphs_tried = static_cast<quint64>(m_current_transaction->getWtx()->nPowNonce) + 1;
    return tr("Graphs tried: %1").arg(QLocale().toString(graphs_tried));
}

QString SendCoinsDialog::sendWorkGraphsConfirmationText() const
{
    if (!m_current_transaction || !m_current_transaction->getWtx()) {
        return QString();
    }
    const quint64 graphs_tried = static_cast<quint64>(m_current_transaction->getWtx()->nPowNonce) + 1;
    return tr("Proof-of-work completed after %1 graphs tried.").arg(QLocale().toString(graphs_tried));
}

QString SendCoinsDialog::sendWorkResourceText() const
{
    return sendWorkResourceText(TxProofRequiresConfiguredGpuSolver(), ConfiguredGpuSolverPath(), m_gpu_solver_probe_status);
}

QString SendCoinsDialog::sendWorkResourceTextForTesting(bool requires_configured_gpu_solver, const QString& solver_path, GpuSolverProbeStatus probe_status)
{
    return sendWorkResourceText(requires_configured_gpu_solver, solver_path, probe_status);
}

bool SendCoinsDialog::cpuFallbackWarningRequiredForTesting(bool slow_network, GpuSolverProbeStatus probe_status, bool warning_enabled)
{
    return cpuFallbackWarningRequired(slow_network, probe_status, warning_enabled);
}

void SendCoinsDialog::refreshGpuSolverProbe(bool requires_configured_gpu_solver, const QString& solver_path)
{
    const QString trimmed_solver_path = solver_path.trimmed();
    if (!requires_configured_gpu_solver || !IsExecutableSolverFile(trimmed_solver_path)) {
        stopGpuSolverProbe();
        m_gpu_solver_probe_path.clear();
        m_gpu_solver_probe_status = GpuSolverProbeStatus::Unchecked;
        return;
    }

    if (m_gpu_solver_probe_path == trimmed_solver_path && m_gpu_solver_probe_status != GpuSolverProbeStatus::Unchecked) {
        return;
    }

    stopGpuSolverProbe();
    m_gpu_solver_probe_path = trimmed_solver_path;
    m_gpu_solver_probe_status = GpuSolverProbeStatus::Checking;

    QProcess* probe = new QProcess(this);
    m_gpu_solver_probe = probe;
    probe->setProgram(trimmed_solver_path);
    probe->setArguments(gpuSolverProbeArguments(Params().GetConsensus().nTxEdgeBits));
    probe->setProcessChannelMode(QProcess::ForwardedErrorChannel);

    connect(probe, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this, probe](int exit_code, QProcess::ExitStatus exit_status) {
        if (m_gpu_solver_probe != probe) {
            probe->deleteLater();
            return;
        }
        m_gpu_solver_probe = nullptr;
        if (m_gpu_solver_probe_status != GpuSolverProbeStatus::TimedOut) {
            m_gpu_solver_probe_status = (exit_status == QProcess::NormalExit && exit_code == 0)
                ? GpuSolverProbeStatus::Available
                : GpuSolverProbeStatus::Failed;
        }
        updateSendWorkDisclosure();
        probe->deleteLater();
    });
    connect(probe, &QProcess::errorOccurred, this, [this, probe](QProcess::ProcessError error) {
        if (m_gpu_solver_probe != probe || error == QProcess::Timedout || m_gpu_solver_probe_status == GpuSolverProbeStatus::TimedOut) return;
        m_gpu_solver_probe_status = GpuSolverProbeStatus::Failed;
        if (error == QProcess::FailedToStart) {
            m_gpu_solver_probe = nullptr;
            updateSendWorkDisclosure();
            probe->deleteLater();
        }
    });

    probe->start();
    QTimer::singleShot(3000, this, [this, probe]() {
        if (m_gpu_solver_probe != probe || probe->state() == QProcess::NotRunning) return;
        m_gpu_solver_probe_status = GpuSolverProbeStatus::TimedOut;
        probe->kill();
    });
}

void SendCoinsDialog::stopGpuSolverProbe()
{
    if (!m_gpu_solver_probe) return;
    QProcess* probe = m_gpu_solver_probe;
    m_gpu_solver_probe = nullptr;
    probe->kill();
    probe->deleteLater();
}

QStringList SendCoinsDialog::gpuSolverProbeArguments(uint8_t edgebits)
{
    return QStringList{
        QString::number(edgebits),
        QStringLiteral("00000000"),
        QStringLiteral("0"),
        QStringLiteral("0"),
    };
}

QString SendCoinsDialog::sendWorkResourceText(bool requires_configured_gpu_solver, const QString& solver_path, GpuSolverProbeStatus probe_status)
{
    QStringList disclosure;
    disclosure << tr("Quicksilver sends the full amount. Before broadcast, this desktop prepares a transfer proof; that work can take time.");

    if (requires_configured_gpu_solver) {
        const QString trimmed_solver_path = solver_path.trimmed();
        if (trimmed_solver_path.isEmpty()) {
            disclosure << tr("Graphics acceleration is not configured. The processor will take over, which may take many minutes. Choose a transfer helper in Settings → Options → Main for faster preparation.");
        } else {
            const QFileInfo solver_info(trimmed_solver_path);
            if (!solver_info.exists()) {
                disclosure << tr("The configured transfer helper could not be found. The processor will take over, which may take many minutes. Choose a working helper in Settings → Options → Main.");
            } else if (!solver_info.isFile() || !solver_info.isExecutable()) {
                disclosure << tr("The configured transfer helper cannot be started. The processor will take over, which may take many minutes. Choose a working helper in Settings → Options → Main.");
            } else {
                switch (probe_status) {
                case GpuSolverProbeStatus::Checking:
                    disclosure << tr("Checking whether graphics acceleration is ready for this network.");
                    break;
                case GpuSolverProbeStatus::Available:
                    // The wait is a geometric search with no upper bound, so a
                    // single number would be wrong half the time by construction.
                    // State the typical case and the tail together -- without
                    // this, the success branch was the only one that named no
                    // duration at all, leaving the user whose acceleration works
                    // the least informed about the wait ahead.
                    disclosure << tr("Graphics acceleration is ready for this network. Preparing a transfer usually takes one to two minutes, but the search is random: some transfers finish in seconds and some run past five minutes. You can stop the work at any time.");
                    break;
                case GpuSolverProbeStatus::Failed:
                    disclosure << tr("Graphics acceleration did not start. The processor will take over, which may take many minutes. Check the transfer helper in Settings → Options → Main.");
                    break;
                case GpuSolverProbeStatus::TimedOut:
                    disclosure << tr("The graphics acceleration check timed out. The processor will take over, which may take many minutes.");
                    break;
                case GpuSolverProbeStatus::Unchecked:
                    disclosure << tr("A transfer helper is configured. The desktop will check it before preparation starts.");
                    break;
                }
            }
        }
    } else {
        disclosure << tr("Sandbox transfers use quick built-in preparation and do not need graphics acceleration.");
    }

    return disclosure.join(QLatin1Char('\n'));
}

bool SendCoinsDialog::cpuFallbackWarningRequired(bool slow_network, GpuSolverProbeStatus probe_status, bool warning_enabled)
{
    return slow_network && probe_status != GpuSolverProbeStatus::Available && warning_enabled;
}

void SendCoinsDialog::showStartupAccelerationWarningIfNeeded()
{
    if (g_startup_acceleration_warning_shown || Params().GetConsensus().nTxEdgeBits != 28) return;
    if (m_gpu_state.has_gpu && m_gpu_state.is_configured) return;

    g_startup_acceleration_warning_shown = true;
    auto* box = new QMessageBox{QMessageBox::Information, tr("Transfer acceleration"), QString(), QMessageBox::NoButton, this};
    box->setObjectName(QStringLiteral("cpuFallbackStartupWarning"));
    if (m_gpu_state.has_gpu) {
        box->setText(tr("Graphics acceleration is available, but no transfer helper is configured."));
        box->setInformativeText(tr("Quicksilver can still send using this computer's processor, but preparing a transfer may take many minutes. Choose a transfer helper in Settings for faster preparation."));
    } else {
        box->setText(tr("Graphics acceleration was not detected on this computer."));
        box->setInformativeText(tr("Quicksilver can still send using this computer's processor, but preparing a transfer may take many minutes. The transfer screen will stay responsive and lets you stop the work at any time."));
    }
    QPushButton* settings_button{box->addButton(tr("Open settings"), QMessageBox::ActionRole)};
    QPushButton* continue_button{box->addButton(tr("Continue"), QMessageBox::AcceptRole)};
    box->setDefaultButton(continue_button);
    connect(box, &QMessageBox::finished, this, [this, box, settings_button] {
        if (box->clickedButton() == settings_button) Q_EMIT solverSettingsRequested();
    });
    GUIUtil::ShowModalDialogAsynchronously(box);
}

void SendCoinsDialog::confirmCpuFallbackIfNeededAndPrepare(std::unique_ptr<VaultModelTransaction> transaction, CCoinControl coin_control)
{
    const bool warning_enabled{model && model->getOptionsModel()
        ? model->getOptionsModel()->getOption(OptionsModel::ShowCpuFallbackWarning).toBool()
        : true};
    if (!cpuFallbackWarningRequired(Params().GetConsensus().nTxEdgeBits == 28, m_gpu_solver_probe_status, warning_enabled)) {
        startAsyncSendPrepare(std::move(transaction), coin_control);
        return;
    }

    auto* box = new QMessageBox{QMessageBox::Warning, tr("Slower transfer preparation"),
                    tr("Graphics acceleration is not ready, so Quicksilver will use this computer's processor."),
                    QMessageBox::NoButton, this};
    box->setObjectName(QStringLiteral("cpuFallbackSendWarning"));
    box->setInformativeText(tr("This may take many minutes. The transfer screen will stay responsive, and you can stop the work at any time."));
    auto* suppress_warning{new QCheckBox(tr("Don't warn me again"), box)};
    suppress_warning->setObjectName(QStringLiteral("cpuFallbackDontWarnAgain"));
    box->setCheckBox(suppress_warning);
    QPushButton* cancel_button{box->addButton(QMessageBox::Cancel)};
    QPushButton* proceed_button{box->addButton(tr("Use processor"), QMessageBox::AcceptRole)};
    proceed_button->setObjectName(QStringLiteral("cpuFallbackProceedButton"));
    box->setDefaultButton(cancel_button);
    auto pending = std::make_shared<std::unique_ptr<VaultModelTransaction>>(std::move(transaction));
    connect(box, &QMessageBox::finished, this, [this, box, proceed_button, suppress_warning, pending, coin_control]() {
        const bool proceed = box->clickedButton() == proceed_button;
        const bool suppress = suppress_warning->isChecked();
        QTimer::singleShot(0, this, [this, proceed, suppress, pending, coin_control]() mutable {
            if (!proceed) {
                fNewRecipientAllowed = true;
                return;
            }
            if (suppress && model && model->getOptionsModel()) {
                model->getOptionsModel()->setOption(OptionsModel::ShowCpuFallbackWarning, false);
            }
            startAsyncSendPrepare(std::move(*pending), coin_control);
        });
    });
    GUIUtil::ShowModalDialogAsynchronously(box);
}

void SendCoinsDialog::startAsyncSendPrepare(std::unique_ptr<VaultModelTransaction> transaction, CCoinControl coin_control)
{
    const quint64 generation = ++m_solve_generation;
    setSendControlsEnabled(false);
    beginSendWorkProgress();

    VaultModel* vault_model = model;
    QPointer<SendCoinsDialog> self(this);
    QThread* thread = QThread::create([self, vault_model, transaction = std::move(transaction), coin_control, generation]() mutable {
        auto progress_callback = [self, generation](uint32_t nonce) {
            if (!self) return;
            QMetaObject::invokeMethod(self, [self, generation, nonce]() {
                if (!self || !self->acceptSolveResult(generation)) return;
                self->updateSendWorkGraphs(nonce);
            }, Qt::QueuedConnection);
        };
        VaultModel::SendCoinsReturn status = vault_model->prepareTransaction(*transaction, coin_control, progress_callback);
        auto result = QSharedPointer<PreparedSendResult>::create(PreparedSendResult{
            std::move(transaction),
            status,
            generation,
        });

        if (!self) {
            return;
        }

        QMetaObject::invokeMethod(self, [self, result]() {
            if (!self) return;
            self->finishSendPrepare(std::move(result->transaction), result->status, result->generation);
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void SendCoinsDialog::sendButtonClicked([[maybe_unused]] bool checked)
{
    if(!model || !model->getOptionsModel())
        return;

    std::unique_ptr<VaultModelTransaction> transaction;
    CCoinControl coin_control;
    if (!prepareTransactionForAsync(transaction, coin_control)) return;
    QPointer<SendCoinsDialog> self(this);
    auto pending = std::make_shared<std::unique_ptr<VaultModelTransaction>>(std::move(transaction));
    model->requestUnlock([self, pending, coin_control](std::shared_ptr<VaultModel::UnlockContext> ctx) {
        if (!self) return;
        if (!ctx || !ctx->isValid()) {
            self->fNewRecipientAllowed = true;
            return;
        }
        self->confirmCpuFallbackIfNeededAndPrepare(std::move(*pending), coin_control);
    });
}

void SendCoinsDialog::bumpSolveGeneration()
{
    ++m_solve_generation;
    // Reach into the grind as well as disowning its result. Whoever asked for this
    // is entitled to have the machine stop working on their behalf.
    if (model) model->requestProofOfWorkCancel();
}

void SendCoinsDialog::cancelSendWork()
{
    bumpSolveGeneration();
    setSendControlsEnabled(true);
    fNewRecipientAllowed = true;
    m_current_transaction.reset();
    m_send_work_update_timer.stop();
    updateSendWorkElapsed();
    ui->sendWorkStatusLabel->setText(tr("Proof-of-work canceled"));
    ui->sendWorkGraphsLabel->setText(tr("Graphs tried: canceled"));
    ui->sendWorkProgressBar->setRange(0, 1);
    ui->sendWorkProgressBar->setValue(0);
    ui->sendWorkProgressBar->setFormat(tr("Canceled"));
    ui->sendWorkCancelButton->setEnabled(false);
    ui->sendWorkProgressPanel->show();
}

void SendCoinsDialog::clear()
{
    bumpSolveGeneration();
    setSendControlsEnabled(true);
    hideSendWorkProgress();
    fNewRecipientAllowed = true;
    m_current_transaction.reset();

    // Clear coin control settings
    m_coin_control->UnSelectAll();
    ui->checkBoxCoinControlChange->setChecked(false);
    ui->lineEditCoinControlChange->clear();
    coinControlUpdateLabels();

    // Remove entries until only one left
    while(ui->entries->count())
    {
        ui->entries->takeAt(0)->widget()->deleteLater();
    }
    addEntry();

    updateTabsAndLabels();
}

void SendCoinsDialog::reject()
{
    clear();
}

void SendCoinsDialog::accept()
{
    clear();
}

SendCoinsEntry *SendCoinsDialog::addEntry()
{
    SendCoinsEntry *entry = new SendCoinsEntry(platformStyle, this);
    entry->setModel(model);
    ui->entries->addWidget(entry);
    connect(entry, &SendCoinsEntry::removeEntry, this, &SendCoinsDialog::removeEntry);
    connect(entry, &SendCoinsEntry::useAvailableBalance, this, &SendCoinsDialog::useAvailableBalance);
    connect(entry, &SendCoinsEntry::payAmountChanged, this, &SendCoinsDialog::coinControlUpdateLabels);

    // Focus the field, so that entry can start immediately
    entry->clear();
    entry->setFocus();
    ui->scrollAreaWidgetContents->resize(ui->scrollAreaWidgetContents->sizeHint());

    // Scroll to the newly added entry on a QueuedConnection because Qt doesn't
    // adjust the scroll area and scrollbar immediately when the widget is added.
    // Invoking on a DirectConnection will only scroll to the second-to-last entry.
    QMetaObject::invokeMethod(ui->scrollArea, [this] {
        if (ui->scrollArea->verticalScrollBar()) {
            ui->scrollArea->verticalScrollBar()->setValue(ui->scrollArea->verticalScrollBar()->maximum());
        }
    }, Qt::QueuedConnection);

    updateTabsAndLabels();
    return entry;
}

void SendCoinsDialog::updateTabsAndLabels()
{
    setupTabChain(nullptr);
    coinControlUpdateLabels();
}

void SendCoinsDialog::removeEntry(SendCoinsEntry* entry)
{
    entry->hide();

    // If the last entry is about to be removed add an empty one
    if (ui->entries->count() == 1)
        addEntry();

    entry->deleteLater();

    updateTabsAndLabels();
}

QWidget *SendCoinsDialog::setupTabChain(QWidget *prev)
{
    for(int i = 0; i < ui->entries->count(); ++i)
    {
        SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
        if(entry)
        {
            prev = entry->setupTabChain(prev);
        }
    }
    QWidget::setTabOrder(prev, ui->sendButton);
    QWidget::setTabOrder(ui->sendButton, ui->clearButton);
    QWidget::setTabOrder(ui->clearButton, ui->addButton);
    return ui->addButton;
}

void SendCoinsDialog::setAddress(const QString &address)
{
    SendCoinsEntry *entry = nullptr;
    // Replace the first entry if it is still unused
    if(ui->entries->count() == 1)
    {
        SendCoinsEntry *first = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(0)->widget());
        if(first->isClear())
        {
            entry = first;
        }
    }
    if(!entry)
    {
        entry = addEntry();
    }

    entry->setAddress(address);
}

void SendCoinsDialog::pasteEntry(const SendCoinsRecipient &rv)
{
    if(!fNewRecipientAllowed)
        return;

    SendCoinsEntry *entry = nullptr;
    // Replace the first entry if it is still unused
    if(ui->entries->count() == 1)
    {
        SendCoinsEntry *first = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(0)->widget());
        if(first->isClear())
        {
            entry = first;
        }
    }
    if(!entry)
    {
        entry = addEntry();
    }

    entry->setValue(rv);
    updateTabsAndLabels();
}

bool SendCoinsDialog::handlePaymentRequest(const SendCoinsRecipient &rv)
{
    // Just paste the entry, all pre-checks
    // are done in paymentserver.cpp.
    pasteEntry(rv);
    return true;
}

void SendCoinsDialog::setBalance(const interfaces::VaultBalances& balances)
{
    if(model && model->getOptionsModel())
    {
        if (model->vault().hasExternalSigner()) {
            ui->labelBalanceName->setText(tr("External balance:"));
        }
        ui->labelBalance->setText(QuicksilverUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), balances.balance));
    }
}

void SendCoinsDialog::refreshBalance()
{
    setBalance(model->getCachedBalance());
}

void SendCoinsDialog::processSendCoinsReturn(const VaultModel::SendCoinsReturn &sendCoinsReturn, const QString &msgArg)
{
    QPair<QString, CClientUIInterface::MessageBoxFlags> msgParams;
    // Default to a warning message, override if error message is needed
    msgParams.second = CClientUIInterface::MSG_WARNING;

    // This comment is specific to SendCoinsDialog usage of VaultModel::SendCoinsReturn.
    // All status values are used only in VaultModel::prepareTransaction()
    switch(sendCoinsReturn.status)
    {
    case VaultModel::InvalidAddress:
        msgParams.first = tr("The recipient address is not valid. Please recheck.");
        break;
    case VaultModel::InvalidAmount:
        msgParams.first = tr("The transfer amount must be larger than 0.");
        break;
    case VaultModel::AmountExceedsBalance:
        msgParams.first = tr("The amount exceeds your balance.");
        break;
    case VaultModel::DuplicateAddress:
        msgParams.first = tr("Duplicate address found: addresses should only be used once each.");
        break;
    case VaultModel::VaultSyncUnavailable:
        msgParams.first = tr("Transfer is waiting for vault header sync. Start consensus or wait for the thin header source before sending.");
        break;
    case VaultModel::TransactionCreationFailed:
        // Prefer what the layer that failed had to say. Only when nothing was
        // reported does the transfer get described in the abstract.
        msgParams.first = sendCoinsReturn.reason.isEmpty()
            ? tr("The transfer could not be prepared.")
            : sendCoinsReturn.reason;
        msgParams.second = CClientUIInterface::MSG_ERROR;
        break;
    case VaultModel::TransactionCommitFailed:
        msgParams.first = sendCoinsReturn.reason.isEmpty()
            ? tr("The transfer was rejected and was not saved to the vault.")
            : sendCoinsReturn.reason;
        msgParams.second = CClientUIInterface::MSG_ERROR;
        break;
    // included to prevent a compiler warning.
    case VaultModel::OK:
    default:
        return;
    }

    if (sendCoinsReturn.solver_configuration_required) {
        auto* box = new QMessageBox(QMessageBox::Critical, tr("Transfer Quicksilver"), msgParams.first,
                                    QMessageBox::NoButton, this);
        QPushButton* settings_button = box->addButton(tr("Open solver settings"), QMessageBox::ActionRole);
        box->addButton(QMessageBox::Ok);
        connect(box, &QMessageBox::finished, this, [this, box, settings_button] {
            if (box->clickedButton() == settings_button) Q_EMIT solverSettingsRequested();
        });
        GUIUtil::ShowModalDialogAsynchronously(box);
        return;
    }

    Q_EMIT message(tr("Transfer Quicksilver"), msgParams.first, msgParams.second);
}

void SendCoinsDialog::useAvailableBalance(SendCoinsEntry* entry)
{
    // Same behavior as send: if we have selected coins, only obtain their available balance.
    // Copy to avoid modifying the member's data.
    CCoinControl coin_control = *m_coin_control;
    coin_control.m_allow_other_inputs = !coin_control.HasSelected();

    // Calculate available amount to send.
    CAmount amount = model->getAvailableBalance(&coin_control);
    for (int i = 0; i < ui->entries->count(); ++i) {
        SendCoinsEntry* e = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
        if (e && !e->isHidden() && e != entry) {
            amount -= e->getValue().amount;
        }
    }

    if (amount > 0) {
      entry->setAmount(amount);
    } else {
      entry->setAmount(0);
    }
}

void SendCoinsDialog::updateNumberOfBlocks(int count, const QDateTime& blockDate, double nVerificationProgress, SyncType synctype, SynchronizationState sync_state) {
    // During shutdown, clientModel will be nullptr. Attempting to update views at this point may cause a crash
    // due to accessing backend models that might no longer exist.
    if (!clientModel) return;
}

// Coin Control: copy label "Quantity" to clipboard
void SendCoinsDialog::coinControlClipboardQuantity()
{
    GUIUtil::setClipboard(ui->labelCoinControlQuantity->text());
}

// Coin Control: copy label "Amount" to clipboard
void SendCoinsDialog::coinControlClipboardAmount()
{
    GUIUtil::setClipboard(ui->labelCoinControlAmount->text().left(ui->labelCoinControlAmount->text().indexOf(" ")));
}

// Coin Control: copy label "Bytes" to clipboard
void SendCoinsDialog::coinControlClipboardBytes()
{
    GUIUtil::setClipboard(ui->labelCoinControlBytes->text().replace(ASYMP_UTF8, ""));
}

// Coin Control: copy label "Change" to clipboard
void SendCoinsDialog::coinControlClipboardChange()
{
    GUIUtil::setClipboard(ui->labelCoinControlChange->text().left(ui->labelCoinControlChange->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// Coin Control: settings menu - coin control enabled/disabled by user
void SendCoinsDialog::coinControlFeatureChanged(bool checked)
{
    ui->frameCoinControl->setVisible(checked);

    if (!checked && model) { // input selection disabled
        m_coin_control = std::make_unique<CCoinControl>();
    }

    coinControlUpdateLabels();
}

// Coin Control: button inputs -> show actual coin control dialog
void SendCoinsDialog::coinControlButtonClicked()
{
    auto dlg = new CoinControlDialog(*m_coin_control, model, platformStyle);
    connect(dlg, &QDialog::finished, this, &SendCoinsDialog::coinControlUpdateLabels);
    GUIUtil::ShowModalDialogAsynchronously(dlg);
}

// Coin Control: checkbox custom change address
void SendCoinsDialog::coinControlChangeChecked(int state)
{
    if (state == Qt::Unchecked)
    {
        m_coin_control->destChange = CNoDestination();
        ui->labelCoinControlChangeLabel->clear();
    }
    else
        // use this to re-validate an already entered address
        coinControlChangeEdited(ui->lineEditCoinControlChange->text());

    ui->lineEditCoinControlChange->setEnabled((state == Qt::Checked));
}

// Coin Control: custom change address changed
void SendCoinsDialog::coinControlChangeEdited(const QString& text)
{
    if (model && model->getAddressTableModel())
    {
        // Default to no change address until verified
        m_coin_control->destChange = CNoDestination();
        ui->labelCoinControlChangeLabel->setStyleSheet("QLabel{color:red;}");

        const CTxDestination dest = DecodeDestination(text.toStdString());

        if (text.isEmpty()) // Nothing entered
        {
            ui->labelCoinControlChangeLabel->setText("");
        }
        else if (!IsValidDestination(dest)) // Invalid address
        {
            ui->labelCoinControlChangeLabel->setText(tr("Warning: Invalid Quicksilver address"));
        }
        else // Valid address
        {
            if (!model->vault().isSpendable(dest)) {
                ui->labelCoinControlChangeLabel->setText(tr("Warning: Unknown change address"));

                if (findChild<QMessageBox*>(QStringLiteral("customChangeAddressConfirm"))) {
                    return;
                }

                auto* box = new QMessageBox(this);
                box->setObjectName(QStringLiteral("customChangeAddressConfirm"));
                box->setIcon(QMessageBox::Question);
                box->setWindowTitle(tr("Confirm custom change address"));
                box->setText(tr("The address you selected for change is not part of this vault. Any or all funds in your vault may be sent to this address. Are you sure?"));
                box->setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
                box->setDefaultButton(QMessageBox::Cancel);
                const QString confirmed_text = text;
                QPointer<SendCoinsDialog> self{this};
                GUIUtil::ShowModalMessageBoxAsynchronously(box, [self, dest, confirmed_text](int result, QAbstractButton*) {
                    if (!self || self->ui->lineEditCoinControlChange->text() != confirmed_text) return;
                    if (result == QMessageBox::Yes) {
                        self->m_coin_control->destChange = dest;
                        return;
                    }
                    self->ui->lineEditCoinControlChange->setText("");
                    self->ui->labelCoinControlChangeLabel->setStyleSheet("QLabel{color:black;}");
                    self->ui->labelCoinControlChangeLabel->setText("");
                });
            }
            else // Known change address
            {
                ui->labelCoinControlChangeLabel->setStyleSheet("QLabel{color:black;}");

                // Query label
                QString associatedLabel = model->getAddressTableModel()->labelForAddress(text);
                if (!associatedLabel.isEmpty())
                    ui->labelCoinControlChangeLabel->setText(associatedLabel);
                else
                    ui->labelCoinControlChangeLabel->setText(tr("(no label)"));

                m_coin_control->destChange = dest;
            }
        }
    }
}

// Coin Control: update labels
void SendCoinsDialog::coinControlUpdateLabels()
{
    if (!model || !model->getOptionsModel())
        return;

    // set pay amounts
    CoinControlDialog::payAmounts.clear();

    for(int i = 0; i < ui->entries->count(); ++i)
    {
        SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
        if(entry && !entry->isHidden())
        {
            SendCoinsRecipient rcp = entry->getValue();
            CoinControlDialog::payAmounts.append(rcp.amount);
        }
    }

    if (m_coin_control->HasSelected())
    {
        // actual coin control calculation
        CoinControlDialog::updateLabels(*m_coin_control, model, this);

        // show coin control stats
        ui->labelCoinControlAutomaticallySelected->hide();
        ui->widgetCoinControl->show();
    }
    else
    {
        // hide coin control stats
        ui->labelCoinControlAutomaticallySelected->show();
        ui->widgetCoinControl->hide();
        ui->labelCoinControlInsuffFunds->hide();
    }
}

SendConfirmationDialog::SendConfirmationDialog(const QString& title, const QString& text, const QString& informative_text, const QString& detailed_text, int _secDelay, bool enable_send, bool always_show_unsigned, QWidget* parent)
    : QMessageBox(parent), secDelay(_secDelay), m_enable_send(enable_send)
{
    setIcon(QMessageBox::Question);
    setWindowTitle(title); // On macOS, the window title is ignored (as required by the macOS Guidelines).
    setText(text);
    setInformativeText(informative_text);
    setDetailedText(detailed_text);
    setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
    if (always_show_unsigned || !enable_send) addButton(QMessageBox::Save);
    setDefaultButton(QMessageBox::Cancel);
    yesButton = button(QMessageBox::Yes);
    if (confirmButtonText.isEmpty()) {
        confirmButtonText = yesButton->text();
    }
    m_psqt_button = button(QMessageBox::Save);
    updateButtons();
    connect(&countDownTimer, &QTimer::timeout, this, &SendConfirmationDialog::countDown);
}

void SendConfirmationDialog::showEvent(QShowEvent* event)
{
    QMessageBox::showEvent(event);
    updateButtons();
    if (!countDownTimer.isActive()) {
        countDownTimer.start(1s);
    }
}

void SendConfirmationDialog::countDown()
{
    secDelay--;
    updateButtons();

    if(secDelay <= 0)
    {
        countDownTimer.stop();
    }
}

void SendConfirmationDialog::updateButtons()
{
    if(secDelay > 0)
    {
        yesButton->setEnabled(false);
        yesButton->setText(confirmButtonText + (m_enable_send ? (" (" + QString::number(secDelay) + ")") : QString("")));
        if (m_psqt_button) {
            m_psqt_button->setEnabled(false);
            m_psqt_button->setText(m_psqt_button_text + " (" + QString::number(secDelay) + ")");
        }
    }
    else
    {
        yesButton->setEnabled(m_enable_send);
        yesButton->setText(confirmButtonText);
        if (m_psqt_button) {
            m_psqt_button->setEnabled(true);
            m_psqt_button->setText(m_psqt_button_text);
        }
    }
}
