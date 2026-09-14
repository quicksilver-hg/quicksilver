// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_QUICKSILVER_H
#define QUICKSILVER_QT_QUICKSILVER_H

#include <quicksilver-build-config.h> // IWYU pragma: keep

#include <interfaces/node.h>
#include <qt/initexecutor.h>

#include <assert.h>
#include <memory>
#include <optional>

#include <QApplication>
#include <QString>
#include <QStringList>

class QuicksilverGUI;
class ClientModel;
class NetworkStyle;
class OptionsModel;
class PaymentServer;
class PlatformStyle;
class SplashScreen;
class VaultController;
class VaultModel;
namespace interfaces {
class Init;
} // namespace interfaces


/** Main Quicksilver application object */
class QuicksilverApplication: public QApplication
{
    Q_OBJECT
public:
    explicit QuicksilverApplication();
    ~QuicksilverApplication();

#ifdef ENABLE_VAULT
    /// Create payment server
    void createPaymentServer();
#endif
    /// parameter interaction/setup based on rules
    void parameterSetup();
    /// Create options model
    [[nodiscard]] bool createOptionsModel(bool resetSettings);
    /// Initialize prune setting
    void InitPruneSetting(int64_t prune_MiB);
    /// Create main window
    void createWindow(const NetworkStyle *networkStyle);
    /// Show the main shell before or after consensus initialization.
    void showWindow();
    /// Create splash screen
    void createSplashScreen(const NetworkStyle *networkStyle);
    /// Create or spawn node
    void createNode(interfaces::Init& init);
    /// Store the original process command line for network-context restarts.
    void setRestartCommandLine(int argc, char* argv[]);
    /// Test wrapper for the network-context restart argument sanitizer.
    static QStringList restartArgumentsForChainForTesting(const QStringList& original_arguments, const QString& chain_token);
    /**
     * One tick of the shutdown poll, with the node's answer passed in.
     *
     * A granted shutdown request must not be parked behind a dialog nobody is
     * going to answer, so an open modal is closed rather than deferred to. The
     * shutdown itself is taken on a later tick, once the modal's nested event
     * loop has unwound: requestShutdown() deletes the models and controllers
     * that loop may still be standing on.
     */
    void pollShutdownTick(bool shutdown_requested);
    /// Basic initialization, before starting initialization/shutdown thread. Return true on success.
    bool baseInitialize();

    /// Request core initialization
    void requestInitialize();

    /// Get window identifier of QMainWindow (QuicksilverGUI)
    WId getMainWinId() const;

    /// Setup platform style
    void setupPlatformStyle();

    interfaces::Node& node() const { assert(m_node); return *m_node; }

public Q_SLOTS:
    void initializeResult(bool success, interfaces::BlockAndHeaderTipInfo tip_info);
    /// Request core shutdown
    void requestShutdown();
    /// Request process restart with a different startup-bound network context.
    void requestRestart(const QString& chain_token);
    void shutdownResult();
    /// Handle runaway exceptions. Shows a message box with the problem and quits the program.
    void handleRunawayException(const QString &message);

    /**
     * A helper function that shows a message box
     * with details about a non-fatal exception.
     */
    void handleNonFatalException(const QString& message);

Q_SIGNALS:
    void requestedInitialize();
    void requestedShutdown();
    void windowShown(QuicksilverGUI* window);

protected:
    bool event(QEvent* e) override;

private:
    std::optional<InitExecutor> m_executor;
    OptionsModel* optionsModel{nullptr};
    ClientModel* clientModel{nullptr};
    QuicksilverGUI* window{nullptr};
    QTimer* pollShutdownTimer{nullptr};
    bool m_consensus_initialization_requested{false};
    bool m_consensus_failure_cleanup{false};
    bool m_window_shown{false};
    QString m_restart_executable;
    QStringList m_restart_arguments;
    std::optional<QString> m_pending_restart_chain;
#ifdef ENABLE_VAULT
    PaymentServer* paymentServer{nullptr};
    VaultController* m_vault_controller{nullptr};
#endif
    const PlatformStyle* platformStyle{nullptr};
    std::unique_ptr<QWidget> shutdownWindow;
    SplashScreen* m_splash = nullptr;
    std::unique_ptr<interfaces::Node> m_node;

    void startThread();
#ifdef ENABLE_VAULT
    void createVaultController();
#endif
    void cleanupFailedConsensusInitialization();
    void launchPendingRestart();
};

int GuiMain(int argc, char* argv[]);

#endif // QUICKSILVER_QT_QUICKSILVER_H
