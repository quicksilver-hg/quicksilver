// Copyright (c) 2018-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/apptests.h>

#include <quicksilver-build-config.h> // IWYU pragma: keep

#include <chainparams.h>
#include <common/args.h>
#include <interfaces/node.h>
#include <key.h>
#include <logging.h>
#include <node/interface_ui.h>
#include <qt/clientmodel.h>
#include <qt/modaloverlay.h>
#include <qt/quicksilver.h>
#include <qt/quicksilvergui.h>
#include <qt/quicksilverstyle.h>
#include <qt/networkstyle.h>
#include <qt/platformstyle.h>
#include <qt/rpcconsole.h>
#ifdef ENABLE_VAULT
#include <qt/vaultcontroller.h>
#include <qt/vaultframe.h>
#endif
#include <qt/test/util.h>
#include <test/util/setup_common.h>
#include <tor/bundled_tor.h>
#include <util/fs.h>
#include <util/translation.h>
#include <validation.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
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
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopedPointer>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStatusBar>
#include <QStatusTipEvent>
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
class MessageBoxWorkerGuard
{
public:
    MessageBoxWorkerGuard(std::thread& worker, const std::atomic<bool>& worker_done) :
        m_worker{worker}, m_worker_done{worker_done}
    {
    }

    ~MessageBoxWorkerGuard()
    {
        if (!m_worker.joinable()) return;

        static constexpr auto TIMEOUT{std::chrono::seconds{30}};
        const auto deadline{std::chrono::steady_clock::now() + TIMEOUT};
        bool box_present{false};
        while (!m_worker_done.load() && std::chrono::steady_clock::now() < deadline) {
            QApplication::processEvents();
            box_present = false;
            for (QWidget* widget : QApplication::allWidgets()) {
                auto* box = qobject_cast<QMessageBox*>(widget);
                if (box && box->objectName() == QStringLiteral("clientMessageBox")) {
                    box_present = true;
                    box->reject();
                }
            }
            if (!m_worker_done.load()) std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }

        if (m_worker_done.load()) {
            m_worker.join();
            return;
        }

        std::fputs("F-382 message-box worker guard timed out after 30 s "
                   "(clientMessageBox present: ", stderr);
        std::fputs(box_present ? "yes" : "no", stderr);
        std::fputs(", worker_done: ", stderr);
        std::fputs(m_worker_done.load() ? "yes" : "no", stderr);
        std::fputs(")\n", stderr);
        std::fflush(stdout);
        std::fflush(stderr);
        std::abort();
    }

private:
    std::thread& m_worker;
    const std::atomic<bool>& m_worker_done;
};

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

void TestAssayBenchChrome(QuicksilverGUI* window)
{
    QToolBar* rail = window->findChild<QToolBar*>(QStringLiteral("primaryCommandRail"));
    QVERIFY(rail);
    QCOMPARE(rail->minimumWidth(), 196);
    QCOMPARE(rail->maximumWidth(), 196);
    QCOMPARE(rail->width(), 196);
    QLabel* brand = rail->findChild<QLabel*>(QStringLiteral("commandRailBrandTitle"));
    QVERIFY(brand);
    QCOMPARE(brand->text(), QStringLiteral("QUICKSILVER"));

    QFrame* top = window->findChild<QFrame*>(QStringLiteral("benchTopBar"));
    QVERIFY(top);
    QVERIFY(qobject_cast<QToolBar*>(top) == nullptr);
    QScrollArea* scroll = window->findChild<QScrollArea*>(QStringLiteral("mainContentScrollArea"));
    QVERIFY(scroll);
    QVERIFY(top->mapTo(window, QPoint(0, top->height())).y() <= scroll->mapTo(window, QPoint(0, 0)).y());

    QLabel* crumb = top->findChild<QLabel*>(QStringLiteral("benchBreadcrumb"));
    QVERIFY(crumb);
    QCOMPARE(crumb->text(), crumb->text().toUpper());
    QVERIFY(crumb->text().contains(QStringLiteral(" / ")));
    QString checked_page;
    const char* action_names[] = {
        "homeBootstrapAction", "sendCoinsAction", "receiveCoinsAction", "historyAction",
        "agentAllotmentAction", "mineMintAction", "networkAction",
    };
    for (const char* name : action_names) {
        QAction* action = window->findChild<QAction*>(QString::fromLatin1(name));
        QVERIFY(action);
        if (!action->isChecked()) continue;
        checked_page = action->text();
        checked_page.remove(QLatin1Char('&'));
        break;
    }
    QVERIFY(!checked_page.isEmpty());
    QVERIFY(crumb->text().endsWith(QStringLiteral(" / ") + checked_page.toUpper()));

    QVERIFY(top->findChild<QLabel*>(QStringLiteral("benchTickerSpendable")));
    QVERIFY(top->findChild<QLabel*>(QStringLiteral("benchTickerPending")));
    QVERIFY(top->findChild<QLabel*>(QStringLiteral("benchTickerMaturing")));
    QVERIFY(top->findChild<QLabel*>(QStringLiteral("benchTickerDelegated")));

    VaultFrame* frame = window->findChild<VaultFrame*>(QStringLiteral("vaultFrame"));
    QVERIFY(frame);
    if (!frame->currentVaultModel()) {
        QLabel* empty = top->findChild<QLabel*>(QStringLiteral("benchTickerEmpty"));
        QVERIFY(empty);
        QCOMPARE(empty->text(), QStringLiteral("No vault open"));
        QVERIFY(!empty->isHidden());
        QWidget* figures = top->findChild<QWidget*>(QStringLiteral("benchTickerFigures"));
        QVERIFY(figures);
        QVERIFY(figures->isHidden());
        QVERIFY(crumb->text().startsWith(QStringLiteral("NO VAULT OPEN / ")));
    }

    QFrame* strip = window->statusBar()->findChild<QFrame*>(QStringLiteral("benchStatusStrip"));
    QVERIFY(strip);
    QLabel* sync = strip->findChild<QLabel*>(QStringLiteral("benchStatusSync"));
    QLabel* height = strip->findChild<QLabel*>(QStringLiteral("benchStatusHeight"));
    QLabel* peers = strip->findChild<QLabel*>(QStringLiteral("benchStatusPeers"));
    QLabel* mining = strip->findChild<QLabel*>(QStringLiteral("benchStatusMining"));
    QLabel* vault = strip->findChild<QLabel*>(QStringLiteral("benchStatusVault"));
    QVERIFY(sync);
    QVERIFY(height);
    QVERIFY(peers);
    QVERIFY(mining);
    QVERIFY(vault);
    // The sync progress bar lives in the strip (Home's node panel has its own).
    QVERIFY(strip->findChild<QProgressBar*>());

    const QString peers_before = peers->text();
    window->setNumConnections(999);
    QCOMPARE(peers->text(), peers_before);
    QVERIFY(!peers->text().contains(QStringLiteral("999")));

    QString mining_state;
    if (QLabel* live = window->findChild<QLabel*>(QStringLiteral("miningStatusValue"))) {
        if (!live->text().isEmpty() && live->text() != QStringLiteral("Not connected")) mining_state = live->text();
    }
    if (mining_state.isEmpty()) {
        if (QLabel* card = window->findChild<QLabel*>(QStringLiteral("launchMiningCardState"))) mining_state = card->text();
    }
    QCOMPARE(mining->text(), mining_state.isEmpty() ? QStringLiteral("Mining locked") : QStringLiteral("Mining %1").arg(mining_state.toLower()));
    if (!frame->currentVaultModel()) QCOMPARE(vault->text(), QStringLiteral("No vault open"));

    ModalOverlay* overlay = window->findChild<ModalOverlay*>();
    QVERIFY(overlay);
    const QDateTime stamp = QDateTime::currentDateTime();
    // Known header height only moves upward. Catch the tip up to a fresh height,
    // then name a taller header, then catch up again so the overlay ends hidden.
    overlay->setKnownBestHeight(700, stamp, /*presync=*/false);
    window->setNumBlocks(700, stamp, 1.0, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    QCOMPARE(sync->text(), QStringLiteral("Synchronized"));
    QCOMPARE(height->text(), QStringLiteral("Height 700"));
    QCOMPARE(sync->property("benchTone").toString(), QStringLiteral("good"));
    QVERIFY(!overlay->isLayerVisible());

    overlay->setKnownBestHeight(740, stamp, /*presync=*/false);
    window->setNumBlocks(700, stamp, 0.5, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    QCOMPARE(sync->text(), QStringLiteral("Catching up"));
    QCOMPARE(height->text(), QStringLiteral("Height 700"));
    QCOMPARE(sync->property("benchTone").toString(), QStringLiteral("plain"));
    QVERIFY(overlay->isLayerVisible());

    window->setNumBlocks(740, stamp, 1.0, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    QCOMPARE(sync->text(), QStringLiteral("Synchronized"));
    QVERIFY(!overlay->isLayerVisible());
}

//! The mining field follows a miner status change on its own.
//!
//! A halt that arrives with no new block, peer, or vault event used to leave the
//! strip on the previous wording, because the field was copied from a page label
//! and only repainted from those other events.
void TestStatusStripFollowsMiningStatus(QuicksilverGUI* window)
{
    VaultFrame* frame = window->findChild<VaultFrame*>(QStringLiteral("vaultFrame"));
    QVERIFY(frame);
    QLabel* mining = window->statusBar()->findChild<QLabel*>(QStringLiteral("benchStatusMining"));
    QVERIFY(mining);
    QLabel* page_state = window->findChild<QLabel*>(QStringLiteral("miningStatusValue"));
    const QString page_before = page_state ? page_state->text() : QString();
    const QString height_before = window->statusBar()->findChild<QLabel*>(QStringLiteral("benchStatusHeight"))->text();
    const QString peers_before = window->statusBar()->findChild<QLabel*>(QStringLiteral("benchStatusPeers"))->text();
    const QString vault_before = window->statusBar()->findChild<QLabel*>(QStringLiteral("benchStatusVault"))->text();

    interfaces::MiningStatus active;
    active.active = true;
    active.block_solving_possible = true;
    frame->setMiningStatus(active);
    QCOMPARE(mining->text(), QStringLiteral("Mining active"));

    interfaces::MiningStatus halted;
    halted.active = true;
    halted.block_solving_possible = false;
    frame->setMiningStatus(halted);
    QCOMPARE(mining->text(), QStringLiteral("Mining halted"));

    // This path does not repaint the mine page, and it does not ride a block,
    // peer, or vault refresh.
    if (page_state) QCOMPARE(page_state->text(), page_before);
    QCOMPARE(window->statusBar()->findChild<QLabel*>(QStringLiteral("benchStatusHeight"))->text(), height_before);
    QCOMPARE(window->statusBar()->findChild<QLabel*>(QStringLiteral("benchStatusPeers"))->text(), peers_before);
    QCOMPARE(window->statusBar()->findChild<QLabel*>(QStringLiteral("benchStatusVault"))->text(), vault_before);
}

//! The rail, top bar and strip as the Assay Bench concept lays them out.
//!
//! The first port recoloured the old chrome and kept its shapes: centred
//! auto-width rail buttons, a breadcrumb beside the ticker, no total and no
//! transfer controls, and a strip any rail hover replaced.
QToolButton* RailButtonFor(QToolBar* rail, QAction* action)
{
    for (QToolButton* button : rail->findChildren<QToolButton*>()) {
        if (button->defaultAction() == action) return button;
    }
    return nullptr;
}

void TestAssayBenchChromeLayout(QuicksilverGUI* window)
{
    // The layout under test is the 1200x800 window; the test platform's
    // screen would otherwise leave the window too short for the rail.
    const QSize restore_size = window->size();
    struct RestoreSize {
        QWidget* window;
        QSize size;
        ~RestoreSize() { window->resize(size); }
    } restore{window, restore_size};
    window->resize(1200, 800);
    QCoreApplication::processEvents();

    QToolBar* rail = window->findChild<QToolBar*>(QStringLiteral("primaryCommandRail"));
    QVERIFY(rail);
    const char* action_names[] = {
        "homeBootstrapAction", "sendCoinsAction", "receiveCoinsAction", "agentAllotmentAction",
        "historyAction", "mineMintAction", "networkAction",
    };
    int previous_top = -1;
    for (const char* name : action_names) {
        QAction* action = window->findChild<QAction*>(QString::fromLatin1(name));
        QVERIFY2(action, name);
        QToolButton* button = RailButtonFor(rail, action);
        QVERIFY2(button, name);
        // A full-width row, in the existing order.
        QVERIFY2(button->width() >= rail->width() - 1,
                 qPrintable(QStringLiteral("%1 is %2 px wide in a %3 px rail").arg(QString::fromLatin1(name)).arg(button->width()).arg(rail->width())));
        QCOMPARE(button->mapTo(rail, QPoint(0, 0)).x(), 0);
        QVERIFY(button->height() >= 36 && button->height() <= 44);
        const int top = button->mapTo(rail, QPoint(0, 0)).y();
        QVERIFY(top > previous_top);
        previous_top = top;
    }

    // The checked row carries the 2 px cinnabar edge; an unchecked row does not.
    QAction* home = window->findChild<QAction*>(QStringLiteral("homeBootstrapAction"));
    QAction* network = window->findChild<QAction*>(QStringLiteral("networkAction"));
    QVERIFY(home);
    QVERIFY(network);
    home->setEnabled(true);
    home->trigger();
    QVERIFY(home->isChecked());
    const auto edge = [](QToolButton* button, int x) {
        const QImage image = button->grab().toImage();
        return image.pixelColor(QPoint(x, button->height() / 2) * image.devicePixelRatio());
    };
    const QColor cinnabar = QuicksilverStyle::Color(QuicksilverStyle::Token::Cinnabar);
    QToolButton* home_button = RailButtonFor(rail, home);
    QToolButton* network_button = RailButtonFor(rail, network);
    QCOMPARE(edge(home_button, 0), cinnabar);
    QCOMPARE(edge(home_button, 1), cinnabar);
    QVERIFY(edge(network_button, 0) != cinnabar);

    // Breadcrumb above the ticker, a total, and the two transfer controls.
    QFrame* top = window->findChild<QFrame*>(QStringLiteral("benchTopBar"));
    QVERIFY(top);
    QLabel* crumb = top->findChild<QLabel*>(QStringLiteral("benchBreadcrumb"));
    QWidget* figures = top->findChild<QWidget*>(QStringLiteral("benchTickerFigures"));
    QLabel* empty = top->findChild<QLabel*>(QStringLiteral("benchTickerEmpty"));
    QVERIFY(crumb);
    QVERIFY(figures);
    QVERIFY(empty);
    QVERIFY(top->findChild<QLabel*>(QStringLiteral("benchTickerTotal")));
    const int crumb_bottom = crumb->mapTo(top, QPoint(0, crumb->height())).y();
    QWidget* ticker = figures->isHidden() ? static_cast<QWidget*>(empty) : figures;
    QVERIFY(!ticker->isHidden());
    QVERIFY(crumb_bottom <= ticker->mapTo(top, QPoint(0, 0)).y());

    // The rail's brand cell and the top bar share one bottom rule.
    QFrame* brand = rail->findChild<QFrame*>(QStringLiteral("commandRailBrand"));
    QVERIFY(brand);
    QCOMPARE(brand->mapTo(window, QPoint(0, brand->height())).y(), top->mapTo(window, QPoint(0, top->height())).y());

    QPushButton* request = top->findChild<QPushButton*>(QStringLiteral("benchRequestButton"));
    QPushButton* transfer = top->findChild<QPushButton*>(QStringLiteral("benchTransferButton"));
    QVERIFY(request);
    QVERIFY(transfer);
    QAction* receive_action = window->findChild<QAction*>(QStringLiteral("receiveCoinsAction"));
    QAction* send_action = window->findChild<QAction*>(QStringLiteral("sendCoinsAction"));
    QVERIFY(receive_action);
    QVERIFY(send_action);
    QCOMPARE(request->isEnabled(), receive_action->isEnabled());
    QCOMPARE(transfer->isEnabled(), send_action->isEnabled());
    const bool receive_was = receive_action->isEnabled();
    const bool send_was = send_action->isEnabled();
    receive_action->setEnabled(true);
    send_action->setEnabled(true);
    QVERIFY(request->isEnabled());
    QVERIFY(transfer->isEnabled());
    request->click();
    QVERIFY(receive_action->isChecked());
    transfer->click();
    QVERIFY(send_action->isChecked());
    home->trigger();
    receive_action->setEnabled(receive_was);
    send_action->setEnabled(send_was);
    QCOMPARE(request->isEnabled(), receive_was);
    QCOMPARE(transfer->isEnabled(), send_was);

    // The strip stays in place while a rail item is hovered; the tip shows
    // beside it instead of replacing it.
    QFrame* strip = window->statusBar()->findChild<QFrame*>(QStringLiteral("benchStatusStrip"));
    QVERIFY(strip);
    QLabel* tip = strip->findChild<QLabel*>(QStringLiteral("benchStatusTip"));
    QVERIFY(tip);
    QVERIFY(strip->isVisible());
    QStatusTipEvent hover(network->statusTip());
    QApplication::sendEvent(network_button, &hover);
    QVERIFY(strip->isVisible());
    QVERIFY(window->statusBar()->currentMessage().isEmpty());
    QCOMPARE(tip->text(), network->statusTip());
    QStatusTipEvent leave{QString()};
    QApplication::sendEvent(network_button, &leave);
    QVERIFY(strip->isVisible());
    QVERIFY(tip->text().isEmpty());

    // A dot before the sync word, toned by the same sync state.
    QLabel* sync = strip->findChild<QLabel*>(QStringLiteral("benchStatusSync"));
    QLabel* dot = strip->findChild<QLabel*>(QStringLiteral("benchStatusDot"));
    QVERIFY(sync);
    QVERIFY(dot);
    QVERIFY(dot->mapTo(strip, QPoint(0, 0)).x() < sync->mapTo(strip, QPoint(0, 0)).x());
    QCOMPARE(dot->property("benchTone").toString(), sync->property("benchTone").toString());
}

void TestAssayBenchHome(QuicksilverGUI* window)
{
    QAction* home = window->findChild<QAction*>(QStringLiteral("homeBootstrapAction"));
    QVERIFY(home);
    home->trigger();

    QStackedWidget* stack = window->findChild<QStackedWidget*>(QStringLiteral("vaultFrameStack"));
    QVERIFY(stack);
    QCOMPARE(stack->currentWidget()->objectName(), QStringLiteral("desktopLaunchPage"));
    QVERIFY(window->findChild<QFrame*>(QStringLiteral("noVaultState")));

    QWidget* launch = stack->currentWidget();
    QLabel* empty = launch->findChild<QLabel*>(QStringLiteral("homeLedgerEmpty"));
    QVERIFY(empty);
    QCOMPARE(empty->text(), QStringLiteral("No vault is open"));
    QVERIFY(!empty->isHidden());
    // The LEDGER panel stays; with no vault it holds the empty line, not rows.
    QWidget* ledger = launch->findChild<QWidget*>(QStringLiteral("homeLedger"));
    QVERIFY(ledger);
    QVERIFY(!ledger->isHidden());
    QVERIFY(ledger->isAncestorOf(empty));
    QWidget* ledger_table = ledger->findChild<QWidget*>(QStringLiteral("homeLedgerTable"));
    QVERIFY(ledger_table);
    QVERIFY(ledger_table->isHidden());

    QFrame* node = launch->findChild<QFrame*>(QStringLiteral("launchConsensusCard"));
    QFrame* mining = launch->findChild<QFrame*>(QStringLiteral("launchMiningCard"));
    QFrame* vault = launch->findChild<QFrame*>(QStringLiteral("launchVaultCard"));
    QVERIFY(node);
    QVERIFY(mining);
    QVERIFY(vault);
    QCOMPARE(node->property("benchPanel").toBool(), true);
    QCOMPARE(mining->property("benchPanel").toBool(), true);
    QCOMPARE(vault->property("benchPanel").toBool(), true);
    QCOMPARE(node->findChild<QLabel*>(QStringLiteral("benchPanelTitle"))->text(), QStringLiteral("NODE"));
    QCOMPARE(mining->findChild<QLabel*>(QStringLiteral("benchPanelTitle"))->text(), QStringLiteral("MINING"));
    QCOMPARE(vault->findChild<QLabel*>(QStringLiteral("benchPanelTitle"))->text(), QStringLiteral("VAULT"));

    QLabel* cost = launch->findChild<QLabel*>(QStringLiteral("desktopLaunchCostCopy"));
    QVERIFY(cost);
    QVERIFY(node->isAncestorOf(cost));
    // The Backup row and its command are the Vault panel's; a missing backup's
    // warning heads the Ledger panel, inside it, which can give up rows to make
    // room. The node's warnings are the Node panel's (send-back 2 item 8: no
    // line floats above the panels).
    QVERIFY(vault->isAncestorOf(launch->findChild<QLabel*>(QStringLiteral("desktopLaunchBackupState"))));
    QVERIFY(vault->isAncestorOf(launch->findChild<QPushButton*>(QStringLiteral("desktopLaunchBackupButton"))));
    QWidget* backup_warning = launch->findChild<QWidget*>(QStringLiteral("desktopLaunchBackupPanel"));
    QVERIFY(backup_warning);
    QVERIFY(launch->findChild<QFrame*>(QStringLiteral("homeLedger"))->isAncestorOf(backup_warning));
    QVERIFY(node->isAncestorOf(launch->findChild<QLabel*>(QStringLiteral("homeNodeAlerts"))));
    QVERIFY(vault->isAncestorOf(launch->findChild<QPushButton*>(QStringLiteral("launchVaultBalancePrivacyButton"))));
    QVERIFY(mining->isAncestorOf(launch->findChild<QPushButton*>(QStringLiteral("launchMiningCardButton"))));
    QVERIFY(mining->isAncestorOf(launch->findChild<QLabel*>(QStringLiteral("launchMiningCardState"))));
    // Panel heads carry the title only: no coloured state badge or icon.
    for (QFrame* panel : {node, mining, vault}) {
        QFrame* head = panel->findChild<QFrame*>(QStringLiteral("benchPanelHead"));
        QVERIFY(head);
        QCOMPARE(head->findChildren<QLabel*>().size(), 1);
    }
    QVERIFY(launch->findChild<QWidget*>(QStringLiteral("sparkline")) == nullptr);

    QLabel* node_height = node->findChild<QLabel*>(QStringLiteral("homeNodeHeight"));
    QLabel* node_peers = node->findChild<QLabel*>(QStringLiteral("homeNodePeers"));
    QLabel* node_last = node->findChild<QLabel*>(QStringLiteral("homeNodeLastBlock"));
    QVERIFY(node_height);
    QVERIFY(node_peers);
    QVERIFY(node_last);
    // Label/value rows: the values are the strip's figures without its words.
    const QString none = QStringLiteral("\u2014");
    const QString strip_height = window->findChild<QLabel*>(QStringLiteral("benchStatusHeight"))->text();
    QCOMPARE(strip_height, node_height->text() == none ? QStringLiteral("Height unavailable") : QStringLiteral("Height %1").arg(node_height->text()));
    const QString strip_peers = window->findChild<QLabel*>(QStringLiteral("benchStatusPeers"))->text();
    if (node_peers->text() == none) {
        QCOMPARE(strip_peers, QStringLiteral("No peers"));
    } else {
        QCOMPARE(strip_peers, node_peers->text() == QStringLiteral("1") ? QStringLiteral("1 peer") : QStringLiteral("%1 peers").arg(node_peers->text()));
    }
    QVERIFY(node_last->text() == none || node_last->text().endsWith(QStringLiteral(" ago")));
    QVERIFY(!node_last->text().contains(QStringLiteral("48")));
    // The launch heading and its tagline are gone: the breadcrumb names the page.
    QVERIFY(!launch->findChild<QLabel*>(QStringLiteral("desktopLaunchTitle")));
    QVERIFY(!launch->findChild<QLabel*>(QStringLiteral("desktopLaunchSubtitle")));
    const QString charge_word = QStringLiteral("fee");
    for (QLabel* label : launch->findChildren<QLabel*>()) {
        QVERIFY(!label->text().contains(QStringLiteral("Last 24 blocks")));
        QVERIFY2(!label->text().contains(charge_word, Qt::CaseInsensitive),
                 qPrintable(label->objectName() + QStringLiteral(": ") + label->text()));
    }

    const QSize home_minimum = window->minimumSizeHint();
    QVERIFY2(home_minimum.height() <= 1015,
             qPrintable(QStringLiteral("Home window minimum is %1x%2 px").arg(home_minimum.width()).arg(home_minimum.height())));
    QVERIFY2(home_minimum.width() <= 1200,
             qPrintable(QStringLiteral("Home window minimum is %1x%2 px").arg(home_minimum.width()).arg(home_minimum.height())));
}

void TestPrimaryNavigation(QuicksilverGUI* window)
{
    struct ExpectedAction {
        const char* object_name;
        QString text;
        QString shortcut;
    };

    const ExpectedAction expected[] = {
        {"homeBootstrapAction", QStringLiteral("Home"), QStringLiteral("Alt+1")},
        {"sendCoinsAction", QStringLiteral("Transfer"), QStringLiteral("Alt+2")},
        {"receiveCoinsAction", QStringLiteral("Request"), QStringLiteral("Alt+3")},
        {"historyAction", QStringLiteral("Ledger"), QStringLiteral("Alt+4")},
        {"agentAllotmentAction", QStringLiteral("Agents"), QStringLiteral("Alt+5")},
        {"mineMintAction", QStringLiteral("Mine / Mint"), QStringLiteral("Alt+6")},
        {"networkAction", QStringLiteral("Network"), QStringLiteral("Alt+7")},
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
        // Every destination is a rail row driven by its action. The rows no
        // longer carry per-page accent colours: icons are muted and only the
        // checked row is cinnabar.
        QToolButton* button = RailButtonFor(rail, action);
        QVERIFY(button);
        QVERIFY(!button->property("accent").isValid());
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
        // Owner ruling 2026-10-10: Vault keeps its name; the other three take the
        // names every desktop uses, so their contents are where people look.
        {"vaultMenu", QStringLiteral("Vault")},
        {"settingsMenu", QStringLiteral("Settings")},
        {"windowMenu", QStringLiteral("Window")},
        {"helpMenu", QStringLiteral("Help")},
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
    QVERIFY(!launch->findChild<QLabel*>(QStringLiteral("desktopLaunchTitle")));
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
    QWidget* backup_panel = launch->findChild<QWidget*>(QStringLiteral("desktopLaunchBackupPanel"));
    QVERIFY(backup_panel);
    QVERIFY(backup_panel->isHidden());
    QPushButton* mining_button = launch->findChild<QPushButton*>(QStringLiteral("launchMiningCardButton"));
    QVERIFY(mining_button);
    QVERIFY(!mining_button->isEnabled());
    // The Mining panel's State row says whether mining is open, where a lock
    // icon used to.
    QLabel* mining_state = launch->findChild<QLabel*>(QStringLiteral("launchMiningCardState"));
    QVERIFY(mining_state);
    QCOMPARE(mining_state->text(), QStringLiteral("Locked"));

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
    // Open now, and once the miner has reported, in the strip's own word.
    QVERIFY(mining_state->text() != QStringLiteral("Locked"));
    QLabel* strip_mining = window->statusBar()->findChild<QLabel*>(QStringLiteral("benchStatusMining"));
    QVERIFY(strip_mining);
    QCOMPARE(strip_mining->text(), QStringLiteral("Mining %1").arg(mining_state->text().toLower()));
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

//! F-441: an opt-in that cannot find the Tor it needs is refused before the node
//! starts -- and before the vault handover closes an open vault -- and the
//! desktop stays usable, says why, and lets the user retry without a restart.
void TestConsensusRefusedWithoutTor(interfaces::Node& node, const NetworkStyle* network_style)
{
    QSettings().setValue(QStringLiteral("Desktop/ConsensusEnabled"), false);
    QScopedPointer<const PlatformStyle> platform_style(PlatformStyle::instantiate(QuicksilverGUI::DEFAULT_UIPLATFORM.c_str()));
    {
        // The launch path: a saved opt-in refused at startup, with Home selected.
        // The sidebar must follow the page it is sent to.
        QuicksilverGUI launch_window(node, platform_style.data(), network_style);
        QAction* launch_network_action = launch_window.findChild<QAction*>(QStringLiteral("networkAction"));
        QVERIFY(launch_network_action);
        QVERIFY(!launch_network_action->isChecked());
        launch_window.markConsensusTorMissing();
        QVERIFY(launch_network_action->isChecked());
    }
    QuicksilverGUI window(node, platform_style.data(), network_style);
    bool tor_found{false};
    window.setConsensusPreflight([&tor_found] { return tor_found; });

    QSignalSpy consensus_spy(&window, &QuicksilverGUI::consensusActivationRequested);
    QVERIFY(consensus_spy.isValid());
    QAction* network_action = window.findChild<QAction*>(QStringLiteral("networkAction"));
    QVERIFY(network_action);
    network_action->setEnabled(true);
    network_action->trigger();
    QWidget* consensus_review = window.findChild<QWidget*>(QStringLiteral("consensusReviewPage"));
    QVERIFY(consensus_review);
    QPushButton* consensus_continue = consensus_review->findChild<QPushButton*>(QStringLiteral("consensusContinueButton"));
    QVERIFY(consensus_continue);

    consensus_continue->click();
    QCOMPARE(consensus_spy.count(), 0);
    QCOMPARE(QSettings().value(QStringLiteral("Desktop/ConsensusEnabled")).toBool(), false);
    QStackedWidget* vault_stack = window.findChild<QStackedWidget*>(QStringLiteral("vaultFrameStack"));
    QVERIFY(vault_stack);
    QCOMPARE(vault_stack->currentWidget()->objectName(), QStringLiteral("networkPage"));
    QWidget* network_page = window.findChild<QWidget*>(QStringLiteral("networkPage"));
    QVERIFY(network_page);
    QCOMPARE(network_page->findChild<QLabel*>(QStringLiteral("networkStatusValue"))->text(), QStringLiteral("Startup failed"));
    QCOMPARE(network_page->findChild<QLabel*>(QStringLiteral("syncStatusValue"))->text(), QStringLiteral("Not running"));
    const QString tor_missing_intro = network_page->findChild<QLabel*>(QStringLiteral("networkIntro"))->text();
    // The node's own sentence, so the two never drift, plus what needs a restart:
    // only a tor placed beside the executable is found on the next opt-in.
    QVERIFY(tor_missing_intro.contains(QString::fromStdString(tor::MissingTorMessage().translated)));
    QVERIFY(tor_missing_intro.contains(QStringLiteral("restart")));
    // Not the restart-required state: placing Tor and opting in again is enough.
    QWidget* launch = window.findChild<QWidget*>(QStringLiteral("desktopLaunchPage"));
    QVERIFY(launch);
    QVERIFY(launch->findChild<QLabel*>(QStringLiteral("launchConsensusCardState"))->text() != QStringLiteral("Restart needed"));

    // The page refreshes on navigation; it must keep saying why, not "starting".
    network_action->trigger();
    QCOMPARE(network_page->findChild<QLabel*>(QStringLiteral("networkStatusValue"))->text(), QStringLiteral("Startup failed"));

    tor_found = true;
    window.findChild<QAction*>(QStringLiteral("mineMintAction"))->trigger();
    QCOMPARE(vault_stack->currentWidget()->objectName(), QStringLiteral("consensusReviewPage"));
    consensus_continue->click();
    QCOMPARE(consensus_spy.count(), 1);
    QVERIFY(QSettings().value(QStringLiteral("Desktop/ConsensusEnabled")).toBool());
    QCOMPARE(network_page->findChild<QLabel*>(QStringLiteral("networkStatusValue"))->text(), QStringLiteral("Starting consensus"));
    QSettings().setValue(QStringLiteral("Desktop/ConsensusEnabled"), false);
}
} // namespace

void AppTests::consensusTorPreflight()
{
    int calls{0};
    fs::path asked;
    const auto locator = [&](std::optional<fs::path> answer) {
        return [&calls, &asked, answer](const fs::path& override_path) {
            ++calls;
            asked = override_path;
            return answer;
        };
    };
    const auto args_with = [](const char* bundled, const char* onion) {
        auto args = std::make_unique<ArgsManager>();
        args->ForceSetArg("-bundledtor", bundled);
        args->ForceSetArg("-listenonion", onion);
        return args;
    };

    // Bundled Tor will start and is found: the node may start.
    auto args = args_with("1", "1");
    args->ForceSetArg("-bundledtorpath", "/opt/tor/bin/tor");
    QVERIFY(QuicksilverApplication::consensusTorAvailableForTesting(*args, locator(fs::u8path("/opt/tor/bin/tor"))));
    QCOMPARE(calls, 1);
    // GetPathArg() normalizes, so on Windows the locator is asked for the
    // same path with native separators.
    QCOMPARE(fs::PathToString(asked), fs::PathToString(fs::PathFromString("/opt/tor/bin/tor").lexically_normal()));

    // Bundled Tor will start and none is found: refuse (the F-441 exit).
    calls = 0;
    QVERIFY(!QuicksilverApplication::consensusTorAvailableForTesting(*args_with("1", "1"), locator(std::nullopt)));
    QCOMPARE(calls, 1);

    // A user who runs their own Tor, or turned onion listening off, is never
    // asked for a bundled one.
    calls = 0;
    QVERIFY(QuicksilverApplication::consensusTorAvailableForTesting(*args_with("0", "1"), locator(std::nullopt)));
    QVERIFY(QuicksilverApplication::consensusTorAvailableForTesting(*args_with("1", "0"), locator(std::nullopt)));
    QCOMPARE(calls, 0);

    // At launch the refusal opens the vault instead. With the vault disabled
    // there is nothing to open: let node startup report the missing Tor itself.
    QVERIFY(QuicksilverApplication::launchOpensVaultOnlyForTesting(*args_with("1", "1"), locator(std::nullopt)));
    QVERIFY(!QuicksilverApplication::launchOpensVaultOnlyForTesting(*args_with("1", "1"), locator(fs::u8path("/opt/tor/bin/tor"))));
    auto no_vault = args_with("1", "1");
    no_vault->ForceSetArg("-disablevault", "1");
    QVERIFY(!QuicksilverApplication::launchOpensVaultOnlyForTesting(*no_vault, locator(std::nullopt)));
}

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
    TestConsensusRefusedWithoutTor(m_app.node(), style.data());
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
    TestAssayBenchChrome(window);
    TestAssayBenchChromeLayout(window);
    TestStatusStripFollowsMiningStatus(window);
    TestAssayBenchHome(window);
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
        MessageBoxWorkerGuard worker_guard{worker, worker_done};
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
    // F-442 send-back 2 item 7 (owner ruling): the Node window takes the bench
    // grammar, so its tabs sit in a titled panel and the old header is gone.
    QVERIFY(!console->findChild<QFrame*>(QStringLiteral("nodeWindowHeader")));
    QVERIFY(!console->findChild<QLabel*>(QStringLiteral("nodeWindowTitle")));
    QVERIFY(console->findChild<QFrame*>(QStringLiteral("nodeWindowPanel")));
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
