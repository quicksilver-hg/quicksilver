// Copyright (c) 2019-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/vaultcontroller.h>

#include <qt/askpassphrasedialog.h>
#include <qt/clientmodel.h>
#include <qt/createvaultdialog.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/thinvaultheadersource.h>
#include <qt/vaultmodel.h>
#include <qt/vaultview.h>

#include <external_signer.h>
#include <interfaces/handler.h>
#include <interfaces/node.h>
#include <util/string.h>
#include <util/threadnames.h>
#include <util/translation.h>
#include <vault/vault.h>

#include <algorithm>
#include <chrono>
#include <set>

#include <QApplication>
#include <QDebug>
#include <QMessageBox>
#include <QMetaObject>
#include <QMutexLocker>
#include <QPointer>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QWindow>

using util::Join;
using vault::VAULT_FLAG_BLANK_VAULT;
using vault::VAULT_FLAG_DESCRIPTORS;
using vault::VAULT_FLAG_DISABLE_PRIVATE_KEYS;
using vault::VAULT_FLAG_EXTERNAL_SIGNER;

namespace {
bool ModalIsParentedUnderVault(QWidget* modal, const VaultModel* vault_model)
{
    for (QWidget* widget = modal; widget; widget = widget->parentWidget()) {
        const auto* view = qobject_cast<const VaultView*>(widget);
        if (view && view->getVaultModel() == vault_model) return true;
    }
    return false;
}
} // namespace

VaultController::VaultController(ClientModel& client_model, const PlatformStyle* platform_style, QObject* parent)
    : VaultController(client_model.node(), client_model.getOptionsModel(), platform_style, parent)
{
    setClientModel(&client_model);
}

VaultController::VaultController(interfaces::Node& node, OptionsModel* options_model, const PlatformStyle* platform_style, QObject* parent)
    : QObject(parent)
    , m_activity_thread(new QThread(this))
    , m_activity_worker(new QObject)
    , m_node(node)
    , m_platform_style(platform_style)
    , m_options_model(options_model)
{
    m_handler_load_vault = m_node.vaultLoader().handleLoadVault([this](std::unique_ptr<interfaces::Vault> vault) {
        getOrCreateVault(std::move(vault));
    });

    m_thin_header_source = std::make_unique<ThinVaultHeaderSource>(DefaultThinVaultHeaderStorePath());
    m_thin_header_timer = new QTimer(this);
    m_thin_header_timer->setInterval(2000);
    connect(m_thin_header_timer, &QTimer::timeout, this, &VaultController::refreshThinHeaderSource);
    refreshThinHeaderSource();
    m_thin_header_timer->start();

    m_activity_worker->moveToThread(m_activity_thread);
    m_activity_thread->start();
    QTimer::singleShot(0, m_activity_worker, []() {
        util::ThreadRename("qt-vaultctrl");
    });
}

void VaultController::setClientModel(ClientModel* client_model)
{
    QMutexLocker locker(&m_mutex);
    m_client_model = client_model;
    if (m_thin_header_timer) {
        if (m_client_model) {
            m_thin_header_timer->stop();
        } else {
            m_thin_header_timer->start();
        }
    }
    for (VaultModel* vault_model : m_vaults) {
        vault_model->setClientModel(client_model);
    }
}

void VaultController::refreshThinHeaderSource()
{
    std::string error;
    if (m_thin_header_source->refresh(m_node.vaultLoader(), error)) {
        m_last_thin_header_error.clear();
        return;
    }
    if (error != m_last_thin_header_error) {
        qWarning() << "Thin vault header refresh failed:" << QString::fromStdString(error);
        m_last_thin_header_error = std::move(error);
    }
}

// Not using the default destructor because not all member types definitions are
// available in the header, just forward declared.
VaultController::~VaultController()
{
    const auto vaults = m_vaults;
    m_vaults.clear();
    for (VaultModel* vault_model : vaults) {
        Q_EMIT vaultRemoved(vault_model);
    }

    m_activity_thread->quit();
    m_activity_thread->wait();
    delete m_activity_worker;
}

std::map<std::string, std::pair<bool, std::string>> VaultController::listVaultDir() const
{
    QMutexLocker locker(&m_mutex);
    std::map<std::string, std::pair<bool, std::string>> vaults;
    for (const auto& [name, format] : m_node.vaultLoader().listVaultDir()) {
        vaults[name] = std::make_pair(false, format);
    }
    for (VaultModel* vault_model : m_vaults) {
        auto it = vaults.find(vault_model->vault().getVaultName());
        if (it != vaults.end()) it->second.first = true;
    }
    return vaults;
}

void VaultController::removeVault(VaultModel* vault_model)
{
    // Once the vault is successfully removed from the node, the model will emit the 'VaultModel::unload' signal.
    // This signal is already connected and will complete the removal of the view from the GUI.
    // Look at 'VaultController::getOrCreateVault' for the signal connection.
    vault_model->vault().remove();
}

void VaultController::openVaultsRecordedForStartup()
{
    const common::SettingsValue setting = m_node.getPersistentSetting("vault");
    if (!setting.isArray()) return;

    std::set<std::string> already_open;
    {
        QMutexLocker locker(&m_mutex);
        for (VaultModel* vault_model : m_vaults) {
            already_open.insert(vault_model->vault().getVaultName());
        }
    }

    for (const auto& value : setting.getValues()) {
        if (!value.isStr()) continue;
        const std::string name = value.get_str();
        if (already_open.count(name)) continue;

        std::vector<bilingual_str> warnings;
        auto vault = m_node.vaultLoader().loadVault(name, warnings);
        // A name that will not open is left to the vault menu, which lists the vault
        // directory as it finds it. Refusing to start over a stale settings entry
        // would be a worse failure than the one being fixed here.
        if (vault) getOrCreateVault(std::move(*vault));
    }
}

bool VaultController::proofOfWorkInFlight() const
{
    QMutexLocker locker(&m_mutex);
    for (VaultModel* vault_model : m_vaults) {
        if (vault_model->proofOfWorkInFlight()) return true;
    }
    return false;
}

void VaultController::stopProofOfWork(std::function<void()> done)
{
    if (!proofOfWorkInFlight()) {
        done();
        return;
    }

    {
        QMutexLocker locker(&m_mutex);
        for (VaultModel* vault_model : m_vaults) {
            vault_model->requestProofOfWorkCancel();
        }
    }

    // The solver polls the cancel predicate inside its sweep, so this normally
    // resolves in well under a second. It is still a wait on another thread, so it
    // is written as one rather than assumed away.
    QTimer* poll = new QTimer(this);
    poll->setInterval(50);
    connect(poll, &QTimer::timeout, this, [this, poll, done = std::move(done)]() {
        if (proofOfWorkInFlight()) return;
        poll->stop();
        poll->deleteLater();
        done();
    });
    poll->start();
}

void VaultController::closeAllVaultsForHandover(std::function<void()> done)
{
    std::vector<VaultModel*> vaults;
    {
        QMutexLocker locker(&m_mutex);
        vaults = m_vaults;
    }
    for (VaultModel* vault_model : vaults) {
        // std::nullopt: leave the startup list alone. Whoever is taking the vaults
        // over reads that list to find them again.
        vault_model->vault().remove(/*load_on_start=*/std::nullopt);
    }

    // removeVault only starts the closure. The database handle survives until the
    // queued unload deletes the model that owns the last reference to it, so the
    // handover is not complete -- and the file not free -- until m_vaults empties.
    QTimer* poll = new QTimer(this);
    poll->setInterval(50);
    connect(poll, &QTimer::timeout, this, [this, poll, done = std::move(done)]() {
        {
            QMutexLocker locker(&m_mutex);
            if (!m_vaults.empty()) return;
        }
        poll->stop();
        poll->deleteLater();
        done();
    });
    poll->start();
}

void VaultController::closeVault(VaultModel* vault_model, QWidget* parent)
{
    auto* box = new QMessageBox(parent);
    box->setObjectName(QStringLiteral("closeVaultConfirm"));
    box->setWindowTitle(tr("Close vault"));
    box->setText(tr("Are you sure you wish to close the vault <i>%1</i>?").arg(GUIUtil::HtmlEscape(vault_model->getDisplayName())));
    box->setInformativeText(tr("Closing the vault for too long can result in having to resync the entire chain if pruning is enabled."));
    box->setStandardButtons(QMessageBox::Yes|QMessageBox::Cancel);
    box->setDefaultButton(QMessageBox::Yes);
    QPointer<VaultController> self{this};
    QPointer<VaultModel> model{vault_model};
    QPointer<QWidget> parent_widget{parent};
    GUIUtil::ShowModalMessageBoxAsynchronously(box, [self, model, parent_widget](int result, QAbstractButton*) {
        if (!self || result != QMessageBox::Yes || !model) return;
        self->closeVaultAfterConfirm(model, parent_widget);
    });
}

void VaultController::closeVaultAfterConfirm(VaultModel* vault_model, QWidget* parent)
{
    auto proceed = [this, vault_model] {
        stopProofOfWork([this, vault_model] { removeVault(vault_model); });
    };
    // A transfer's proof-of-work runs on a worker thread inside this vault. Closing
    // now would delete it underneath that worker, so the transfer has to be given up
    // deliberately rather than lost silently.
    if (!vault_model->proofOfWorkInFlight()) {
        proceed();
        return;
    }
    confirmAbandonProofOfWork(parent, [proceed](bool abandon) {
        if (abandon) proceed();
    });
}

void VaultController::confirmAbandonProofOfWork(QWidget* parent, std::function<void(bool)> done)
{
    auto* box = new QMessageBox(parent);
    box->setObjectName(QStringLiteral("abandonProofOfWorkConfirm"));
    box->setIcon(QMessageBox::Warning);
    box->setWindowTitle(tr("Transfer in progress"));
    box->setText(tr("A transfer is still solving its proof-of-work."));
    box->setInformativeText(tr("Closing the vault now abandons that transfer. The work done so far is "
                              "lost and nothing is sent; no quicksilver leaves the vault. You can "
                              "start the transfer again afterwards."));
    QPushButton* abandon = box->addButton(tr("Abandon transfer and close"), QMessageBox::AcceptRole);
    box->addButton(tr("Keep working"), QMessageBox::RejectRole);
    box->setDefaultButton(abandon);
    GUIUtil::ShowModalMessageBoxAsynchronously(box, [abandon, done = std::move(done)](int, QAbstractButton* clicked) {
        if (done) done(clicked == abandon);
    });
}

void VaultController::closeAllVaults(QWidget* parent)
{
    auto* box = new QMessageBox(parent);
    box->setObjectName(QStringLiteral("closeAllVaultsConfirm"));
    box->setIcon(QMessageBox::Question);
    box->setWindowTitle(tr("Close all vault"));
    box->setText(tr("Are you sure you wish to close all vaults?"));
    box->setStandardButtons(QMessageBox::Yes|QMessageBox::Cancel);
    box->setDefaultButton(QMessageBox::Yes);
    QPointer<VaultController> self{this};
    QPointer<QWidget> parent_widget{parent};
    GUIUtil::ShowModalMessageBoxAsynchronously(box, [self, parent_widget](int result, QAbstractButton*) {
        if (!self || result != QMessageBox::Yes) return;
        auto proceed = [self] {
            if (!self) return;
            self->stopProofOfWork([self] {
                if (!self) return;
                std::vector<VaultModel*> vaults;
                {
                    QMutexLocker locker(&self->m_mutex);
                    vaults = self->m_vaults;
                }
                for (VaultModel* vault_model : vaults) {
                    self->removeVault(vault_model);
                }
            });
        };
        if (!self->proofOfWorkInFlight()) {
            proceed();
            return;
        }
        self->confirmAbandonProofOfWork(parent_widget, [proceed](bool abandon) {
            if (abandon) proceed();
        });
    });
}

VaultModel* VaultController::getOrCreateVault(std::unique_ptr<interfaces::Vault> vault)
{
    QMutexLocker locker(&m_mutex);

    // Return model instance if exists.
    if (!m_vaults.empty()) {
        std::string name = vault->getVaultName();
        for (VaultModel* vault_model : m_vaults) {
            if (vault_model->vault().getVaultName() == name) {
                return vault_model;
            }
        }
    }

    // Instantiate model and register it.
    VaultModel* vault_model = new VaultModel(std::move(vault), m_node, m_options_model, m_platform_style,
                                                nullptr /* required for the following moveToThread() call */);
    if (m_client_model) vault_model->setClientModel(m_client_model);

    // Move VaultModel object to the thread that created the VaultController
    // object (GUI main thread), instead of the current thread, which could be
    // an outside vault thread or RPC thread sending a LoadVault notification.
    // This ensures queued signals sent to the VaultModel object will be
    // handled on the GUI event loop.
    vault_model->moveToThread(thread());
    // setParent(parent) must be called in the thread which created the parent object. More details in #18948.
    QMetaObject::invokeMethod(this, [vault_model, this] {
        vault_model->setParent(this);
    }, GUIUtil::blockingGUIThreadConnection());

    m_vaults.push_back(vault_model);

    // VaultModel::startPollBalance needs to be called in a thread managed by
    // Qt because of startTimer. Considering the current thread can be a RPC
    // thread, better delegate the calling to Qt with Qt::AutoConnection.
    const bool called = QMetaObject::invokeMethod(vault_model, "startPollBalance");
    assert(called);

    connect(vault_model, &VaultModel::unload, this, [this, vault_model] {
        // Backup/export/save pickers are ApplicationModal and parented under VaultView.
        // Deleting that view while a picker is up would dismiss it. Other ApplicationModal
        // widgets (main-window pickers, options, clientMessageBox) are not ancestors of
        // this view and do not need to block unload.
        QWidget* active_dialog = QApplication::activeModalWidget();
        if (ModalIsParentedUnderVault(active_dialog, vault_model)) {
            connect(qApp, &QApplication::focusWindowChanged, vault_model, [this, vault_model]() {
                if (!ModalIsParentedUnderVault(QApplication::activeModalWidget(), vault_model)) {
                    removeAndDeleteVault(vault_model);
                }
            }, Qt::QueuedConnection);
        } else {
            removeAndDeleteVault(vault_model);
        }
    }, Qt::QueuedConnection);

    // Re-emit coinsSent signal from vault model.
    connect(vault_model, &VaultModel::coinsSent, this, &VaultController::coinsSent);

    Q_EMIT vaultAdded(vault_model);

    return vault_model;
}

void VaultController::removeAndDeleteVault(VaultModel* vault_model)
{
    // Unregister vault model.
    {
        QMutexLocker locker(&m_mutex);
        m_vaults.erase(std::remove(m_vaults.begin(), m_vaults.end(), vault_model));
    }
    Q_EMIT vaultRemoved(vault_model);
    // Currently this can trigger the unload since the model can hold the last
    // CVault shared pointer.
    delete vault_model;
}

VaultControllerActivity::VaultControllerActivity(VaultController* vault_controller, QWidget* parent_widget)
    : QObject(vault_controller)
    , m_vault_controller(vault_controller)
    , m_parent_widget(parent_widget)
{
    connect(this, &VaultControllerActivity::finished, this, &QObject::deleteLater);
}

void VaultControllerActivity::showProgressDialog(const QString& title_text, const QString& label_text, bool show_minimized)
{
    auto progress_dialog = new QProgressDialog(m_parent_widget);
    progress_dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(this, &VaultControllerActivity::finished, progress_dialog, &QWidget::close);

    progress_dialog->setWindowTitle(title_text);
    progress_dialog->setLabelText(label_text);
    progress_dialog->setRange(0, 0);
    progress_dialog->setCancelButton(nullptr);
    progress_dialog->setWindowModality(Qt::ApplicationModal);
    GUIUtil::PolishProgressDialog(progress_dialog);
    // The setValue call forces QProgressDialog to start the internal duration estimation.
    // See details in https://bugreports.qt.io/browse/QTBUG-47042.
    progress_dialog->setValue(0);
    // When requested, launch dialog minimized
    if (show_minimized) progress_dialog->showMinimized();
}

CreateVaultActivity::CreateVaultActivity(VaultController* vault_controller, QWidget* parent_widget)
    : VaultControllerActivity(vault_controller, parent_widget)
{
    m_passphrase.reserve(MAX_PASSPHRASE_SIZE);
}

CreateVaultActivity::~CreateVaultActivity()
{
    delete m_create_vault_dialog;
    delete m_passphrase_dialog;
}

void CreateVaultActivity::askPassphrase()
{
    m_passphrase_dialog = new AskPassphraseDialog(AskPassphraseDialog::Encrypt, m_parent_widget, &m_passphrase);
    m_passphrase_dialog->setWindowModality(Qt::ApplicationModal);
    m_passphrase_dialog->show();

    connect(m_passphrase_dialog, &QObject::destroyed, [this] {
        m_passphrase_dialog = nullptr;
    });
    connect(m_passphrase_dialog, &QDialog::accepted, [this] {
        createVault();
    });
    connect(m_passphrase_dialog, &QDialog::rejected, [this] {
        Q_EMIT finished();
    });
}

void CreateVaultActivity::createVault()
{
    showProgressDialog(
        //: Title of window indicating the progress of creation of a new vault.
        tr("Create Vault"),
        /*: Descriptive text of the create vault progress window which indicates
            to the user which vault is currently being created. */
        tr("Creating Vault <b>%1</b>…").arg(m_create_vault_dialog->vaultName().toHtmlEscaped()));

    std::string name = m_create_vault_dialog->vaultName().toStdString();
    uint64_t flags = 0;
    // Enable descriptors by default.
    flags |= VAULT_FLAG_DESCRIPTORS;
    if (m_create_vault_dialog->isDisablePrivateKeysChecked()) {
        flags |= VAULT_FLAG_DISABLE_PRIVATE_KEYS;
    }
    if (m_create_vault_dialog->isMakeBlankVaultChecked()) {
        flags |= VAULT_FLAG_BLANK_VAULT;
    }
    if (m_create_vault_dialog->isExternalSignerChecked()) {
        flags |= VAULT_FLAG_EXTERNAL_SIGNER;
    }

    QTimer::singleShot(500ms, worker(), [this, name, flags] {
        auto vault{node().vaultLoader().createVault(name, m_passphrase, flags, m_warning_message)};

        if (vault) {
            m_vault_model = m_vault_controller->getOrCreateVault(std::move(*vault));
        } else {
            m_error_message = util::ErrorString(vault);
        }

        QTimer::singleShot(500ms, this, &CreateVaultActivity::finish);
    });
}

void CreateVaultActivity::finish()
{
    if (!m_error_message.empty()) {
        auto* box = new QMessageBox(QMessageBox::Critical, tr("Create vault failed"),
            QString::fromStdString(m_error_message.translated), QMessageBox::Ok, m_parent_widget);
        box->setObjectName(QStringLiteral("createVaultFailed"));
        GUIUtil::ShowModalDialogAsynchronously(box);
    } else if (!m_warning_message.empty()) {
        auto* box = new QMessageBox(QMessageBox::Warning, tr("Create vault warning"),
            QString::fromStdString(Join(m_warning_message, Untranslated("\n")).translated), QMessageBox::Ok, m_parent_widget);
        box->setObjectName(QStringLiteral("createVaultWarning"));
        GUIUtil::ShowModalDialogAsynchronously(box);
    }

    if (m_vault_model) Q_EMIT created(m_vault_model);

    Q_EMIT finished();
}

void CreateVaultActivity::create()
{
    m_create_vault_dialog = new CreateVaultDialog(m_parent_widget);

    std::vector<std::unique_ptr<interfaces::ExternalSigner>> signers;
    QString signer_error_title;
    QString signer_error_text;
    QString signer_error_name;
    try {
        signers = node().listExternalSigners();
    } catch (const std::runtime_error& e) {
        signer_error_title = tr("Can't list signers");
        signer_error_text = e.what();
        signer_error_name = QStringLiteral("listSignersFailed");
    }
    if (signer_error_title.isEmpty() && signers.size() > 1) {
        signer_error_title = tr("Too many external signers found");
        signer_error_text = QString::fromStdString("More than one external signer found. Please connect only one at a time.");
        signer_error_name = QStringLiteral("tooManyExternalSigners");
        signers.clear();
    }
    m_create_vault_dialog->setSigners(signers);

    auto present_dialog = [this]() {
        m_create_vault_dialog->setWindowModality(Qt::ApplicationModal);
        m_create_vault_dialog->show();

        connect(m_create_vault_dialog, &QObject::destroyed, [this] {
            m_create_vault_dialog = nullptr;
        });
        connect(m_create_vault_dialog, &QDialog::rejected, [this] {
            Q_EMIT finished();
        });
        connect(m_create_vault_dialog, &QDialog::accepted, [this] {
            if (m_create_vault_dialog->isEncryptVaultChecked()) {
                askPassphrase();
            } else {
                createVault();
            }
        });
    };

    if (!signer_error_title.isEmpty()) {
        auto* box = new QMessageBox(QMessageBox::Critical, signer_error_title, signer_error_text, QMessageBox::Ok, m_parent_widget);
        box->setObjectName(signer_error_name);
        QPointer<CreateVaultActivity> self{this};
        GUIUtil::ShowModalMessageBoxAsynchronously(box, [self, present_dialog](int, QAbstractButton*) {
            if (self) present_dialog();
        });
        return;
    }
    present_dialog();
}

OpenVaultActivity::OpenVaultActivity(VaultController* vault_controller, QWidget* parent_widget)
    : VaultControllerActivity(vault_controller, parent_widget)
{
}

void OpenVaultActivity::finish()
{
    if (!m_error_message.empty()) {
        auto* box = new QMessageBox(QMessageBox::Critical, tr("Open vault failed"),
            QString::fromStdString(m_error_message.translated), QMessageBox::Ok, m_parent_widget);
        box->setObjectName(QStringLiteral("openVaultFailed"));
        GUIUtil::ShowModalDialogAsynchronously(box);
    } else if (!m_warning_message.empty()) {
        auto* box = new QMessageBox(QMessageBox::Warning, tr("Open vault warning"),
            QString::fromStdString(Join(m_warning_message, Untranslated("\n")).translated), QMessageBox::Ok, m_parent_widget);
        box->setObjectName(QStringLiteral("openVaultWarning"));
        GUIUtil::ShowModalDialogAsynchronously(box);
    }

    if (m_vault_model) Q_EMIT opened(m_vault_model);

    Q_EMIT finished();
}

void OpenVaultActivity::open(const std::string& path)
{
    QString name = GUIUtil::VaultDisplayName(path);

    showProgressDialog(
        //: Title of window indicating the progress of opening of a vault.
        tr("Open Vault"),
        /*: Descriptive text of the open vault progress window which indicates
            to the user which vault is currently being opened. */
        tr("Opening Vault <b>%1</b>…").arg(name.toHtmlEscaped()));

    QTimer::singleShot(0, worker(), [this, path] {
        auto vault{node().vaultLoader().loadVault(path, m_warning_message)};

        if (vault) {
            m_vault_model = m_vault_controller->getOrCreateVault(std::move(*vault));
        } else {
            m_error_message = util::ErrorString(vault);
        }

        QTimer::singleShot(0, this, &OpenVaultActivity::finish);
    });
}

LoadVaultsActivity::LoadVaultsActivity(VaultController* vault_controller, QWidget* parent_widget)
    : VaultControllerActivity(vault_controller, parent_widget)
{
}

void LoadVaultsActivity::load(bool show_loading_minimized)
{
    showProgressDialog(
        //: Title of progress window which is displayed when vaults are being loaded.
        tr("Load Vaults"),
        /*: Descriptive text of the load vaults progress window which indicates to
            the user that vaults are currently being loaded.*/
        tr("Loading vault…"),
        /*show_minimized=*/show_loading_minimized);

    QTimer::singleShot(0, worker(), [this] {
        for (auto& vault : node().vaultLoader().getVaults()) {
            m_vault_controller->getOrCreateVault(std::move(vault));
        }
        // getVaults() reports what node initialisation loaded, and with consensus off
        // there was no node initialisation. Open what the startup list names.
        m_vault_controller->openVaultsRecordedForStartup();

        QTimer::singleShot(0, this, [this] { Q_EMIT finished(); });
    });
}

RestoreVaultActivity::RestoreVaultActivity(VaultController* vault_controller, QWidget* parent_widget)
    : VaultControllerActivity(vault_controller, parent_widget)
{
}

void RestoreVaultActivity::restore(const fs::path& backup_file, const std::string& vault_name)
{
    QString name = QString::fromStdString(vault_name);

    showProgressDialog(
        //: Title of progress window which is displayed when vaults are being restored.
        tr("Restore Vault"),
        /*: Descriptive text of the restore vaults progress window which indicates to
            the user that vaults are currently being restored.*/
        tr("Restoring Vault <b>%1</b>…").arg(name.toHtmlEscaped()));

    QTimer::singleShot(0, worker(), [this, backup_file, vault_name] {
        auto vault{node().vaultLoader().restoreVault(backup_file, vault_name, m_warning_message)};

        if (vault) {
            m_vault_model = m_vault_controller->getOrCreateVault(std::move(*vault));
        } else {
            m_error_message = util::ErrorString(vault);
        }

        QTimer::singleShot(0, this, &RestoreVaultActivity::finish);
    });
}

void RestoreVaultActivity::finish()
{
    if (!m_error_message.empty()) {
        //: Title of message box which is displayed when the vault could not be restored.
        auto* box = new QMessageBox(QMessageBox::Critical, tr("Restore vault failed"),
            QString::fromStdString(m_error_message.translated), QMessageBox::Ok, m_parent_widget);
        box->setObjectName(QStringLiteral("restoreVaultFailed"));
        GUIUtil::ShowModalDialogAsynchronously(box);
    } else if (!m_warning_message.empty()) {
        //: Title of message box which is displayed when the vault is restored with some warning.
        auto* box = new QMessageBox(QMessageBox::Warning, tr("Restore vault warning"),
            QString::fromStdString(Join(m_warning_message, Untranslated("\n")).translated), QMessageBox::Ok, m_parent_widget);
        box->setObjectName(QStringLiteral("restoreVaultWarning"));
        GUIUtil::ShowModalDialogAsynchronously(box);
    } else {
        //: Title of message box which is displayed when the vault is successfully restored.
        auto* box = new QMessageBox(QMessageBox::Information, tr("Restore vault message"),
            QString::fromStdString(Untranslated("Vault restored successfully \n").translated), QMessageBox::Ok, m_parent_widget);
        box->setObjectName(QStringLiteral("restoreVaultSuccess"));
        GUIUtil::ShowModalDialogAsynchronously(box);
    }

    if (m_vault_model) Q_EMIT restored(m_vault_model);

    Q_EMIT finished();
}
