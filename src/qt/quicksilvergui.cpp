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
#include <qt/minemintpage.h>
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
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QVBoxLayout>
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
#include <QLayout>
#include <QIcon>
#include <QToolButton>
#include <QStylePainter>
#include <QStyleOptionToolButton>
#include <QStatusTipEvent>
#include <QSystemTrayIcon>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QUrlQuery>
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
        connect(vaultFrame, &VaultFrame::currentVaultSet, [this] {
            updateVaultStatus();
            refreshBenchChrome();
        });
        m_consensus_enabled = vaultFrame->consensusEnabled();
        connect(vaultFrame, &VaultFrame::consensusStateChanged, this, &QuicksilverGUI::setConsensusEnabled);
        connect(vaultFrame, &VaultFrame::networkRestartRequested, this, &QuicksilverGUI::confirmNetworkRestart);
        // Through the rail action, so the rail marks the Ledger page.
        connect(vaultFrame, &VaultFrame::ledgerRequested, this, &QuicksilverGUI::gotoHistoryPage);
        content_scroll->setWidget(vaultFrame);
        createBenchChrome(content_scroll);
        setCentralWidget(m_bench_top_bar->parentWidget());
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
    connectBenchCommands();

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

    // Status strip. The same controls as the old status bar, in the bench order.
    m_bench_status_strip = new QFrame();
    m_bench_status_strip->setObjectName(QStringLiteral("benchStatusStrip"));
    auto* strip_layout = new QHBoxLayout(m_bench_status_strip);
    strip_layout->setContentsMargins(14, 0, 14, 0);
    strip_layout->setSpacing(14);

    auto add_status = [this, strip_layout](const QString& name, const QString& text) {
        auto* label = new QLabel(text, m_bench_status_strip);
        label->setObjectName(name);
        label->setProperty("class", QStringLiteral("benchStatusText"));
        strip_layout->addWidget(label);
        return label;
    };
    m_status_dot = new QLabel(QStringLiteral("\u25CF"), m_bench_status_strip);
    m_status_dot->setObjectName(QStringLiteral("benchStatusDot"));
    strip_layout->addWidget(m_status_dot);
    strip_layout->addSpacing(-8);
    m_status_sync = add_status(QStringLiteral("benchStatusSync"), tr("Connecting"));
    m_status_height = add_status(QStringLiteral("benchStatusHeight"), tr("Height unavailable"));
    m_status_peers = add_status(QStringLiteral("benchStatusPeers"), tr("No peers"));
    m_status_mining = add_status(QStringLiteral("benchStatusMining"), tr("Mining locked"));
    m_status_vault = add_status(QStringLiteral("benchStatusVault"), tr("No vault open"));

    unitDisplayControl = new UnitDisplayStatusBarControl(platformStyle);
    unitDisplayControl->setParent(m_bench_status_strip);
    labelVaultEncryptionIcon = new GUIUtil::ThemedLabel(platformStyle);
    labelVaultHDStatusIcon = new GUIUtil::ThemedLabel(platformStyle);
    labelProxyIcon = new GUIUtil::ClickableLabel(platformStyle);
    connectionsControl = new GUIUtil::ClickableLabel(platformStyle);
    labelBlocksIcon = new GUIUtil::ClickableLabel(platformStyle);
    labelVaultEncryptionIcon->hide();
    labelVaultHDStatusIcon->hide();

    // The app style is Fusion, and the progress chunk comes from the token sheet.
    // A platform stylesheet here used to paint an inherited orange gradient.
    progressBarLabel = new QLabel();
    progressBarLabel->setVisible(false);
    progressBar = new GUIUtil::ProgressBar();
    progressBar->setAlignment(Qt::AlignCenter);
    progressBar->setVisible(false);

    strip_layout->addWidget(labelBlocksIcon);
    strip_layout->addWidget(progressBarLabel);
    strip_layout->addWidget(progressBar);
    strip_layout->addWidget(connectionsControl);
    strip_layout->addWidget(labelProxyIcon);
    if (enableVault) {
        strip_layout->addWidget(labelVaultEncryptionIcon);
        strip_layout->addWidget(labelVaultHDStatusIcon);
    }
    strip_layout->addStretch();
    // Hover tips land here. A QStatusBar message would hide the whole strip.
    m_status_tip = new QLabel(m_bench_status_strip);
    m_status_tip->setObjectName(QStringLiteral("benchStatusTip"));
    m_status_tip->setProperty("class", QStringLiteral("benchStatusText"));
    strip_layout->addWidget(m_status_tip);
    if (enableVault) strip_layout->addWidget(unitDisplayControl);

    statusBar()->addWidget(m_bench_status_strip, 1);
    refreshStatusStrip();

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

namespace {
//! A rail destination: a full-width row with the icon and label at the left.
//!
//! QToolButton centres its contents and the sheet's text-align does not reach
//! it, so the row paints its own icon and label over the sheet's panel. The
//! panel (background and the checked row's left edge) still comes from the sheet.
class RailButton : public QToolButton
{
public:
    explicit RailButton(QWidget* parent) : QToolButton(parent)
    {
        setFocusPolicy(Qt::NoFocus);
        setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setAttribute(Qt::WA_Hover);
    }

    QSize sizeHint() const override { return {RAIL_WIDTH - 1, ROW_HEIGHT}; }
    QSize minimumSizeHint() const override { return sizeHint(); }

    static constexpr int RAIL_WIDTH{196};
    static constexpr int ROW_HEIGHT{38};

protected:
    void paintEvent(QPaintEvent*) override
    {
        QStylePainter painter(this);
        QStyleOptionToolButton option;
        initStyleOption(&option);
        const QIcon icon = option.icon;
        const QString text = option.text;
        option.icon = QIcon();
        option.text.clear();
        painter.drawComplexControl(QStyle::CC_ToolButton, option);

        constexpr int left{16};
        constexpr int icon_size{18};
        constexpr int gap{10};
        const QIcon::Mode mode = isEnabled() ? QIcon::Normal : QIcon::Disabled;
        const QIcon::State state = isChecked() ? QIcon::On : QIcon::Off;
        icon.paint(&painter, QRect(left, (height() - icon_size) / 2, icon_size, icon_size), Qt::AlignCenter, mode, state);

        using QuicksilverStyle::Token;
        const Token tone = !isEnabled() ? Token::RailDisabled
                         : (isChecked() || underMouse()) ? Token::SilverHi
                                                         : Token::RailText;
        painter.setPen(QuicksilverStyle::Color(tone));
        painter.drawText(rect().adjusted(left + icon_size + gap, 0, -8, 0), Qt::AlignLeft | Qt::AlignVCenter, text);
    }
};

//! The rail's rows run edge to edge. QToolBar's layout takes a margin from the
//! style's toolbar metrics and re-reads it on every style change.
class RailToolBar : public QToolBar
{
public:
    RailToolBar(const QString& title, QWidget* parent) : QToolBar(title, parent) { flatten(); }

protected:
    void changeEvent(QEvent* event) override
    {
        QToolBar::changeEvent(event);
        if (event->type() == QEvent::StyleChange) flatten();
    }

private:
    void flatten()
    {
        if (layout()) layout()->setContentsMargins(0, 0, 0, 0);
    }
};

//! Rail icons are muted; the checked destination's icon is cinnabar.
QIcon RailIcon(const PlatformStyle* style, const QString& file)
{
    using QuicksilverStyle::Color;
    using QuicksilverStyle::Token;
    const QSize size{18, 18};
    QIcon icon;
    icon.addPixmap(style->ColorIcon(file, Color(Token::RailText)).pixmap(size), QIcon::Normal, QIcon::Off);
    icon.addPixmap(style->ColorIcon(file, Color(Token::Cinnabar)).pixmap(size), QIcon::Normal, QIcon::On);
    icon.addPixmap(style->ColorIcon(file, Color(Token::RailDisabled)).pixmap(size), QIcon::Disabled, QIcon::Off);
    icon.addPixmap(style->ColorIcon(file, Color(Token::RailDisabled)).pixmap(size), QIcon::Disabled, QIcon::On);
    return icon;
}
} // namespace

void QuicksilverGUI::createActions()
{
    QActionGroup *tabGroup = new QActionGroup(this);
    connect(modalOverlay, &ModalOverlay::triggered, tabGroup, &QActionGroup::setEnabled);

    overviewAction = new QAction(RailIcon(platformStyle, QStringLiteral(":/icons/overview")), tr("&Home"), this);
    overviewAction->setObjectName(QStringLiteral("homeBootstrapAction"));
    overviewAction->setStatusTip(tr("Show the Quicksilver launch screen"));
    overviewAction->setToolTip(overviewAction->statusTip());
    overviewAction->setCheckable(true);
    overviewAction->setShortcut(QKeySequence(QStringLiteral("Alt+1")));
    tabGroup->addAction(overviewAction);

    sendCoinsAction = new QAction(RailIcon(platformStyle, QStringLiteral(":/icons/send")), tr("&Transfer"), this);
    sendCoinsAction->setObjectName(QStringLiteral("sendCoinsAction"));
    sendCoinsAction->setStatusTip(tr("Transfer Quicksilver to an address"));
    sendCoinsAction->setToolTip(sendCoinsAction->statusTip());
    sendCoinsAction->setCheckable(true);
    sendCoinsAction->setShortcut(QKeySequence(QStringLiteral("Alt+2")));
    tabGroup->addAction(sendCoinsAction);

    receiveCoinsAction = new QAction(RailIcon(platformStyle, QStringLiteral(":/icons/receiving_addresses")), tr("&Request"), this);
    receiveCoinsAction->setObjectName(QStringLiteral("receiveCoinsAction"));
    receiveCoinsAction->setStatusTip(tr("Create receiving addresses, QR codes, and quicksilver: URIs"));
    receiveCoinsAction->setToolTip(receiveCoinsAction->statusTip());
    receiveCoinsAction->setCheckable(true);
    receiveCoinsAction->setShortcut(QKeySequence(QStringLiteral("Alt+3")));
    tabGroup->addAction(receiveCoinsAction);

    historyAction = new QAction(RailIcon(platformStyle, QStringLiteral(":/icons/history")), tr("&Ledger"), this);
    historyAction->setObjectName(QStringLiteral("historyAction"));
    historyAction->setStatusTip(tr("Browse ledger activity"));
    historyAction->setToolTip(historyAction->statusTip());
    historyAction->setCheckable(true);
    historyAction->setShortcut(QKeySequence(QStringLiteral("Alt+4")));
    tabGroup->addAction(historyAction);

    agentAllotmentAction = new QAction(RailIcon(platformStyle, QStringLiteral(":/icons/agent")), tr("&Agents"), this);
    agentAllotmentAction->setObjectName(QStringLiteral("agentAllotmentAction"));
    agentAllotmentAction->setStatusTip(tr("Fund and review co-signed agent allotments"));
    agentAllotmentAction->setToolTip(agentAllotmentAction->statusTip());
    agentAllotmentAction->setCheckable(true);
    agentAllotmentAction->setShortcut(QKeySequence(QStringLiteral("Alt+5")));
    tabGroup->addAction(agentAllotmentAction);

    mineMintAction = new QAction(RailIcon(platformStyle, QStringLiteral(":/icons/tx_mined")), tr("&Mine / Mint"), this);
    mineMintAction->setObjectName(QStringLiteral("mineMintAction"));
    mineMintAction->setStatusTip(tr("Show mining and minting setup status"));
    mineMintAction->setToolTip(mineMintAction->statusTip());
    mineMintAction->setCheckable(true);
    mineMintAction->setShortcut(QKeySequence(QStringLiteral("Alt+6")));
    tabGroup->addAction(mineMintAction);

    networkAction = new QAction(RailIcon(platformStyle, QStringLiteral(":/icons/connect_4")), tr("&Network"), this);
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
    connect(overviewAction, &QAction::triggered, this, &QuicksilverGUI::gotoHomePage);
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
    m_mask_values_action->setStatusTip(tr("Mask the amounts shown in this window"));
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
        connect(m_mask_values_action, &QAction::toggled, this, [this](bool) { refreshTicker(); });
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

    QMenu *settings = appMenuBar->addMenu(tr("&Settings"));
    settings->setObjectName(QStringLiteral("settingsMenu"));
    if(vaultFrame)
    {
        settings->addAction(encryptVaultAction);
        settings->addAction(changePassphraseAction);
        settings->addSeparator();
        settings->addAction(m_mask_values_action);
        settings->addSeparator();
    }
    settings->addAction(optionsAction);

    QMenu* window_menu = appMenuBar->addMenu(tr("&Window"));
    window_menu->setObjectName(QStringLiteral("windowMenu"));

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
        QAction* main_window_action = window_menu->addAction(tr("Main Window"));
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

    QMenu *help = appMenuBar->addMenu(tr("&Help"));
    help->setObjectName(QStringLiteral("helpMenu"));
    help->addAction(showHelpMessageAction);
    help->addSeparator();
    help->addAction(aboutAction);
    help->addAction(aboutQtAction);
}

void QuicksilverGUI::createToolBars()
{
    if(vaultFrame)
    {
        QToolBar *toolbar = new RailToolBar(tr("Command rail"), this);
        appToolBar = toolbar;
        toolbar->setObjectName(QStringLiteral("primaryCommandRail"));
        toolbar->setMovable(false);
        toolbar->setFloatable(false);
        toolbar->setAllowedAreas(Qt::LeftToolBarArea | Qt::RightToolBarArea);
        toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        toolbar->setIconSize(QSize(18, 18));
        toolbar->setFixedWidth(RailButton::RAIL_WIDTH);
        addToolBar(Qt::LeftToolBarArea, toolbar);

        auto* brand = new QFrame(toolbar);
        brand->setObjectName(QStringLiteral("commandRailBrand"));
        auto* brand_layout = new QHBoxLayout(brand);
        brand_layout->setContentsMargins(16, 0, 12, 0);
        brand_layout->setSpacing(10);
        // The brand cell and the top bar share one bottom rule; the top bar's
        // height decides it (see eventFilter).
        m_rail_brand = brand;
        brand->setFixedHeight(m_bench_top_bar ? m_bench_top_bar->sizeHint().height() : 72);
        if (m_bench_top_bar) m_bench_top_bar->installEventFilter(this);

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
        QFont section_font = section_label->font();
        section_font.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);
        section_label->setFont(section_font);
        toolbar->addWidget(section_label);

        const auto add_navigation_action = [toolbar](QAction* action) {
            auto* row = new RailButton(toolbar);
            row->setDefaultAction(action);
            toolbar->addWidget(row);
        };
        add_navigation_action(overviewAction);
        add_navigation_action(sendCoinsAction);
        add_navigation_action(receiveCoinsAction);
        add_navigation_action(agentAllotmentAction);
        add_navigation_action(historyAction);
        add_navigation_action(mineMintAction);
        add_navigation_action(networkAction);
        overviewAction->setChecked(true);
        const auto track_page = [this](QAction* action) {
            connect(action, &QAction::toggled, this, [this](bool checked) {
                if (checked) refreshBenchChrome();
            });
        };
        track_page(overviewAction);
        track_page(sendCoinsAction);
        track_page(receiveCoinsAction);
        track_page(historyAction);
        track_page(agentAllotmentAction);
        track_page(mineMintAction);
        track_page(networkAction);
        refreshBenchChrome();

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

void QuicksilverGUI::createBenchChrome(QWidget* content)
{
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_bench_top_bar = new QFrame(central);
    m_bench_top_bar->setObjectName(QStringLiteral("benchTopBar"));
    auto* top_box = new QVBoxLayout(m_bench_top_bar);
    top_box->setContentsMargins(18, 10, 16, 12);
    top_box->setSpacing(6);

    // The breadcrumb sits above the ticker, as in the concept.
    m_bench_breadcrumb = new QLabel(m_bench_top_bar);
    m_bench_breadcrumb->setObjectName(QStringLiteral("benchBreadcrumb"));
    QFont crumb_font = m_bench_breadcrumb->font();
    crumb_font.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
    m_bench_breadcrumb->setFont(crumb_font);
    top_box->addWidget(m_bench_breadcrumb);

    auto* bar = new QHBoxLayout;
    bar->setSpacing(18);
    top_box->addLayout(bar);

    m_ticker_empty = new QLabel(tr("No vault open"), m_bench_top_bar);
    m_ticker_empty->setObjectName(QStringLiteral("benchTickerEmpty"));
    bar->addWidget(m_ticker_empty, 0, Qt::AlignLeft | Qt::AlignBottom);

    m_ticker_figures = new QWidget(m_bench_top_bar);
    m_ticker_figures->setObjectName(QStringLiteral("benchTickerFigures"));
    auto* figures = new QHBoxLayout(m_ticker_figures);
    figures->setContentsMargins(0, 0, 0, 0);
    figures->setSpacing(18);
    const QFont figure_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    const auto add_figure = [&](const QString& key, QLabel*& value, const QString& value_name) {
        auto* box = new QWidget(m_ticker_figures);
        auto* column = new QVBoxLayout(box);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(1);
        auto* key_label = new QLabel(key, box);
        key_label->setObjectName(QStringLiteral("benchTickerKey"));
        QFont key_font = key_label->font();
        key_font.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);
        key_label->setFont(key_font);
        column->addWidget(key_label);
        auto* line = new QHBoxLayout;
        line->setSpacing(5);
        value = new QLabel(box);
        value->setObjectName(value_name);
        value->setFont(figure_font);
        line->addWidget(value, 0, Qt::AlignBottom);
        auto* unit = new QLabel(box);
        unit->setObjectName(QStringLiteral("benchTickerUnit"));
        line->addWidget(unit, 0, Qt::AlignBottom);
        line->addStretch();
        column->addLayout(line);
        figures->addWidget(box);
        return box;
    };
    // "Spendable" is the model's word: the balance includes this vault's own
    // unconfirmed change, which "Confirmed" would misdescribe.
    add_figure(tr("SPENDABLE"), m_ticker_spendable, QStringLiteral("benchTickerSpendable"));
    auto* separator = new QFrame(m_ticker_figures);
    separator->setObjectName(QStringLiteral("benchTickerSeparator"));
    separator->setFixedWidth(1);
    figures->addWidget(separator);
    add_figure(tr("PENDING"), m_ticker_pending, QStringLiteral("benchTickerPending"));
    add_figure(tr("MATURING"), m_ticker_maturing, QStringLiteral("benchTickerMaturing"));
    m_ticker_delegated_box = add_figure(tr("DELEGATED"), m_ticker_delegated, QStringLiteral("benchTickerDelegated"));
    m_ticker_delegated_box->setObjectName(QStringLiteral("benchTickerDelegatedBox"));
    add_figure(tr("TOTAL"), m_ticker_total, QStringLiteral("benchTickerTotal"));
    m_ticker_figures->hide();
    bar->addWidget(m_ticker_figures, 0, Qt::AlignLeft | Qt::AlignBottom);
    bar->addStretch(1);

    // The page's two everyday commands, driven by the rail's actions.
    m_bench_request_button = new QPushButton(tr("Request"), m_bench_top_bar);
    m_bench_request_button->setObjectName(QStringLiteral("benchRequestButton"));
    m_bench_transfer_button = new QPushButton(tr("Transfer"), m_bench_top_bar);
    m_bench_transfer_button->setObjectName(QStringLiteral("benchTransferButton"));
    m_bench_transfer_button->setProperty("class", QStringLiteral("primaryActionButton"));
    bar->addWidget(m_bench_request_button, 0, Qt::AlignBottom);
    bar->addWidget(m_bench_transfer_button, 0, Qt::AlignBottom);

    layout->addWidget(m_bench_top_bar);
    layout->addWidget(content, 1);
}

void QuicksilverGUI::connectBenchCommands()
{
    if (!m_bench_request_button || !m_bench_transfer_button) return;
    const auto bind = [](QPushButton* button, QAction* action) {
        button->setEnabled(action->isEnabled());
        QObject::connect(action, &QAction::changed, button, [button, action] { button->setEnabled(action->isEnabled()); });
        QObject::connect(button, &QPushButton::clicked, action, &QAction::trigger);
    };
    bind(m_bench_request_button, receiveCoinsAction);
    bind(m_bench_transfer_button, sendCoinsAction);
}

void QuicksilverGUI::refreshBenchChrome()
{
    if (!m_bench_breadcrumb) return;

    QString page = tr("Home");
    const QAction* const actions[] = {
        overviewAction, sendCoinsAction, receiveCoinsAction, historyAction,
        agentAllotmentAction, mineMintAction, networkAction,
    };
    for (const QAction* action : actions) {
        if (action && action->isChecked()) {
            page = action->text();
            page.remove(QLatin1Char('&'));
            break;
        }
    }

    QString vault_name = tr("No vault open");
#ifdef ENABLE_VAULT
    if (vaultFrame) {
        if (VaultModel* model = vaultFrame->currentVaultModel()) {
            const QString display = model->getDisplayName();
            if (!display.isEmpty()) vault_name = display;
        }
    }
#endif
    m_bench_breadcrumb->setText((vault_name + QStringLiteral(" / ") + page).toUpper());
    refreshTicker();
    refreshStatusStrip();
}

void QuicksilverGUI::refreshTicker()
{
    if (!m_ticker_empty || !m_ticker_figures) return;

#ifdef ENABLE_VAULT
    VaultModel* model = vaultFrame ? vaultFrame->currentVaultModel() : nullptr;
    if (!model) {
        if (m_ticker_balance_connection) {
            QObject::disconnect(m_ticker_balance_connection);
            m_ticker_balance_connection = {};
        }
        m_ticker_model = nullptr;
        m_ticker_figures->hide();
        m_ticker_empty->show();
        return;
    }

    if (model != m_ticker_model) {
        if (m_ticker_balance_connection) QObject::disconnect(m_ticker_balance_connection);
        m_ticker_model = model;
        m_ticker_balance_connection = connect(model, &VaultModel::balanceChanged, this, [this](const interfaces::VaultBalances&) {
            refreshTicker();
        });
        if (OptionsModel* options = model->getOptionsModel()) {
            connect(options, &OptionsModel::displayUnitChanged, this, [this](QuicksilverUnit) { refreshTicker(); }, Qt::UniqueConnection);
        }
    }

    const interfaces::VaultBalances balances = model->getCachedBalance();
    const QuicksilverUnit unit = model->getOptionsModel() ? model->getOptionsModel()->getDisplayUnit() : QuicksilverUnit::HG;
    const bool privacy = isPrivacyModeActivated();
    const auto paint = [&](QLabel* label, CAmount amount) {
        label->setText(QuicksilverUnits::formatInlineValueWithPrivacy(unit, amount, QuicksilverUnits::SeparatorStyle::ALWAYS, privacy));
    };
    paint(m_ticker_spendable, balances.balance);
    paint(m_ticker_pending, balances.unconfirmed_balance);
    paint(m_ticker_maturing, balances.immature_balance);
    paint(m_ticker_delegated, balances.delegated_balance);
    paint(m_ticker_total, balances.balance + balances.unconfirmed_balance + balances.immature_balance + balances.delegated_balance);
    // The symbol keeps its spelling, Hg, even beside upper-case captions.
    const QString unit_name = QuicksilverUnits::shortName(unit);
    for (QLabel* unit_label : m_ticker_figures->findChildren<QLabel*>(QStringLiteral("benchTickerUnit"))) {
        unit_label->setText(unit_name);
    }
    m_ticker_delegated_box->setVisible(balances.delegated_balance != 0);
    m_ticker_empty->hide();
    m_ticker_figures->show();
#else
    m_ticker_figures->hide();
    m_ticker_empty->show();
#endif
}

void QuicksilverGUI::refreshStatusStrip()
{
    if (!m_status_sync) return;

    if (m_reported_blocks < 0) {
        m_status_sync->setText(tr("Connecting"));
    } else if (m_reported_synced) {
        m_status_sync->setText(tr("Synchronized"));
    } else {
        m_status_sync->setText(tr("Catching up"));
    }
    const char* tone = (m_reported_blocks >= 0 && m_reported_synced) ? "good" : "plain";
    for (QLabel* label : {m_status_dot, m_status_sync}) {
        label->setProperty("benchTone", tone);
        label->style()->unpolish(label);
        label->style()->polish(label);
    }

    m_status_height->setText(m_reported_blocks >= 0 ? tr("Height %1").arg(m_reported_blocks) : tr("Height unavailable"));

    if (m_reported_peers < 0) {
        m_status_peers->setText(tr("No peers"));
    } else if (m_reported_peers == 1) {
        m_status_peers->setText(tr("1 peer"));
    } else {
        m_status_peers->setText(tr("%1 peers").arg(m_reported_peers));
    }

    // The miner status wins once the mining model has reported. Before that,
    // the launch card is the consensus answer: Locked or Available.
    QString mining_word;
    if (m_have_mining_status) {
        mining_word = m_mining_status_word;
    } else if (const QLabel* card = findChild<QLabel*>(QStringLiteral("launchMiningCardState"))) {
        mining_word = card->text();
    }
    m_status_mining->setText(mining_word.isEmpty() ? tr("Mining locked") : tr("Mining %1").arg(mining_word.toLower()));

    QString vault_text = tr("No vault open");
#ifdef ENABLE_VAULT
    VaultModel* model = vaultFrame ? vaultFrame->currentVaultModel() : nullptr;
    if (model) {
        switch (model->getEncryptionStatus()) {
        case VaultModel::Locked:
            vault_text = tr("Vault locked");
            break;
        case VaultModel::Unlocked:
            vault_text = tr("Vault unlocked");
            break;
        case VaultModel::Unencrypted:
            vault_text = tr("Vault not encrypted");
            break;
        case VaultModel::NoKeys:
            vault_text = tr("Vault has no keys");
            break;
        }
        const QWidget* backup_panel = findChild<QWidget*>(QStringLiteral("desktopLaunchBackupPanel"));
        const QLabel* backup_state = findChild<QLabel*>(QStringLiteral("desktopLaunchBackupState"));
        if (backup_panel && backup_state && !backup_panel->isHidden() && !backup_state->text().isEmpty()) {
            vault_text += QStringLiteral(" · ");
            vault_text += tr("backup %1").arg(backup_state->text().toLower());
        }
    }
#endif
    m_status_vault->setText(vault_text);
}

void QuicksilverGUI::applyMiningStatus(const interfaces::MiningStatus& status)
{
#ifdef ENABLE_VAULT
    m_have_mining_status = true;
    m_mining_status_word = MineMintPage::statusText(status).block_mining;
    refreshStatusStrip();
#else
    Q_UNUSED(status);
#endif
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
            connect(vaultFrame, &VaultFrame::miningStatusUpdated, this, &QuicksilverGUI::applyMiningStatus, Qt::UniqueConnection);
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
        m_have_mining_status = false;
        m_mining_status_word.clear();
        refreshStatusStrip();
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
        if (historyAction->isChecked()) gotoHomePage();
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
    refreshBenchChrome();
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

void QuicksilverGUI::markConsensusTorMissing()
{
    if (!vaultFrame) return;
    // The vault frame shows the network page; keep the sidebar on it too.
    networkAction->setChecked(true);
    vaultFrame->markConsensusTorMissing();
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
    refreshStatusStrip();
    if (enabled && !was_enabled && !clientModel) {
        beginConsensusActivation();
    }
}

void QuicksilverGUI::beginConsensusActivation()
{
    // Asked first: a refused start must not have closed an open vault for a
    // handover that is never going to happen.
    if (m_consensus_preflight && !m_consensus_preflight()) {
        markConsensusTorMissing();
        return;
    }

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

void QuicksilverGUI::gotoHomePage()
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
    m_reported_peers = count;
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
    refreshStatusStrip();
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

    m_reported_blocks = count;

    // Prevent orphan statusbar messages (e.g. hover Quit in main menu, wait until chain-sync starts -> garbled text)
    statusBar()->clearMessage();

    // Acquire current block source
    BlockSource blockSource{clientModel->getBlockSource()};
    switch (blockSource) {
        case BlockSource::NETWORK:
            if (synctype == SyncType::HEADER_PRESYNC) {
                updateHeadersPresyncProgressLabel(count, blockDate);
                refreshStatusStrip();
                return;
            } else if (synctype == SyncType::HEADER_SYNC) {
                updateHeadersSyncProgressLabel();
                refreshStatusStrip();
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
                refreshStatusStrip();
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
    m_reported_synced = !catching_up;
    if (!catching_up) {
        tooltip = tr("Up to date") + QString(".<br>") + tooltip;
        labelBlocksIcon->setThemedPixmap(QStringLiteral(":/icons/synced"), STATUSBAR_ICONSIZE, STATUSBAR_ICONSIZE);

#ifdef ENABLE_VAULT
        if (vaultFrame) modalOverlay->showHide(true, true);
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
        if (vaultFrame) modalOverlay->showHide();
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
#ifdef ENABLE_VAULT
    if (vaultFrame && synctype == SyncType::BLOCK_SYNC) {
        vaultFrame->setChainTip(count, blockDate);
        vaultFrame->setSyncState(m_reported_synced, nVerificationProgress);
    }
#endif
    refreshStatusStrip();
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
        overviewAction->setIcon(RailIcon(platformStyle, QStringLiteral(":/icons/overview")));
        sendCoinsAction->setIcon(RailIcon(platformStyle, QStringLiteral(":/icons/send")));
        receiveCoinsAction->setIcon(RailIcon(platformStyle, QStringLiteral(":/icons/receiving_addresses")));
        agentAllotmentAction->setIcon(RailIcon(platformStyle, QStringLiteral(":/icons/agent")));
        historyAction->setIcon(RailIcon(platformStyle, QStringLiteral(":/icons/history")));
        mineMintAction->setIcon(RailIcon(platformStyle, QStringLiteral(":/icons/tx_mined")));
        networkAction->setIcon(RailIcon(platformStyle, QStringLiteral(":/icons/connect_4")));
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
    if (object == m_bench_top_bar && event->type() == QEvent::Resize && m_rail_brand) {
        m_rail_brand->setFixedHeight(m_bench_top_bar->height());
    }
    // Catch status tip events
    if (event->type() == QEvent::StatusTip)
    {
        // The status strip is persistent. QMainWindow would show a tip as a
        // status bar message, which hides the strip, so the tip goes to the
        // strip's own tip label instead; none while sync progress is showing.
        if (m_status_tip && object == this) {
            const bool busy = progressBarLabel->isVisible() || progressBar->isVisible();
            m_status_tip->setText(busy ? QString() : static_cast<QStatusTipEvent*>(event)->tip());
        }
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
    refreshStatusStrip();
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
