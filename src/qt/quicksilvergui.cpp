// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <quicksilver-build-config.h> // IWYU pragma: keep

#include <qt/quicksilvergui.h>

#include <qt/quicksilverunits.h>
#include <qt/clientmodel.h>
#include <qt/createvaultdialog.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/modaloverlay.h>
#include <qt/networkstyle.h>
#include <qt/notificator.h>
#include <qt/openuridialog.h>
#include <qt/optionsdialog.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/quicksilverstyle.h>
#include <qt/rpcconsole.h>
#include <qt/utilitydialog.h>

#ifdef ENABLE_VAULT
#include <qt/vaultcontroller.h>
#include <qt/vaultframe.h>
#include <qt/vaultmodel.h>
#include <qt/vaultview.h>
#endif // ENABLE_VAULT

#ifdef Q_OS_MACOS
#include <qt/macdockiconhandler.h>
#endif

#include <chainparams.h>
#include <common/system.h>
#include <gpu/detection_service.h>
#include <interfaces/handler.h>
#include <interfaces/node.h>
#include <node/interface_ui.h>
#include <util/translation.h>
#include <validation.h>

#include <functional>
#include <memory>

#include <QAbstractButton>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QCursor>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QEventLoop>
#include <QMessageBox>
#include <QMimeData>
#include <QPointer>
#include <QProgressDialog>
#include <QSemaphore>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QSize>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QWindow>


const std::string QuicksilverGUI::DEFAULT_UIPLATFORM =
#if defined(Q_OS_MACOS)
        "macosx"
#elif defined(Q_OS_WIN)
        "windows"
#else
        "other"
#endif
        ;

QuicksilverGUI::QuicksilverGUI(interfaces::Node& node, const PlatformStyle *_platformStyle, const NetworkStyle *networkStyle, QWidget *parent) :
    QMainWindow(parent),
    m_node(node),
    trayIconMenu{new QMenu()},
    platformStyle(_platformStyle),
    m_network_style(networkStyle)
{
    // Prime the process-wide hardware cache before a transfer page needs it.
    (void)gpu::GpuDetectionService::detectGpu();

    QSettings settings;
    if (!restoreGeometry(settings.value("MainWindowGeometry").toByteArray())) {
        // The platform default was about 717x565 on the Windows launch walk. At
        // that size the command rail left too little room for the three Home
        // cards, so the Mining card was outside the visible page. Start at a
        // deliberate desktop size while still respecting smaller work areas;
        // every page remains inside the as-needed central scroll area below.
        const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
        resize(QSize{1200, 800}.boundedTo(available.size()));
        move(available.center() - frameGeometry().center());
    }

    setContextMenuPolicy(Qt::PreventContextMenu);

#ifdef ENABLE_VAULT
    enableVault = VaultModel::isVaultEnabled();
#endif // ENABLE_VAULT
    QApplication::setWindowIcon(m_network_style->getTrayAndWindowIcon());
    setWindowIcon(m_network_style->getTrayAndWindowIcon());
    updateWindowTitle();

    rpcConsole = new RPCConsole(node, _platformStyle, nullptr);
    helpMessageDialog = new HelpMessageDialog(this, false);
#ifdef ENABLE_VAULT
    if(enableVault)
    {
        /** Create vault frame and make it the central widget */
        auto* content_scroll = new QScrollArea(this);
        content_scroll->setObjectName(QStringLiteral("mainContentScrollArea"));
        content_scroll->setFrameShape(QFrame::NoFrame);
        content_scroll->setWidgetResizable(true);
        content_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        content_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

        vaultFrame = new VaultFrame(_platformStyle, content_scroll);
        connect(vaultFrame, &VaultFrame::createVaultButtonClicked, this, &QuicksilverGUI::createVault);
        connect(vaultFrame, &VaultFrame::openVaultButtonClicked, this, &QuicksilverGUI::openVault);
        connect(vaultFrame, &VaultFrame::message, [this](const QString& title, const QString& message, unsigned int style) {
            this->message(title, message, style);
        });
        connect(vaultFrame, &VaultFrame::currentVaultSet, [this] { updateVaultStatus(); });
        m_consensus_enabled = vaultFrame->consensusEnabled();
        connect(vaultFrame, &VaultFrame::consensusStateChanged, this, &QuicksilverGUI::setConsensusEnabled);
        connect(vaultFrame, &VaultFrame::networkRestartRequested, this, &QuicksilverGUI::confirmNetworkRestart);
        content_scroll->setWidget(vaultFrame);
        setCentralWidget(content_scroll);
    } else
#endif // ENABLE_VAULT
    {
        /* When compiled without vault or -disablevault is provided,
         * the central widget is the rpc console.
         */
        setCentralWidget(rpcConsole);
        Q_EMIT consoleShown(rpcConsole);
    }

    modalOverlay = new ModalOverlay(enableVault, enableVault ? static_cast<QWidget*>(vaultFrame) : this->centralWidget());

    // Accept D&D of URIs
    setAcceptDrops(true);

    // Create actions for the toolbar, menu bar and tray/dock icon
    // Needs vaultFrame to be initialized
    createActions();

    // Create application menu bar
    createMenuBar();

    // Create the toolbars
    createToolBars();

    // Create system tray icon and notification
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        createTrayIcon();
    }
    notificator = new Notificator(QApplication::applicationName(), trayIcon, this);

    // Create status bar
    statusBar();

    // Disable size grip because it looks ugly and nobody needs it
    statusBar()->setSizeGripEnabled(false);

    // Status bar notification icons
    QFrame *frameBlocks = new QFrame();
    frameBlocks->setContentsMargins(0,0,0,0);
    frameBlocks->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    QHBoxLayout *frameBlocksLayout = new QHBoxLayout(frameBlocks);
    frameBlocksLayout->setContentsMargins(3,0,3,0);
    frameBlocksLayout->setSpacing(3);
    unitDisplayControl = new UnitDisplayStatusBarControl(platformStyle);
    labelVaultEncryptionIcon = new GUIUtil::ThemedLabel(platformStyle);
    labelVaultHDStatusIcon = new GUIUtil::ThemedLabel(platformStyle);
    labelProxyIcon = new GUIUtil::ClickableLabel(platformStyle);
    connectionsControl = new GUIUtil::ClickableLabel(platformStyle);
    labelBlocksIcon = new GUIUtil::ClickableLabel(platformStyle);
    if(enableVault)
    {
        frameBlocksLayout->addStretch();
        frameBlocksLayout->addWidget(unitDisplayControl);
        frameBlocksLayout->addStretch();
        frameBlocksLayout->addWidget(labelVaultEncryptionIcon);
        labelVaultEncryptionIcon->hide();
        frameBlocksLayout->addWidget(labelVaultHDStatusIcon);
        labelVaultHDStatusIcon->hide();
    }
    frameBlocksLayout->addWidget(labelProxyIcon);
    frameBlocksLayout->addStretch();
    frameBlocksLayout->addWidget(connectionsControl);
    frameBlocksLayout->addStretch();
    frameBlocksLayout->addWidget(labelBlocksIcon);
    frameBlocksLayout->addStretch();

    // Progress bar and label for blocks download
    progressBarLabel = new QLabel();
    progressBarLabel->setVisible(false);
    progressBar = new GUIUtil::ProgressBar();
    progressBar->setAlignment(Qt::AlignCenter);
    progressBar->setVisible(false);

    // Override style sheet for progress bar for styles that have a segmented progress bar,
    // as they make the text unreadable (workaround for issue #1071)
    // See https://doc.qt.io/qt-5/gallery.html
    QString curStyle = QApplication::style()->metaObject()->className();
    if(curStyle == "QWindowsStyle" || curStyle == "QWindowsXPStyle")
    {
        progressBar->setStyleSheet("QProgressBar { background-color: #e8e8e8; border: 1px solid grey; border-radius: 7px; padding: 1px; text-align: center; } QProgressBar::chunk { background: QLinearGradient(x1: 0, y1: 0, x2: 1, y2: 0, stop: 0 #FF8000, stop: 1 orange); border-radius: 7px; margin: 0px; }");
    }

    statusBar()->addWidget(progressBarLabel);
    statusBar()->addWidget(progressBar);
    statusBar()->addPermanentWidget(frameBlocks);

    // Install event filter to be able to catch status tip events (QEvent::StatusTip)
    this->installEventFilter(this);

    // Initially vault actions should be disabled
    setVaultActionsEnabled(false);

    // Subscribe to notifications from core
    subscribeToCoreSignals();

    connect(labelProxyIcon, &GUIUtil::ClickableLabel::clicked, [this] {
        openOptionsDialogWithTab(OptionsDialog::TAB_NETWORK);
    });

    connect(labelBlocksIcon, &GUIUtil::ClickableLabel::clicked, this, &QuicksilverGUI::showModalOverlay);
    connect(progressBar, &GUIUtil::ClickableProgressBar::clicked, this, &QuicksilverGUI::showModalOverlay);

#ifdef Q_OS_MACOS
    m_app_nap_inhibitor = new CAppNapInhibitor;
#endif

    GUIUtil::handleCloseWindowShortcut(this);
}

QuicksilverGUI::~QuicksilverGUI()
{
    // Unsubscribe from notifications from core
    unsubscribeFromCoreSignals();

    QSettings settings;
    settings.setValue("MainWindowGeometry", saveGeometry());
    if(trayIcon) // Hide tray icon, as deleting will let it linger until quit (on Ubuntu)
        trayIcon->hide();
#ifdef Q_OS_MACOS
    delete m_app_nap_inhibitor;
    MacDockIconHandler::cleanup();
#endif

    delete rpcConsole;
}

void QuicksilverGUI::createActions()
{
    QActionGroup *tabGroup = new QActionGroup(this);
    connect(modalOverlay, &ModalOverlay::triggered, tabGroup, &QActionGroup::setEnabled);

    overviewAction = new QAction(platformStyle->ColorIcon(":/icons/overview", QuicksilverStyle::Color(QuicksilverStyle::Token::CinnabarBright)), tr("&Home"), this);
    overviewAction->setObjectName(QStringLiteral("homeBootstrapAction"));
    overviewAction->setStatusTip(tr("Show the Quicksilver launch screen"));
    overviewAction->setToolTip(overviewAction->statusTip());
    overviewAction->setCheckable(true);
    overviewAction->setShortcut(QKeySequence(QStringLiteral("Alt+1")));
    tabGroup->addAction(overviewAction);

    sendCoinsAction = new QAction(platformStyle->ColorIcon(":/icons/send", QuicksilverStyle::Color(QuicksilverStyle::Token::Amber)), tr("&Transfer"), this);
    sendCoinsAction->setObjectName(QStringLiteral("sendCoinsAction"));
    sendCoinsAction->setStatusTip(tr("Transfer Quicksilver to an address"));
    sendCoinsAction->setToolTip(sendCoinsAction->statusTip());
    sendCoinsAction->setCheckable(true);
    sendCoinsAction->setShortcut(QKeySequence(QStringLiteral("Alt+2")));
    tabGroup->addAction(sendCoinsAction);

    receiveCoinsAction = new QAction(platformStyle->ColorIcon(":/icons/receiving_addresses", QuicksilverStyle::Color(QuicksilverStyle::Token::Teal)), tr("&Request"), this);
    receiveCoinsAction->setObjectName(QStringLiteral("receiveCoinsAction"));
    receiveCoinsAction->setStatusTip(tr("Create receiving addresses, QR codes, and quicksilver: URIs"));
    receiveCoinsAction->setToolTip(receiveCoinsAction->statusTip());
    receiveCoinsAction->setCheckable(true);
    receiveCoinsAction->setShortcut(QKeySequence(QStringLiteral("Alt+3")));
    tabGroup->addAction(receiveCoinsAction);

    historyAction = new QAction(platformStyle->ColorIcon(":/icons/history", QuicksilverStyle::Color(QuicksilverStyle::Token::SilverMuted)), tr("&Ledger"), this);
    historyAction->setObjectName(QStringLiteral("historyAction"));
    historyAction->setStatusTip(tr("Browse ledger activity"));
    historyAction->setToolTip(historyAction->statusTip());
    historyAction->setCheckable(true);
    historyAction->setShortcut(QKeySequence(QStringLiteral("Alt+4")));
    tabGroup->addAction(historyAction);

    agentAllotmentAction = new QAction(platformStyle->ColorIcon(":/icons/agent", QuicksilverStyle::Color(QuicksilverStyle::Token::Violet)), tr("&Agents"), this);
    agentAllotmentAction->setObjectName(QStringLiteral("agentAllotmentAction"));
    agentAllotmentAction->setStatusTip(tr("Fund and review shared-key agent allotments"));
    agentAllotmentAction->setToolTip(agentAllotmentAction->statusTip());
    agentAllotmentAction->setCheckable(true);
    agentAllotmentAction->setShortcut(QKeySequence(QStringLiteral("Alt+5")));
    tabGroup->addAction(agentAllotmentAction);

    mineMintAction = new QAction(platformStyle->ColorIcon(":/icons/tx_mined", QuicksilverStyle::Color(QuicksilverStyle::Token::Amber)), tr("&Mine / Mint"), this);
    mineMintAction->setObjectName(QStringLiteral("mineMintAction"));
    mineMintAction->setStatusTip(tr("Show mining and minting setup status"));
    mineMintAction->setToolTip(mineMintAction->statusTip());
    mineMintAction->setCheckable(true);
    mineMintAction->setShortcut(QKeySequence(QStringLiteral("Alt+6")));
    tabGroup->addAction(mineMintAction);

    networkAction = new QAction(platformStyle->ColorIcon(":/icons/connect_4", QuicksilverStyle::Color(QuicksilverStyle::Token::Teal)), tr("&Network"), this);
    networkAction->setObjectName(QStringLiteral("networkAction"));
    networkAction->setStatusTip(tr("Show network bootstrap health"));
    networkAction->setToolTip(networkAction->statusTip());
    networkAction->setCheckable(true);
    networkAction->setShortcut(QKeySequence(QStringLiteral("Alt+7")));
    tabGroup->addAction(networkAction);

#ifdef ENABLE_VAULT
    // These showNormalIfMinimized calls are needed because transfer and request
    // can be triggered from the tray menu, and need to show the GUI to be useful.
    connect(overviewAction, &QAction::triggered, [this]{ showNormalIfMinimized(); });
    connect(overviewAction, &QAction::triggered, this, &QuicksilverGUI::gotoOverviewPage);
    connect(sendCoinsAction, &QAction::triggered, [this]{ showNormalIfMinimized(); });
    connect(sendCoinsAction, &QAction::triggered, [this]{ gotoSendCoinsPage(); });
    connect(receiveCoinsAction, &QAction::triggered, [this]{ showNormalIfMinimized(); });
    connect(receiveCoinsAction, &QAction::triggered, this, &QuicksilverGUI::gotoReceiveCoinsPage);
    connect(historyAction, &QAction::triggered, [this]{ showNormalIfMinimized(); });
    connect(historyAction, &QAction::triggered, this, &QuicksilverGUI::gotoHistoryPage);
    connect(agentAllotmentAction, &QAction::triggered, [this]{ showNormalIfMinimized(); });
    connect(agentAllotmentAction, &QAction::triggered, this, &QuicksilverGUI::gotoAgentAllotmentPage);
    connect(mineMintAction, &QAction::triggered, [this]{ showNormalIfMinimized(); });
    connect(mineMintAction, &QAction::triggered, this, &QuicksilverGUI::gotoMineMintPage);
    connect(networkAction, &QAction::triggered, [this]{ showNormalIfMinimized(); });
    connect(networkAction, &QAction::triggered, this, &QuicksilverGUI::gotoNetworkPage);
#endif // ENABLE_VAULT

    quitAction = new QAction(tr("E&xit"), this);
    quitAction->setStatusTip(tr("Quit application"));
    quitAction->setShortcut(QKeySequence(tr("Ctrl+Q")));
    quitAction->setMenuRole(QAction::QuitRole);
    aboutAction = new QAction(tr("&About %1").arg(CLIENT_NAME), this);
    aboutAction->setStatusTip(tr("Show information about %1").arg(CLIENT_NAME));
    aboutAction->setMenuRole(QAction::AboutRole);
    aboutAction->setEnabled(false);
    aboutQtAction = new QAction(tr("About &Qt"), this);
    aboutQtAction->setStatusTip(tr("Show information about Qt"));
    aboutQtAction->setMenuRole(QAction::AboutQtRole);
    optionsAction = new QAction(tr("&Options…"), this);
    optionsAction->setStatusTip(tr("Modify configuration options for %1").arg(CLIENT_NAME));
    optionsAction->setMenuRole(QAction::PreferencesRole);
    optionsAction->setEnabled(false);

    encryptVaultAction = new QAction(tr("&Encrypt Vault…"), this);
    encryptVaultAction->setStatusTip(tr("Encrypt the private keys that belong to your vault"));
    encryptVaultAction->setCheckable(true);
    backupVaultAction = new QAction(tr("&Backup Vault…"), this);
    backupVaultAction->setStatusTip(tr("Backup vault to another location"));
    changePassphraseAction = new QAction(tr("&Change Passphrase…"), this);
    changePassphraseAction->setStatusTip(tr("Change the passphrase used for vault encryption"));
    signMessageAction = new QAction(tr("Sign &message…"), this);
    signMessageAction->setStatusTip(tr("Sign messages with your Quicksilver addresses to prove you own them"));
    verifyMessageAction = new QAction(tr("&Verify message…"), this);
    verifyMessageAction->setStatusTip(tr("Verify messages to ensure they were signed with specified Quicksilver addresses"));
    m_load_psqt_action = new QAction(tr("&Load PSQT from file…"), this);
    m_load_psqt_action->setStatusTip(tr("Load Partially Signed Quicksilver Transaction"));
    m_load_psqt_clipboard_action = new QAction(tr("Load PSQT from &clipboard…"), this);
    m_load_psqt_clipboard_action->setStatusTip(tr("Load Partially Signed Quicksilver Transaction from clipboard"));

    openRPCConsoleAction = new QAction(tr("Node window"), this);
    openRPCConsoleAction->setStatusTip(tr("Open node debugging and diagnostic console"));
    // initially disable the debug window menu item
    openRPCConsoleAction->setEnabled(false);
    openRPCConsoleAction->setObjectName("openRPCConsoleAction");

    usedSendingAddressesAction = new QAction(tr("&Destination addresses"), this);
    usedSendingAddressesAction->setStatusTip(tr("Show saved transfer destination addresses and labels"));
    usedReceivingAddressesAction = new QAction(tr("&Receiving addresses"), this);
    usedReceivingAddressesAction->setStatusTip(tr("Show saved receiving addresses and labels"));

    openAction = new QAction(tr("Open &URI…"), this);
    openAction->setStatusTip(tr("Open a quicksilver: URI"));

    m_open_vault_action = new QAction(tr("Open Vault"), this);
    m_open_vault_action->setEnabled(false);
    m_open_vault_action->setStatusTip(tr("Open a vault"));
    m_open_vault_menu = new QMenu(this);

    m_close_vault_action = new QAction(tr("Close Vault…"), this);
    m_close_vault_action->setStatusTip(tr("Close vault"));

    m_create_vault_action = new QAction(tr("Create Vault…"), this);
    m_create_vault_action->setEnabled(false);
    m_create_vault_action->setStatusTip(tr("Create a new vault"));

    //: Name of the menu item that restores vault from a backup file.
    m_restore_vault_action = new QAction(tr("Restore Vault…"), this);
    m_restore_vault_action->setEnabled(false);
    //: Status tip for Restore Vault menu item
    m_restore_vault_action->setStatusTip(tr("Restore a vault from a backup file"));

    m_close_all_vaults_action = new QAction(tr("Close All Vaults…"), this);
    m_close_all_vaults_action->setStatusTip(tr("Close all vault"));

    showHelpMessageAction = new QAction(tr("&Command-line options"), this);
    showHelpMessageAction->setMenuRole(QAction::NoRole);
    showHelpMessageAction->setStatusTip(tr("Show the %1 help message to get a list with possible Quicksilver command-line options").arg(CLIENT_NAME));

    m_mask_values_action = new QAction(tr("&Mask values"), this);
    m_mask_values_action->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M));
    m_mask_values_action->setStatusTip(tr("Mask values in the HUD"));
    m_mask_values_action->setCheckable(true);

    connect(quitAction, &QAction::triggered, this, &QuicksilverGUI::quitRequested);
    connect(aboutAction, &QAction::triggered, this, &QuicksilverGUI::aboutClicked);
    connect(aboutQtAction, &QAction::triggered, qApp, QApplication::aboutQt);
    connect(optionsAction, &QAction::triggered, this, &QuicksilverGUI::optionsClicked);
    connect(showHelpMessageAction, &QAction::triggered, this, &QuicksilverGUI::showHelpMessageClicked);
    connect(openRPCConsoleAction, &QAction::triggered, this, &QuicksilverGUI::showDebugWindow);
    // prevents an open debug window from becoming stuck/unusable on client shutdown
    connect(quitAction, &QAction::triggered, rpcConsole, &QWidget::hide);

#ifdef ENABLE_VAULT
    if(vaultFrame)
    {
        connect(encryptVaultAction, &QAction::triggered, vaultFrame, &VaultFrame::encryptVault);
        connect(backupVaultAction, &QAction::triggered, vaultFrame, &VaultFrame::backupVault);
        connect(changePassphraseAction, &QAction::triggered, vaultFrame, &VaultFrame::changePassphrase);
        connect(signMessageAction, &QAction::triggered, [this]{ showNormalIfMinimized(); });
        connect(signMessageAction, &QAction::triggered, [this]{ gotoSignMessageTab(); });
        connect(m_load_psqt_action, &QAction::triggered, [this]{ gotoLoadPSQT(); });
        connect(m_load_psqt_clipboard_action, &QAction::triggered, [this]{ gotoLoadPSQT(true); });
        connect(verifyMessageAction, &QAction::triggered, [this]{ showNormalIfMinimized(); });
        connect(verifyMessageAction, &QAction::triggered, [this]{ gotoVerifyMessageTab(); });
        connect(usedSendingAddressesAction, &QAction::triggered, vaultFrame, &VaultFrame::usedSendingAddresses);
        connect(usedReceivingAddressesAction, &QAction::triggered, vaultFrame, &VaultFrame::usedReceivingAddresses);
        connect(openAction, &QAction::triggered, this, &QuicksilverGUI::openClicked);
        connect(m_open_vault_menu, &QMenu::aboutToShow, [this] {
            m_open_vault_menu->clear();
            for (const auto& [path, info] : m_vault_controller->listVaultDir()) {
                const auto& [loaded, _] = info;
                QString name = GUIUtil::VaultDisplayName(path);
                // An single ampersand in the menu item's text sets a shortcut for this item.
                // Single & are shown when && is in the string. So replace & with &&.
                name.replace(QChar('&'), QString("&&"));
                QAction* action = m_open_vault_menu->addAction(name);

                if (loaded) {
                    // This vault is already loaded
                    action->setEnabled(false);
                    continue;
                }

                connect(action, &QAction::triggered, [this, path] {
                    auto activity = new OpenVaultActivity(m_vault_controller, this);
                    connect(activity, &OpenVaultActivity::opened, this, &QuicksilverGUI::setCurrentVault, Qt::QueuedConnection);
                    connect(activity, &OpenVaultActivity::opened, rpcConsole, &RPCConsole::setCurrentVault, Qt::QueuedConnection);
                    activity->open(path);
                });
            }
            if (m_open_vault_menu->isEmpty()) {
                QAction* action = m_open_vault_menu->addAction(tr("No vault available"));
                action->setEnabled(false);
            }
        });
        connect(m_restore_vault_action, &QAction::triggered, [this] {
            //: Name of the vault data file format.
            QString name_data_file = tr("Vault Data");

            //: The title for Restore Vault File Windows
            QString title_windows = tr("Load Vault Backup");

            QPointer<QuicksilverGUI> self{this};
            GUIUtil::getOpenFileName(this, title_windows, QString(), name_data_file + QLatin1String(" (*.dat)"),
                [self](const QString& backup_file) {
                    if (!self || backup_file.isEmpty()) return;
                    /*: Title of pop-up window shown when the user is attempting to
                        restore a vault. */
                    QString title = self->tr("Restore Vault");
                    //: Label of the input field where the name of the vault is entered.
                    QString label = self->tr("Vault Name");
                    GUIUtil::getText(self, title, label, [self, backup_file](const QString& vault_name, bool ok) {
                        if (!self || !ok || vault_name.isEmpty()) return;
                        auto activity = new RestoreVaultActivity(self->m_vault_controller, self);
                        connect(activity, &RestoreVaultActivity::restored, self, &QuicksilverGUI::setCurrentVault, Qt::QueuedConnection);
                        connect(activity, &RestoreVaultActivity::restored, self->rpcConsole, &RPCConsole::setCurrentVault, Qt::QueuedConnection);
                        auto backup_file_path = fs::PathFromString(backup_file.toStdString());
                        activity->restore(backup_file_path, vault_name.toStdString());
                    });
                });
        });
        connect(m_close_vault_action, &QAction::triggered, [this] {
            m_vault_controller->closeVault(vaultFrame->currentVaultModel(), this);
        });
        connect(m_create_vault_action, &QAction::triggered, this, &QuicksilverGUI::createVault);
        connect(m_close_all_vaults_action, &QAction::triggered, [this] {
            m_vault_controller->closeAllVaults(this);
        });
        connect(m_mask_values_action, &QAction::toggled, this, &QuicksilverGUI::setPrivacy);
        connect(m_mask_values_action, &QAction::toggled, this, &QuicksilverGUI::enableHistoryAction);
        connect(m_mask_values_action, &QAction::toggled, vaultFrame, &VaultFrame::setPrivacy);
        connect(vaultFrame, &VaultFrame::privacyRequested, m_mask_values_action, &QAction::setChecked);
    }
#endif // ENABLE_VAULT

    connect(new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), this), &QShortcut::activated, this, &QuicksilverGUI::showDebugWindowActivateConsole);
    connect(new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D), this), &QShortcut::activated, this, &QuicksilverGUI::showDebugWindow);
}

void QuicksilverGUI::createMenuBar()
{
    appMenuBar = menuBar();

    // Configure the menus
    QMenu *file = appMenuBar->addMenu(tr("&Vault"));
    file->setObjectName(QStringLiteral("vaultMenu"));
    if(vaultFrame)
    {
        file->addAction(m_create_vault_action);
        file->addAction(m_open_vault_action);
        file->addAction(m_close_vault_action);
        file->addAction(m_close_all_vaults_action);
        file->addSeparator();
        file->addAction(backupVaultAction);
        file->addAction(m_restore_vault_action);
        file->addSeparator();
        file->addAction(openAction);
        file->addAction(signMessageAction);
        file->addAction(verifyMessageAction);
        file->addAction(m_load_psqt_action);
        file->addAction(m_load_psqt_clipboard_action);
        file->addSeparator();
    }
    file->addAction(quitAction);

    QMenu *settings = appMenuBar->addMenu(tr("&Controls"));
    settings->setObjectName(QStringLiteral("controlsMenu"));
    if(vaultFrame)
    {
        settings->addAction(encryptVaultAction);
        settings->addAction(changePassphraseAction);
        settings->addSeparator();
        settings->addAction(m_mask_values_action);
        settings->addSeparator();
    }
    settings->addAction(optionsAction);

    QMenu* window_menu = appMenuBar->addMenu(tr("&Panels"));
    window_menu->setObjectName(QStringLiteral("panelsMenu"));

    QAction* minimize_action = window_menu->addAction(tr("&Minimize"));
    minimize_action->setShortcut(QKeySequence(tr("Ctrl+M")));
    connect(minimize_action, &QAction::triggered, [] {
        QApplication::activeWindow()->showMinimized();
    });
    connect(qApp, &QApplication::focusWindowChanged, this, [minimize_action] (QWindow* window) {
        minimize_action->setEnabled(window != nullptr && (window->flags() & Qt::Dialog) != Qt::Dialog && window->windowState() != Qt::WindowMinimized);
    });

#ifdef Q_OS_MACOS
    QAction* zoom_action = window_menu->addAction(tr("Zoom"));
    connect(zoom_action, &QAction::triggered, [] {
        QWindow* window = qApp->focusWindow();
        if (window->windowState() != Qt::WindowMaximized) {
            window->showMaximized();
        } else {
            window->showNormal();
        }
    });

    connect(qApp, &QApplication::focusWindowChanged, this, [zoom_action] (QWindow* window) {
        zoom_action->setEnabled(window != nullptr);
    });
#endif

    if (vaultFrame) {
#ifdef Q_OS_MACOS
        window_menu->addSeparator();
        QAction* main_window_action = window_menu->addAction(tr("Main HUD"));
        connect(main_window_action, &QAction::triggered, [this] {
            GUIUtil::bringToFront(this);
        });
#endif
        window_menu->addSeparator();
        window_menu->addAction(usedSendingAddressesAction);
        window_menu->addAction(usedReceivingAddressesAction);
    }

    window_menu->addSeparator();
    for (RPCConsole::TabTypes tab_type : rpcConsole->tabs()) {
        QAction* tab_action = window_menu->addAction(rpcConsole->tabTitle(tab_type));
        tab_action->setShortcut(rpcConsole->tabShortcut(tab_type));
        connect(tab_action, &QAction::triggered, [this, tab_type] {
            rpcConsole->setTabFocus(tab_type);
            showDebugWindow();
        });
    }

    QMenu *help = appMenuBar->addMenu(tr("&Signal"));
    help->setObjectName(QStringLiteral("signalMenu"));
    help->addAction(showHelpMessageAction);
    help->addSeparator();
    help->addAction(aboutAction);
    help->addAction(aboutQtAction);
}

void QuicksilverGUI::createToolBars()
{
    if(vaultFrame)
    {
        QToolBar *toolbar = new QToolBar(tr("Command rail"), this);
        appToolBar = toolbar;
        toolbar->setObjectName(QStringLiteral("primaryCommandRail"));
        toolbar->setMovable(false);
        toolbar->setFloatable(false);
        toolbar->setAllowedAreas(Qt::LeftToolBarArea | Qt::RightToolBarArea);
        toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        toolbar->setIconSize(QSize(22, 22));
        addToolBar(Qt::LeftToolBarArea, toolbar);

        auto* brand = new QFrame(toolbar);
        brand->setObjectName(QStringLiteral("commandRailBrand"));
        auto* brand_layout = new QHBoxLayout(brand);
        brand_layout->setContentsMargins(5, 3, 5, 0);
        brand_layout->setSpacing(9);

        auto* brand_mark = new QLabel(brand);
        brand_mark->setObjectName(QStringLiteral("commandRailBrandMark"));
        brand_mark->setPixmap(m_network_style->getAppIcon().pixmap(QSize(30, 30)));
        brand_mark->setFixedSize(QSize(30, 30));
        brand_layout->addWidget(brand_mark);

        auto* brand_title = new QLabel(QStringLiteral("QUICKSILVER"), brand);
        brand_title->setObjectName(QStringLiteral("commandRailBrandTitle"));
        brand_layout->addWidget(brand_title, 1, Qt::AlignVCenter);
        toolbar->addWidget(brand);

        auto* section_label = new QLabel(tr("Navigation").toUpper(), toolbar);
        section_label->setObjectName(QStringLiteral("commandRailSectionLabel"));
        toolbar->addWidget(section_label);

        const auto add_navigation_action = [toolbar](QAction* action, const char* accent) {
            toolbar->addAction(action);
            toolbar->widgetForAction(action)->setProperty("accent", QString::fromLatin1(accent));
        };
        add_navigation_action(overviewAction, "cinnabar");
        add_navigation_action(sendCoinsAction, "amber");
        add_navigation_action(receiveCoinsAction, "teal");
        add_navigation_action(agentAllotmentAction, "violet");
        add_navigation_action(historyAction, "silver");
        add_navigation_action(mineMintAction, "amber");
        add_navigation_action(networkAction, "teal");
        overviewAction->setChecked(true);

#ifdef ENABLE_VAULT
        QWidget *spacer = new QWidget();
        spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        toolbar->addWidget(spacer);

        m_vault_selector = new QComboBox();
        m_vault_selector->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        connect(m_vault_selector, qOverload<int>(&QComboBox::currentIndexChanged), this, &QuicksilverGUI::setCurrentVaultBySelectorIndex);

        m_vault_selector_label = new QLabel();
        m_vault_selector_label->setText(tr("Vault:") + " ");
        m_vault_selector_label->setBuddy(m_vault_selector);

        m_vault_selector_label_action = appToolBar->addWidget(m_vault_selector_label);
        m_vault_selector_action = appToolBar->addWidget(m_vault_selector);

        m_vault_selector_label_action->setVisible(false);
        m_vault_selector_action->setVisible(false);
#endif
    }
}

void QuicksilverGUI::setClientModel(ClientModel *_clientModel, interfaces::BlockAndHeaderTipInfo* tip_info)
{
    this->clientModel = _clientModel;
    if(_clientModel)
    {
        // Create system tray menu (or setup the dock menu) that late to prevent users from calling actions,
        // while the client has not yet fully loaded
        createTrayIconMenu();

        // Keep up to date with client
        setNetworkActive(m_node.getNetworkActive());
        connect(connectionsControl, &GUIUtil::ClickableLabel::clicked, [this] {
            GUIUtil::PopupMenu(m_network_context_menu, QCursor::pos());
        });
        connect(_clientModel, &ClientModel::numConnectionsChanged, this, &QuicksilverGUI::setNumConnections);
        connect(_clientModel, &ClientModel::networkActiveChanged, this, &QuicksilverGUI::setNetworkActive);

        modalOverlay->setKnownBestHeight(tip_info->header_height, QDateTime::fromSecsSinceEpoch(tip_info->header_time), /*presync=*/false);
        setNumBlocks(tip_info->block_height, QDateTime::fromSecsSinceEpoch(tip_info->block_time), tip_info->verification_progress, SyncType::BLOCK_SYNC, SynchronizationState::INIT_DOWNLOAD);
        connect(_clientModel, &ClientModel::numBlocksChanged, this, &QuicksilverGUI::setNumBlocks);

        // Receive and report messages from client model
        connect(_clientModel, &ClientModel::message, [this](const QString &title, const QString &message, unsigned int style){
            this->message(title, message, style);
        });

        // Show progress dialog
        connect(_clientModel, &ClientModel::showProgress, this, &QuicksilverGUI::showProgress);

        rpcConsole->setClientModel(_clientModel, tip_info->block_height, tip_info->block_time, tip_info->verification_progress);

        updateProxyIcon();

#ifdef ENABLE_VAULT
        if(vaultFrame)
        {
            vaultFrame->setClientModel(_clientModel);
        }
#endif // ENABLE_VAULT
        unitDisplayControl->setOptionsModel(_clientModel->getOptionsModel());

        OptionsModel* optionsModel = _clientModel->getOptionsModel();
        if (optionsModel && trayIcon) {
            // be aware of the tray icon disable state change reported by the OptionsModel object.
            connect(optionsModel, &OptionsModel::showTrayIconChanged, trayIcon, &QSystemTrayIcon::setVisible);

            // initialize the disable state of the tray icon with the current value in the model.
            trayIcon->setVisible(optionsModel->getShowTrayIcon());
        }

        m_mask_values_action->setChecked(_clientModel->getOptionsModel()->getOption(OptionsModel::OptionID::MaskValues).toBool());
    } else {
        // Shutdown requested, disable menus
        if (trayIconMenu)
        {
            // Disable context menu on tray icon
            trayIconMenu->clear();
        }
        // Propagate cleared model to child objects
        rpcConsole->setClientModel(nullptr);
#ifdef ENABLE_VAULT
        if (vaultFrame)
        {
            vaultFrame->setClientModel(nullptr);
        }
#endif // ENABLE_VAULT
        unitDisplayControl->setOptionsModel(nullptr);
        // Disable top bar menu actions
        appMenuBar->clear();
    }
}

#ifdef ENABLE_VAULT
void QuicksilverGUI::enableHistoryAction(bool privacy)
{
    if (vaultFrame->currentVaultModel()) {
        historyAction->setEnabled(!privacy);
        if (historyAction->isChecked()) gotoOverviewPage();
    }
}

void QuicksilverGUI::setVaultController(VaultController* vault_controller, bool show_loading_minimized)
{
    assert(!m_vault_controller);
    assert(vault_controller);

    m_vault_controller = vault_controller;
    vaultFrame->setVaultRuntimeAvailable(true);

    m_create_vault_action->setEnabled(true);
    m_open_vault_action->setEnabled(true);
    m_open_vault_action->setMenu(m_open_vault_menu);
    m_restore_vault_action->setEnabled(true);

    GUIUtil::ExceptionSafeConnect(vault_controller, &VaultController::vaultAdded, this, &QuicksilverGUI::addVault);
    connect(vault_controller, &VaultController::vaultRemoved, this, &QuicksilverGUI::removeVault);
    connect(vault_controller, &VaultController::destroyed, this, [this] {
        // vault_controller gets destroyed manually, but it leaves our member copy dangling
        m_vault_controller = nullptr;
        m_create_vault_action->setEnabled(false);
        m_open_vault_action->setEnabled(false);
        m_open_vault_action->setMenu(nullptr);
        m_restore_vault_action->setEnabled(false);
        setVaultActionsEnabled(false);
        if (vaultFrame) vaultFrame->setVaultRuntimeAvailable(false);
    });

    auto activity = new LoadVaultsActivity(m_vault_controller, this);
    activity->load(show_loading_minimized);
}

VaultController* QuicksilverGUI::getVaultController()
{
    return m_vault_controller;
}

void QuicksilverGUI::addVault(VaultModel* vaultModel)
{
    if (!vaultFrame || !m_vault_controller) return;

    VaultView* vault_view = new VaultView(vaultModel, platformStyle, vaultFrame);
    if (!vaultFrame->addView(vault_view)) return;

    rpcConsole->addVault(vaultModel);
    if (m_vault_selector->count() == 0) {
        setVaultActionsEnabled(true);
    } else if (m_vault_selector->count() == 1) {
        m_vault_selector_label_action->setVisible(true);
        m_vault_selector_action->setVisible(true);
    }

    connect(vault_view, &VaultView::outOfSyncWarningClicked, this, &QuicksilverGUI::showModalOverlay);
    connect(vault_view, &VaultView::transactionClicked, this, &QuicksilverGUI::gotoHistoryPage);
    connect(vault_view, &VaultView::coinsSent, this, &QuicksilverGUI::gotoHistoryPage);
    connect(vault_view, &VaultView::solverSettingsRequested, this, [this] {
        openOptionsDialogWithTab(OptionsDialog::TAB_MAIN);
    });
    connect(vault_view, &VaultView::message, [this](const QString& title, const QString& message, unsigned int style) {
        this->message(title, message, style);
    });
    connect(vault_view, &VaultView::encryptionStatusChanged, this, &QuicksilverGUI::updateVaultStatus);
    connect(vault_view, &VaultView::incomingTransaction, this, &QuicksilverGUI::incomingTransaction);
    connect(this, &QuicksilverGUI::setPrivacy, vault_view, &VaultView::setPrivacy);
    const bool privacy = isPrivacyModeActivated();
    vault_view->setPrivacy(privacy);
    enableHistoryAction(privacy);
    const QString display_name = vaultModel->getDisplayName();
    m_vault_selector->addItem(display_name, QVariant::fromValue(vaultModel));
}

void QuicksilverGUI::removeVault(VaultModel* vaultModel)
{
    if (!vaultFrame) return;

    labelVaultHDStatusIcon->hide();
    labelVaultEncryptionIcon->hide();

    int index = m_vault_selector->findData(QVariant::fromValue(vaultModel));
    m_vault_selector->removeItem(index);
    if (m_vault_selector->count() == 0) {
        setVaultActionsEnabled(false);
        overviewAction->setChecked(true);
    } else if (m_vault_selector->count() == 1) {
        m_vault_selector_label_action->setVisible(false);
        m_vault_selector_action->setVisible(false);
    }
    rpcConsole->removeVault(vaultModel);
    vaultFrame->removeVault(vaultModel);
    updateWindowTitle();
}

void QuicksilverGUI::setCurrentVault(VaultModel* vault_model)
{
    if (!vaultFrame || !m_vault_controller) return;
    vaultFrame->setCurrentVault(vault_model);
    for (int index = 0; index < m_vault_selector->count(); ++index) {
        if (m_vault_selector->itemData(index).value<VaultModel*>() == vault_model) {
            m_vault_selector->setCurrentIndex(index);
            break;
        }
    }
    updateWindowTitle();
}

void QuicksilverGUI::setCurrentVaultBySelectorIndex(int index)
{
    VaultModel* vault_model = m_vault_selector->itemData(index).value<VaultModel*>();
    if (vault_model) setCurrentVault(vault_model);
}

void QuicksilverGUI::removeAllVaults()
{
    if(!vaultFrame)
        return;
    setVaultActionsEnabled(false);
    vaultFrame->removeAllVaults();
}

void QuicksilverGUI::markConsensusInitializationFailed()
{
    if (!vaultFrame) return;
    vaultFrame->markConsensusInitializationFailed();
    m_consensus_enabled = false;
    setVaultActionsEnabled(vaultFrame->currentVaultModel());
}
#endif // ENABLE_VAULT

void QuicksilverGUI::setVaultActionsEnabled(bool enabled)
{
    overviewAction->setEnabled(enableVault);
    sendCoinsAction->setEnabled(enabled);
    receiveCoinsAction->setEnabled(enabled);
    agentAllotmentAction->setEnabled(enabled);
    historyAction->setEnabled(enabled);
    // Keep the capability reachable while locked. VaultFrame routes it to the
    // consensus cost/consent page, which is more useful than a silent disabled
    // toolbar item.
    mineMintAction->setEnabled(enableVault);
    mineMintAction->setStatusTip(m_consensus_enabled
        ? tr("Show mining and minting setup status")
        : tr("Review consensus cost before mining can be enabled"));
    mineMintAction->setToolTip(mineMintAction->statusTip());
    networkAction->setEnabled(enableVault);
    encryptVaultAction->setEnabled(enabled);
    backupVaultAction->setEnabled(enabled);
    changePassphraseAction->setEnabled(enabled);
    signMessageAction->setEnabled(enabled);
    verifyMessageAction->setEnabled(enabled);
    usedSendingAddressesAction->setEnabled(enabled);
    usedReceivingAddressesAction->setEnabled(enabled);
    openAction->setEnabled(enabled);
    m_close_vault_action->setEnabled(enabled);
    m_close_all_vaults_action->setEnabled(enabled);
}

void QuicksilverGUI::setConsensusEnabled(bool enabled)
{
    const bool was_enabled = m_consensus_enabled;
    m_consensus_enabled = enabled;
    setVaultActionsEnabled(vaultFrame && vaultFrame->currentVaultModel());
    if (enabled && !was_enabled && !clientModel) {
        beginConsensusActivation();
    }
}

void QuicksilverGUI::beginConsensusActivation()
{
    // With no vault open there is nothing to hand over: node initialisation opens
    // whatever the startup list names, which is the ordinary path.
    const bool vault_open = m_vault_controller && vaultFrame && vaultFrame->currentVaultModel();
    if (!vault_open) {
        Q_EMIT consensusActivationRequested();
        return;
    }

    // A vault open in this process holds an exclusive lock on its file, and node
    // initialisation has to take that same lock to attach the vault to the chain it
    // is about to build. Only one of them can hold it, and node initialisation is the
    // one that ends with a working vault: a vault opened without consensus was never
    // attached to a chain at all, so it has to be reopened rather than adopted.
    const bool grinding = m_vault_controller->proofOfWorkInFlight();
    confirmConsensusVaultHandover(grinding, [this](bool proceed) {
        if (!proceed) {
            // The opt-in was already saved by the page that offered it. Put it back, or
            // the desktop would believe consensus is on while nothing ever starts it.
            // The review page already navigated to the status screen under the modal;
            // send the user home rather than leave them on a consensus page that is
            // not going to start.
            if (vaultFrame) {
                vaultFrame->revertConsensusOptIn();
                vaultFrame->gotoLaunchPage();
            }
            return;
        }
        if (vaultFrame) vaultFrame->gotoNetworkStatusPage();
        if (!m_vault_controller) return;
        m_vault_controller->stopProofOfWork([this] {
            if (!m_vault_controller) return;
            m_vault_controller->closeAllVaultsForHandover([this] {
                Q_EMIT consensusActivationRequested();
            });
        });
    });
}

void QuicksilverGUI::confirmConsensusVaultHandover(bool proof_of_work_in_flight, std::function<void(bool)> done)
{
    auto* box = new QMessageBox(this);
    box->setObjectName(QStringLiteral("consensusVaultHandover"));
    box->setIcon(QMessageBox::Information);
    box->setWindowTitle(tr("Enable consensus"));
    box->setText(tr("Your vault will close and reopen."));

    QString detail = tr(
        "Joining consensus starts the node, and the node has to open your vault itself "
        "to connect it to the chain it downloads. So the vault closes for a moment and "
        "is reopened automatically once the node is running.\n\n"
        "Your quicksilver, your keys and your vault file are not touched. Nothing is "
        "sent and nothing is deleted. The vault is simply unavailable while the node "
        "starts, and your balance may take a moment to appear while it catches up with "
        "the chain.");

    if (proof_of_work_in_flight) {
        detail += QLatin1String("\n\n");
        detail += tr(
            "A transfer is still solving its proof-of-work. Continuing abandons it: the "
            "work done so far is lost and nothing is sent. You can start the transfer "
            "again once consensus is running.");
        box->setIcon(QMessageBox::Warning);
    }
    box->setInformativeText(detail);

    QPushButton* proceed = box->addButton(proof_of_work_in_flight
                                             ? tr("Abandon transfer and enable consensus")
                                             : tr("Close vault and enable consensus"),
                                         QMessageBox::AcceptRole);
    box->addButton(tr("Not now"), QMessageBox::RejectRole);
    box->setDefaultButton(proceed);
    GUIUtil::ShowModalMessageBoxAsynchronously(box, [proceed, done = std::move(done)](int, QAbstractButton* clicked) {
        if (done) done(clicked == proceed);
    });
}

void QuicksilverGUI::createTrayIcon()
{
    assert(QSystemTrayIcon::isSystemTrayAvailable());

#ifndef Q_OS_MACOS
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        trayIcon = new QSystemTrayIcon(m_network_style->getTrayAndWindowIcon(), this);
        QString toolTip = tr("%1 client").arg(CLIENT_NAME) + " " + m_network_style->getTitleAddText();
        trayIcon->setToolTip(toolTip);
    }
#endif
}

void QuicksilverGUI::createTrayIconMenu()
{
#ifndef Q_OS_MACOS
    if (!trayIcon) return;
#endif // Q_OS_MACOS

    // Configuration of the tray icon (or Dock icon) menu.
    QAction* show_hide_action{nullptr};
#ifndef Q_OS_MACOS
    // Note: On macOS, the Dock icon's menu already has Show / Hide action.
    show_hide_action = trayIconMenu->addAction(QString(), this, &QuicksilverGUI::toggleHidden);
    trayIconMenu->addSeparator();
#endif // Q_OS_MACOS

    QAction* send_action{nullptr};
    QAction* receive_action{nullptr};
    QAction* sign_action{nullptr};
    QAction* verify_action{nullptr};
    if (enableVault) {
        send_action = trayIconMenu->addAction(sendCoinsAction->text(), sendCoinsAction, &QAction::trigger);
        receive_action = trayIconMenu->addAction(receiveCoinsAction->text(), receiveCoinsAction, &QAction::trigger);
        trayIconMenu->addSeparator();
        sign_action = trayIconMenu->addAction(signMessageAction->text(), signMessageAction, &QAction::trigger);
        verify_action = trayIconMenu->addAction(verifyMessageAction->text(), verifyMessageAction, &QAction::trigger);
        trayIconMenu->addSeparator();
    }
    QAction* options_action = trayIconMenu->addAction(optionsAction->text(), optionsAction, &QAction::trigger);
    options_action->setMenuRole(QAction::PreferencesRole);
    QAction* node_window_action = trayIconMenu->addAction(openRPCConsoleAction->text(), openRPCConsoleAction, &QAction::trigger);
    QAction* quit_action{nullptr};
#ifndef Q_OS_MACOS
    // Note: On macOS, the Dock icon's menu already has Quit action.
    trayIconMenu->addSeparator();
    quit_action = trayIconMenu->addAction(quitAction->text(), quitAction, &QAction::trigger);

    trayIcon->setContextMenu(trayIconMenu.get());
    connect(trayIcon, &QSystemTrayIcon::activated, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger) {
            // Click on system tray icon triggers show/hide of the main window
            toggleHidden();
        }
    });
#else
    // Note: On macOS, the Dock icon is used to provide the tray's functionality.
    MacDockIconHandler* dockIconHandler = MacDockIconHandler::instance();
    connect(dockIconHandler, &MacDockIconHandler::dockIconClicked, [this] {
        if (m_node.shutdownRequested()) return; // nothing to show, node is shutting down.
        show();
        activateWindow();
    });
    trayIconMenu->setAsDockMenu();
#endif // Q_OS_MACOS

    connect(
        // Using QSystemTrayIcon::Context is not reliable.
        // See https://bugreports.qt.io/browse/QTBUG-91697
        trayIconMenu.get(), &QMenu::aboutToShow,
        [this, show_hide_action, send_action, receive_action, sign_action, verify_action, options_action, node_window_action, quit_action] {
            if (m_node.shutdownRequested()) return; // nothing to do, node is shutting down.

            if (show_hide_action) show_hide_action->setText(
                (!isHidden() && !isMinimized() && !GUIUtil::isObscured(this)) ?
                    tr("&Hide") :
                    tr("S&how"));
            if (QApplication::activeModalWidget()) {
                for (QAction* a : trayIconMenu.get()->actions()) {
                    a->setEnabled(false);
                }
            } else {
                if (show_hide_action) show_hide_action->setEnabled(true);
                if (enableVault) {
                    send_action->setEnabled(sendCoinsAction->isEnabled());
                    receive_action->setEnabled(receiveCoinsAction->isEnabled());
                    sign_action->setEnabled(signMessageAction->isEnabled());
                    verify_action->setEnabled(verifyMessageAction->isEnabled());
                }
                options_action->setEnabled(optionsAction->isEnabled());
                node_window_action->setEnabled(openRPCConsoleAction->isEnabled());
                if (quit_action) quit_action->setEnabled(true);
            }
        });
}

void QuicksilverGUI::optionsClicked()
{
    openOptionsDialogWithTab(OptionsDialog::TAB_MAIN);
}

void QuicksilverGUI::aboutClicked()
{
    if(!clientModel)
        return;

    auto dlg = new HelpMessageDialog(this, /*about=*/true);
    GUIUtil::ShowModalDialogAsynchronously(dlg);
}

void QuicksilverGUI::showDebugWindow()
{
    GUIUtil::bringToFront(rpcConsole);
    Q_EMIT consoleShown(rpcConsole);
}

void QuicksilverGUI::showDebugWindowActivateConsole()
{
    rpcConsole->setTabFocus(RPCConsole::TabTypes::CONSOLE);
    showDebugWindow();
}

void QuicksilverGUI::showHelpMessageClicked()
{
    GUIUtil::bringToFront(helpMessageDialog);
}

void QuicksilverGUI::confirmNetworkRestart(const QString& chain_token)
{
    auto* box = new QMessageBox(this);
    box->setObjectName(QStringLiteral("networkRestartConfirm"));
    box->setIcon(QMessageBox::Question);
    box->setWindowTitle(tr("Restart network context"));
    box->setText(tr("Restart Quicksilver with -chain=%1?").arg(chain_token));
    box->setInformativeText(tr("Network selection changes chain parameters, ports, and datadir, so the application must close and reopen. Your live vault is unaffected by developer network context."));
    QPushButton* restart_button = box->addButton(tr("Restart"), QMessageBox::AcceptRole);
    QPushButton* cancel_button = box->addButton(tr("Cancel"), QMessageBox::RejectRole);
    box->setDefaultButton(cancel_button);
    GUIUtil::ShowModalMessageBoxAsynchronously(box, [this, chain_token, restart_button](int, QAbstractButton* clicked) {
        if (clicked == static_cast<QAbstractButton*>(restart_button)) {
            Q_EMIT networkRestartRequested(chain_token);
        }
    });
}

#ifdef ENABLE_VAULT
void QuicksilverGUI::openClicked()
{
    auto dlg = new OpenURIDialog(platformStyle, this);
    connect(dlg, &QDialog::accepted, this, [this, dlg] {
        Q_EMIT receivedURI(dlg->getURI());
    });
    GUIUtil::ShowModalDialogAsynchronously(dlg);
}

void QuicksilverGUI::gotoOverviewPage()
{
    overviewAction->setChecked(true);
    if (vaultFrame) vaultFrame->gotoLaunchPage();
}

void QuicksilverGUI::gotoHistoryPage()
{
    historyAction->setChecked(true);
    if (vaultFrame) vaultFrame->gotoHistoryPage();
}

void QuicksilverGUI::gotoAgentAllotmentPage()
{
    agentAllotmentAction->setChecked(true);
    if (vaultFrame) vaultFrame->gotoAgentAllotmentPage();
}

void QuicksilverGUI::gotoMineMintPage()
{
    mineMintAction->setChecked(true);
    if (vaultFrame) vaultFrame->gotoMineMintPage();
}

void QuicksilverGUI::gotoNetworkPage()
{
    networkAction->setChecked(true);
    if (vaultFrame) vaultFrame->gotoNetworkPage();
}

void QuicksilverGUI::gotoReceiveCoinsPage()
{
    receiveCoinsAction->setChecked(true);
    if (vaultFrame) vaultFrame->gotoReceiveCoinsPage();
}

void QuicksilverGUI::gotoSendCoinsPage(QString addr)
{
    sendCoinsAction->setChecked(true);
    if (vaultFrame) vaultFrame->gotoSendCoinsPage(addr);
}

void QuicksilverGUI::gotoSignMessageTab(QString addr)
{
    if (vaultFrame) vaultFrame->gotoSignMessageTab(addr);
}

void QuicksilverGUI::gotoVerifyMessageTab(QString addr)
{
    if (vaultFrame) vaultFrame->gotoVerifyMessageTab(addr);
}
void QuicksilverGUI::gotoLoadPSQT(bool from_clipboard)
{
    if (vaultFrame) vaultFrame->gotoLoadPSQT(from_clipboard);
}
#endif // ENABLE_VAULT

void QuicksilverGUI::updateNetworkState()
{
    if (!clientModel) return;
    int count = clientModel->getNumConnections();
    QString icon;
    switch(count)
    {
    case 0: icon = ":/icons/connect_0"; break;
    case 1: case 2: case 3: icon = ":/icons/connect_1"; break;
    case 4: case 5: case 6: icon = ":/icons/connect_2"; break;
    case 7: case 8: case 9: icon = ":/icons/connect_3"; break;
    default: icon = ":/icons/connect_4"; break;
    }

    QString tooltip;

    if (m_node.getNetworkActive()) {
        // Singular and plural as separate strings rather than the "%n connection(s)"
        // idiom, for the reason set out in qt/maturity.cpp: no app catalogue is
        // installed, so %n is never resolved and the literal "(s)" ships.
        tooltip = count == 1
                      //: A substring of the tooltip.
                      ? tr("1 active connection to Quicksilver network.")
                      //: A substring of the tooltip.
                      : tr("%1 active connections to Quicksilver network.").arg(count);
    } else {
        //: A substring of the tooltip.
        tooltip = tr("Network activity disabled.");
        icon = ":/icons/network_disabled";
    }

    // Don't word-wrap this (fixed-width) tooltip
    tooltip = QLatin1String("<nobr>") + tooltip + QLatin1String("<br>") +
              //: A substring of the tooltip. "More actions" are available via the context menu.
              tr("Click for more actions.") + QLatin1String("</nobr>");
    connectionsControl->setToolTip(tooltip);

    connectionsControl->setThemedPixmap(icon, STATUSBAR_ICONSIZE, STATUSBAR_ICONSIZE);
}

void QuicksilverGUI::setNumConnections(int count)
{
    updateNetworkState();
}

void QuicksilverGUI::setNetworkActive(bool network_active)
{
    updateNetworkState();
    m_network_context_menu->clear();
    m_network_context_menu->addAction(
        //: A context menu item. The "Peers tab" is an element of the "Node window".
        tr("Show Peers tab"),
        [this] {
            rpcConsole->setTabFocus(RPCConsole::TabTypes::PEERS);
            showDebugWindow();
        });
    m_network_context_menu->addAction(
        network_active ?
            //: A context menu item.
            tr("Disable network activity") :
            //: A context menu item. The network activity was disabled previously.
            tr("Enable network activity"),
        [this, new_state = !network_active] { m_node.setNetworkActive(new_state); });
}

void QuicksilverGUI::updateHeadersSyncProgressLabel()
{
    int64_t headersTipTime = clientModel->getHeaderTipTime();
    int headersTipHeight = clientModel->getHeaderTipHeight();
    int estHeadersLeft = (GetTime() - headersTipTime) / Params().GetConsensus().nPowTargetSpacing;
    if (estHeadersLeft > HEADER_HEIGHT_DELTA_SYNC)
        progressBarLabel->setText(tr("Syncing Headers (%1%)…").arg(QString::number(100.0 / (headersTipHeight+estHeadersLeft)*headersTipHeight, 'f', 1)));
}

void QuicksilverGUI::updateHeadersPresyncProgressLabel(int64_t height, const QDateTime& blockDate)
{
    int estHeadersLeft = blockDate.secsTo(QDateTime::currentDateTime()) / Params().GetConsensus().nPowTargetSpacing;
    if (estHeadersLeft > HEADER_HEIGHT_DELTA_SYNC)
        progressBarLabel->setText(tr("Pre-syncing Headers (%1%)…").arg(QString::number(100.0 / (height+estHeadersLeft)*height, 'f', 1)));
}

void QuicksilverGUI::openOptionsDialogWithTab(OptionsDialog::Tab tab)
{
    if (!clientModel || !clientModel->getOptionsModel())
        return;

    auto dlg = new OptionsDialog(this, enableVault);
    connect(dlg, &OptionsDialog::quitOnReset, this, &QuicksilverGUI::quitRequested);
    dlg->setCurrentTab(tab);
    dlg->setClientModel(clientModel);
    dlg->setModel(clientModel->getOptionsModel());
    GUIUtil::ShowModalDialogAsynchronously(dlg);
}

void QuicksilverGUI::setNumBlocks(int count, const QDateTime& blockDate, double nVerificationProgress, SyncType synctype, SynchronizationState sync_state)
{
// Disabling macOS App Nap on initial sync, disk and reindex operations.
#ifdef Q_OS_MACOS
    if (sync_state == SynchronizationState::POST_INIT) {
        m_app_nap_inhibitor->enableAppNap();
    } else {
        m_app_nap_inhibitor->disableAppNap();
    }
#endif

    if (modalOverlay)
    {
        if (synctype != SyncType::BLOCK_SYNC)
            modalOverlay->setKnownBestHeight(count, blockDate, synctype == SyncType::HEADER_PRESYNC);
        else
            modalOverlay->tipUpdate(count, blockDate, nVerificationProgress);
    }
    if (!clientModel)
        return;

    // Prevent orphan statusbar messages (e.g. hover Quit in main menu, wait until chain-sync starts -> garbled text)
    statusBar()->clearMessage();

    // Acquire current block source
    BlockSource blockSource{clientModel->getBlockSource()};
    switch (blockSource) {
        case BlockSource::NETWORK:
            if (synctype == SyncType::HEADER_PRESYNC) {
                updateHeadersPresyncProgressLabel(count, blockDate);
                return;
            } else if (synctype == SyncType::HEADER_SYNC) {
                updateHeadersSyncProgressLabel();
                return;
            }
            progressBarLabel->setText(tr("Synchronizing with network…"));
            updateHeadersSyncProgressLabel();
            break;
        case BlockSource::DISK:
            if (synctype != SyncType::BLOCK_SYNC) {
                progressBarLabel->setText(tr("Indexing blocks on disk…"));
            } else {
                progressBarLabel->setText(tr("Processing blocks on disk…"));
            }
            break;
        case BlockSource::NONE:
            if (synctype != SyncType::BLOCK_SYNC) {
                return;
            }
            progressBarLabel->setText(tr("Connecting to peers…"));
            break;
    }

    QString tooltip;

    QDateTime currentDate = QDateTime::currentDateTime();
    qint64 secs = blockDate.secsTo(currentDate);

    tooltip = count == 1 ? tr("Processed 1 block of transaction history.")
                         : tr("Processed %1 blocks of transaction history.").arg(count);

    // Set icon state: spinning if catching up, tick otherwise.
    // "Catching up" is having known work left to validate, not an old tip. See
    // ModalOverlay::isBehindKnownHeaders.
    const bool catching_up{modalOverlay && modalOverlay->isBehindKnownHeaders(count)};
    if (!catching_up) {
        tooltip = tr("Up to date") + QString(".<br>") + tooltip;
        labelBlocksIcon->setThemedPixmap(QStringLiteral(":/icons/synced"), STATUSBAR_ICONSIZE, STATUSBAR_ICONSIZE);

#ifdef ENABLE_VAULT
        if(vaultFrame)
        {
            vaultFrame->showOutOfSyncWarning(false);
            modalOverlay->showHide(true, true);
        }
#endif // ENABLE_VAULT

        progressBarLabel->setVisible(false);
        progressBar->setVisible(false);
    }
    else
    {
        QString timeBehindText = GUIUtil::formatNiceTimeOffset(secs);

        progressBarLabel->setVisible(true);
        progressBar->setFormat(tr("%1 behind").arg(timeBehindText));
        progressBar->setMaximum(1000000000);
        progressBar->setValue(nVerificationProgress * 1000000000.0 + 0.5);
        progressBar->setVisible(true);

        tooltip = tr("Catching up…") + QString("<br>") + tooltip;
        if(count != prevBlocks)
        {
            labelBlocksIcon->setThemedPixmap(
                QString(":/animation/spinner-%1").arg(spinnerFrame, 3, 10, QChar('0')),
                STATUSBAR_ICONSIZE, STATUSBAR_ICONSIZE);
            spinnerFrame = (spinnerFrame + 1) % SPINNER_FRAMES;
        }
        prevBlocks = count;

#ifdef ENABLE_VAULT
        if(vaultFrame)
        {
            vaultFrame->showOutOfSyncWarning(true);
            modalOverlay->showHide();
        }
#endif // ENABLE_VAULT

        tooltip += QString("<br>");
        tooltip += tr("Last received block was generated %1 ago.").arg(timeBehindText);
        tooltip += QString("<br>");
        tooltip += tr("Transactions after this will not yet be visible.");
    }

    // Don't word-wrap this (fixed-width) tooltip
    tooltip = QString("<nobr>") + tooltip + QString("</nobr>");

    labelBlocksIcon->setToolTip(tooltip);
    progressBarLabel->setToolTip(tooltip);
    progressBar->setToolTip(tooltip);
}

void QuicksilverGUI::createVault()
{
#ifdef ENABLE_VAULT
#ifndef USE_SQLITE
    // Compiled without sqlite support (required for vaults)
    message(tr("Error creating vault"), tr("Cannot create new vault, the software was compiled without sqlite support"), CClientUIInterface::MSG_ERROR);
    return;
#endif // USE_SQLITE
    if (!getVaultController()) {
        message(tr("Create vault"), tr("Vault access is not ready yet. Try again after startup finishes."), CClientUIInterface::MSG_INFORMATION);
        return;
    }
    auto activity = new CreateVaultActivity(getVaultController(), this);
    connect(activity, &CreateVaultActivity::created, this, &QuicksilverGUI::setCurrentVault);
    connect(activity, &CreateVaultActivity::created, rpcConsole, &RPCConsole::setCurrentVault);
    activity->create();
#endif // ENABLE_VAULT
}

void QuicksilverGUI::openVault()
{
#ifdef ENABLE_VAULT
    if (!m_open_vault_action || !m_open_vault_action->isEnabled() || !m_open_vault_menu) {
        message(tr("Open vault"), tr("Vault loading is not ready yet. Try again after startup finishes."), CClientUIInterface::MSG_INFORMATION);
        return;
    }

    QWidget* anchor = qobject_cast<QWidget*>(sender());
    const QPoint popup_position = anchor ? anchor->mapToGlobal(QPoint(0, anchor->height())) : mapToGlobal(rect().center());
    m_open_vault_menu->popup(popup_position);
#endif // ENABLE_VAULT
}

void QuicksilverGUI::message(const QString& title, QString message, unsigned int style, bool* ret, const QString& detailed_message)
{
    // Default title. On macOS, the window title is ignored (as required by the macOS Guidelines).
    QString strTitle{CLIENT_NAME};
    // Default to information icon
    int nMBoxIcon = QMessageBox::Information;
    int nNotifyIcon = Notificator::Information;

    QString msgType;
    if (!title.isEmpty()) {
        msgType = title;
    } else {
        switch (style) {
        case CClientUIInterface::MSG_ERROR:
            msgType = tr("Error");
            message = tr("Error: %1").arg(message);
            break;
        case CClientUIInterface::MSG_WARNING:
            msgType = tr("Warning");
            message = tr("Warning: %1").arg(message);
            break;
        case CClientUIInterface::MSG_INFORMATION:
            msgType = tr("Information");
            // No need to prepend the prefix here.
            break;
        default:
            break;
        }
    }

    if (!msgType.isEmpty()) {
        strTitle += " - " + msgType;
    }

    if (style & CClientUIInterface::ICON_ERROR) {
        nMBoxIcon = QMessageBox::Critical;
        nNotifyIcon = Notificator::Critical;
    } else if (style & CClientUIInterface::ICON_WARNING) {
        nMBoxIcon = QMessageBox::Warning;
        nNotifyIcon = Notificator::Warning;
    }

    if (style & CClientUIInterface::MODAL) {
        // Check for buttons, use OK as default, if none was supplied
        QMessageBox::StandardButton buttons;
        if (!(buttons = (QMessageBox::StandardButton)(style & CClientUIInterface::BTN_MASK)))
            buttons = QMessageBox::Ok;

        showNormalIfMinimized();
        auto* box = new QMessageBox(static_cast<QMessageBox::Icon>(nMBoxIcon), strTitle, message, buttons, this);
        box->setObjectName(QStringLiteral("clientMessageBox"));
        box->setTextFormat(Qt::PlainText);
        box->setDetailedText(detailed_message);
        GUIUtil::ShowModalMessageBoxAsynchronously(box, [ret](int result, QAbstractButton*) {
            if (ret) *ret = result == QMessageBox::Ok;
        });
    } else {
        notificator->notify(static_cast<Notificator::Class>(nNotifyIcon), strTitle, message);
    }
}

void QuicksilverGUI::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::PaletteChange) {
        overviewAction->setIcon(platformStyle->ColorIcon(QStringLiteral(":/icons/overview"), QuicksilverStyle::Color(QuicksilverStyle::Token::CinnabarBright)));
        sendCoinsAction->setIcon(platformStyle->ColorIcon(QStringLiteral(":/icons/send"), QuicksilverStyle::Color(QuicksilverStyle::Token::Amber)));
        receiveCoinsAction->setIcon(platformStyle->ColorIcon(QStringLiteral(":/icons/receiving_addresses"), QuicksilverStyle::Color(QuicksilverStyle::Token::Teal)));
        agentAllotmentAction->setIcon(platformStyle->ColorIcon(QStringLiteral(":/icons/agent"), QuicksilverStyle::Color(QuicksilverStyle::Token::Violet)));
        historyAction->setIcon(platformStyle->ColorIcon(QStringLiteral(":/icons/history"), QuicksilverStyle::Color(QuicksilverStyle::Token::SilverMuted)));
        mineMintAction->setIcon(platformStyle->ColorIcon(QStringLiteral(":/icons/tx_mined"), QuicksilverStyle::Color(QuicksilverStyle::Token::Amber)));
        networkAction->setIcon(platformStyle->ColorIcon(QStringLiteral(":/icons/connect_4"), QuicksilverStyle::Color(QuicksilverStyle::Token::Teal)));
    }

    QMainWindow::changeEvent(e);

#ifndef Q_OS_MACOS // Ignored on Mac
    if(e->type() == QEvent::WindowStateChange)
    {
        if(clientModel && clientModel->getOptionsModel() && clientModel->getOptionsModel()->getMinimizeToTray())
        {
            QWindowStateChangeEvent *wsevt = static_cast<QWindowStateChangeEvent*>(e);
            if(!(wsevt->oldState() & Qt::WindowMinimized) && isMinimized())
            {
                QTimer::singleShot(0, this, &QuicksilverGUI::hide);
                e->ignore();
            }
            else if((wsevt->oldState() & Qt::WindowMinimized) && !isMinimized())
            {
                QTimer::singleShot(0, this, &QuicksilverGUI::show);
                e->ignore();
            }
        }
    }
#endif
}

void QuicksilverGUI::closeEvent(QCloseEvent *event)
{
#ifndef Q_OS_MACOS // Ignored on Mac
    if(!clientModel)
    {
        rpcConsole->close();
        Q_EMIT quitRequested();
    }
    else if(clientModel->getOptionsModel())
    {
        if(!clientModel->getOptionsModel()->getMinimizeOnClose())
        {
            // close rpcConsole in case it was open to make some space for the shutdown window
            rpcConsole->close();

            Q_EMIT quitRequested();
        }
        else
        {
            QMainWindow::showMinimized();
            event->ignore();
        }
    }
#else
    QMainWindow::closeEvent(event);
#endif
}

void QuicksilverGUI::showEvent(QShowEvent *event)
{
    // enable the debug window when the main window shows up
    openRPCConsoleAction->setEnabled(true);
    aboutAction->setEnabled(true);
    optionsAction->setEnabled(true);
}

#ifdef ENABLE_VAULT
void QuicksilverGUI::incomingTransaction(const QString& date, QuicksilverUnit unit, const CAmount& amount, const QString& type, const QString& address, const QString& label, const QString& vaultName)
{
    // On new transaction, make an info balloon
    QString msg = tr("Date: %1\n").arg(date) +
                  tr("Amount: %1\n").arg(QuicksilverUnits::formatWithUnit(unit, amount, true));
    if (m_node.vaultLoader().getVaults().size() > 1 && !vaultName.isEmpty()) {
        msg += tr("Vault: %1\n").arg(vaultName);
    }
    msg += tr("Type: %1\n").arg(type);
    if (!label.isEmpty())
        msg += tr("Label: %1\n").arg(label);
    else if (!address.isEmpty())
        msg += tr("Address: %1\n").arg(address);
    message((amount)<0 ? tr("Sent transaction") : tr("Incoming transaction"),
             msg, CClientUIInterface::MSG_INFORMATION);
}
#endif // ENABLE_VAULT

void QuicksilverGUI::dragEnterEvent(QDragEnterEvent *event)
{
    // Accept only URIs
    if(event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void QuicksilverGUI::dropEvent(QDropEvent *event)
{
    if(event->mimeData()->hasUrls())
    {
        for (const QUrl &uri : event->mimeData()->urls())
        {
            Q_EMIT receivedURI(uri.toString());
        }
    }
    event->acceptProposedAction();
}

bool QuicksilverGUI::eventFilter(QObject *object, QEvent *event)
{
    // Catch status tip events
    if (event->type() == QEvent::StatusTip)
    {
        // Prevent adding text from setStatusTip(), if we currently use the status bar for displaying other stuff
        if (progressBarLabel->isVisible() || progressBar->isVisible())
            return true;
    }
    return QMainWindow::eventFilter(object, event);
}

#ifdef ENABLE_VAULT
bool QuicksilverGUI::handlePaymentRequest(const SendCoinsRecipient& recipient)
{
    // URI has to be valid
    if (vaultFrame && vaultFrame->handlePaymentRequest(recipient))
    {
        showNormalIfMinimized();
        gotoSendCoinsPage();
        return true;
    }
    return false;
}

void QuicksilverGUI::setHDStatus(bool privkeyDisabled, int hdEnabled)
{
    labelVaultHDStatusIcon->setThemedPixmap(privkeyDisabled ? QStringLiteral(":/icons/eye") : hdEnabled ? QStringLiteral(":/icons/hd_enabled") : QStringLiteral(":/icons/hd_disabled"), STATUSBAR_ICONSIZE, STATUSBAR_ICONSIZE);
    labelVaultHDStatusIcon->setToolTip(privkeyDisabled ? tr("Private key <b>disabled</b>") : hdEnabled ? tr("HD key generation is <b>enabled</b>") : tr("HD key generation is <b>disabled</b>"));
    labelVaultHDStatusIcon->show();
}

void QuicksilverGUI::setEncryptionStatus(int status)
{
    switch(status)
    {
    case VaultModel::NoKeys:
        labelVaultEncryptionIcon->hide();
        encryptVaultAction->setChecked(false);
        changePassphraseAction->setEnabled(false);
        encryptVaultAction->setEnabled(false);
        break;
    case VaultModel::Unencrypted:
        labelVaultEncryptionIcon->hide();
        encryptVaultAction->setChecked(false);
        changePassphraseAction->setEnabled(false);
        encryptVaultAction->setEnabled(true);
        break;
    case VaultModel::Unlocked:
        labelVaultEncryptionIcon->show();
        labelVaultEncryptionIcon->setThemedPixmap(QStringLiteral(":/icons/lock_open"), STATUSBAR_ICONSIZE, STATUSBAR_ICONSIZE);
        labelVaultEncryptionIcon->setToolTip(tr("Vault is <b>encrypted</b> and currently <b>unlocked</b>"));
        encryptVaultAction->setChecked(true);
        changePassphraseAction->setEnabled(true);
        encryptVaultAction->setEnabled(false);
        break;
    case VaultModel::Locked:
        labelVaultEncryptionIcon->show();
        labelVaultEncryptionIcon->setThemedPixmap(QStringLiteral(":/icons/lock_closed"), STATUSBAR_ICONSIZE, STATUSBAR_ICONSIZE);
        labelVaultEncryptionIcon->setToolTip(tr("Vault is <b>encrypted</b> and currently <b>locked</b>"));
        encryptVaultAction->setChecked(true);
        changePassphraseAction->setEnabled(true);
        encryptVaultAction->setEnabled(false);
        break;
    }
}

void QuicksilverGUI::updateVaultStatus()
{
    assert(vaultFrame);

    VaultView * const vaultView = vaultFrame->currentVaultView();
    if (!vaultView) {
        return;
    }
    VaultModel * const vaultModel = vaultView->getVaultModel();
    setEncryptionStatus(vaultModel->getEncryptionStatus());
    setHDStatus(vaultModel->vault().privateKeysDisabled(), vaultModel->vault().hdEnabled());
}
#endif // ENABLE_VAULT

void QuicksilverGUI::updateProxyIcon()
{
    std::string ip_port;
    bool proxy_enabled = clientModel->getProxyInfo(ip_port);

    if (proxy_enabled) {
        if (!GUIUtil::HasPixmap(labelProxyIcon)) {
            QString ip_port_q = QString::fromStdString(ip_port);
            labelProxyIcon->setThemedPixmap((":/icons/proxy"), STATUSBAR_ICONSIZE, STATUSBAR_ICONSIZE);
            labelProxyIcon->setToolTip(tr("Proxy is <b>enabled</b>: %1").arg(ip_port_q));
        } else {
            labelProxyIcon->show();
        }
    } else {
        labelProxyIcon->hide();
    }
}

void QuicksilverGUI::updateWindowTitle()
{
    QString window_title = CLIENT_NAME;
#ifdef ENABLE_VAULT
    if (vaultFrame) {
        VaultModel* const vault_model = vaultFrame->currentVaultModel();
        if (vault_model && !vault_model->getVaultName().isEmpty()) {
            window_title += " - " + vault_model->getDisplayName();
        }
    }
#endif
    if (!m_network_style->getTitleAddText().isEmpty()) {
        window_title += " - " + m_network_style->getTitleAddText();
    }
    setWindowTitle(window_title);
}

void QuicksilverGUI::showNormalIfMinimized(bool fToggleHidden)
{
    if(!clientModel)
        return;

    if (!isHidden() && !isMinimized() && !GUIUtil::isObscured(this) && fToggleHidden) {
        hide();
    } else {
        GUIUtil::bringToFront(this);
    }
}

void QuicksilverGUI::toggleHidden()
{
    showNormalIfMinimized(true);
}

void QuicksilverGUI::detectShutdown()
{
    if (m_node.shutdownRequested())
    {
        if(rpcConsole)
            rpcConsole->hide();
        Q_EMIT quitRequested();
    }
}

void QuicksilverGUI::showProgress(const QString &title, int nProgress)
{
    if (nProgress == 0) {
        progressDialog = new QProgressDialog(title, QString(), 0, 100);
        GUIUtil::PolishProgressDialog(progressDialog);
        progressDialog->setWindowModality(Qt::ApplicationModal);
        progressDialog->setAutoClose(false);
        progressDialog->setValue(0);
    } else if (nProgress == 100) {
        if (progressDialog) {
            progressDialog->close();
            progressDialog->deleteLater();
            progressDialog = nullptr;
        }
    } else if (progressDialog) {
        progressDialog->setValue(nProgress);
    }
}

void QuicksilverGUI::showModalOverlay()
{
    if (modalOverlay && (progressBar->isVisible() || modalOverlay->isLayerVisible()))
        modalOverlay->toggleVisibility();
}

static bool ThreadSafeMessageBox(QuicksilverGUI* gui, const bilingual_str& message, const std::string& caption, unsigned int style)
{
    bool modal = (style & CClientUIInterface::MODAL);
    // The SECURE flag has no effect in the Qt GUI.
    style &= ~CClientUIInterface::SECURE;

    QString detailed_message; // This is original message, in English, for googling and referencing.
    if (message.original != message.translated) {
        detailed_message = QuicksilverGUI::tr("Original message:") + "\n" + QString::fromStdString(message.original);
    }

    const QString qcaption = QString::fromStdString(caption);
    const QString qmessage = QString::fromStdString(message.translated);

    if (!modal) {
        bool invoked = QMetaObject::invokeMethod(gui, "message",
                               Qt::QueuedConnection,
                               Q_ARG(QString, qcaption),
                               Q_ARG(QString, qmessage),
                               Q_ARG(unsigned int, style),
                               Q_ARG(bool*, nullptr),
                               Q_ARG(QString, detailed_message));
        assert(invoked);
        return false;
    }

    // Node callers still need the bool on this stack. Wait off the GUI thread
    // when possible so the GUI is not in QDialog::exec(). A GUI-thread caller
    // (startup InitError) still nested-waits with QEventLoop.
    auto result = std::make_shared<bool>(false);
    auto dismissed = std::make_shared<QSemaphore>(0);
    QPointer<QuicksilverGUI> gui_ptr(gui);

    auto present = [gui_ptr, qcaption, qmessage, style, detailed_message, result, dismissed]() {
        if (!gui_ptr) {
            dismissed->release();
            return;
        }
        gui_ptr->message(qcaption, qmessage, style, nullptr, detailed_message);
        const QList<QMessageBox*> boxes = gui_ptr->findChildren<QMessageBox*>(QStringLiteral("clientMessageBox"));
        QMessageBox* box = boxes.isEmpty() ? nullptr : boxes.constLast();
        if (!box) {
            dismissed->release();
            return;
        }
        QObject::connect(box, &QMessageBox::finished, [result, dismissed](int r) {
            *result = (r == QMessageBox::Ok);
            dismissed->release();
        });
    };

    if (QThread::currentThread() == qApp->thread()) {
        present();
        if (!dismissed->tryAcquire()) {
            QEventLoop loop;
            const QList<QMessageBox*> boxes = gui->findChildren<QMessageBox*>(QStringLiteral("clientMessageBox"));
            if (QMessageBox* box = boxes.isEmpty() ? nullptr : boxes.constLast()) {
                QObject::connect(box, &QMessageBox::finished, &loop, &QEventLoop::quit);
                loop.exec();
            }
        }
        return *result;
    }

    bool invoked = QMetaObject::invokeMethod(gui, present, Qt::QueuedConnection);
    assert(invoked);
    dismissed->acquire();
    return *result;
}

void QuicksilverGUI::subscribeToCoreSignals()
{
    // Connect signals to client
    m_handler_message_box = m_node.handleMessageBox(std::bind(ThreadSafeMessageBox, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
    m_handler_question = m_node.handleQuestion(std::bind(ThreadSafeMessageBox, this, std::placeholders::_1, std::placeholders::_3, std::placeholders::_4));
}

void QuicksilverGUI::unsubscribeFromCoreSignals()
{
    // Disconnect signals from client
    if (m_handler_message_box) {
        m_handler_message_box->disconnect();
        m_handler_message_box.reset();
    }
    if (m_handler_question) {
        m_handler_question->disconnect();
        m_handler_question.reset();
    }
}

bool QuicksilverGUI::isPrivacyModeActivated() const
{
    assert(m_mask_values_action);
    return m_mask_values_action->isChecked();
}

UnitDisplayStatusBarControl::UnitDisplayStatusBarControl(const PlatformStyle* platformStyle)
    : m_platform_style{platformStyle}
{
    createContextMenu();
    setToolTip(tr("Unit to show amounts in. Click to select another unit."));
    QList<QuicksilverUnit> units = QuicksilverUnits::availableUnits();
    int max_width = 0;
    const QFontMetrics fm(font());
    for (const QuicksilverUnit unit : units) {
        max_width = qMax(max_width, GUIUtil::TextWidth(fm, QuicksilverUnits::longName(unit)));
    }
    setMinimumSize(max_width, 0);
    setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    setStyleSheet(QString("QLabel { color : %1 }").arg(m_platform_style->SingleColor().name()));
}

/** So that it responds to button clicks */
void UnitDisplayStatusBarControl::mousePressEvent(QMouseEvent *event)
{
    onDisplayUnitsClicked(event->pos());
}

void UnitDisplayStatusBarControl::changeEvent(QEvent* e)
{
    if (e->type() == QEvent::PaletteChange) {
        QString style = QString("QLabel { color : %1 }").arg(m_platform_style->SingleColor().name());
        if (style != styleSheet()) {
            setStyleSheet(style);
        }
    }

    QLabel::changeEvent(e);
}

/** Creates context menu, its actions, and wires up all the relevant signals for mouse events. */
void UnitDisplayStatusBarControl::createContextMenu()
{
    menu = new QMenu(this);
    for (const QuicksilverUnit u : QuicksilverUnits::availableUnits()) {
        menu->addAction(QuicksilverUnits::longName(u))->setData(QVariant::fromValue(u));
    }
    connect(menu, &QMenu::triggered, this, &UnitDisplayStatusBarControl::onMenuSelection);
}

/** Lets the control know about the Options Model (and its signals) */
void UnitDisplayStatusBarControl::setOptionsModel(OptionsModel *_optionsModel)
{
    if (_optionsModel)
    {
        this->optionsModel = _optionsModel;

        // be aware of a display unit change reported by the OptionsModel object.
        connect(_optionsModel, &OptionsModel::displayUnitChanged, this, &UnitDisplayStatusBarControl::updateDisplayUnit);

        // initialize the display units label with the current value in the model.
        updateDisplayUnit(_optionsModel->getDisplayUnit());
    }
}

/** When Display Units are changed on OptionsModel it will refresh the display text of the control on the status bar */
void UnitDisplayStatusBarControl::updateDisplayUnit(QuicksilverUnit newUnits)
{
    setText(QuicksilverUnits::longName(newUnits));
}

/** Shows context menu with Display Unit options by the mouse coordinates */
void UnitDisplayStatusBarControl::onDisplayUnitsClicked(const QPoint& point)
{
    QPoint globalPos = mapToGlobal(point);
    menu->exec(globalPos);
}

/** Tells underlying optionsModel to update its current display unit. */
void UnitDisplayStatusBarControl::onMenuSelection(QAction* action)
{
    if (action)
    {
        optionsModel->setDisplayUnit(action->data());
    }
}
