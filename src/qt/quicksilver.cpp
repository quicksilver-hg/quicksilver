// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <quicksilver-build-config.h> // IWYU pragma: keep

#include <qt/quicksilver.h>

#include <chainparams.h>
#include <common/args.h>
#include <common/init.h>
#include <common/system.h>
#include <init.h>
#include <interfaces/handler.h>
#include <interfaces/init.h>
#include <interfaces/node.h>
#include <logging.h>
#include <node/context.h>
#include <node/interface_ui.h>
#include <noui.h>
#include <tor/bundled_tor.h>
#include <torcontrol.h>
#include <qt/quicksilvergui.h>
#include <qt/clientmodel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/initexecutor.h>
#include <qt/intro.h>
#include <qt/networkstyle.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/quicksilverstyle.h>
#include <qt/splashscreen.h>
#include <qt/utilitydialog.h>
#include <qt/winshutdownmonitor.h>
#include <uint256.h>
#include <util/exception.h>
#include <util/string.h>
#include <util/threadnames.h>
#include <util/translation.h>
#include <validation.h>

#ifdef ENABLE_VAULT
#include <qt/paymentserver.h>
#include <qt/vaultcontroller.h>
#include <qt/vaultmodel.h>
#include <vault/types.h>
#endif // ENABLE_VAULT

#include <boost/signals2/connection.hpp>
#include <chrono>
#include <memory>
#include <optional>

#include <QApplication>
#include <QDebug>
#include <QLatin1String>
#include <QLibraryInfo>
#include <QLocale>
#include <QMessageBox>
#include <QProcess>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <QTranslator>
#include <QWindow>

// Declare meta types used for QMetaObject::invokeMethod
Q_DECLARE_METATYPE(bool*)
Q_DECLARE_METATYPE(CAmount)
Q_DECLARE_METATYPE(SynchronizationState)
Q_DECLARE_METATYPE(SyncType)
Q_DECLARE_METATYPE(uint256)
#ifdef ENABLE_VAULT
Q_DECLARE_METATYPE(vault::AddressPurpose)
#endif // ENABLE_VAULT

using util::MakeUnorderedList;

namespace {
constexpr const char* CONSENSUS_ENABLED_SETTING = "Desktop/ConsensusEnabled";

bool ConsensusEnabledOnThisDesktop()
{
    return QSettings().value(QLatin1String(CONSENSUS_ENABLED_SETTING), false).toBool();
}

bool IsChainSelectorArgument(QString arg)
{
    if (arg.startsWith(QLatin1String("--"))) arg.remove(0, 1);
#ifdef WIN32
    if (arg.startsWith(QLatin1Char('/'))) arg[0] = QLatin1Char('-');
#endif
    const QString lower = arg.toLower();
    return lower == QLatin1String("-sandbox") ||
        lower.startsWith(QLatin1String("-sandbox=")) ||
        lower == QLatin1String("-nosandbox") ||
        lower.startsWith(QLatin1String("-nosandbox=")) ||
        lower == QLatin1String("-publictest") ||
        lower.startsWith(QLatin1String("-publictest=")) ||
        lower == QLatin1String("-nopublictest") ||
        lower.startsWith(QLatin1String("-nopublictest=")) ||
        lower == QLatin1String("-chain") ||
        lower.startsWith(QLatin1String("-chain="));
}

bool IsSandboxOnlyArgument(QString arg)
{
    if (arg.startsWith(QLatin1String("--"))) arg.remove(0, 1);
#ifdef WIN32
    if (arg.startsWith(QLatin1Char('/'))) arg[0] = QLatin1Char('-');
#endif
    const QString lower = arg.toLower();
    return lower == QLatin1String("-test") ||
        lower.startsWith(QLatin1String("-test=")) ||
        lower == QLatin1String("-testactivationheight") ||
        lower.startsWith(QLatin1String("-testactivationheight=")) ||
        lower == QLatin1String("-txpownocycle") ||
        lower.startsWith(QLatin1String("-txpownocycle=")) ||
        lower == QLatin1String("-notxpownocycle") ||
        lower.startsWith(QLatin1String("-notxpownocycle=")) ||
        lower == QLatin1String("-vbparams") ||
        lower.startsWith(QLatin1String("-vbparams="));
}

QStringList RestartArgumentsForChain(const QStringList& original_arguments, const QString& chain_token)
{
    QStringList arguments;
    for (const QString& argument : original_arguments) {
        if (!argument.startsWith(QLatin1Char('-'))) break;
        if (IsChainSelectorArgument(argument)) continue;
        if (chain_token != QLatin1String("sandbox") && IsSandboxOnlyArgument(argument)) continue;
        arguments.push_back(argument);
    }
    arguments.push_back(QStringLiteral("-chain=%1").arg(chain_token));
    return arguments;
}

bool StartRestartProcess(const QString& executable, const QStringList& arguments)
{
#ifdef WIN32
    return QProcess::startDetached(executable, arguments);
#else
    QStringList delayed_arguments;
    delayed_arguments << QStringLiteral("-c")
                      << QStringLiteral("sleep 1; exec \"$@\"")
                      << QStringLiteral("quicksilver-restart")
                      << executable;
    delayed_arguments << arguments;
    return QProcess::startDetached(QStringLiteral("/bin/sh"), delayed_arguments);
#endif
}
} // namespace

static void RegisterMetaTypes()
{
    // Register meta types used for QMetaObject::invokeMethod and Qt::QueuedConnection
    qRegisterMetaType<bool*>();
    qRegisterMetaType<SynchronizationState>();
    qRegisterMetaType<SyncType>();
  #ifdef ENABLE_VAULT
    qRegisterMetaType<VaultModel*>();
    qRegisterMetaType<vault::AddressPurpose>();
  #endif // ENABLE_VAULT
    // Register typedefs (see https://doc.qt.io/qt-5/qmetatype.html#qRegisterMetaType)
    // IMPORTANT: if CAmount is no longer a typedef use the normal variant above (see https://doc.qt.io/qt-5/qmetatype.html#qRegisterMetaType-1)
    qRegisterMetaType<CAmount>("CAmount");
    qRegisterMetaType<size_t>("size_t");

    qRegisterMetaType<std::function<void()>>("std::function<void()>");
    qRegisterMetaType<QMessageBox::Icon>("QMessageBox::Icon");
    qRegisterMetaType<interfaces::BlockAndHeaderTipInfo>("interfaces::BlockAndHeaderTipInfo");

#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    qRegisterMetaTypeStreamOperators<QuicksilverUnit>("QuicksilverUnit");
#else
    qRegisterMetaType<QuicksilverUnit>("QuicksilverUnit");
#endif
}

static QString GetLangTerritory()
{
    QSettings settings;
    // Get desired locale (e.g. "de_DE")
    // 1) System default language
    QString lang_territory = QLocale::system().name();
    // 2) Language from QSettings
    QString lang_territory_qsettings = settings.value("language", "").toString();
    if(!lang_territory_qsettings.isEmpty())
        lang_territory = lang_territory_qsettings;
    // 3) -lang command line argument
    lang_territory = QString::fromStdString(gArgs.GetArg("-lang", lang_territory.toStdString()));
    return lang_territory;
}

/** Set up translations */
static void initTranslations(QTranslator &qtTranslatorBase, QTranslator &qtTranslator)
{
    // Remove old translators
    QApplication::removeTranslator(&qtTranslatorBase);
    QApplication::removeTranslator(&qtTranslator);

    // Get desired locale (e.g. "de_DE")
    // 1) System default language
    QString lang_territory = GetLangTerritory();

    // Convert to "de" only by truncating "_DE"
    QString lang = lang_territory;
    lang.truncate(lang_territory.lastIndexOf('_'));

    // Load only Qt's own base translators for the configured locale so that
    // OS-native dialog strings (standard buttons, file dialogs) stay localized.
    // The Quicksilver application itself is English-only (SP4), so no app
    // translator is installed.

#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    const QString translation_path{QLibraryInfo::location(QLibraryInfo::TranslationsPath)};
#else
    const QString translation_path{QLibraryInfo::path(QLibraryInfo::TranslationsPath)};
#endif
    // Load e.g. qt_de.qm
    if (qtTranslatorBase.load("qt_" + lang, translation_path)) {
        QApplication::installTranslator(&qtTranslatorBase);
    }

    // Load e.g. qt_de_DE.qm
    if (qtTranslator.load("qt_" + lang_territory, translation_path)) {
        QApplication::installTranslator(&qtTranslator);
    }
}

static bool ErrorSettingsRead(const bilingual_str& error, const std::vector<std::string>& details)
{
    QMessageBox messagebox(QMessageBox::Critical, CLIENT_NAME, QString::fromStdString(strprintf("%s.", error.translated)), QMessageBox::Reset | QMessageBox::Abort);
    /*: Explanatory text shown on startup when the settings file cannot be read.
      Prompts user to make a choice between resetting or aborting. */
    messagebox.setInformativeText(QObject::tr("Do you want to reset settings to default values, or to abort without making changes?"));
    messagebox.setDetailedText(QString::fromStdString(MakeUnorderedList(details)));
    messagebox.setTextFormat(Qt::PlainText);
    messagebox.setDefaultButton(QMessageBox::Reset);
    switch (messagebox.exec()) {
    case QMessageBox::Reset:
        return false;
    case QMessageBox::Abort:
        return true;
    default:
        assert(false);
    }
}

static void ErrorSettingsWrite(const bilingual_str& error, const std::vector<std::string>& details)
{
    QMessageBox messagebox(QMessageBox::Critical, CLIENT_NAME, QString::fromStdString(strprintf("%s.", error.translated)), QMessageBox::Ok);
    /*: Explanatory text shown on startup when the settings file could not be written.
        Prompts user to check that we have the ability to write to the file.
        Explains that the user has the option of running without a settings file.*/
    messagebox.setInformativeText(QObject::tr("A fatal error occurred. Check that settings file is writable, or try running with -nosettings."));
    messagebox.setDetailedText(QString::fromStdString(MakeUnorderedList(details)));
    messagebox.setTextFormat(Qt::PlainText);
    messagebox.setDefaultButton(QMessageBox::Ok);
    messagebox.exec();
}

/* qDebug() message handler --> debug.log */
void DebugMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString &msg)
{
    Q_UNUSED(context);
    if (type == QtDebugMsg) {
        LogDebug(HgLog::QT, "GUI: %s\n", msg.toStdString());
    } else {
        LogInfo(HgLog::QT, "GUI: %s\n", msg.toStdString());
    }
}

static int qt_argc = 1;
static const char* qt_argv = "quicksilver";

QuicksilverApplication::QuicksilverApplication()
    : QApplication(qt_argc, const_cast<char**>(&qt_argv))
{
    // Qt runs setlocale(LC_ALL, "") on initialization.
    RegisterMetaTypes();
    setQuitOnLastWindowClosed(false);
}

void QuicksilverApplication::setupPlatformStyle()
{
    // UI per-platform customization
    // This must be done inside the QuicksilverApplication constructor, or after it, because
    // PlatformStyle::instantiate requires a QApplication
    std::string platformName;
    platformName = gArgs.GetArg("-uiplatform", QuicksilverGUI::DEFAULT_UIPLATFORM);
    platformStyle = PlatformStyle::instantiate(QString::fromStdString(platformName));
    if (!platformStyle) // Fall back to "other" if specified name not found
        platformStyle = PlatformStyle::instantiate("other");
    assert(platformStyle);
    QuicksilverStyle::Apply(*this);
}

QuicksilverApplication::~QuicksilverApplication()
{
    m_executor.reset();

    delete window;
    window = nullptr;
    delete platformStyle;
    platformStyle = nullptr;
}

#ifdef ENABLE_VAULT
void QuicksilverApplication::createPaymentServer()
{
    paymentServer = new PaymentServer(this);
}
#endif

bool QuicksilverApplication::createOptionsModel(bool resetSettings)
{
    optionsModel = new OptionsModel(node(), this);
    if (resetSettings) {
        optionsModel->Reset();
    }
    bilingual_str error;
    if (!optionsModel->Init(error)) {
        fs::path settings_path;
        if (gArgs.GetSettingsPath(&settings_path)) {
            error += Untranslated("\n");
            std::string quoted_path = strprintf("%s", fs::quoted(fs::PathToString(settings_path)));
            error.original += strprintf("Settings file %s might be corrupt or invalid.", quoted_path);
            error.translated += tr("Settings file %1 might be corrupt or invalid.").arg(QString::fromStdString(quoted_path)).toStdString();
        }
        InitError(error);
        QMessageBox::critical(nullptr, CLIENT_NAME, QString::fromStdString(error.translated));
        return false;
    }
    return true;
}

void QuicksilverApplication::createWindow(const NetworkStyle *networkStyle)
{
    window = new QuicksilverGUI(node(), platformStyle, networkStyle, nullptr);
    connect(window, &QuicksilverGUI::quitRequested, this, &QuicksilverApplication::requestShutdown);
    connect(window, &QuicksilverGUI::consensusActivationRequested, this, &QuicksilverApplication::requestInitialize);
    connect(window, &QuicksilverGUI::networkRestartRequested, this, &QuicksilverApplication::requestRestart);

    pollShutdownTimer = new QTimer(window);
    connect(pollShutdownTimer, &QTimer::timeout, [this]{
        pollShutdownTick(node().shutdownRequested());
    });
}

void QuicksilverApplication::pollShutdownTick(bool shutdown_requested)
{
    if (!shutdown_requested) return;

    if (QWidget* modal = QApplication::activeModalWidget()) {
        // Closing it unwinds the nested event loop; the next tick finds no modal
        // and takes the shutdown with a clear stack. Skipping the tick instead --
        // which is what this poll used to do -- means SIGTERM and the `stop` RPC
        // both report success and do nothing for as long as the dialog is up,
        // and a desktop session ending sends SIGTERM before it sends SIGKILL.
        qInfo() << "Shutdown requested with a modal dialog open; closing" << modal->objectName();
        modal->close();
        return;
    }

    if (window) window->detectShutdown();
}

void QuicksilverApplication::createSplashScreen(const NetworkStyle *networkStyle)
{
    assert(!m_splash);
    m_splash = new SplashScreen(networkStyle);
    m_splash->show();
}

void QuicksilverApplication::createNode(interfaces::Init& init)
{
    assert(!m_node);
    m_node = init.makeNode();
    if (m_splash) m_splash->setNode(*m_node);
}

void QuicksilverApplication::setRestartCommandLine(int argc, char* argv[])
{
    m_restart_executable = QCoreApplication::applicationFilePath();
    m_restart_arguments.clear();
    for (int i = 1; i < argc; ++i) {
        m_restart_arguments.push_back(QString::fromLocal8Bit(argv[i]));
    }
}

QStringList QuicksilverApplication::restartArgumentsForChainForTesting(const QStringList& original_arguments, const QString& chain_token)
{
    return RestartArgumentsForChain(original_arguments, chain_token);
}

bool QuicksilverApplication::baseInitialize()
{
    if (!node().baseInitialize()) return false;
#ifdef ENABLE_VAULT
    // A persisted consensus opt-in means AppInitMain owns startup vault loading.
    // Creating the GUI controller here would race it by opening the same SQLite
    // vault from settings.json before node initialisation can attach it to the
    // chain. Vault-first launches still create the controller immediately.
    if (!ConsensusEnabledOnThisDesktop()) createVaultController();
#endif
    return true;
}

#ifdef ENABLE_VAULT
void QuicksilverApplication::createVaultController()
{
    if (m_vault_controller || !window || !VaultModel::isVaultEnabled()) return;
    m_vault_controller = new VaultController(node(), optionsModel, platformStyle, this);
    if (clientModel) {
        m_vault_controller->setClientModel(clientModel);
    }
    window->setVaultController(m_vault_controller, /*show_loading_minimized=*/gArgs.GetBoolArg("-min", false));
    if (paymentServer && optionsModel) {
        paymentServer->setOptionsModel(optionsModel);
    }
}
#endif // ENABLE_VAULT

void QuicksilverApplication::startThread()
{
    assert(!m_executor);
    m_executor.emplace(node());

    /*  communication to and from thread */
    connect(&m_executor.value(), &InitExecutor::initializeResult, this, &QuicksilverApplication::initializeResult);
    connect(&m_executor.value(), &InitExecutor::shutdownResult, this, &QuicksilverApplication::shutdownResult);
    connect(&m_executor.value(), &InitExecutor::runawayException, this, &QuicksilverApplication::handleRunawayException);
    connect(this, &QuicksilverApplication::requestedInitialize, &m_executor.value(), &InitExecutor::initialize);
    connect(this, &QuicksilverApplication::requestedShutdown, &m_executor.value(), &InitExecutor::shutdown);
}

void QuicksilverApplication::parameterSetup()
{
    // Default printtoconsole to false for the GUI. GUI programs should not
    // print to the console unnecessarily.
    gArgs.SoftSetBoolArg("-printtoconsole", false);

    // The desktop bundles Tor: the seed is onion-only, and a first run on a
    // machine that has never installed Tor cannot otherwise find the network at
    // all. Soft, so quicksilver.conf and the command line still win -- and this
    // runs after common::InitConfig(), so the config file has already been read.
    // Conditional on -listenonion because AppInitParameterInteraction refuses
    // the pair: a user who turned onion listening off must not be stopped by a
    // default of OURS.
    if (gArgs.GetBoolArg("-listenonion", DEFAULT_LISTEN_ONION)) {
        gArgs.SoftSetBoolArg("-bundledtor", true);
    }

    InitLogging(gArgs);
    InitParameterInteraction(gArgs);
}

void QuicksilverApplication::InitPruneSetting(int64_t prune_MiB)
{
    optionsModel->SetPruneTargetGB(PruneMiBtoGB(prune_MiB));
}

void QuicksilverApplication::requestInitialize()
{
    if (m_consensus_initialization_requested) return;
    m_consensus_initialization_requested = true;
    qDebug() << __func__ << ": Requesting initialize";
    startThread();
    Q_EMIT requestedInitialize();
}

void QuicksilverApplication::showWindow()
{
    assert(window);
    if (m_window_shown) return;
    m_window_shown = true;

    // If '-min' option passed, start window minimized (iconified) or minimized to tray
    bool start_minimized = gArgs.GetBoolArg("-min", false);
    if (!start_minimized) {
        window->show();
    } else if (optionsModel && optionsModel->getMinimizeToTray() && window->hasTrayIcon()) {
        // do nothing as the window is managed by the tray icon
    } else {
        window->showMinimized();
    }
    Q_EMIT windowShown(window);
    if (pollShutdownTimer) pollShutdownTimer->start(SHUTDOWN_POLLING_DELAY);
}

void QuicksilverApplication::requestShutdown()
{
    for (const auto w : QGuiApplication::topLevelWindows()) {
        w->hide();
    }

    delete m_splash;
    m_splash = nullptr;

    // Show a simple window indicating shutdown status
    // Do this first as some of the steps may take some time below,
    // for example the RPC console may still be executing a command.
    shutdownWindow.reset(ShutdownWindow::showShutdownWindow(window));

    qDebug() << __func__ << ": Requesting shutdown";

    // Must disconnect node signals otherwise current thread can deadlock since
    // no event loop is running.
    if (window) window->unsubscribeFromCoreSignals();
    // Request node shutdown, which can interrupt long operations, like
    // rescanning a vault.
    node().startShutdown();
    // Prior to unsetting the client model, stop listening backend signals
    if (clientModel) {
        clientModel->stop();
    }

    // Unsetting the client model can cause the current thread to wait for node
    // to complete an operation, like wait for a RPC execution to complete.
    if (window) window->setClientModel(nullptr);
    if (pollShutdownTimer) pollShutdownTimer->stop();

#ifdef ENABLE_VAULT
    // Delete vault controller here manually, instead of relying on Qt object
    // tracking (https://doc.qt.io/qt-5/objecttrees.html). This makes sure
    // vaultmodel m_handle_* notification handlers are deleted before vaults
    // are unloaded, which can simplify vault implementations. It also avoids
    // these notifications having to be handled while GUI objects are being
    // destroyed, making GUI code less fragile as well.
    delete m_vault_controller;
    m_vault_controller = nullptr;
#endif // ENABLE_VAULT

    delete clientModel;
    clientModel = nullptr;

    if (!m_executor) {
        node().appShutdown();
        launchPendingRestart();
        QCoreApplication::exit(0);
        return;
    }

    // Request shutdown from core thread
    Q_EMIT requestedShutdown();
}

void QuicksilverApplication::cleanupFailedConsensusInitialization()
{
    delete m_splash;
    m_splash = nullptr;

#ifdef ENABLE_VAULT
    delete m_vault_controller;
    m_vault_controller = nullptr;
#endif // ENABLE_VAULT

    m_consensus_failure_cleanup = true;
    if (!m_executor) {
        node().appShutdown();
        m_consensus_failure_cleanup = false;
        return;
    }
    Q_EMIT requestedShutdown();
}

void QuicksilverApplication::shutdownResult()
{
    if (m_consensus_failure_cleanup) {
        m_consensus_failure_cleanup = false;
        m_executor.reset();
        return;
    }

    launchPendingRestart();
    QCoreApplication::exit(0);
}

void QuicksilverApplication::requestRestart(const QString& chain_token)
{
    if (m_pending_restart_chain) return;
    m_pending_restart_chain = chain_token;
    requestShutdown();
}

void QuicksilverApplication::launchPendingRestart()
{
    if (!m_pending_restart_chain) return;

    const QString executable = m_restart_executable.isEmpty()
        ? QCoreApplication::applicationFilePath()
        : m_restart_executable;
    const QStringList arguments = RestartArgumentsForChain(m_restart_arguments, *m_pending_restart_chain);
    if (!StartRestartProcess(executable, arguments)) {
        qWarning() << "Failed to restart" << executable << arguments;
    }
    m_pending_restart_chain.reset();
}

void QuicksilverApplication::initializeResult(bool success, interfaces::BlockAndHeaderTipInfo tip_info)
{
    qDebug() << __func__ << ": Initialization result: " << success;

    if (success) {
        delete m_splash;
        m_splash = nullptr;

        // Log this only after AppInitMain finishes, as then logging setup is guaranteed complete
        qInfo() << "Platform customization:" << platformStyle->getName();
        clientModel = new ClientModel(node(), optionsModel);
#ifdef ENABLE_VAULT
        if (m_vault_controller) {
            m_vault_controller->setClientModel(clientModel);
        }
#endif // ENABLE_VAULT
        window->setClientModel(clientModel, &tip_info);

#ifdef ENABLE_VAULT
        if (VaultModel::isVaultEnabled()) {
            createVaultController();
        }
#endif // ENABLE_VAULT

        showWindow();

#ifdef ENABLE_VAULT
        // Now that initialization/startup is done, process any command-line
        // quicksilver: URIs or payment requests:
        if (paymentServer) {
            connect(paymentServer, &PaymentServer::receivedPaymentRequest, window, &QuicksilverGUI::handlePaymentRequest);
            connect(window, &QuicksilverGUI::receivedURI, paymentServer, &PaymentServer::handleURIOrFile);
            connect(paymentServer, &PaymentServer::message, [this](const QString& title, const QString& message, unsigned int style) {
                window->message(title, message, style);
            });
            QTimer::singleShot(100ms, paymentServer, &PaymentServer::uiReady);
        }
#endif
    } else {
#ifdef ENABLE_VAULT
        if (m_window_shown && window && !clientModel) {
            window->markConsensusInitializationFailed();
            cleanupFailedConsensusInitialization();
            return;
        }
#endif // ENABLE_VAULT
        requestShutdown();
    }
}

void QuicksilverApplication::handleRunawayException(const QString &message)
{
    QMessageBox::critical(
        nullptr, tr("Runaway exception"),
        tr("A fatal error occurred. %1 can no longer continue safely and will quit.").arg(CLIENT_NAME) +
        QLatin1String("<br><br>") + GUIUtil::MakeHtmlLink(message, CLIENT_BUGREPORT));
    ::exit(EXIT_FAILURE);
}

void QuicksilverApplication::handleNonFatalException(const QString& message)
{
    assert(QThread::currentThread() == thread());
    QMessageBox::warning(
        nullptr, tr("Internal error"),
        tr("An internal error occurred. %1 will attempt to continue safely. This is "
           "an unexpected bug which can be reported as described below.").arg(CLIENT_NAME) +
        QLatin1String("<br><br>") + GUIUtil::MakeHtmlLink(message, CLIENT_BUGREPORT));
}

WId QuicksilverApplication::getMainWinId() const
{
    if (!window)
        return 0;

    return window->winId();
}

bool QuicksilverApplication::event(QEvent* e)
{
    if (e->type() == QEvent::Quit) {
        requestShutdown();
        return true;
    }

    return QApplication::event(e);
}

static void SetupUIArgs(ArgsManager& argsman)
{
    argsman.AddArg("-choosedatadir", strprintf("Choose data directory on startup (default: %u)", DEFAULT_CHOOSE_DATADIR), ArgsManager::ALLOW_ANY, OptionsCategory::GUI);
    argsman.AddArg("-lang=<lang>", "Set language, for example \"de_DE\" (default: system locale)", ArgsManager::ALLOW_ANY, OptionsCategory::GUI);
    argsman.AddArg("-min", "Start minimized", ArgsManager::ALLOW_ANY, OptionsCategory::GUI);
    argsman.AddArg("-resetguisettings", "Reset all settings changed in the GUI", ArgsManager::ALLOW_ANY, OptionsCategory::GUI);
    argsman.AddArg("-splash", strprintf("Show splash screen on startup (default: %u)", DEFAULT_SPLASHSCREEN), ArgsManager::ALLOW_ANY, OptionsCategory::GUI);
    argsman.AddArg("-uiplatform", strprintf("Select platform to customize UI for (one of windows, macosx, other; default: %s)", QuicksilverGUI::DEFAULT_UIPLATFORM), ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::GUI);
}

int GuiMain(int argc, char* argv[])
{
#ifdef WIN32
    common::WinCmdLineArgs winArgs;
    std::tie(argc, argv) = winArgs.get();
#endif

    std::unique_ptr<interfaces::Init> init = interfaces::MakeGuiInit();

    SetupEnvironment();
    util::ThreadSetInternalName("main");

    // Subscribe to global signals from core
    boost::signals2::scoped_connection handler_message_box = ::uiInterface.ThreadSafeMessageBox_connect(noui_ThreadSafeMessageBox);
    boost::signals2::scoped_connection handler_question = ::uiInterface.ThreadSafeQuestion_connect(noui_ThreadSafeQuestion);
    boost::signals2::scoped_connection handler_init_message = ::uiInterface.InitMessage_connect(noui_InitMessage);

    // Do not refer to data directory yet, this can be overridden by Intro::pickDataDirectory

    /// 1. Basic Qt initialization (not dependent on parameters or configuration)
    Q_INIT_RESOURCE(quicksilver);

#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    // Generate high-dpi pixmaps
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif

#if defined(QT_QPA_PLATFORM_ANDROID)
    QApplication::setAttribute(Qt::AA_DontUseNativeMenuBar);
    QApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
#endif

    QuicksilverApplication app;
    app.setRestartCommandLine(argc, argv);
    GUIUtil::LoadFont(QStringLiteral(":/fonts/monospace"));

    /// 2. Parse command-line options. We do this after qt in order to show an error if there are problems parsing these
    // Command-line options take precedence:
    SetupServerArgs(gArgs);
    SetupUIArgs(gArgs);
    std::string error;
    if (!gArgs.ParseParameters(argc, argv, error)) {
        InitError(Untranslated(strprintf("Error parsing command line arguments: %s", error)));
        // Create a message box, because the gui has neither been created nor has subscribed to core signals
        QMessageBox::critical(nullptr, CLIENT_NAME,
            // message cannot be translated because translations have not been initialized
            QString::fromStdString("Error parsing command line arguments: %1.").arg(QString::fromStdString(error)));
        return EXIT_FAILURE;
    }

    // Error out when loose non-argument tokens are encountered on command line
    // However, allow BIP-21 URIs only if no options follow
    bool payment_server_token_seen = false;
    for (int i = 1; i < argc; i++) {
        QString arg(argv[i]);
        bool invalid_token = !arg.startsWith("-");
#ifdef ENABLE_VAULT
        if (arg.startsWith(QUICKSILVER_IPC_PREFIX, Qt::CaseInsensitive)) {
            invalid_token &= false;
            payment_server_token_seen = true;
        }
#endif
        if (payment_server_token_seen && arg.startsWith("-")) {
            InitError(Untranslated(strprintf("Options ('%s') cannot follow a BIP-21 payment URI", argv[i])));
            QMessageBox::critical(nullptr, CLIENT_NAME,
                                  // message cannot be translated because translations have not been initialized
                                  QString::fromStdString("Options ('%1') cannot follow a BIP-21 payment URI").arg(QString::fromStdString(argv[i])));
            return EXIT_FAILURE;
        }
        if (invalid_token) {
            InitError(Untranslated(strprintf("Command line contains unexpected token '%s', see quicksilver -h for a list of options.", argv[i])));
            QMessageBox::critical(nullptr, CLIENT_NAME,
                                  // message cannot be translated because translations have not been initialized
                                  QString::fromStdString("Command line contains unexpected token '%1', see quicksilver -h for a list of options.").arg(QString::fromStdString(argv[i])));
            return EXIT_FAILURE;
        }
    }

    // Now that the QApplication is setup and we have parsed our parameters, we can set the platform style
    app.setupPlatformStyle();

    /// 3. Application identification
    // must be set before OptionsModel is initialized or translations are loaded,
    // as it is used to locate QSettings
    QApplication::setOrganizationName(QAPP_ORG_NAME);
    QApplication::setOrganizationDomain(QAPP_ORG_DOMAIN);
    QApplication::setApplicationName(QAPP_APP_NAME_DEFAULT);

    /// 4. Initialization of translations, so that intro dialog is in user's language
    // Now that QSettings are accessible, initialize translations
    QTranslator qtTranslatorBase, qtTranslator;
    initTranslations(qtTranslatorBase, qtTranslator);

    // Show help message immediately after parsing command-line options (for "-lang") and setting locale,
    // but before showing splash screen.
    if (HelpRequested(gArgs) || gArgs.GetBoolArg("-version", false)) {
        HelpMessageDialog help(nullptr, gArgs.GetBoolArg("-version", false));
        help.showOrPrint();
        return EXIT_SUCCESS;
    }

    // Install global event filter that makes sure that long tooltips can be word-wrapped
    app.installEventFilter(new GUIUtil::ToolTipToRichTextFilter(TOOLTIP_WRAP_THRESHOLD, &app));

    /// 5. Now that settings and translations are available, ask user for data directory
    // User language is set up: pick a data directory
    bool did_show_intro = false;
    // Intro dialog prune configuration; unset when no choice was offered because the
    // data directory already holds a chain, in which case its storage policy stands.
    std::optional<int64_t> prune_MiB;
    // Gracefully exit if the user cancels
    if (!Intro::showIfNeeded(did_show_intro, prune_MiB)) return EXIT_SUCCESS;

    /// 6-7. Parse quicksilver.conf, determine network, switch to network specific
    /// options, and create datadir and settings.json.
    // - Do not call gArgs.GetDataDirNet() before this step finishes
    // - Do not call Params() before this step
    // - QSettings() will use the new application name after this, resulting in network-specific settings
    // - Needs to be done before createOptionsModel
    if (auto error = common::InitConfig(gArgs, ErrorSettingsRead)) {
        InitError(error->message, error->details);
        if (error->status == common::ConfigStatus::FAILED_WRITE) {
            // Show a custom error message to provide more information in the
            // case of a datadir write error.
            ErrorSettingsWrite(error->message, error->details);
        } else if (error->status != common::ConfigStatus::ABORTED) {
            // Show a generic message in other cases, and no additional error
            // message in the case of a read error if the user decided to abort.
            QMessageBox::critical(nullptr, CLIENT_NAME, QObject::tr("Error: %1").arg(QString::fromStdString(error->message.translated)));
        }
        return EXIT_FAILURE;
    }
#ifdef ENABLE_VAULT
    // Parse URIs on command line
    PaymentServer::ipcParseCommandLine(argc, argv);
#endif

    QScopedPointer<const NetworkStyle> networkStyle(NetworkStyle::instantiate(Params().GetChainType()));
    assert(!networkStyle.isNull());
    // Allow for separate UI settings for each network
    QApplication::setApplicationName(networkStyle->getAppName());
    // Re-initialize translations after changing application name (language in network-specific settings can be different)
    initTranslations(qtTranslatorBase, qtTranslator);

#ifdef ENABLE_VAULT
    /// 8. URI IPC sending
    // - Do this early as we don't want to bother initializing if we are just calling IPC
    // - Do this *after* setting up the data directory, as the data directory hash is used in the name
    // of the server.
    // - Do this after creating app and setting up translations, so errors are
    // translated properly.
    if (PaymentServer::ipcSendCommandLine())
        exit(EXIT_SUCCESS);

    // Start up the payment server early, too, so impatient users that click on
    // quicksilver: links repeatedly have their payment requests routed to this process:
    if (VaultModel::isVaultEnabled()) {
        app.createPaymentServer();
    }
#endif // ENABLE_VAULT

    /// 9. Main GUI initialization
    // Install global event filter that makes sure that out-of-focus labels do not contain text cursor.
    app.installEventFilter(new GUIUtil::LabelOutOfFocusEventFilter(&app));
#if defined(Q_OS_WIN)
    // Install global event filter for processing Windows session related Windows messages (WM_QUERYENDSESSION and WM_ENDSESSION)
    // Note: it is safe to call app.node() in the lambda below despite the fact
    // that app.createNode() hasn't been called yet, because native events will
    // not be processed until the Qt event loop is executed.
    qApp->installNativeEventFilter(new WinShutdownMonitor([&app] { app.node().startShutdown(); }));
#endif
    // Install qDebug() message handler to route to debug.log
    qInstallMessageHandler(DebugMessageHandler);
    // Allow parameter interaction before we create the options model
    app.parameterSetup();
    GUIUtil::LogQtInfo();

    const bool consensus_enabled = ConsensusEnabledOnThisDesktop();
    if (consensus_enabled && gArgs.GetBoolArg("-splash", DEFAULT_SPLASHSCREEN) && !gArgs.GetBoolArg("-min", false))
        app.createSplashScreen(networkStyle.data());

    app.createNode(*init);

    // Load GUI settings from QSettings
    if (!app.createOptionsModel(gArgs.GetBoolArg("-resetguisettings", false))) {
        return EXIT_FAILURE;
    }

    if (did_show_intro && prune_MiB) {
        // Store intro dialog settings other than datadir (network specific)
        app.InitPruneSetting(*prune_MiB);
    }

    try
    {
        app.createWindow(networkStyle.data());
        // Perform base initialization before spinning up initialization/shutdown thread
        // This is acceptable because this function only contains steps that are quick to execute,
        // so the GUI thread won't be held up.
        if (app.baseInitialize()) {
            if (consensus_enabled) {
                app.requestInitialize();
            } else {
                app.showWindow();
            }
#if defined(Q_OS_WIN)
            WinShutdownMonitor::registerShutdownBlockReason(QObject::tr("%1 didn't yet exit safely…").arg(CLIENT_NAME), (HWND)app.getMainWinId());
#endif
            app.exec();
        } else {
            // A dialog with detailed error will have been shown by InitError()
            return EXIT_FAILURE;
        }
    } catch (const std::exception& e) {
        PrintExceptionContinue(&e, "Runaway exception");
        app.handleRunawayException(QString::fromStdString(app.node().getWarnings().translated));
    } catch (...) {
        PrintExceptionContinue(nullptr, "Runaway exception");
        app.handleRunawayException(QString::fromStdString(app.node().getWarnings().translated));
    }
    return app.node().getExitStatus();
}
