// Copyright (c) 2019-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_VAULTCONTROLLER_H
#define QUICKSILVER_QT_VAULTCONTROLLER_H

#include <qt/sendcoinsrecipient.h>
#include <support/allocators/secure.h>
#include <sync.h>
#include <util/translation.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <QMessageBox>
#include <QMutex>
#include <QProgressDialog>
#include <QThread>
#include <QTimer>
#include <QString>

class ClientModel;
class OptionsModel;
class PlatformStyle;
class VaultModel;
class ThinVaultHeaderSource;

namespace interfaces {
class Handler;
class Node;
class Vault;
} // namespace interfaces

namespace fs {
class path;
}

class AskPassphraseDialog;
class CreateVaultActivity;
class CreateVaultDialog;
class OpenVaultActivity;
class VaultControllerActivity;

/**
 * Controller between interfaces::Node, VaultModel instances and the GUI.
 */
class VaultController : public QObject
{
    Q_OBJECT

    void removeAndDeleteVault(VaultModel* vault_model);

public:
    VaultController(ClientModel& client_model, const PlatformStyle* platform_style, QObject* parent);
    VaultController(interfaces::Node& node, OptionsModel* options_model, const PlatformStyle* platform_style, QObject* parent);
    ~VaultController();

    void setClientModel(ClientModel* client_model);
    VaultModel* getOrCreateVault(std::unique_ptr<interfaces::Vault> vault);

    //! Returns all vault names in the vault dir mapped to whether the vault
    //! is loaded.
    std::map<std::string, std::pair<bool, std::string>> listVaultDir() const;

    void closeVault(VaultModel* vault_model, QWidget* parent = nullptr);
    void closeAllVaults(QWidget* parent = nullptr);

    //! True while any open vault has a proof-of-work grind running on a worker
    //! thread. Unloading a vault in that state deletes the CVault the worker is
    //! executing inside, so every close path has to consult this first.
    bool proofOfWorkInFlight() const;

    /** Ask every running grind to stop, then call `done` once none is left.
     *
     * Calls `done` immediately when nothing is in flight. Otherwise it polls, and
     * deliberately does not spin a nested event loop: the interface has to stay
     * alive to keep telling the user why it is waiting, and a nested loop here is
     * how the send path previously froze.
     */
    void stopProofOfWork(std::function<void()> done);

    /** Close every open vault, leaving the startup vault list untouched.
     *
     * `Close vault` unlists the vault so it does not come back. This does not: it is
     * for handing vaults over to node initialisation, which reopens exactly the list
     * that is left behind. `done` runs once the last model is gone and, with it, the
     * database handle that node initialisation needs to take.
     */
    void closeAllVaultsForHandover(std::function<void()> done);

    //! Asks whether a running transfer may be given up, then calls `done`.
    void confirmAbandonProofOfWork(QWidget* parent, std::function<void(bool)> done);

    /** Open the vaults the startup list names, skipping any already open.
     *
     * Node initialisation does this for itself, so with consensus on this finds
     * everything already loaded and does nothing. With consensus off nothing else
     * reads that list, and the vault a user created last session simply never
     * reappeared -- the desktop offered to create one, as though theirs were gone.
     */
    void openVaultsRecordedForStartup();

Q_SIGNALS:
    void vaultAdded(VaultModel* vault_model);
    void vaultRemoved(VaultModel* vault_model);

    void coinsSent(VaultModel* vault_model, SendCoinsRecipient recipient, QByteArray transaction);

private:
    QThread* const m_activity_thread;
    QObject* const m_activity_worker;
    interfaces::Node& m_node;
    const PlatformStyle* const m_platform_style;
    OptionsModel* const m_options_model;
    ClientModel* m_client_model{nullptr};
    mutable QMutex m_mutex;
    std::vector<VaultModel*> m_vaults;
    std::unique_ptr<interfaces::Handler> m_handler_load_vault;
    std::unique_ptr<ThinVaultHeaderSource> m_thin_header_source;
    QTimer* m_thin_header_timer{nullptr};
    std::string m_last_thin_header_error;

    friend class VaultControllerActivity;
    void refreshThinHeaderSource();
    //! Starts the vault closure procedure
    void removeVault(VaultModel* vault_model);
    void closeVaultAfterConfirm(VaultModel* vault_model, QWidget* parent);
};

class VaultControllerActivity : public QObject
{
    Q_OBJECT

public:
    VaultControllerActivity(VaultController* vault_controller, QWidget* parent_widget);
    virtual ~VaultControllerActivity() = default;

Q_SIGNALS:
    void finished();

protected:
    interfaces::Node& node() const { return m_vault_controller->m_node; }
    QObject* worker() const { return m_vault_controller->m_activity_worker; }

    void showProgressDialog(const QString& title_text, const QString& label_text, bool show_minimized=false);

    VaultController* const m_vault_controller;
    QWidget* const m_parent_widget;
    VaultModel* m_vault_model{nullptr};
    bilingual_str m_error_message;
    std::vector<bilingual_str> m_warning_message;
};


class CreateVaultActivity : public VaultControllerActivity
{
    Q_OBJECT

public:
    CreateVaultActivity(VaultController* vault_controller, QWidget* parent_widget);
    virtual ~CreateVaultActivity();

    void create();

Q_SIGNALS:
    void created(VaultModel* vault_model);

private:
    void askPassphrase();
    void createVault();
    void finish();

    SecureString m_passphrase;
    CreateVaultDialog* m_create_vault_dialog{nullptr};
    AskPassphraseDialog* m_passphrase_dialog{nullptr};
};

class OpenVaultActivity : public VaultControllerActivity
{
    Q_OBJECT

public:
    OpenVaultActivity(VaultController* vault_controller, QWidget* parent_widget);

    void open(const std::string& path);

Q_SIGNALS:
    void opened(VaultModel* vault_model);

private:
    void finish();
};

class LoadVaultsActivity : public VaultControllerActivity
{
    Q_OBJECT

public:
    LoadVaultsActivity(VaultController* vault_controller, QWidget* parent_widget);

    void load(bool show_loading_minimized);
};

class RestoreVaultActivity : public VaultControllerActivity
{
    Q_OBJECT

public:
    RestoreVaultActivity(VaultController* vault_controller, QWidget* parent_widget);

    void restore(const fs::path& backup_file, const std::string& vault_name);

Q_SIGNALS:
    void restored(VaultModel* vault_model);

private:
    void finish();
};

#endif // QUICKSILVER_QT_VAULTCONTROLLER_H
