// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <quicksilver-build-config.h> // IWYU pragma: keep

#include <qt/optionsdialog.h>
#include <qt/benchpanel.h>
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

#include <algorithm>
#include <chrono>

#include <QApplication>
#include <QCheckBox>
#include <QGuiApplication>
#include <QScreen>
#include <QShowEvent>
#include <QSizePolicy>
#include <QDataWidgetMapper>
#include <QFileInfo>
#include <QFrame>
#include <QIntValidator>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVBoxLayout>

namespace {
QStringList ShippedUiLanguages()
{
    // F-286 hook: add a language code here when its application catalogue
    // ships. initTranslations (quicksilver.cpp:198) must then also install the
    // application translator; deliberately, it installs no such translator today.
    return {};
}
} // namespace

OptionsDialog::OptionsDialog(QWidget* parent, bool enableVault)
    : QDialog(parent, GUIUtil::dialog_flags | Qt::WindowMaximizeButtonHint),
      ui(new Ui::OptionsDialog)
{
    ui->setupUi(this);
    setProperty("class", QStringLiteral("quicksilverDialog"));
    setMinimumSize(QSize(700, 540));
    // QCheckBox reports these multi-line labels as wider than the dialog.
    // setMinimumSize above then resizes to 700x540 and marks that as the user's
    // size, so showing the dialog does not grow it, and the layout crushes
    // every Main-tab control into what is left. Take the dialog's width, keep
    // the height of the lines, and let the scroll area move when a short screen
    // cannot show them all.
    for (QCheckBox* box : {ui->allowCpuBlockMining}) {
        QSizePolicy policy = box->sizePolicy();
        policy.setHorizontalPolicy(QSizePolicy::Ignored);
        policy.setVerticalPolicy(QSizePolicy::Fixed);
        box->setSizePolicy(policy);
        box->setFixedHeight(box->sizeHint().height());
    }
    ui->verticalLayout->setContentsMargins(18, 16, 18, 18);
    ui->verticalLayout->setSpacing(12);
    ui->tabWidget->setDocumentMode(true);

    const auto panel = BenchPanel::Make(QStringLiteral("optionsPanel"), tr("Options"), this);
    panel.frame->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    ui->verticalLayout->removeWidget(ui->tabWidget);
    panel.body->addWidget(ui->tabWidget, 1);
    ui->verticalLayout->insertWidget(0, panel.frame, 1);
    for (QLabel* label : ui->tabWidget->findChildren<QLabel*>()) {
        label->setProperty("class", QStringLiteral("benchKey"));
    }
    for (QPushButton* button : findChildren<QPushButton*>()) {
        button->setProperty("class", QStringLiteral("benchQuiet"));
    }

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
    configureLanguageRow(ShippedUiLanguages(), ui->lang, ui->langLabel);
    // The Display rows share one label column, so their fields start together.
    int display_label_width{0};
    for (QLabel* label : {ui->langLabel, ui->unitLabel, ui->thirdPartyTxUrlsLabel}) {
        display_label_width = std::max(display_label_width, label->sizeHint().width());
    }
    for (QLabel* label : {ui->langLabel, ui->unitLabel, ui->thirdPartyTxUrlsLabel}) {
        label->setFixedWidth(display_label_width);
    }
    ui->unit->setModel(new QuicksilverUnits(this));

    /* Widget-to-option mapper */
    mapper = new QDataWidgetMapper(this);
    mapper->setSubmitPolicy(QDataWidgetMapper::ManualSubmit);
    mapper->setOrientation(Qt::Vertical);

    GUIUtil::ItemDelegate* delegate = new GUIUtil::ItemDelegate(mapper);
    connect(delegate, &GUIUtil::ItemDelegate::keyEscapePressed, this, &OptionsDialog::reject);
    mapper->setItemDelegate(delegate);

    /* setup/change UI elements when proxy IPs are invalid/valid */
    // Parented to this dialog, not to `parent`: QValidatedLineEdit::setCheckValidator
    // stores a raw pointer and takes no ownership, so the QObject parent is the only
    // owner. Parenting to `parent` leaked both validators outright whenever the dialog
    // was built parentless, and otherwise accumulated a pair on the parent for every
    // time the user opened Settings. The validators are used only by this dialog's own
    // line edits, so this dialog is the right owner.
    ui->proxyIp->setCheckValidator(new ProxyAddressValidator(this));
    ui->proxyIpTor->setCheckValidator(new ProxyAddressValidator(this));
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
    if (!ui->lang->isHidden()) {
        connect(ui->lang, qOverload<>(&QValueComboBox::valueChanged), [this]{ showRestartWarning(); });
    }
    connect(ui->thirdPartyTxUrls, &QLineEdit::textChanged, [this]{ showRestartWarning(); });
    validateGpuSolverPath();
}

void OptionsDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    // setMinimumSize resized this dialog to 700x540 before the rows existed,
    // and that counts as a user resize, so showing it does not grow it. Ask
    // the Main tab for the size of its rows at the width they actually need,
    // then stop at the screen. A shorter screen scrolls instead of crushing
    // the rows; the scroll area is what keeps a 1366x768 laptop honest.
    if (ui->tabMainScrollArea->viewport()->width() <= 0) return;
    layout()->activate();
    const QSize contents = ui->tabMainScrollContents->sizeHint().expandedTo(ui->tabMainScrollContents->minimumSizeHint());
    const int chrome_w = width() - ui->tabMainScrollArea->viewport()->width();
    const int chrome_h = height() - ui->tabMainScrollArea->viewport()->height();
    int want_w = qMax(minimumWidth(), contents.width() + qMax(0, chrome_w));
    int want_h = qMax(minimumHeight(), contents.height() + qMax(0, chrome_h));
    QScreen* screen = this->screen() ? this->screen() : QGuiApplication::primaryScreen();
    const QRect avail = screen ? screen->availableGeometry() : QRect();
    // availableGeometry is the screen area the window frame may occupy.
    // resize() sets the client size, so leave a margin for the title bar.
    if (avail.width() > 0) want_w = qMin(want_w, qMax(minimumWidth(), avail.width() - 32));
    if (avail.height() > 0) want_h = qMin(want_h, qMax(minimumHeight(), avail.height() - 48));
    resize(want_w, want_h);
    // The vertical bar can appear or go as the height changes, which changes
    // the width the rows actually get. One more pass covers that difference.
    layout()->activate();
    const int overflow = ui->tabMainScrollContents->width() - ui->tabMainScrollArea->viewport()->width();
    if (overflow > 0) {
        const int limit = avail.width() > 0 ? qMax(minimumWidth(), avail.width() - 32) : want_w + overflow;
        resize(qMin(width() + overflow, limit), height());
    }
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
    mapper->addMapping(ui->allowCpuBlockMining, OptionsModel::AllowCpuBlockMining);
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
    if (!ui->lang->isHidden()) {
        mapper->addMapping(ui->lang, OptionsModel::Language);
    }
    mapper->addMapping(ui->unit, OptionsModel::DisplayUnit);
    mapper->addMapping(ui->thirdPartyTxUrls, OptionsModel::ThirdPartyTxUrls);
}

void OptionsDialog::configureLanguageRow(const QStringList& shipped_languages, QComboBox* languages, QLabel* language_label)
{
    const bool has_catalogues{!shipped_languages.isEmpty()};
    languages->setVisible(has_catalogues);
    language_label->setVisible(has_catalogues);
    languages->clear();
    if (!has_catalogues) return;

    languages->addItem(QStringLiteral("(") + tr("default") + QStringLiteral(")"), QVariant{QString{}});
    languages->addItem(tr("English (en)"), QStringLiteral("en"));
    for (const QString& code : shipped_languages) {
        const QString native_name{QLocale{code}.nativeLanguageName()};
        languages->addItem(QStringLiteral("%1 (%2)").arg(native_name, code), code);
    }
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
        ui->gpuSolverStatusLabel->setText(tr("No GPU solver is configured. Transfers fall back to this computer's processor and may take many minutes; block mining stays off unless processor block mining is enabled above."));
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
