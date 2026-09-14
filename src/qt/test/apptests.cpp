// Copyright (c) 2018-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/apptests.h>

#include <quicksilver-build-config.h> // IWYU pragma: keep

#include <chainparams.h>
#include <key.h>
#include <logging.h>
#include <node/interface_ui.h>
#include <qt/clientmodel.h>
#include <qt/modaloverlay.h>
#include <qt/quicksilver.h>
#include <qt/quicksilvergui.h>
#include <qt/networkstyle.h>
#include <qt/platformstyle.h>
#include <qt/rpcconsole.h>
#ifdef ENABLE_VAULT
#include <qt/vaultcontroller.h>
#include <qt/vaultframe.h>
#endif
#include <qt/test/util.h>
#include <test/util/setup_common.h>
#include <util/translation.h>
#include <validation.h>

#include <atomic>
#include <thread>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopedPointer>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QString>
#include <QStackedWidget>
#include <QTest>
#include <QTextEdit>
#include <QToolBar>
#include <QToolButton>
#include <QTimer>
#include <QtGlobal>
#include <QtTest/QtTestWidgets>
#include <QtTest/QtTestGui>

namespace {
//! Regex find a string group inside of the console output
QString FindInConsole(const QString& output, const QString& pattern)
{
    const QRegularExpression re(pattern);
    return re.match(output).captured(1);
}

//! Call getblockchaininfo RPC and check first field of JSON output.
void TestRpcCommand(RPCConsole* console)
{
    QTextEdit* messagesWidget = console->findChild<QTextEdit*>("messagesWidget");
    QLineEdit* lineEdit = console->findChild<QLineEdit*>("lineEdit");
    QSignalSpy mw_spy(messagesWidget, &QTextEdit::textChanged);
    QVERIFY(mw_spy.isValid());
    QTest::keyClicks(lineEdit, "getblockchaininfo");
    QTest::keyClick(lineEdit, Qt::Key_Return);
    QVERIFY(mw_spy.wait(1000));
    QCOMPARE(mw_spy.count(), 4);
    const QString output = messagesWidget->toPlainText();
    const QString pattern = QStringLiteral("\"chain\": \"(\\w+)\"");
    QCOMPARE(FindInConsole(output, pattern), QString("sandbox"));
}

void TestPrimaryNavigation(QuicksilverGUI* window)
{
    struct ExpectedAction {
        const char* object_name;
        QString text;
        QString shortcut;
        QString accent;
    };

    const ExpectedAction expected[] = {
        {"homeBootstrapAction", QStringLiteral("Home"), QStringLiteral("Alt+1"), QStringLiteral("cinnabar")},
        {"sendCoinsAction", QStringLiteral("Transfer"), QStringLiteral("Alt+2"), QStringLiteral("amber")},
        {"receiveCoinsAction", QStringLiteral("Request"), QStringLiteral("Alt+3"), QStringLiteral("teal")},
        {"historyAction", QStringLiteral("Ledger"), QStringLiteral("Alt+4"), QStringLiteral("silver")},
        {"agentAllotmentAction", QStringLiteral("Agents"), QStringLiteral("Alt+5"), QStringLiteral("violet")},
        {"mineMintAction", QStringLiteral("Mine / Mint"), QStringLiteral("Alt+6"), QStringLiteral("amber")},
        {"networkAction", QStringLiteral("Network"), QStringLiteral("Alt+7"), QStringLiteral("teal")},
    };

    QToolBar* rail = window->findChild<QToolBar*>(QStringLiteral("primaryCommandRail"));
    QVERIFY(rail);
    for (const auto& item : expected) {
        QAction* action = window->findChild<QAction*>(QString::fromLatin1(item.object_name));
        QVERIFY2(action, item.object_name);
        QString text = action->text();
        text.remove(QLatin1Char('&'));
        QCOMPARE(text, item.text);
        QCOMPARE(action->shortcut().toString(QKeySequence::PortableText), item.shortcut);
        QVERIFY(action->isCheckable());
        QWidget* button = rail->widgetForAction(action);
        QVERIFY(button);
        QCOMPARE(button->property("accent").toString(), item.accent);
    }

    QAction* mine_mint_action = window->findChild<QAction*>(QStringLiteral("mineMintAction"));
    QVERIFY(mine_mint_action);

    QAction* network_action = window->findChild<QAction*>(QStringLiteral("networkAction"));
    QVERIFY(network_action);
    network_action->setEnabled(true);
    network_action->trigger();
    QVERIFY(network_action->isChecked());
}

void TestHudMenuBar(QuicksilverGUI* window)
{
    struct ExpectedMenu {
        const char* object_name;
        QString text;
    };

    const ExpectedMenu expected[] = {
        {"vaultMenu", QStringLiteral("Vault")},
        {"controlsMenu", QStringLiteral("Controls")},
        {"panelsMenu", QStringLiteral("Panels")},
        {"signalMenu", QStringLiteral("Signal")},
    };

    for (const auto& item : expected) {
        QMenu* menu = window->menuBar()->findChild<QMenu*>(QString::fromLatin1(item.object_name));
        QVERIFY2(menu, item.object_name);
        QString text = menu->title();
        text.remove(QLatin1Char('&'));
        QCOMPARE(text, item.text);
    }
}

void TestModernShell(QuicksilverGUI* window)
{
    // Exercise the assembled main window, including the command rail, status bar,
    // every VaultFrame page, and window chrome allowance. The old 900x650 test
    // constructed only an AgentAllotmentPage in a bare stack and could pass while
    // QuicksilverGUI itself demanded a 1031 px client height.
    QScrollArea* content_scroll = window->findChild<QScrollArea*>(QStringLiteral("mainContentScrollArea"));
    QVERIFY(content_scroll);
    QCOMPARE(content_scroll->verticalScrollBarPolicy(), Qt::ScrollBarAsNeeded);
    constexpr int WORKAREA_1080P{1047};
    constexpr int TITLEBAR_ALLOWANCE{32};
    const int max_client_height{WORKAREA_1080P - TITLEBAR_ALLOWANCE};
    const QSize assembled_minimum = window->minimumSizeHint();
    QVERIFY2(assembled_minimum.height() <= max_client_height,
             qPrintable(QStringLiteral("Assembled window minimum is %1x%2 px; 1080p workarea permits %3 px of client height")
                            .arg(assembled_minimum.width()).arg(assembled_minimum.height()).arg(max_client_height)));

    // F-113 was observed on the untouched Windows geometry: Qt chose roughly
    // 717x565 and placed the Mining card outside the visible Home page. On a
    // 1080p-class work area, a first launch now has a deliberate 1200x800 size
    // and the complete Home page fits without either scrollbar. Smaller test
    // screens still exercise the central as-needed scroll area.
    const QSize available_screen = QGuiApplication::primaryScreen()->availableGeometry().size();
    if (available_screen.width() >= 1200 && available_screen.height() >= 800) {
        QCOMPARE(window->size(), QSize(1200, 800));
        QAction* home_action = window->findChild<QAction*>(QStringLiteral("homeBootstrapAction"));
        QVERIFY(home_action);
        home_action->trigger();
        QTRY_COMPARE(content_scroll->horizontalScrollBar()->maximum(), 0);
        QTRY_COMPARE(content_scroll->verticalScrollBar()->maximum(), 0);
    }

    QToolBar* rail = window->findChild<QToolBar*>(QStringLiteral("primaryCommandRail"));
    QVERIFY(rail);
    QCOMPARE(rail->toolButtonStyle(), Qt::ToolButtonTextBesideIcon);

    QLabel* brand = rail->findChild<QLabel*>(QStringLiteral("commandRailBrandTitle"));
    QVERIFY(brand);
    QCOMPARE(brand->text(), QStringLiteral("QUICKSILVER"));
    QVERIFY(!rail->findChild<QLabel*>(QStringLiteral("commandRailBrandSubtitle")));

    QAction* request_action = window->findChild<QAction*>(QStringLiteral("receiveCoinsAction"));
    QAction* agent_action = window->findChild<QAction*>(QStringLiteral("agentAllotmentAction"));
    QVERIFY(request_action);
    QVERIFY(agent_action);
    const QImage request_icon = request_action->icon().pixmap(QSize(22, 22)).toImage();
    const QImage agent_icon = agent_action->icon().pixmap(QSize(22, 22)).toImage();
    QVERIFY(request_icon.createAlphaMask() != agent_icon.createAlphaMask());

    QFrame* empty_state = window->findChild<QFrame*>(QStringLiteral("noVaultState"));
    QVERIFY(empty_state);
    QVERIFY(empty_state->findChild<QLabel*>(QStringLiteral("emptyVaultTitle")));
    QPushButton* empty_open = empty_state->findChild<QPushButton*>(QStringLiteral("emptyVaultOpenButton"));
    QPushButton* empty_create = empty_state->findChild<QPushButton*>(QStringLiteral("emptyVaultCreateButton"));
    QVERIFY(empty_create);
    QVERIFY(empty_open);
    QVERIFY(empty_create->isEnabled());
    QVERIFY(empty_open->isEnabled());
    QCOMPARE(empty_open->text(), QStringLiteral("Open existing"));
    QCOMPARE(empty_open->property("class").toString(), QStringLiteral("secondaryActionButton"));

    QWidget* launch = window->findChild<QWidget*>(QStringLiteral("desktopLaunchPage"));
    QVERIFY(launch);
    QCOMPARE(launch->findChild<QLabel*>(QStringLiteral("desktopLaunchTitle"))->text(), QStringLiteral("Quicksilver"));
    QVERIFY(launch->findChild<QFrame*>(QStringLiteral("launchVaultCard")));
    QPushButton* launch_vault_button = launch->findChild<QPushButton*>(QStringLiteral("launchVaultCardButton"));
    QVERIFY(launch_vault_button);
    QCOMPARE(launch_vault_button->text(), QStringLiteral("Create or open vault"));
    QVERIFY(launch_vault_button->isEnabled());
    QPushButton* launch_privacy = launch->findChild<QPushButton*>(QStringLiteral("launchVaultBalancePrivacyButton"));
    QVERIFY(launch_privacy);
    QVERIFY(launch_privacy->isHidden());
    QVERIFY(launch->findChild<QFrame*>(QStringLiteral("launchConsensusCard")));
    QVERIFY(launch->findChild<QFrame*>(QStringLiteral("launchMiningCard")));
    QLabel* launch_cost = launch->findChild<QLabel*>(QStringLiteral("desktopLaunchCostCopy"));
    QVERIFY(launch_cost);
    // Sandbox publishes zero for both assumed sizes (chainparams.cpp), and this suite runs
    // on sandbox, so a screen reading its figures from the active chain says "0 GB per
    // year" here. That is the assertion: no hardcoded literal can render 0 on sandbox and
    // 154 on mainnet, so this pins the wiring in VaultFrame that passes Params() through.
    // The figures a real user reads are asserted in StorageCostsTests, at 128/26 injected.
    QVERIFY(launch_cost->text().contains(QStringLiteral("2 GB recent-block window")));
    QVERIFY(launch_cost->text().contains(QStringLiteral("0 GB per year")));
    QFrame* backup_panel = launch->findChild<QFrame*>(QStringLiteral("desktopLaunchBackupPanel"));
    QVERIFY(backup_panel);
    QVERIFY(backup_panel->isHidden());
    QPushButton* mining_button = launch->findChild<QPushButton*>(QStringLiteral("launchMiningCardButton"));
    QVERIFY(mining_button);
    QVERIFY(!mining_button->isEnabled());
    QLabel* mining_icon = launch->findChild<QLabel*>(QStringLiteral("launchMiningCardIcon"));
    QVERIFY(mining_icon);
    const QPixmap locked_mining_pixmap = mining_icon->pixmap(Qt::ReturnByValue);
    QVERIFY(!locked_mining_pixmap.isNull());
    const QImage locked_mining_icon = locked_mining_pixmap.toImage();

    QStackedWidget* vault_stack = window->findChild<QStackedWidget*>(QStringLiteral("vaultFrameStack"));
    QVERIFY(vault_stack);
    VaultFrame* vault_frame = window->findChild<VaultFrame*>(QStringLiteral("vaultFrame"));
    QVERIFY(vault_frame);
    vault_frame->gotoNetworkPage();
    QCOMPARE(vault_stack->currentWidget()->objectName(), QStringLiteral("consensusReviewPage"));
    QWidget* consensus_review = window->findChild<QWidget*>(QStringLiteral("consensusReviewPage"));
    QVERIFY(consensus_review);
    QCOMPARE(consensus_review->findChild<QLabel*>(QStringLiteral("consensusReviewTitle"))->text(), QStringLiteral("Consensus is optional"));
    QLabel* consensus_storage = consensus_review->findChild<QLabel*>(QStringLiteral("consensusStorageCostValue"));
    QVERIFY(consensus_storage);
    // Sandbox figures, for the reason given at the launch-page assertions above.
    QVERIFY(consensus_storage->text().contains(QStringLiteral("2 GB recent-block window")));
    QVERIFY(consensus_storage->text().contains(QStringLiteral("0 GB per year")));
    QPushButton* consensus_decline = consensus_review->findChild<QPushButton*>(QStringLiteral("consensusDeclineButton"));
    QVERIFY(consensus_decline);
    QPushButton* consensus_continue = consensus_review->findChild<QPushButton*>(QStringLiteral("consensusContinueButton"));
    QVERIFY(consensus_continue);
    QCOMPARE(consensus_continue->text(), QStringLiteral("Enable consensus"));
    QCOMPARE(consensus_decline->property("class").toString(), QStringLiteral("primaryActionButton"));
    QCOMPARE(consensus_continue->property("class").toString(), QStringLiteral("secondaryActionButton"));
    consensus_continue->click();
    QCOMPARE(vault_stack->currentWidget()->objectName(), QStringLiteral("networkPage"));
    QCOMPARE(launch->findChild<QLabel*>(QStringLiteral("launchConsensusCardState"))->text(), QStringLiteral("Enabled"));
    QCOMPARE(mining_button->text(), QStringLiteral("Open mining"));
    QVERIFY(mining_button->isEnabled());
    const QPixmap available_mining_pixmap = mining_icon->pixmap(Qt::ReturnByValue);
    QVERIFY(!available_mining_pixmap.isNull());
    QVERIFY(available_mining_pixmap.toImage() != locked_mining_icon);
    QCOMPARE(consensus_decline->property("class").toString(), QStringLiteral("secondaryActionButton"));
    QCOMPARE(consensus_continue->property("class").toString(), QStringLiteral("primaryActionButton"));
    mining_button->click();
    QCOMPARE(vault_stack->currentWidget()->objectName(), QStringLiteral("mineMintPage"));
    QPushButton* new_address_button = vault_stack->currentWidget()->findChild<QPushButton*>(QStringLiteral("newAddressButton"));
    QVERIFY(new_address_button);
    QVERIFY(!new_address_button->isEnabled());
}

bool ClickNetworkRestartDialogButton(const QString& button_text, QString* text, QString* informative_text)
{
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (!widget->inherits("QMessageBox")) continue;
        QMessageBox* box = qobject_cast<QMessageBox*>(widget);
        if (!box || box->windowTitle() != QStringLiteral("Restart network context")) continue;
        if (text) *text = box->text();
        if (informative_text) *informative_text = box->informativeText();
        for (QAbstractButton* button : box->buttons()) {
            QString normalized = button->text();
            normalized.remove(QLatin1Char('&'));
            if (normalized == button_text) {
                button->click();
                return true;
            }
        }
    }
    return false;
}

void TestNetworkRestartConfirmation(QuicksilverGUI& window, QWidget& network_page)
{
    QSignalSpy restart_spy(&window, &QuicksilverGUI::networkRestartRequested);
    QVERIFY(restart_spy.isValid());

    QPushButton* publictest_restart = network_page.findChild<QPushButton*>(QStringLiteral("developerNetworkPublicTestCardButton"));
    QVERIFY(publictest_restart);
    QVERIFY(publictest_restart->isEnabled());

    QString text;
    QString informative_text;
    ExpectModalWithoutNestedEventLoop("QMessageBox", [&] {
        publictest_restart->click();
    });
    QCOMPARE(restart_spy.count(), 0);

    bool clicked = false;
    QTimer::singleShot(0, [&] {
        clicked = ClickNetworkRestartDialogButton(QStringLiteral("Restart"), &text, &informative_text);
    });
    publictest_restart->click();
    QTRY_VERIFY(clicked);
    QTRY_COMPARE(restart_spy.count(), 1);
    QCOMPARE(restart_spy.takeFirst().at(0).toString(), QStringLiteral("publictest"));
    QCOMPARE(text, QStringLiteral("Restart Quicksilver with -chain=publictest?"));
    QVERIFY(informative_text.contains(QStringLiteral("chain parameters")));
    QVERIFY(informative_text.contains(QStringLiteral("live vault is unaffected")));
    QVERIFY(informative_text.contains(QStringLiteral("datadir")));
}

//! The sync warning must follow known work, not the clock.
//!
//! The first node on a new chain sits on a genesis block whose timestamp is the
//! launch stamp, which can be days old before anybody downloads the software.
//! The inherited rule -- a tip older than MAX_BLOCK_TIME_GAP means you are behind
//! -- was written for the chain this one forked from, running since 2009, and it
//! produces the wrong answer here: it warns the one operator on the chain that their balance
//! might be wrong, over a table reading 100% complete. `count` at the tip with
//! no better header known means there is nothing to catch up on, whatever the
//! stamp says. The second half drives the other direction so the fix cannot be
//! "never warn".
void TestSyncWarningFollowsKnownHeaders(QuicksilverGUI* window)
{
    ModalOverlay* overlay = window->findChild<ModalOverlay*>();
    QVERIFY(overlay);
    QVERIFY(!overlay->isLayerVisible());

    const QDateTime launch_stamp{QDateTime::currentDateTime().addDays(-2)};
    window->setNumBlocks(0, launch_stamp, 1.0, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    QVERIFY(!overlay->isLayerVisible());

    // A header chain better than the tip is exactly the condition the node's own
    // MiningShouldWaitForSync waits on, and it must still raise the overlay.
    overlay->setKnownBestHeight(500, QDateTime::currentDateTime(), /*presync=*/false);
    window->setNumBlocks(0, launch_stamp, 0.5, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    QVERIFY(overlay->isLayerVisible());

    window->setNumBlocks(500, QDateTime::currentDateTime(), 1.0, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    QVERIFY(!overlay->isLayerVisible());
}

void TestLazyConsensusActivation(interfaces::Node& node, const NetworkStyle* network_style)
{
    QSettings().setValue(QStringLiteral("Desktop/ConsensusEnabled"), false);
    QScopedPointer<const PlatformStyle> platform_style(PlatformStyle::instantiate(QuicksilverGUI::DEFAULT_UIPLATFORM.c_str()));
    QuicksilverGUI window(node, platform_style.data(), network_style);

    QSignalSpy consensus_spy(&window, &QuicksilverGUI::consensusActivationRequested);
    QVERIFY(consensus_spy.isValid());

    QWidget* initial_launch = window.findChild<QWidget*>(QStringLiteral("desktopLaunchPage"));
    QVERIFY(initial_launch);
    QCOMPARE(initial_launch->findChild<QLabel*>(QStringLiteral("launchVaultCardState"))->text(), QStringLiteral("Starting"));
    QPushButton* initial_vault_button = initial_launch->findChild<QPushButton*>(QStringLiteral("launchVaultCardButton"));
    QVERIFY(initial_vault_button);
    QCOMPARE(initial_vault_button->text(), QStringLiteral("Vault runtime starting"));
    QVERIFY(!initial_vault_button->isEnabled());
    QVERIFY(initial_launch->findChild<QLabel*>(QStringLiteral("launchVaultCardBody"))->text().contains(QStringLiteral("startup attaches the vault runtime")));

    QAction* mine_mint_action = window.findChild<QAction*>(QStringLiteral("mineMintAction"));
    QVERIFY(mine_mint_action);
    QVERIFY(mine_mint_action->isEnabled());
    mine_mint_action->trigger();
    QStackedWidget* vault_stack = window.findChild<QStackedWidget*>(QStringLiteral("vaultFrameStack"));
    QVERIFY(vault_stack);
    QCOMPARE(vault_stack->currentWidget()->objectName(), QStringLiteral("consensusReviewPage"));

    QAction* network_action = window.findChild<QAction*>(QStringLiteral("networkAction"));
    QVERIFY(network_action);
    network_action->setEnabled(true);
    network_action->trigger();

    QWidget* consensus_review = window.findChild<QWidget*>(QStringLiteral("consensusReviewPage"));
    QVERIFY(consensus_review);
    QPushButton* consensus_continue = consensus_review->findChild<QPushButton*>(QStringLiteral("consensusContinueButton"));
    QVERIFY(consensus_continue);
    consensus_continue->click();
    QCOMPARE(consensus_spy.count(), 1);
    QWidget* network_page = window.findChild<QWidget*>(QStringLiteral("networkPage"));
    QVERIFY(network_page);
    QCOMPARE(network_page->findChild<QLabel*>(QStringLiteral("networkStatusValue"))->text(), QStringLiteral("Starting consensus"));
    QCOMPARE(network_page->findChild<QLabel*>(QStringLiteral("syncStatusValue"))->text(), QStringLiteral("Waiting for consensus"));
    QVERIFY(QSettings().value(QStringLiteral("Desktop/ConsensusEnabled")).toBool());

    TestNetworkRestartConfirmation(window, *network_page);

    consensus_continue->click();
    QCOMPARE(consensus_spy.count(), 1);
    window.markConsensusInitializationFailed();
    QCOMPARE(consensus_spy.count(), 1);
    QCOMPARE(network_page->findChild<QLabel*>(QStringLiteral("networkStatusValue"))->text(), QStringLiteral("Startup failed"));
    QCOMPARE(network_page->findChild<QLabel*>(QStringLiteral("syncStatusValue"))->text(), QStringLiteral("Not running"));
    QCOMPARE(QSettings().value(QStringLiteral("Desktop/ConsensusEnabled")).toBool(), false);
    QWidget* launch = window.findChild<QWidget*>(QStringLiteral("desktopLaunchPage"));
    QVERIFY(launch);
    QCOMPARE(launch->findChild<QLabel*>(QStringLiteral("launchConsensusCardState"))->text(), QStringLiteral("Restart needed"));
    QCOMPARE(launch->findChild<QPushButton*>(QStringLiteral("launchConsensusCardButton"))->text(), QStringLiteral("View issue"));
    QPushButton* mining_button = launch->findChild<QPushButton*>(QStringLiteral("launchMiningCardButton"));
    QVERIFY(mining_button);
    QCOMPARE(mining_button->text(), QStringLiteral("Locked"));
    QVERIFY(!mining_button->isEnabled());
    network_action->trigger();
    QCOMPARE(window.findChild<QStackedWidget*>(QStringLiteral("vaultFrameStack"))->currentWidget()->objectName(), QStringLiteral("consensusReviewPage"));
    QCOMPARE(consensus_review->findChild<QLabel*>(QStringLiteral("consensusReviewTitle"))->text(), QStringLiteral("Consensus restart required"));
    QVERIFY(!consensus_continue->isEnabled());
    consensus_continue->click();
    QCOMPARE(consensus_spy.count(), 1);
    QVERIFY(mine_mint_action);
    QVERIFY(mine_mint_action->isEnabled());
    mine_mint_action->trigger();
    QCOMPARE(window.findChild<QStackedWidget*>(QStringLiteral("vaultFrameStack"))->currentWidget()->objectName(), QStringLiteral("consensusReviewPage"));
    QSettings().setValue(QStringLiteral("Desktop/ConsensusEnabled"), false);
}
} // namespace

void AppTests::restartArgumentsForDeveloperNetwork()
{
    const QStringList original_arguments{
        QStringLiteral("-min"),
        QStringLiteral("-chain=sandbox"),
        QStringLiteral("--publictest=1"),
        QStringLiteral("-txpownocycle=1"),
        QStringLiteral("-testactivationheight=segwit@10"),
        QStringLiteral("-cuckatoosolver=/opt/quicksilver/qsgpusolve"),
        QStringLiteral("-datadir=/home/user/.quicksilver"),
        QStringLiteral("quicksilver:ignored-on-restart"),
    };

    QCOMPARE(QuicksilverApplication::restartArgumentsForChainForTesting(original_arguments, QStringLiteral("publictest")),
        QStringList({
            QStringLiteral("-min"),
            QStringLiteral("-cuckatoosolver=/opt/quicksilver/qsgpusolve"),
            QStringLiteral("-datadir=/home/user/.quicksilver"),
            QStringLiteral("-chain=publictest"),
        }));

    QCOMPARE(QuicksilverApplication::restartArgumentsForChainForTesting(original_arguments, QStringLiteral("sandbox")),
        QStringList({
            QStringLiteral("-min"),
            QStringLiteral("-txpownocycle=1"),
            QStringLiteral("-testactivationheight=segwit@10"),
            QStringLiteral("-cuckatoosolver=/opt/quicksilver/qsgpusolve"),
            QStringLiteral("-datadir=/home/user/.quicksilver"),
            QStringLiteral("-chain=sandbox"),
        }));
}

//! Entry point for QuicksilverApplication tests.
void AppTests::appTests()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        // Disable for mac on "minimal" platform to avoid crashes inside the Qt
        // framework when it tries to look up unimplemented cocoa functions,
        // and fails to handle returned nulls
        // (https://bugreports.qt.io/browse/QTBUG-49686).
        qWarning() << "Skipping AppTests on mac build with 'minimal' platform set due to Qt bugs. To run AppTests, invoke "
                      "with 'QT_QPA_PLATFORM=cocoa test_quicksilver-qt' on mac, or else use a linux or windows build.";
        return;
    }
#endif

    qRegisterMetaType<interfaces::BlockAndHeaderTipInfo>("interfaces::BlockAndHeaderTipInfo");
    m_app.parameterSetup();
    QVERIFY(m_app.createOptionsModel(/*resetSettings=*/true));
    QScopedPointer<const NetworkStyle> style(NetworkStyle::instantiate(Params().GetChainType()));
    m_app.setupPlatformStyle();
    TestLazyConsensusActivation(m_app.node(), style.data());
    // Reproduce the returning-user path: persisted consensus must let AppInitMain
    // load settings-listed vaults before the GUI controller is constructed.
    QSettings().setValue(QStringLiteral("Desktop/ConsensusEnabled"), true);
    m_app.createWindow(style.data());
    connect(&m_app, &QuicksilverApplication::windowShown, this, &AppTests::guiTests);
    expectCallback("guiTests");
    QVERIFY(m_app.baseInitialize());
#ifdef ENABLE_VAULT
    QVERIFY(!m_app.findChild<VaultController*>());
#endif
    m_app.requestInitialize();
    m_app.exec();
    m_app.requestShutdown();
    m_app.exec();

    // Reset global state to avoid interfering with later tests.
    LogInstance().DisconnectTestLogger();
}

//! Entry point for QuicksilverGUI tests.
void AppTests::guiTests(QuicksilverGUI* window)
{
    HandleCallback callback{"guiTests", *this};
#ifdef ENABLE_VAULT
    VaultController* app_vault_controller = m_app.findChild<VaultController*>();
    QVERIFY(app_vault_controller);
    QCOMPARE(window->getVaultController(), app_vault_controller);
    VaultFrame* persisted_frame = window->findChild<VaultFrame*>(QStringLiteral("vaultFrame"));
    QVERIFY(persisted_frame);
    QVERIFY(persisted_frame->consensusEnabled());
    QVERIFY(QSettings().value(QStringLiteral("Desktop/ConsensusEnabled")).toBool());
    persisted_frame->revertConsensusOptIn();
#endif
    TestPrimaryNavigation(window);
    TestHudMenuBar(window);
    TestModernShell(window);
    TestSyncWarningFollowsKnownHeaders(window);
    shutdownIsNotParkedBehindAModalDialog(window);
    ExpectModalWithoutNestedEventLoop("QMessageBox", [&] {
        window->message(QString(), QStringLiteral("client modal boom"), CClientUIInterface::MSG_ERROR);
    });
    {
        std::atomic<bool> worker_done{false};
        std::atomic<bool> worker_ok{false};
        std::thread worker([&] {
            worker_ok = uiInterface.ThreadSafeMessageBox(
                Untranslated("from worker"), "", CClientUIInterface::MSG_ERROR);
            worker_done = true;
        });
        QTRY_VERIFY(QApplication::activeModalWidget());
        QVERIFY(!worker_done.load());
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        QVERIFY(box);
        QCOMPARE(box->objectName(), QStringLiteral("clientMessageBox"));
        QAbstractButton* button = box->defaultButton() ? static_cast<QAbstractButton*>(box->defaultButton()) : box->escapeButton();
        QVERIFY(button);
        button->click();
        QTRY_VERIFY(worker_done.load());
        QVERIFY(worker_ok.load());
        worker.join();
    }
    connect(window, &QuicksilverGUI::consoleShown, this, &AppTests::consoleTests);
    expectCallback("consoleTests");
    QAction* action = window->findChild<QAction*>("openRPCConsoleAction");
    action->activate(QAction::Trigger);
}

//! Phase 6 S1-13: a granted shutdown request must survive an open dialog.
//!
//! Found by walking, not by testing: a GUI test node sat for twelve minutes through repeated
//! SIGTERMs and a `stop` RPC that answered "Quicksilver stopping", with a leftover
//! "Transfer proof-of-work canceled" message box on screen. It exited within five
//! seconds of that box being clicked. The poll that turns a node-side shutdown
//! request into a Qt quit skipped every tick while a modal was up, so both routes
//! reported success and did nothing -- and a desktop session ending sends SIGTERM
//! before it sends SIGKILL, which is an unclean kill of a node holding LevelDB and
//! SQLite.
//!
//! The tick is driven directly with the flag the node would have supplied, so the
//! test never actually shuts the node down and the rest of the app tests still
//! have one to talk to.
void AppTests::shutdownIsNotParkedBehindAModalDialog(QuicksilverGUI* window)
{
    QSignalSpy quit_spy(window, &QuicksilverGUI::quitRequested);
    QVERIFY(quit_spy.isValid());

    QMessageBox box(QMessageBox::Critical, QStringLiteral("Transfer Quicksilver"),
                    QStringLiteral("Transfer proof-of-work canceled"), QMessageBox::Ok, window);
    box.setObjectName(QStringLiteral("leftoverModal"));
    box.setModal(true);
    box.show();
    QApplication::processEvents();
    QCOMPARE(QApplication::activeModalWidget(), static_cast<QWidget*>(&box));

    // No shutdown asked for: the dialog is the user's and must be left alone.
    m_app.pollShutdownTick(/*shutdown_requested=*/false);
    QApplication::processEvents();
    QVERIFY(box.isVisible());
    QCOMPARE(quit_spy.count(), 0);

    // Shutdown asked for: the dialog goes, and the quit waits for the next tick so
    // the teardown does not run underneath the dialog's own event loop.
    m_app.pollShutdownTick(/*shutdown_requested=*/true);
    QApplication::processEvents();
    QVERIFY(!box.isVisible());
    QVERIFY(!QApplication::activeModalWidget());
    QCOMPARE(quit_spy.count(), 0);
}

//! Entry point for RPCConsole tests.
void AppTests::consoleTests(RPCConsole* console)
{
    HandleCallback callback{"consoleTests", *this};
    QVERIFY(console->findChild<QFrame*>(QStringLiteral("nodeWindowHeader")));
    QVERIFY(console->findChild<QLabel*>(QStringLiteral("nodeWindowTitle")));
    // H1: this console has a live client model, so the consensus-off banner must be gone.
    // The banner defaults to visible in the form; only setClientModel clears it.
    QLabel* consensus_off = console->findChild<QLabel*>(QStringLiteral("consensusOffBanner"));
    QVERIFY(consensus_off);
    QVERIFY(!consensus_off->isVisibleTo(console));

    TestRpcCommand(console);
}

//! Destructor to shut down after the last expected callback completes.
AppTests::HandleCallback::~HandleCallback()
{
    auto& callbacks = m_app_tests.m_callbacks;
    auto it = callbacks.find(m_callback);
    assert(it != callbacks.end());
    callbacks.erase(it);
    if (callbacks.empty()) {
        m_app_tests.m_app.exit(0);
    }
}
