// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <quicksilver-build-config.h> // IWYU pragma: keep

#include <qt/optionsdialog.h>
#include <qt/forms/ui_optionsdialog.h>

#include <qt/quicksilverunits.h>
#include <qt/clientmodel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>

#include <chainparams.h>
#include <common/system.h>
#include <interfaces/node.h>
#include <netbase.h>
#include <node/caches.h>
#include <node/chainstatemanager_args.h>
#include <util/strencodings.h>

#include <chrono>
#include <memory>

#include <QApplication>
#include <QDataWidgetMapper>
#include <QFileInfo>
#include <QFrame>
#include <QIntValidator>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVBoxLayout>

int setFontChoice(QComboBox* cb, const OptionsModel::FontChoice& fc)
{
    int i;
    for (i = cb->count(); --i >= 0; ) {
        QVariant item_data = cb->itemData(i);
        if (!item_data.canConvert<OptionsModel::FontChoice>()) continue;
        if (item_data.value<OptionsModel::FontChoice>() == fc) {
            break;
        }
    }
    if (i == -1) {
        // New item needed
        QFont chosen_font = OptionsModel::getFontForChoice(fc);
        QSignalBlocker block_currentindexchanged_signal(cb);  // avoid triggering QFontDialog
        cb->insertItem(0, QFontInfo(chosen_font).family(), QVariant::fromValue(fc));
        i = 0;
    }

    cb->setCurrentIndex(i);
    return i;
}

void setupFontOptions(QComboBox* cb, QLabel* preview)
{
    QFont embedded_font{GUIUtil::fixedPitchFont(true)};
    QFont system_font{GUIUtil::fixedPitchFont(false)};
    cb->addItem(QObject::tr("Embedded \"%1\"").arg(QFontInfo(embedded_font).family()), QVariant::fromValue(OptionsModel::FontChoice{OptionsModel::FontChoiceAbstract::EmbeddedFont}));
    cb->addItem(QObject::tr("Default system font \"%1\"").arg(QFontInfo(system_font).family()), QVariant::fromValue(OptionsModel::FontChoice{OptionsModel::FontChoiceAbstract::BestSystemFont}));
    cb->addItem(QObject::tr("Custom…"));

    auto previous_index = std::make_shared<int>(-1);
    const auto& on_font_choice_changed = [cb, preview, previous_index](int index) {
        QVariant item_data = cb->itemData(index);
        QFont f;
        if (item_data.canConvert<OptionsModel::FontChoice>()) {
            f = OptionsModel::getFontForChoice(item_data.value<OptionsModel::FontChoice>());
        } else {
            QPointer<QComboBox> combo{cb};
            QPointer<QLabel> preview_label{preview};
            GUIUtil::getFont(cb->parentWidget(), GUIUtil::fixedPitchFont(false),
                [combo, preview_label, previous_index](const QFont& chosen, bool ok) {
                    if (!combo) return;
                    if (!ok) {
                        combo->setCurrentIndex(*previous_index);
                        return;
                    }
                    int chosen_index = setFontChoice(combo, OptionsModel::FontChoice{chosen});
                    if (preview_label) preview_label->setFont(chosen);
                    *previous_index = chosen_index;
                });
            return;
        }
        if (preview) {
            preview->setFont(f);
        }
        *previous_index = index;
    };
    QObject::connect(cb, QOverload<int>::of(&QComboBox::currentIndexChanged), on_font_choice_changed);
    on_font_choice_changed(cb->currentIndex());
}

OptionsDialog::OptionsDialog(QWidget* parent, bool enableVault)
    : QDialog(parent, GUIUtil::dialog_flags | Qt::WindowMaximizeButtonHint),
      ui(new Ui::OptionsDialog)
{
    ui->setupUi(this);
    setProperty("class", QStringLiteral("quicksilverDialog"));
    setMinimumSize(QSize(700, 540));
    ui->verticalLayout->setContentsMargins(18, 16, 18, 18);
    ui->verticalLayout->setSpacing(12);
    ui->tabWidget->setDocumentMode(true);

    auto* header = new QFrame(this);
    header->setObjectName(QStringLiteral("optionsHeader"));
    auto* header_layout = new QVBoxLayout(header);
    header_layout->setContentsMargins(2, 0, 2, 2);
    header_layout->setSpacing(2);

    auto* eyebrow = new QLabel(tr("Application controls").toUpper(), header);
    eyebrow->setObjectName(QStringLiteral("optionsEyebrow"));
    eyebrow->setProperty("class", QStringLiteral("pageEyebrow"));
    header_layout->addWidget(eyebrow);

    auto* title = new QLabel(tr("Quicksilver options"), header);
    title->setObjectName(QStringLiteral("optionsTitle"));
    title->setProperty("class", QStringLiteral("pageTitle"));
    header_layout->addWidget(title);

    auto* subtitle = new QLabel(tr("Node, vault, network, window, and display settings."), header);
    subtitle->setObjectName(QStringLiteral("optionsSubtitle"));
    subtitle->setProperty("class", QStringLiteral("muted"));
    subtitle->setWordWrap(true);
    header_layout->addWidget(subtitle);
    ui->verticalLayout->insertWidget(0, header);

    ui->verticalLayout->setStretchFactor(ui->tabWidget, 1);

    /* Main elements init */
    ui->databaseCache->setRange(MIN_DB_CACHE >> 20, std::numeric_limits<int>::max());
    ui->threadsScriptVerif->setMinimum(-GetNumCores());
    ui->threadsScriptVerif->setMaximum(MAX_SCRIPTCHECK_THREADS);
    ui->pruneWarning->setVisible(false);
    ui->pruneWarning->setStyleSheet("QLabel { color: red; }");

    ui->pruneSize->setEnabled(false);
    connect(ui->prune, &QPushButton::toggled, ui->pruneSize, &QWidget::setEnabled);

    /* Network elements init */
    ui->proxyIp->setEnabled(false);
    ui->proxyPort->setEnabled(false);
    ui->proxyPort->setValidator(new QIntValidator(1, 65535, this));

    ui->proxyIpTor->setEnabled(false);
    ui->proxyPortTor->setEnabled(false);
    ui->proxyPortTor->setValidator(new QIntValidator(1, 65535, this));

    connect(ui->connectSocks, &QPushButton::toggled, ui->proxyIp, &QWidget::setEnabled);
    connect(ui->connectSocks, &QPushButton::toggled, ui->proxyPort, &QWidget::setEnabled);
    connect(ui->connectSocks, &QPushButton::toggled, this, &OptionsDialog::updateProxyValidationState);

    connect(ui->connectSocksTor, &QPushButton::toggled, ui->proxyIpTor, &QWidget::setEnabled);
    connect(ui->connectSocksTor, &QPushButton::toggled, ui->proxyPortTor, &QWidget::setEnabled);
    connect(ui->connectSocksTor, &QPushButton::toggled, this, &OptionsDialog::updateProxyValidationState);
    connect(ui->gpuSolverBrowseButton, &QPushButton::clicked, this, &OptionsDialog::chooseGpuSolverPath);

    /* Window elements init */
#ifdef Q_OS_MACOS
    /* remove Window tab on Mac */
    ui->tabWidget->removeTab(ui->tabWidget->indexOf(ui->tabWindow));
    /* hide launch at startup option on macOS */
    ui->quicksilverAtStartup->setVisible(false);
    ui->verticalLayout_Main->removeWidget(ui->quicksilverAtStartup);
    ui->verticalLayout_Main->removeItem(ui->horizontalSpacer_0_Main);
#endif

    /* remove Vault tab and 3rd party-URL textbox in case of -disablevault */
    if (!enableVault) {
        ui->tabWidget->removeTab(ui->tabWidget->indexOf(ui->tabVault));
        ui->thirdPartyTxUrlsLabel->setVisible(false);
        ui->thirdPartyTxUrls->setVisible(false);
    }

#ifdef ENABLE_EXTERNAL_SIGNER
    ui->externalSignerPath->setToolTip(ui->externalSignerPath->toolTip().arg(CLIENT_NAME));
#else
    // Hide the whole group box rather than greying the field out. A permanently
    // disabled control with a "compiled without support" tooltip reads as a
    // feature that is on its way; external signing is deliberately absent from
    // v1, not pending. Hiding the box takes its title and the path label with it.
    ui->groupBoxHww->setVisible(false);
    ui->externalSignerPath->setEnabled(false);
#endif
    /* Display elements init */
    ui->quicksilverAtStartup->setToolTip(ui->quicksilverAtStartup->toolTip().arg(CLIENT_NAME));
    ui->quicksilverAtStartup->setText(ui->quicksilverAtStartup->text().arg(CLIENT_NAME));

    ui->openQuicksilverConfButton->setToolTip(ui->openQuicksilverConfButton->toolTip().arg(CLIENT_NAME));

    ui->lang->setToolTip(ui->lang->toolTip().arg(CLIENT_NAME));
    ui->lang->addItem(QString("(") + tr("default") + QString(")"), QVariant(""));
    // Quicksilver's own UI is English-only. Keep an explicit English choice so
    // users can also force Qt's standard dialog strings to English instead of
    // inheriting the system locale.
    ui->lang->addItem(tr("English (en)"), QStringLiteral("en"));
    ui->unit->setModel(new QuicksilverUnits(this));

    /* Widget-to-option mapper */
    mapper = new QDataWidgetMapper(this);
    mapper->setSubmitPolicy(QDataWidgetMapper::ManualSubmit);
    mapper->setOrientation(Qt::Vertical);

    GUIUtil::ItemDelegate* delegate = new GUIUtil::ItemDelegate(mapper);
    connect(delegate, &GUIUtil::ItemDelegate::keyEscapePressed, this, &OptionsDialog::reject);
    mapper->setItemDelegate(delegate);

    /* setup/change UI elements when proxy IPs are invalid/valid */
    ui->proxyIp->setCheckValidator(new ProxyAddressValidator(parent));
    ui->proxyIpTor->setCheckValidator(new ProxyAddressValidator(parent));
    connect(ui->proxyIp, &QValidatedLineEdit::validationDidChange, this, &OptionsDialog::updateProxyValidationState);
    connect(ui->proxyIpTor, &QValidatedLineEdit::validationDidChange, this, &OptionsDialog::updateProxyValidationState);
    connect(ui->proxyPort, &QLineEdit::textChanged, this, &OptionsDialog::updateProxyValidationState);
    connect(ui->proxyPortTor, &QLineEdit::textChanged, this, &OptionsDialog::updateProxyValidationState);

    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        ui->showTrayIcon->setChecked(false);
        ui->showTrayIcon->setEnabled(false);
        ui->minimizeToTray->setChecked(false);
        ui->minimizeToTray->setEnabled(false);
    }

    setupFontOptions(ui->moneyFont, ui->moneyFont_preview);

    GUIUtil::handleCloseWindowShortcut(this);
}

OptionsDialog::~OptionsDialog()
{
    stopGpuSolverProbe();
    delete ui;
}

void OptionsDialog::setClientModel(ClientModel* client_model)
{
    m_client_model = client_model;
}

void OptionsDialog::setModel(OptionsModel *_model)
{
    this->model = _model;

    if(_model)
    {
        /* check if client restart is needed and show persistent message */
        if (_model->isRestartRequired())
            showRestartWarning(true);

        // Prune values are in GB to be consistent with intro.cpp
        static constexpr uint64_t nMinDiskSpace = (MIN_DISK_SPACE_FOR_BLOCK_FILES / GB_BYTES) + (MIN_DISK_SPACE_FOR_BLOCK_FILES % GB_BYTES) ? 1 : 0;
        ui->pruneSize->setRange(nMinDiskSpace, std::numeric_limits<int>::max());

        QString strLabel = _model->getOverriddenByCommandLine();
        if (strLabel.isEmpty())
            strLabel = tr("none");
        ui->overriddenByCommandLineLabel->setText(strLabel);

        mapper->setModel(_model);
        setMapper();
        mapper->toFirst();

        const auto& font_for_money = _model->data(_model->index(OptionsModel::FontForMoney, 0), Qt::EditRole).value<OptionsModel::FontChoice>();
        setFontChoice(ui->moneyFont, font_for_money);

        updateDefaultProxyNets();
    }

    /* warn when one of the following settings changes by user action (placed here so init via mapper doesn't trigger them) */

    /* Main */
    connect(ui->prune, &QCheckBox::clicked, this, &OptionsDialog::showRestartWarning);
    connect(ui->prune, &QCheckBox::clicked, this, &OptionsDialog::togglePruneWarning);
    connect(ui->pruneSize, qOverload<int>(&QSpinBox::valueChanged), this, &OptionsDialog::showRestartWarning);
    connect(ui->databaseCache, qOverload<int>(&QSpinBox::valueChanged), this, &OptionsDialog::showRestartWarning);
    connect(ui->gpuSolverPath, &QLineEdit::textChanged, this, [this] {
        showRestartWarning();
        validateGpuSolverPath();
    });
    connect(ui->externalSignerPath, &QLineEdit::textChanged, [this]{ showRestartWarning(); });
    connect(ui->threadsScriptVerif, qOverload<int>(&QSpinBox::valueChanged), this, &OptionsDialog::showRestartWarning);
    /* Vault */
    connect(ui->spendZeroConfChange, &QCheckBox::clicked, this, &OptionsDialog::showRestartWarning);
    /* Network */
    connect(ui->allowIncoming, &QCheckBox::clicked, this, &OptionsDialog::showRestartWarning);
    connect(ui->enableServer, &QCheckBox::clicked, this, &OptionsDialog::showRestartWarning);
    connect(ui->connectSocks, &QCheckBox::clicked, this, &OptionsDialog::showRestartWarning);
    connect(ui->connectSocksTor, &QCheckBox::clicked, this, &OptionsDialog::showRestartWarning);
    /* Display */
    connect(ui->lang, qOverload<>(&QValueComboBox::valueChanged), [this]{ showRestartWarning(); });
    connect(ui->thirdPartyTxUrls, &QLineEdit::textChanged, [this]{ showRestartWarning(); });
    validateGpuSolverPath();
}

void OptionsDialog::setCurrentTab(OptionsDialog::Tab tab)
{
    QWidget *tab_widget = nullptr;
    if (tab == OptionsDialog::Tab::TAB_NETWORK) tab_widget = ui->tabNetwork;
    if (tab == OptionsDialog::Tab::TAB_MAIN) tab_widget = ui->tabMain;
    if (tab_widget && ui->tabWidget->currentWidget() != tab_widget) {
        ui->tabWidget->setCurrentWidget(tab_widget);
    }
}

void OptionsDialog::setMapper()
{
    /* Main */
    mapper->addMapping(ui->quicksilverAtStartup, OptionsModel::StartAtStartup);
    mapper->addMapping(ui->threadsScriptVerif, OptionsModel::ThreadsScriptVerif);
    mapper->addMapping(ui->databaseCache, OptionsModel::DatabaseCache);
    mapper->addMapping(ui->gpuSolverPath, OptionsModel::GpuSolverPath);
    mapper->addMapping(ui->prune, OptionsModel::Prune);
    mapper->addMapping(ui->pruneSize, OptionsModel::PruneSize);

    /* Vault */
    mapper->addMapping(ui->spendZeroConfChange, OptionsModel::SpendZeroConfChange);
    mapper->addMapping(ui->coinControlFeatures, OptionsModel::CoinControlFeatures);
    mapper->addMapping(ui->externalSignerPath, OptionsModel::ExternalSignerPath);
    mapper->addMapping(ui->m_enable_psqt_controls, OptionsModel::EnablePSQTControls);

    /* Network */
    mapper->addMapping(ui->mapPortNatpmp, OptionsModel::MapPortNatpmp);
    mapper->addMapping(ui->allowIncoming, OptionsModel::Listen);
    mapper->addMapping(ui->enableServer, OptionsModel::Server);

    mapper->addMapping(ui->connectSocks, OptionsModel::ProxyUse);
    mapper->addMapping(ui->proxyIp, OptionsModel::ProxyIP);
    mapper->addMapping(ui->proxyPort, OptionsModel::ProxyPort);

    mapper->addMapping(ui->connectSocksTor, OptionsModel::ProxyUseTor);
    mapper->addMapping(ui->proxyIpTor, OptionsModel::ProxyIPTor);
    mapper->addMapping(ui->proxyPortTor, OptionsModel::ProxyPortTor);

    /* Window */
#ifndef Q_OS_MACOS
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        mapper->addMapping(ui->showTrayIcon, OptionsModel::ShowTrayIcon);
        mapper->addMapping(ui->minimizeToTray, OptionsModel::MinimizeToTray);
    }
    mapper->addMapping(ui->minimizeOnClose, OptionsModel::MinimizeOnClose);
#endif

    /* Display */
    mapper->addMapping(ui->lang, OptionsModel::Language);
    mapper->addMapping(ui->unit, OptionsModel::DisplayUnit);
    mapper->addMapping(ui->thirdPartyTxUrls, OptionsModel::ThirdPartyTxUrls);
}

void OptionsDialog::setOkButtonState(bool fState)
{
    ui->okButton->setEnabled(fState);
}

void OptionsDialog::updateOkButtonState()
{
    setOkButtonState(m_proxy_valid && m_gpu_solver_valid);
}

void OptionsDialog::chooseGpuSolverPath()
{
    QPointer<OptionsDialog> self{this};
    GUIUtil::getOpenFileName(
        this,
        tr("Choose GPU solver executable"),
        ui->gpuSolverPath->text().trimmed(),
        tr("Executables (*)"),
        [self](const QString& selected) {
            if (self && !selected.isEmpty()) self->ui->gpuSolverPath->setText(selected);
        });
}

void OptionsDialog::stopGpuSolverProbe()
{
    if (!m_gpu_solver_probe) return;
    QProcess* probe = m_gpu_solver_probe;
    m_gpu_solver_probe = nullptr;
    probe->kill();
    probe->deleteLater();
}

void OptionsDialog::validateGpuSolverPath()
{
    stopGpuSolverProbe();
    const QString path = ui->gpuSolverPath->text().trimmed();
    ui->gpuSolverStatusLabel->setStyleSheet(QString());

    if (path.isEmpty()) {
        m_gpu_solver_probe_status = SendCoinsDialog::GpuSolverProbeStatus::Unchecked;
        m_gpu_solver_valid = true;
        ui->gpuSolverStatusLabel->setText(tr("No GPU solver is configured. Live transfers and mining need one."));
        updateOkButtonState();
        return;
    }

    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || !info.isExecutable()) {
        m_gpu_solver_probe_status = SendCoinsDialog::GpuSolverProbeStatus::Failed;
        m_gpu_solver_valid = false;
        ui->gpuSolverStatusLabel->setStyleSheet(QStringLiteral("QLabel { color: red; }"));
        ui->gpuSolverStatusLabel->setText(
            !info.exists()
                ? tr("The selected GPU solver does not exist.")
                : tr("The selected GPU solver is not an executable file."));
        updateOkButtonState();
        return;
    }

    m_gpu_solver_probe_status = SendCoinsDialog::GpuSolverProbeStatus::Checking;
    m_gpu_solver_valid = false;
    ui->gpuSolverStatusLabel->setText(tr("Checking whether the selected solver starts for this network…"));
    updateOkButtonState();

    QProcess* probe = new QProcess(this);
    m_gpu_solver_probe = probe;
    probe->setProgram(path);
    probe->setArguments(SendCoinsDialog::gpuSolverProbeArguments(Params().GetConsensus().nTxEdgeBits));
    probe->setProcessChannelMode(QProcess::ForwardedErrorChannel);

    connect(probe, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, probe](int exit_code, QProcess::ExitStatus exit_status) {
        if (m_gpu_solver_probe != probe) {
            probe->deleteLater();
            return;
        }
        m_gpu_solver_probe = nullptr;
        if (m_gpu_solver_probe_status == SendCoinsDialog::GpuSolverProbeStatus::TimedOut) {
            m_gpu_solver_valid = false;
            ui->gpuSolverStatusLabel->setStyleSheet(QStringLiteral("QLabel { color: red; }"));
            ui->gpuSolverStatusLabel->setText(tr("The selected GPU solver startup check timed out."));
        } else {
            const bool available = exit_status == QProcess::NormalExit && exit_code == 0;
            m_gpu_solver_probe_status = available
                ? SendCoinsDialog::GpuSolverProbeStatus::Available
                : SendCoinsDialog::GpuSolverProbeStatus::Failed;
            m_gpu_solver_valid = available;
            if (available) {
                ui->gpuSolverStatusLabel->setText(tr("The selected GPU solver passed the startup check."));
            } else {
                ui->gpuSolverStatusLabel->setStyleSheet(QStringLiteral("QLabel { color: red; }"));
                ui->gpuSolverStatusLabel->setText(tr("The selected GPU solver did not pass the startup check."));
            }
        }
        updateOkButtonState();
        probe->deleteLater();
    });
    connect(probe, &QProcess::errorOccurred, this, [this, probe](QProcess::ProcessError error) {
        if (m_gpu_solver_probe != probe || error != QProcess::FailedToStart) return;
        m_gpu_solver_probe = nullptr;
        m_gpu_solver_probe_status = SendCoinsDialog::GpuSolverProbeStatus::Failed;
        m_gpu_solver_valid = false;
        ui->gpuSolverStatusLabel->setStyleSheet(QStringLiteral("QLabel { color: red; }"));
        ui->gpuSolverStatusLabel->setText(tr("The selected GPU solver could not be started."));
        updateOkButtonState();
        probe->deleteLater();
    });
    probe->start();
    QTimer::singleShot(3000, this, [this, probe] {
        if (m_gpu_solver_probe != probe || probe->state() == QProcess::NotRunning) return;
        m_gpu_solver_probe_status = SendCoinsDialog::GpuSolverProbeStatus::TimedOut;
        probe->kill();
    });
}

void OptionsDialog::on_resetButton_clicked()
{
    if (model) {
        // confirmation dialog
        /*: Text explaining that the settings changed will not come into effect
            until the client is restarted. */
        QString reset_dialog_text = tr("Client restart required to activate changes.") + "<br><br>";
        /*: Text explaining to the user that the client's current settings
            will be backed up at a specific location. %1 is a stand-in
            argument for the backup location's path. */
        reset_dialog_text.append(tr("Current settings will be backed up at \"%1\".").arg(m_client_model->dataDir()) + "<br><br>");
        /*: Text asking the user to confirm if they would like to proceed
            with a client shutdown. */
        reset_dialog_text.append(tr("Client will be shut down. Do you want to proceed?"));
        auto* box = new QMessageBox(this);
        box->setObjectName(QStringLiteral("optionsResetConfirm"));
        box->setIcon(QMessageBox::Question);
        //: Window title text of pop-up window shown when the user has chosen to reset options.
        box->setWindowTitle(tr("Confirm options reset"));
        box->setText(reset_dialog_text);
        box->setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
        box->setDefaultButton(QMessageBox::Cancel);
        QPointer<OptionsDialog> self{this};
        GUIUtil::ShowModalMessageBoxAsynchronously(box, [self](int result, QAbstractButton*) {
            if (!self || result != QMessageBox::Yes) return;
            self->model->Reset();
            self->close();
            Q_EMIT self->quitOnReset();
        });
    }
}

void OptionsDialog::on_openQuicksilverConfButton_clicked()
{
    auto* config_msgbox = new QMessageBox(this);
    config_msgbox->setIcon(QMessageBox::Information);
    //: Window title text of pop-up box that allows opening up of configuration file.
    config_msgbox->setWindowTitle(tr("Configuration options"));
    /*: Explanatory text about the priority order of instructions considered by client.
        The order from high to low being: command-line, configuration file, GUI settings. */
    config_msgbox->setText(tr("The configuration file is used to specify advanced user options which override GUI settings. "
                             "Additionally, any command-line options will override this configuration file."));

    QPushButton* open_button = config_msgbox->addButton(tr("Continue"), QMessageBox::ActionRole);
    config_msgbox->addButton(tr("Cancel"), QMessageBox::RejectRole);
    open_button->setDefault(true);

    connect(config_msgbox, &QMessageBox::finished, this, [this, config_msgbox, open_button] {
        if (config_msgbox->clickedButton() != open_button) return;
        if (GUIUtil::openQuicksilverConf()) return;
        auto* err = new QMessageBox(QMessageBox::Critical, tr("Error"), tr("The configuration file could not be opened."), QMessageBox::Ok, this);
        GUIUtil::ShowModalDialogAsynchronously(err);
    });
    GUIUtil::ShowModalDialogAsynchronously(config_msgbox);
}

void OptionsDialog::on_okButton_clicked()
{
    model->setData(model->index(OptionsModel::FontForMoney, 0), ui->moneyFont->itemData(ui->moneyFont->currentIndex()));

    mapper->submit();
    accept();
    updateDefaultProxyNets();
}

void OptionsDialog::on_cancelButton_clicked()
{
    reject();
}

void OptionsDialog::on_showTrayIcon_stateChanged(int state)
{
    if (state == Qt::Checked) {
        ui->minimizeToTray->setEnabled(true);
    } else {
        ui->minimizeToTray->setChecked(false);
        ui->minimizeToTray->setEnabled(false);
    }
}

void OptionsDialog::togglePruneWarning(bool enabled)
{
    ui->pruneWarning->setVisible(!ui->pruneWarning->isVisible());
}

void OptionsDialog::showRestartWarning(bool fPersistent)
{
    ui->statusLabel->setStyleSheet("QLabel { color: red; }");

    if(fPersistent)
    {
        ui->statusLabel->setText(tr("Client restart required to activate changes."));
    }
    else
    {
        ui->statusLabel->setText(tr("This change would require a client restart."));
        // clear non-persistent status label after 10 seconds
        // Todo: should perhaps be a class attribute, if we extend the use of statusLabel
        QTimer::singleShot(10s, this, &OptionsDialog::clearStatusLabel);
    }
}

void OptionsDialog::clearStatusLabel()
{
    ui->statusLabel->clear();
    if (model && model->isRestartRequired()) {
        showRestartWarning(true);
    }
}

void OptionsDialog::updateProxyValidationState()
{
    QValidatedLineEdit *pUiProxyIp = ui->proxyIp;
    QValidatedLineEdit *otherProxyWidget = (pUiProxyIp == ui->proxyIpTor) ? ui->proxyIp : ui->proxyIpTor;
    if (pUiProxyIp->isValid() && (!ui->proxyPort->isEnabled() || ui->proxyPort->text().toInt() > 0) && (!ui->proxyPortTor->isEnabled() || ui->proxyPortTor->text().toInt() > 0))
    {
        m_proxy_valid = otherProxyWidget->isValid();
        updateOkButtonState();
        clearStatusLabel();
    }
    else
    {
        m_proxy_valid = false;
        updateOkButtonState();
        ui->statusLabel->setStyleSheet("QLabel { color: red; }");
        ui->statusLabel->setText(tr("The supplied proxy address is invalid."));
    }
}

void OptionsDialog::updateDefaultProxyNets()
{
    std::string proxyIpText{ui->proxyIp->text().toStdString()};
    if (!IsUnixSocketPath(proxyIpText)) {
        const std::optional<CNetAddr> ui_proxy_netaddr{LookupHost(proxyIpText, /*fAllowLookup=*/false)};
        const CService ui_proxy{ui_proxy_netaddr.value_or(CNetAddr{}), ui->proxyPort->text().toUShort()};
        proxyIpText = ui_proxy.ToStringAddrPort();
    }

    Proxy proxy;
    bool has_proxy;

    has_proxy = model->node().getProxy(NET_IPV4, proxy);
    ui->proxyReachIPv4->setChecked(has_proxy && proxy.ToString() == proxyIpText);

    has_proxy = model->node().getProxy(NET_IPV6, proxy);
    ui->proxyReachIPv6->setChecked(has_proxy && proxy.ToString() == proxyIpText);

    has_proxy = model->node().getProxy(NET_ONION, proxy);
    ui->proxyReachTor->setChecked(has_proxy && proxy.ToString() == proxyIpText);
}

ProxyAddressValidator::ProxyAddressValidator(QObject *parent) :
QValidator(parent)
{
}

QValidator::State ProxyAddressValidator::validate(QString &input, int &pos) const
{
    Q_UNUSED(pos);
    uint16_t port{0};
    std::string hostname;
    if (!SplitHostPort(input.toStdString(), port, hostname) || port != 0) return QValidator::Invalid;

    CService serv(LookupNumeric(input.toStdString(), DEFAULT_GUI_PROXY_PORT));
    Proxy addrProxy = Proxy(serv, true);
    if (addrProxy.IsValid())
        return QValidator::Acceptable;

    return QValidator::Invalid;
}
