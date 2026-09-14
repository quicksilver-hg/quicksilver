// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_QUICKSILVERGUI_H
#define QUICKSILVER_QT_QUICKSILVERGUI_H

#include <quicksilver-build-config.h> // IWYU pragma: keep

#include <qt/quicksilverunits.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/optionsdialog.h>

#include <consensus/amount.h>

#include <QLabel>
#include <QMainWindow>
#include <QMap>
#include <QMenu>
#include <QPoint>
#include <QSystemTrayIcon>

#ifdef Q_OS_MACOS
#include <qt/macos_appnap.h>
#endif

#include <functional>
#include <memory>

class NetworkStyle;
class Notificator;
class OptionsModel;
class PlatformStyle;
class RPCConsole;
class SendCoinsRecipient;
class UnitDisplayStatusBarControl;
class VaultController;
class VaultFrame;
class VaultModel;
class HelpMessageDialog;
class ModalOverlay;
enum class SynchronizationState;

namespace interfaces {
class Handler;
class Node;
struct BlockAndHeaderTipInfo;
}

QT_BEGIN_NAMESPACE
class QAction;
class QComboBox;
class QDateTime;
class QProgressBar;
class QProgressDialog;
QT_END_NAMESPACE

namespace GUIUtil {
class ClickableLabel;
class ClickableProgressBar;
}

/**
  Quicksilver GUI main class. This class represents the main window of the Quicksilver UI. It communicates with both the client and
  vault models to give the user an up-to-date view of the current core state.
*/
class QuicksilverGUI : public QMainWindow
{
    Q_OBJECT

public:
    static const std::string DEFAULT_UIPLATFORM;

    explicit QuicksilverGUI(interfaces::Node& node, const PlatformStyle *platformStyle, const NetworkStyle *networkStyle, QWidget *parent = nullptr);
    ~QuicksilverGUI();

    /** Set the client model.
        The client model represents the part of the core that communicates with the P2P network, and is vault-agnostic.
    */
    void setClientModel(ClientModel *clientModel = nullptr, interfaces::BlockAndHeaderTipInfo* tip_info = nullptr);
#ifdef ENABLE_VAULT
    void setVaultController(VaultController* vault_controller, bool show_loading_minimized);
    VaultController* getVaultController();
    void markConsensusInitializationFailed();
#endif

#ifdef ENABLE_VAULT
    /** Set the vault model.
        The vault model represents a Quicksilver vault, and offers access to the list of transactions, address book and sending
        functionality.
    */
    void addVault(VaultModel* vaultModel);
    void removeVault(VaultModel* vaultModel);
    void removeAllVaults();
#endif // ENABLE_VAULT
    bool enableVault = false;

    /** Get the tray icon status.
        Some systems have not "system tray" or "notification area" available.
    */
    bool hasTrayIcon() const { return trayIcon; }

    /** Disconnect core signals from GUI client */
    void unsubscribeFromCoreSignals();

    bool isPrivacyModeActivated() const;

protected:
    void changeEvent(QEvent *e) override;
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    interfaces::Node& m_node;
    VaultController* m_vault_controller{nullptr};
    std::unique_ptr<interfaces::Handler> m_handler_message_box;
    std::unique_ptr<interfaces::Handler> m_handler_question;
    ClientModel* clientModel = nullptr;
    VaultFrame* vaultFrame = nullptr;
    bool m_consensus_enabled{false};

    UnitDisplayStatusBarControl* unitDisplayControl = nullptr;
    GUIUtil::ThemedLabel* labelVaultEncryptionIcon = nullptr;
    GUIUtil::ThemedLabel* labelVaultHDStatusIcon = nullptr;
    GUIUtil::ClickableLabel* labelProxyIcon = nullptr;
    GUIUtil::ClickableLabel* connectionsControl = nullptr;
    GUIUtil::ClickableLabel* labelBlocksIcon = nullptr;
    QLabel* progressBarLabel = nullptr;
    GUIUtil::ClickableProgressBar* progressBar = nullptr;
    QProgressDialog* progressDialog = nullptr;

    QMenuBar* appMenuBar = nullptr;
    QToolBar* appToolBar = nullptr;
    QAction* overviewAction = nullptr;
    QAction* historyAction = nullptr;
    QAction* agentAllotmentAction = nullptr;
    QAction* mineMintAction = nullptr;
    QAction* networkAction = nullptr;
    QAction* quitAction = nullptr;
    QAction* sendCoinsAction = nullptr;
    QAction* usedSendingAddressesAction = nullptr;
    QAction* usedReceivingAddressesAction = nullptr;
    QAction* signMessageAction = nullptr;
    QAction* verifyMessageAction = nullptr;
    QAction* m_load_psqt_action = nullptr;
    QAction* m_load_psqt_clipboard_action = nullptr;
    QAction* aboutAction = nullptr;
    QAction* receiveCoinsAction = nullptr;
    QAction* optionsAction = nullptr;
    QAction* encryptVaultAction = nullptr;
    QAction* backupVaultAction = nullptr;
    QAction* changePassphraseAction = nullptr;
    QAction* aboutQtAction = nullptr;
    QAction* openRPCConsoleAction = nullptr;
    QAction* openAction = nullptr;
    QAction* showHelpMessageAction = nullptr;
    QAction* m_create_vault_action{nullptr};
    QAction* m_open_vault_action{nullptr};
    QMenu* m_open_vault_menu{nullptr};
    QAction* m_restore_vault_action{nullptr};
    QAction* m_close_vault_action{nullptr};
    QAction* m_close_all_vaults_action{nullptr};
    QAction* m_vault_selector_label_action = nullptr;
    QAction* m_vault_selector_action = nullptr;
    QAction* m_mask_values_action{nullptr};

    QLabel *m_vault_selector_label = nullptr;
    QComboBox* m_vault_selector = nullptr;

    QSystemTrayIcon* trayIcon = nullptr;
    const std::unique_ptr<QMenu> trayIconMenu;
    Notificator* notificator = nullptr;
    RPCConsole* rpcConsole = nullptr;
    HelpMessageDialog* helpMessageDialog = nullptr;
    ModalOverlay* modalOverlay = nullptr;

    QMenu* m_network_context_menu = new QMenu(this);

#ifdef Q_OS_MACOS
    CAppNapInhibitor* m_app_nap_inhibitor = nullptr;
#endif

    /** Keep track of previous number of blocks, to detect progress */
    int prevBlocks = 0;
    int spinnerFrame = 0;

    const PlatformStyle *platformStyle;
    const NetworkStyle* const m_network_style;

    /** Create the main UI actions. */
    void createActions();
    /** Create the menu bar and sub-menus. */
    void createMenuBar();
    /** Create the toolbars */
    void createToolBars();
    /** Create system tray icon and notification */
    void createTrayIcon();
    /** Create system tray menu (or setup the dock menu) */
    void createTrayIconMenu();

    /** Enable or disable all vault-related actions */
    void setVaultActionsEnabled(bool enabled);
    void setConsensusEnabled(bool enabled);

    /** Start consensus, handing any open vault over to node initialisation first. */
    void beginConsensusActivation();
    /** Explain the close and reopen, and what an in-flight transfer costs. */
    void confirmConsensusVaultHandover(bool proof_of_work_in_flight, std::function<void(bool)> done);

    /** Connect core signals to GUI client */
    void subscribeToCoreSignals();

    /** Update UI with latest network info from model. */
    void updateNetworkState();
    /** Confirm a requested network-context restart before handing it to the application. */
    void confirmNetworkRestart(const QString& chain_token);

    void updateHeadersSyncProgressLabel();
    void updateHeadersPresyncProgressLabel(int64_t height, const QDateTime& blockDate);

    /** Open the OptionsDialog on the specified tab index */
    void openOptionsDialogWithTab(OptionsDialog::Tab tab);

Q_SIGNALS:
    void quitRequested();
    /** Signal raised when a URI was entered or dragged to the GUI */
    void receivedURI(const QString &uri);
    /** Signal raised when RPC console shown */
    void consoleShown(RPCConsole* console);
    void setPrivacy(bool privacy);
    void consensusActivationRequested();
    void networkRestartRequested(const QString& chain_token);

public Q_SLOTS:
    /** Set number of connections shown in the UI */
    void setNumConnections(int count);
    /** Set network state shown in the UI */
    void setNetworkActive(bool network_active);
    /** Set number of blocks and last block date shown in the UI */
    void setNumBlocks(int count, const QDateTime& blockDate, double nVerificationProgress, SyncType synctype, SynchronizationState sync_state);
    /** Launch the vault creation modal (no-op if vault is not compiled) **/
    void createVault();
    /** Open the existing-vault picker (no-op if vault is not compiled) **/
    void openVault();

    /** Notify the user of an event from the core network or transaction handling code.
       @param[in] title             the message box / notification title
       @param[in] message           the displayed text
       @param[in] style             modality and style definitions (icon and used buttons - buttons only for message boxes)
                                    @see CClientUIInterface::MessageBoxFlags
       @param[in] ret               pointer to a bool that will be modified to whether Ok was clicked (modal only). Written when the box is dismissed, not when this returns.
       @param[in] detailed_message  the text to be displayed in the details area
    */
    void message(const QString& title, QString message, unsigned int style, bool* ret = nullptr, const QString& detailed_message = QString());

#ifdef ENABLE_VAULT
    void setCurrentVault(VaultModel* vault_model);
    void setCurrentVaultBySelectorIndex(int index);
    /** Set the UI status indicators based on the currently selected vault.
    */
    void updateVaultStatus();

private:
    /** Set the encryption status as shown in the UI.
       @param[in] status            current encryption status
       @see VaultModel::EncryptionStatus
    */
    void setEncryptionStatus(int status);

    /** Set the hd-enabled status as shown in the UI.
     @param[in] hdEnabled         current hd enabled status
     @see VaultModel::EncryptionStatus
     */
    void setHDStatus(bool privkeyDisabled, int hdEnabled);

public Q_SLOTS:
    bool handlePaymentRequest(const SendCoinsRecipient& recipient);

    /** Show incoming transaction notification for new transactions. */
    void incomingTransaction(const QString& date, QuicksilverUnit unit, const CAmount& amount, const QString& type, const QString& address, const QString& label, const QString& vaultName);
#endif // ENABLE_VAULT

private:
    /** Set the proxy-enabled icon as shown in the UI. */
    void updateProxyIcon();
    void updateWindowTitle();

public Q_SLOTS:
#ifdef ENABLE_VAULT
    /** Switch to overview (home) page */
    void gotoOverviewPage();
    /** Switch to history (transactions) page */
    void gotoHistoryPage();
    /** Switch to agent allotments page */
    void gotoAgentAllotmentPage();
    /** Switch to mine/mint page */
    void gotoMineMintPage();
    /** Switch to network page */
    void gotoNetworkPage();
    /** Switch to request page */
    void gotoReceiveCoinsPage();
    /** Switch to transfer page */
    void gotoSendCoinsPage(QString addr = "");

    /** Show Sign/Verify Message dialog and switch to sign message tab */
    void gotoSignMessageTab(QString addr = "");
    /** Show Sign/Verify Message dialog and switch to verify message tab */
    void gotoVerifyMessageTab(QString addr = "");
    /** Load Partially Signed Quicksilver Transaction from file or clipboard */
    void gotoLoadPSQT(bool from_clipboard = false);
    /** Enable history action when privacy is changed */
    void enableHistoryAction(bool privacy);

    /** Show open dialog */
    void openClicked();
#endif // ENABLE_VAULT
    /** Show configuration dialog */
    void optionsClicked();
    /** Show about dialog */
    void aboutClicked();
    /** Show debug window */
    void showDebugWindow();
    /** Show debug window and set focus to the console */
    void showDebugWindowActivateConsole();
    /** Show help message dialog */
    void showHelpMessageClicked();

    /** Show window if hidden, unminimize when minimized, rise when obscured or show if hidden and fToggleHidden is true */
    void showNormalIfMinimized() { showNormalIfMinimized(false); }
    void showNormalIfMinimized(bool fToggleHidden);
    /** Simply calls showNormalIfMinimized(true) */
    void toggleHidden();

    /** called by a timer to check if shutdown has been requested */
    void detectShutdown();

    /** Show progress dialog e.g. for verifychain */
    void showProgress(const QString &title, int nProgress);

    void showModalOverlay();
};

class UnitDisplayStatusBarControl : public QLabel
{
    Q_OBJECT

public:
    explicit UnitDisplayStatusBarControl(const PlatformStyle *platformStyle);
    /** Lets the control know about the Options Model (and its signals) */
    void setOptionsModel(OptionsModel *optionsModel);

protected:
    /** So that it responds to left-button clicks */
    void mousePressEvent(QMouseEvent *event) override;
    void changeEvent(QEvent* e) override;

private:
    OptionsModel* optionsModel{nullptr};
    QMenu* menu{nullptr};
    const PlatformStyle* m_platform_style;

    /** Shows context menu with Display Unit options by the mouse coordinates */
    void onDisplayUnitsClicked(const QPoint& point);
    /** Creates context menu, its actions, and wires up all the relevant signals for mouse events. */
    void createContextMenu();

private Q_SLOTS:
    /** When Display Units are changed on OptionsModel it will refresh the display text of the control on the status bar */
    void updateDisplayUnit(QuicksilverUnit newUnits);
    /** Tells underlying optionsModel to update its current display unit. */
    void onMenuSelection(QAction* action);
};

#endif // QUICKSILVER_QT_QUICKSILVERGUI_H
