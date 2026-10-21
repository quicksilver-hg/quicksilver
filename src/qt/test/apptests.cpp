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
#include <qt/quicksilver.h>
#include <qt/quicksilvergui.h>
#include <qt/quicksilverstyle.h>
#include <qt/networkstyle.h>
#include <qt/platformstyle.h>
#include <qt/guiutil.h>
#include <qt/optionsdialog.h>
#include <qt/optionsmodel.h>
#include <qt/qvaluecombobox.h>
#include <qt/quicksilverunits.h>
#include <qt/rpcconsole.h>
#ifdef ENABLE_VAULT
#include <interfaces/vault.h>
#include <qt/vaultcontroller.h>
#include <qt/vaultframe.h>
#include <qt/vaultmodel.h>
#include <qt/vaultview.h>
#include <vault/vaultutil.h>
#endif
#include <qt/test/util.h>
#include <test/util/setup_common.h>
#include <tor/bundled_tor.h>
#include <univalue.h>
#include <util/fs.h>
#include <util/translation.h>
#include <validation.h>

#include <util/chaintype.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <thread>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QFileInfo>
#include <QVBoxLayout>
#include <QRadioButton>
#include <QSpinBox>
#include <QSystemTrayIcon>
#include <QCoreApplication>
#include <QEvent>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopedPointer>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QStatusBar>
#include <QStatusTipEvent>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QStackedWidget>
#include <QTest>
#include <QTextEdit>
#include <QToolBar>
#include <QToolButton>
#include <QTranslator>
#include <QTimer>
#include <QtGlobal>
#include <QtTest/QtTestWidgets>
#include <QtTest/QtTestGui>

namespace {
class SyncStateTranslator final : public QTranslator
{
public:
    QString translate(const char* context, const char* source_text, const char*, int) const override
    {
        const QString source{QString::fromUtf8(source_text)};
        const bool sync_state = source == QStringLiteral("Connecting") ||
            source == QStringLiteral("Waiting for peers") ||
            source == QStringLiteral("Catching up") ||
            source == QStringLiteral("Synchronized");
        if (sync_state && qstrcmp(context, "QuicksilverGUI") != 0) {
            return QStringLiteral("{%1}").arg(source);
        }
        return QStringLiteral("[%1]").arg(source);
    }
};

class TranslatorGuard
{
public:
    explicit TranslatorGuard(QTranslator& translator) : m_translator{translator}
    {
        QCoreApplication::installTranslator(&m_translator);
    }

    ~TranslatorGuard()
    {
        QCoreApplication::removeTranslator(&m_translator);
    }

private:
    QTranslator& m_translator;
};

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
    // One sync indicator: the strip's own word. Home's Node card holds the progress bar.
    QVERIFY(window->statusBar()->findChildren<QProgressBar*>().isEmpty());

    window->setNumConnections(999);
    QCOMPARE(peers->text(), QStringLiteral("999 peers"));
    QVERIFY(peers->text().contains(QStringLiteral("999")));
    window->setNumConnections(0);

    QString mining_state;
    if (QLabel* live = window->findChild<QLabel*>(QStringLiteral("miningStatusValue"))) {
        if (!live->text().isEmpty() && live->text() != QStringLiteral("Not connected")) mining_state = live->text();
    }
    if (mining_state.isEmpty()) {
        if (QLabel* card = window->findChild<QLabel*>(QStringLiteral("launchMiningCardState"))) mining_state = card->text();
    }
    QCOMPARE(mining->text(), mining_state.isEmpty() ? QStringLiteral("Mining locked") : QStringLiteral("Mining %1").arg(mining_state.toLower()));
    if (!frame->currentVaultModel()) QCOMPARE(vault->text(), QStringLiteral("No vault open"));

    const QDateTime stamp = QDateTime::currentDateTime();
    // Known header height only moves upward. Catch the tip up to a fresh height,
    // then name a taller header, then catch up again.
    window->setKnownBestHeight(700, /*presync=*/false);
    window->setNumBlocks(700, stamp, 1.0, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    QCOMPARE(sync->text(), QStringLiteral("Waiting for peers"));
    QVERIFY(!window->isBehindKnownHeaders(700));
    QCOMPARE(height->text(), QStringLiteral("Height 700"));
    QCOMPARE(sync->property("benchTone").toString(), QStringLiteral("plain"));

    window->setKnownBestHeight(740, /*presync=*/false);
    window->setNumBlocks(700, stamp, 0.5, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    QVERIFY(window->isBehindKnownHeaders(700));
    QCOMPARE(height->text(), QStringLiteral("Height 700"));
    QCOMPARE(sync->property("benchTone").toString(), QStringLiteral("plain"));

    window->setNumBlocks(740, stamp, 1.0, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    QCOMPARE(sync->text(), QStringLiteral("Waiting for peers"));
    QVERIFY(!window->isBehindKnownHeaders(740));
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

    // The checked row carries the 2 px vermilion edge; an unchecked row does not.
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
    const QColor vermilion = QuicksilverStyle::Color(QuicksilverStyle::Token::Vermilion);
    QToolButton* home_button = RailButtonFor(rail, home);
    QToolButton* network_button = RailButtonFor(rail, network);
    QCOMPARE(edge(home_button, 0), vermilion);
    QCOMPARE(edge(home_button, 1), vermilion);
    QVERIFY(edge(network_button, 0) != vermilion);

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
        // checked row is vermilion.
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
    // F-500: Network opens its own page. This block still reviews consensus,
    // through the route Home and Mine use when consensus is off.
    vault_frame->gotoConsensusReviewPage();
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
        if (!box || box->windowTitle() != QStringLiteral("Switch network")) continue;
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
        clicked = ClickNetworkRestartDialogButton(QStringLiteral("Switch and restart"), &text, &informative_text);
    });
    publictest_restart->click();
    QTRY_VERIFY(clicked);
    QTRY_COMPARE(restart_spy.count(), 1);
    QCOMPARE(restart_spy.takeFirst().at(0).toString(), QStringLiteral("publictest"));
    // F-477: the confirmation names the network, not the -chain token.
    QCOMPARE(text, QStringLiteral("Switch to Public Test?"));
    QCOMPARE(informative_text, QStringLiteral("Quicksilver will close and reopen on Public Test. Each network keeps its own data folder, so your main-network vault and balances are not affected."));
    // The data folder changes with the network: said in words, not as "datadir".
    QVERIFY(informative_text.contains(QStringLiteral("data folder")));
    QVERIFY(!informative_text.contains(QStringLiteral("datadir")));
}

//! Sync state follows known work, not the clock.
//!
//! The first node on a new chain sits on a genesis block whose timestamp is the
//! launch stamp, which can be days old before anybody downloads the software.
//! A tip older than ninety minutes does not imply known work to catch up on:
//! the tip with no better header known has nothing left to connect, whatever
//! the stamp says. The second half drives a known header ahead so the fix
//! cannot become "never behind".
void TestSyncStateFollowsKnownHeaders(QuicksilverGUI* window)
{
    const QDateTime launch_stamp{QDateTime::currentDateTime().addDays(-2)};
    window->setNumBlocks(0, launch_stamp, 1.0, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    QVERIFY(!window->isBehindKnownHeaders(0));

    // A header chain better than the tip is exactly the condition the node's own
    // MiningShouldWaitForSync waits on, and it must still mean known work ahead.
    window->setKnownBestHeight(500, /*presync=*/false);
    window->setNumBlocks(0, launch_stamp, 0.5, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    QVERIFY(window->isBehindKnownHeaders(0));

    window->setNumBlocks(500, QDateTime::currentDateTime(), 1.0, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    QVERIFY(!window->isBehindKnownHeaders(500));

    window->setKnownBestHeight(900, /*presync=*/true);
    QVERIFY(!window->isBehindKnownHeaders(500));
    window->setKnownBestHeight(100, /*presync=*/false);
    QVERIFY(window->isBehindKnownHeaders(400));

    // Header events must reach the tracker without becoming the connected tip.
    QLabel* height = window->statusBar()->findChild<QLabel*>(QStringLiteral("benchStatusHeight"));
    QVERIFY(height);
    const QDateTime stamp = QDateTime::currentDateTime();
    window->setNumBlocks(600, stamp, 0.5, SyncType::HEADER_SYNC, SynchronizationState::POST_INIT);
    QVERIFY(window->isBehindKnownHeaders(500));
    QCOMPARE(height->text(), QStringLiteral("Height 500"));
    window->setNumBlocks(900, stamp, 0.5, SyncType::HEADER_PRESYNC, SynchronizationState::POST_INIT);
    QVERIFY(!window->isBehindKnownHeaders(600));
    QCOMPARE(height->text(), QStringLiteral("Height 500"));
    window->setNumBlocks(100, stamp, 0.5, SyncType::HEADER_SYNC, SynchronizationState::POST_INIT);
    QVERIFY(window->isBehindKnownHeaders(500));
    window->setNumBlocks(600, stamp, 1.0, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    QVERIFY(!window->isBehindKnownHeaders(600));
    QVERIFY(window->isReportedSynchronized());

    // A new header must not flicker the reported state before a block connects.
    window->setNumBlocks(601, stamp, 1.0, SyncType::HEADER_SYNC, SynchronizationState::POST_INIT);
    QVERIFY(window->isBehindKnownHeaders(600));
    QVERIFY(window->isReportedSynchronized());
    window->setNumBlocks(600, stamp, 1.0, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    QVERIFY(!window->isReportedSynchronized());
    window->setNumBlocks(601, stamp, 1.0, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    QVERIFY(!window->isBehindKnownHeaders(601));
    QVERIFY(window->isReportedSynchronized());
}

//! Sync wording follows reported peers and known work, including before a peer count arrives.
void TestSyncStateWords()
{
    using SyncState = ClientModel::SyncState;
    QCOMPARE(ClientModel::syncState(-1, 3, 3, false), SyncState::Connecting);
    QCOMPARE(ClientModel::syncState(700, 0, 0, false), SyncState::WaitingForPeers);
    QCOMPARE(ClientModel::syncState(700, 0, 0, true), SyncState::WaitingForPeers);
    QCOMPARE(ClientModel::syncState(700, 3, 3, true), SyncState::CatchingUp);
    QCOMPARE(ClientModel::syncState(700, 3, 3, false), SyncState::Synchronized);
    QCOMPARE(ClientModel::syncState(700, -1, -1, false), SyncState::Synchronized);
    QCOMPARE(ClientModel::syncState(700, 3, 0, false), SyncState::Connecting);
    QCOMPARE(ClientModel::syncState(700, 3, 0, true), SyncState::Connecting);
    QCOMPARE(ClientModel::syncState(700, 3, -1, false), SyncState::Synchronized);
}

//! Decisions use the sync value even when each display context translates the words differently.
void TestTranslatedSyncStateUsesValue(interfaces::Node& node, OptionsModel* options_model, const NetworkStyle* network_style)
{
    QScopedPointer<const PlatformStyle> platform_style(PlatformStyle::instantiate(QuicksilverGUI::DEFAULT_UIPLATFORM.c_str()));
    ClientModel client_model(node, options_model);
    QuicksilverGUI window(node, platform_style.data(), network_style);
    struct DetachClient {
        QuicksilverGUI& window;
        ~DetachClient() { window.setClientModel(nullptr); }
    } detach{window};
    const QDateTime stale = QDateTime::currentDateTime().addSecs(-9 * 60 * 60);
    interfaces::BlockAndHeaderTipInfo tip{1000, stale.toSecsSinceEpoch(), 1000, stale.toSecsSinceEpoch(), 1.0};
    window.setClientModel(&client_model, &tip);

    SyncStateTranslator translator;
    const TranslatorGuard guard{translator};

    auto* sync = window.findChild<QLabel*>(QStringLiteral("benchStatusSync"));
    auto* node_state = window.findChild<QLabel*>(QStringLiteral("homeNodeState"));
    auto* progress = window.findChild<QProgressBar*>(QStringLiteral("homeNodeSync"));
    auto* network_sync = window.findChild<QLabel*>(QStringLiteral("syncStatusValue"));
    QVERIFY(sync && node_state && progress && network_sync);

    window.setNumConnections(1);
    window.setHeadersAnsweredPeers(1);
    window.setNumBlocks(1000, stale, 1.0, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    QCOMPARE(sync->text(), QStringLiteral("[Synchronized]"));
    QCOMPARE(sync->property("benchTone").toString(), QStringLiteral("good"));
    QCOMPARE(progress->value(), 1000);
    QCOMPARE(node_state->property("benchTone").toString(), QStringLiteral("good"));

    window.setNumBlocks(1000, stale, 1.0, SyncType::HEADER_SYNC, SynchronizationState::POST_INIT);
    window.setNumBlocks(987, stale, 0.5, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    QCOMPARE(sync->text(), QStringLiteral("[Catching up]"));
    QCOMPARE(progress->value(), 987);
    QCOMPARE(network_sync->text(), QStringLiteral("[[Catching up] 98.7%]"));

    window.setNumConnections(0);
    QCOMPARE(node_state->text(), QStringLiteral("[No peers]"));
    QCOMPARE(node_state->property("benchTone").toString(), QStringLiteral("plain"));

    window.setNumConnections(1);
    window.setNumBlocks(1000, stale, 1.0, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    QCOMPARE(node_state->property("benchTone").toString(), QStringLiteral("good"));
}

//! A node that has heard from no peer cannot know it is in sync.
//!
//! "Catching up" is decided by known work (a header ahead of the tip), not by
//! the tip's age. With no peer nothing can be known, so the absence of a
//! known header ahead says nothing, and the strip must say so instead of
//! calling the node synchronized.
void TestStripWithoutPeersIsNotSynchronized(QuicksilverGUI* window)
{
    QLabel* sync = window->statusBar()->findChild<QLabel*>(QStringLiteral("benchStatusSync"));
    QLabel* peers = window->statusBar()->findChild<QLabel*>(QStringLiteral("benchStatusPeers"));
    QVERIFY(sync);
    QVERIFY(peers);
    QCOMPARE(peers->text(), QStringLiteral("0 peers")); // zero reported; "No peers" is the not-yet-reported -1 case

    const QDateTime stale{QDateTime::currentDateTime().addSecs(-36 * 60 * 60)};
    window->setNumBlocks(8000, stale, 1.0, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    QCOMPARE(sync->text(), QStringLiteral("Waiting for peers"));
    QCOMPARE(sync->property("benchTone").toString(), QStringLiteral("plain"));
}

//! An answered empty headers response establishes sync even when the tip is old.
void TestAnsweredPeerWithNoHeadersAheadIsSynchronized(QuicksilverGUI* window)
{
    auto* sync = window->findChild<QLabel*>(QStringLiteral("benchStatusSync"));
    auto* node_state = window->findChild<QLabel*>(QStringLiteral("homeNodeState"));
    auto* progress = window->findChild<QProgressBar*>(QStringLiteral("homeNodeSync"));
    auto* network_sync = window->findChild<QLabel*>(QStringLiteral("syncStatusValue"));
    QVERIFY(sync && node_state && progress && network_sync);
    window->setNumConnections(1);
    window->setHeadersAnsweredPeers(0);
    window->setNumBlocks(8294, QDateTime::currentDateTime().addSecs(-9 * 60 * 60), 0.01,
                         SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    window->setHeadersAnsweredPeers(1);
    QCOMPARE(sync->text(), QStringLiteral("Synchronized"));
    QCOMPARE(node_state->text(), QStringLiteral("Synchronized"));
    QCOMPARE(network_sync->text(), QStringLiteral("Synchronized"));
    QCOMPARE(sync->property("benchTone").toString(), QStringLiteral("good"));
    QCOMPARE(progress->value(), 1000);
}

//! A real connection-count notification cannot substitute for a headers answer.
void TestConnectedPeerWithoutHeadersAnswerIsConnecting(QuicksilverGUI* window)
{
    window->setNumBlocks(8294, QDateTime::currentDateTime(), 1.0, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    window->setHeadersAnsweredPeers(0);
    window->setNumConnections(1);
    auto* sync = window->findChild<QLabel*>(QStringLiteral("benchStatusSync"));
    QVERIFY(sync);
    QCOMPARE(sync->text(), QStringLiteral("Connecting"));
}

//! The first answer may reveal work without a connected-block event.
void TestFirstHeadersAnswerReevaluatesKnownWork(QuicksilverGUI* window)
{
    const QDateTime stale = QDateTime::currentDateTime().addSecs(-9 * 60 * 60);
    window->setNumConnections(1);
    window->setHeadersAnsweredPeers(0);
    window->setNumBlocks(8404, stale, 1.0, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    window->setNumBlocks(8405, stale, 1.0, SyncType::HEADER_SYNC, SynchronizationState::POST_INIT);
    window->setHeadersAnsweredPeers(1);
    QVERIFY(!window->isReportedSynchronized());
    window->setNumBlocks(8404, stale, 1.0, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
}

//! Progress is connected height divided by known best height, regardless of the legacy estimate.
void TestSyncProgressUsesHeight(QuicksilverGUI* window)
{
    window->setNumConnections(1);
    window->setHeadersAnsweredPeers(1);
    window->setKnownBestHeight(8404, false);
    window->setNumBlocks(8294, QDateTime::currentDateTime(), 0.01, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    auto* progress = window->findChild<QProgressBar*>(QStringLiteral("homeNodeSync"));
    QVERIFY(progress);
    QCOMPARE(progress->value(), 987);
}

//! The network row receives the same state word and height-based percentage.
void TestNetworkSyncUsesSharedState(QuicksilverGUI* window)
{
    window->setNumConnections(1);
    window->setHeadersAnsweredPeers(1);
    window->setKnownBestHeight(8404, false);
    window->setNumBlocks(8294, QDateTime::currentDateTime(), 0.01, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    auto* sync = window->findChild<QLabel*>(QStringLiteral("syncStatusValue"));
    QVERIFY(sync);
    QCOMPARE(sync->text(), QStringLiteral("Catching up 98.7%"));
}

#ifdef ENABLE_VAULT
//! A vault opened after sync seeding receives the existing snapshot and later changes.
void TestLateVaultNetworkSyncUsesSharedState(QuicksilverGUI* window, interfaces::Node& node)
{
    window->setNumConnections(1);
    window->setHeadersAnsweredPeers(1);
    window->setKnownBestHeight(8405, false);
    window->setNumBlocks(8294, QDateTime::currentDateTime(), 0.01, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    auto* controller = window->getVaultController();
    QVERIFY(controller);
    std::vector<bilingual_str> warnings;
    auto vault = node.vaultLoader().createVault("desktop-sync-state", SecureString{},
        vault::VAULT_FLAG_DESCRIPTORS | vault::VAULT_FLAG_BLANK_VAULT, warnings);
    QVERIFY(vault);
    auto* model = controller->getOrCreateVault(std::move(*vault));
    QVERIFY(model);
    struct UnloadVault {
        QPointer<VaultModel> model;
        ~UnloadVault()
        {
            if (model) model->vault().remove();
            QCoreApplication::processEvents();
        }
    } unload{model};
    auto* frame = window->findChild<VaultFrame*>(QStringLiteral("vaultFrame"));
    QVERIFY(frame);
    VaultView* view = nullptr;
    for (auto* candidate : frame->findChildren<VaultView*>()) {
        if (candidate->getVaultModel() == model) view = candidate;
    }
    QVERIFY(view);
    // F-482 removed VaultView's network page. The sync sentence now lives on
    // the frame's page; the expected words below are unchanged.
    auto* sync = frame->findChild<QLabel*>(QStringLiteral("syncStatusValue"));
    QVERIFY(sync);
    QCOMPARE(sync->text(), QStringLiteral("Catching up 98.7%"));
    window->setHeadersAnsweredPeers(0);
    QCOMPARE(sync->text(), QStringLiteral("Connecting"));
    window->setHeadersAnsweredPeers(1);
    window->setNumBlocks(8405, QDateTime::currentDateTime(), 0.01, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    QCOMPARE(sync->text(), QStringLiteral("Synchronized"));
}

#endif

//! A stale genesis tip is complete after a peer answers with no better header.
void TestStaleGenesisAnsweredPeerIsSynchronized(interfaces::Node& node, OptionsModel* options_model, const NetworkStyle* network_style)
{
    QScopedPointer<const PlatformStyle> platform_style(PlatformStyle::instantiate(QuicksilverGUI::DEFAULT_UIPLATFORM.c_str()));
    ClientModel client_model(node, options_model);
    QuicksilverGUI window(node, platform_style.data(), network_style);
    struct DetachClient {
        QuicksilverGUI& window;
        ~DetachClient() { window.setClientModel(nullptr); }
    } detach{window};
    const QDateTime stale = QDateTime::currentDateTime().addSecs(-9 * 60 * 60);
    interfaces::BlockAndHeaderTipInfo tip{0, stale.toSecsSinceEpoch(), 0, stale.toSecsSinceEpoch(), 0.01};
    window.setClientModel(&client_model, &tip);
    auto* sync = window.findChild<QLabel*>(QStringLiteral("benchStatusSync"));
    auto* state = window.findChild<QLabel*>(QStringLiteral("homeNodeState"));
    auto* progress = window.findChild<QProgressBar*>(QStringLiteral("homeNodeSync"));
    auto* network_sync = window.findChild<QLabel*>(QStringLiteral("syncStatusValue"));
    QVERIFY(sync && state && progress && network_sync);
    window.setNumConnections(1);
    window.setHeadersAnsweredPeers(0);
    QCOMPARE(sync->text(), QStringLiteral("Connecting"));
    QCOMPARE(state->text(), QStringLiteral("Connecting"));
    QCOMPARE(network_sync->text(), QStringLiteral("Connecting"));
    QCOMPARE(progress->value(), 0);
    window.setHeadersAnsweredPeers(1);
    QCOMPARE(sync->text(), QStringLiteral("Synchronized"));
    QCOMPARE(state->text(), QStringLiteral("Synchronized"));
    QCOMPARE(network_sync->text(), QStringLiteral("Synchronized"));
    QCOMPARE(sync->property("benchTone").toString(), QStringLiteral("good"));
    QCOMPARE(progress->value(), 1000);
}

//! The live node-only GUI consumes sync notifications without any vault surfaces.
void TestDisabledVaultStripSyncState(interfaces::Node& node, OptionsModel* options_model, const NetworkStyle* network_style)
{
    struct RestoreDisableVault {
        std::optional<common::SettingsValue> previous;
        RestoreDisableVault()
        {
            gArgs.LockSettings([this](const auto& settings) {
                const auto it = settings.forced_settings.find("disablevault");
                if (it != settings.forced_settings.end()) previous = it->second;
            });
            gArgs.ForceSetArg("-disablevault", "1");
        }
        ~RestoreDisableVault()
        {
            gArgs.LockSettings([this](auto& settings) {
                if (previous) settings.forced_settings["disablevault"] = *previous;
                else settings.forced_settings.erase("disablevault");
            });
        }
    } restore_disable_vault;
    QScopedPointer<const PlatformStyle> platform_style(PlatformStyle::instantiate(QuicksilverGUI::DEFAULT_UIPLATFORM.c_str()));
    ClientModel client_model(node, options_model);
    QuicksilverGUI window(node, platform_style.data(), network_style);
    struct DetachClient {
        QuicksilverGUI& window;
        ~DetachClient() { window.setClientModel(nullptr); }
    } detach{window};
    const QDateTime stale = QDateTime::currentDateTime().addSecs(-9 * 60 * 60);
    interfaces::BlockAndHeaderTipInfo tip{8294, stale.toSecsSinceEpoch(), 8294, stale.toSecsSinceEpoch(), 0.01};
    window.setClientModel(&client_model, &tip);
    QVERIFY(!window.enableVault);
    QVERIFY(!window.findChild<QWidget*>(QStringLiteral("vaultFrame")));
    QVERIFY(!window.findChild<QWidget*>(QStringLiteral("desktopLaunchPage")));
    QVERIFY(!window.findChild<QLabel*>(QStringLiteral("homeNodeState")));
    QVERIFY(!window.findChild<QWidget*>(QStringLiteral("networkPage")));
    auto* sync = window.findChild<QLabel*>(QStringLiteral("benchStatusSync"));
    QVERIFY(sync);
    window.setNumConnections(1);
    window.setHeadersAnsweredPeers(0);
    QCOMPARE(sync->text(), QStringLiteral("Connecting"));
    QCOMPARE(sync->property("benchTone").toString(), QStringLiteral("plain"));
    window.setNumBlocks(8404, stale, 1.0, SyncType::HEADER_SYNC, SynchronizationState::POST_INIT);
    window.setHeadersAnsweredPeers(1);
    QCOMPARE(sync->text(), QStringLiteral("Catching up"));
    QVERIFY(sync->toolTip().contains(QStringLiteral("110 blocks behind")));
    window.setNumBlocks(8404, stale, 0.01, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    QCOMPARE(sync->text(), QStringLiteral("Synchronized"));
    QCOMPARE(sync->property("benchTone").toString(), QStringLiteral("good"));
}

//! Connecting to a peer alone says nothing about that peer's chain.
void TestFirstPeerBeforeHeadersIsNotSynchronized(QuicksilverGUI* window)
{
    auto* sync = window->findChild<QLabel*>(QStringLiteral("benchStatusSync"));
    auto* node_state = window->findChild<QLabel*>(QStringLiteral("homeNodeState"));
    auto* progress = window->findChild<QProgressBar*>(QStringLiteral("homeNodeSync"));
    auto* network_sync = window->findChild<QLabel*>(QStringLiteral("syncStatusValue"));
    QVERIFY(sync && node_state && progress && network_sync);
    const QDateTime stale = QDateTime::currentDateTime().addSecs(-9 * 60 * 60);
    window->setNumConnections(0);
    window->setHeadersAnsweredPeers(0);
    window->setNumBlocks(8294, stale, 1.0, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    QCOMPARE(sync->text(), QStringLiteral("Waiting for peers"));
    QCOMPARE(node_state->text(), QStringLiteral("No peers"));
    QCOMPARE(network_sync->text(), QStringLiteral("Waiting for peers"));
    QCOMPARE(progress->value(), 0);

    window->setNumConnections(1);
    QCOMPARE(sync->text(), QStringLiteral("Connecting"));
    QCOMPARE(node_state->text(), QStringLiteral("Connecting"));
    QCOMPARE(network_sync->text(), QStringLiteral("Connecting"));
    QCOMPARE(sync->property("benchTone").toString(), QStringLiteral("plain"));
    QCOMPARE(node_state->property("benchTone").toString(), QStringLiteral("plain"));
    QCOMPARE(progress->value(), 0);

    window->setNumBlocks(8404, stale, 1.0, SyncType::HEADER_SYNC, SynchronizationState::POST_INIT);
    window->setHeadersAnsweredPeers(1);
    QCOMPARE(sync->text(), QStringLiteral("Catching up"));
    QCOMPARE(node_state->text(), QStringLiteral("Catching up"));
    QVERIFY(sync->toolTip().contains(QStringLiteral("110 blocks behind")));
    QVERIFY(progress->value() > 0 && progress->value() < 1000);
    QCOMPARE(network_sync->text(), QStringLiteral("Catching up 98.7%"));

    window->setNumBlocks(8404, stale, 0.01, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    QCOMPARE(sync->text(), QStringLiteral("Synchronized"));
    QCOMPARE(node_state->text(), QStringLiteral("Synchronized"));
    QCOMPARE(network_sync->text(), QStringLiteral("Synchronized"));
    QCOMPARE(sync->property("benchTone").toString(), QStringLiteral("good"));
    QCOMPARE(progress->value(), 1000);

    window->setHeadersAnsweredPeers(0);
    QCOMPARE(sync->text(), QStringLiteral("Connecting"));
    QCOMPARE(node_state->text(), QStringLiteral("Connecting"));
    QCOMPARE(network_sync->text(), QStringLiteral("Connecting"));
    QCOMPARE(progress->value(), 0);
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
    // F-500: after a failed start the Network action shows the network page.
    // The restart-required review is the separate route, and the title
    // assertion below is unchanged.
    network_action->trigger();
    QStackedWidget* failed_stack = window.findChild<QStackedWidget*>(QStringLiteral("vaultFrameStack"));
    QVERIFY(failed_stack);
    QWidget* failed_page = failed_stack->currentWidget();
    QVERIFY(failed_page);
    QCOMPARE(failed_page->objectName(), QStringLiteral("networkPage"));
    QLabel* failed_status = failed_page->findChild<QLabel*>(QStringLiteral("networkStatusValue"));
    QVERIFY(failed_status);
    QCOMPARE(failed_status->text(), QStringLiteral("Startup failed"));
    QPushButton* review_consensus = failed_page->findChild<QPushButton*>(QStringLiteral("reviewConsensusButton"));
    QVERIFY(review_consensus);
    QVERIFY(review_consensus->isHidden());
    window.findChild<VaultFrame*>(QStringLiteral("vaultFrame"))->gotoConsensusReviewPage();
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
        QStringLiteral("-cuckatoosolver=/opt/quicksilver/qsgpusolve-opencl"),
        QStringLiteral("-datadir=/home/user/.quicksilver"),
        QStringLiteral("quicksilver:ignored-on-restart"),
    };

    QCOMPARE(QuicksilverApplication::restartArgumentsForChainForTesting(original_arguments, QStringLiteral("publictest")),
        QStringList({
            QStringLiteral("-min"),
            QStringLiteral("-cuckatoosolver=/opt/quicksilver/qsgpusolve-opencl"),
            QStringLiteral("-datadir=/home/user/.quicksilver"),
            QStringLiteral("-chain=publictest"),
        }));

    QCOMPARE(QuicksilverApplication::restartArgumentsForChainForTesting(original_arguments, QStringLiteral("sandbox")),
        QStringList({
            QStringLiteral("-min"),
            QStringLiteral("-txpownocycle=1"),
            QStringLiteral("-testactivationheight=segwit@10"),
            QStringLiteral("-cuckatoosolver=/opt/quicksilver/qsgpusolve-opencl"),
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
    setupPlatformStyleOnce();
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
    TestSyncStateFollowsKnownHeaders(window);
    TestStripWithoutPeersIsNotSynchronized(window);
    TestSyncStateWords();
    TestAssayBenchChrome(window);
    TestAssayBenchChromeLayout(window);
    TestStatusStripFollowsMiningStatus(window);
    TestAssayBenchHome(window);
    TestAnsweredPeerWithNoHeadersAheadIsSynchronized(window);
    TestConnectedPeerWithoutHeadersAnswerIsConnecting(window);
    TestFirstPeerBeforeHeadersIsNotSynchronized(window);
    // Restore the connected tip for the independent first-answer scenario.
    TestFirstHeadersAnswerReevaluatesKnownWork(window);
    TestSyncProgressUsesHeight(window);
    TestNetworkSyncUsesSharedState(window);
#ifdef ENABLE_VAULT
    TestLateVaultNetworkSyncUsesSharedState(window, m_app.node());
#endif
    window->setNumConnections(0);
    window->setHeadersAnsweredPeers(-1);
    auto* options_model = m_app.findChild<OptionsModel*>();
    QVERIFY(options_model);
    QScopedPointer<const NetworkStyle> network_style(NetworkStyle::instantiate(Params().GetChainType()));
    TestTranslatedSyncStateUsesValue(m_app.node(), options_model, network_style.data());
    TestStaleGenesisAnsweredPeerIsSynchronized(m_app.node(), options_model, network_style.data());
    const QWidgetList widgets_before_list{QApplication::allWidgets()};
    const QSet<QWidget*> widgets_before{widgets_before_list.cbegin(), widgets_before_list.cend()};
    TestDisabledVaultStripSyncState(m_app.node(), options_model, network_style.data());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    const QWidgetList widgets_after{QApplication::allWidgets()};
    QStringList extra_widgets;
    for (QWidget* widget : widgets_after) {
        if (!widgets_before.contains(widget)) {
            extra_widgets.push_back(QStringLiteral("%1 objectName=\"%2\"")
                                        .arg(QString::fromLatin1(widget->metaObject()->className()), widget->objectName()));
        }
    }
    QVERIFY2(extra_widgets.isEmpty(),
             qPrintable(QStringLiteral("widgets survived disabled-vault window destruction:\n%1")
                            .arg(extra_widgets.join(QLatin1Char('\n')))));
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

namespace {
fs::path AutostartDesktopPath()
{
    const char* config_home = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    fs::path dir;
    if (config_home && *config_home) dir = fs::path{config_home} / "autostart";
    else if (home && *home) dir = fs::path{home} / ".config" / "autostart";
    else return {};

    const std::string chain = ChainTypeToString(gArgs.GetChainType());
    if (chain == "main") return dir / "quicksilver.desktop";
    return dir / fs::u8path("quicksilver-" + chain + ".desktop");
}

struct AutostartFileGuard {
    fs::path path{AutostartDesktopPath()};
    std::optional<std::string> original;
    explicit AutostartFileGuard()
    {
        if (path.empty()) return;
        std::ifstream in{path};
        if (!in) return;
        std::ostringstream bytes;
        bytes << in.rdbuf();
        original = bytes.str();
    }
    ~AutostartFileGuard()
    {
        if (path.empty()) return;
        if (!original) {
            fs::remove(path);
            return;
        }
        std::ofstream out{path, std::ios::trunc};
        out << *original;
    }
};

QAction* PreferencesAction(QuicksilverGUI* window)
{
    for (QAction* action : window->findChildren<QAction*>()) {
        if (action->menuRole() == QAction::PreferencesRole) return action;
    }
    return nullptr;
}

int ChangedSpin(const QSpinBox* box)
{
    return box->value() < box->maximum() ? box->value() + 1 : box->value() - 1;
}

//! Empty when Options › Network shows the consensus box with this saved state,
//! the signed wording, and a place above port mapping. QVERIFY in a helper
//! would return from the helper only.
QString ConsensusBoxProblem(QWidget* dialog, bool ticked)
{
    auto* box = dialog->findChild<QCheckBox*>(QStringLiteral("runConsensus"));
    if (!box) return QStringLiteral("missing runConsensus");
    if (box->text() != QStringLiteral("R&un consensus on this network")) {
        return QStringLiteral("text '%1'").arg(box->text());
    }
    const QString tooltip = QStringLiteral("Applies to this network only. Each network keeps its own setting. Turning consensus off takes effect when the client restarts. Turning it on opens the consensus review first.");
    if (box->toolTip() != tooltip) return QStringLiteral("tooltip '%1'").arg(box->toolTip());
    if (box->isChecked() != ticked) return QStringLiteral("checked=%1 want=%2").arg(box->isChecked()).arg(ticked);
    auto* natpmp = dialog->findChild<QCheckBox*>(QStringLiteral("mapPortNatpmp"));
    if (!natpmp) return QStringLiteral("missing mapPortNatpmp");
    auto* network = dialog->findChild<QWidget*>(QStringLiteral("tabNetwork"));
    if (!network || !network->isAncestorOf(box)) return QStringLiteral("runConsensus is not on tabNetwork");
    auto* layout = qobject_cast<QVBoxLayout*>(network->layout());
    if (!layout || layout->objectName() != QStringLiteral("verticalLayout_Network")) {
        return QStringLiteral("tabNetwork layout is not verticalLayout_Network");
    }
    if (layout->indexOf(box) < 0 || layout->indexOf(natpmp) < 0 || layout->indexOf(box) >= layout->indexOf(natpmp)) {
        return QStringLiteral("runConsensus index %1, mapPortNatpmp index %2").arg(layout->indexOf(box)).arg(layout->indexOf(natpmp));
    }
    return {};
}

//! Puts the saved opt-in back. A failed slot must not leave it true for the
//! consensus-off window's later siblings.
struct SavedConsensusGuard {
    ~SavedConsensusGuard() { QSettings().setValue(QStringLiteral("Desktop/ConsensusEnabled"), false); }
};

struct CloseModalGuard {
    ~CloseModalGuard()
    {
        if (QWidget* modal = QApplication::activeModalWidget()) modal->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
};
} // namespace

QuicksilverGUI* AppTests::consensusOffWindow()
{
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* gui = qobject_cast<QuicksilverGUI*>(widget)) return gui;
    }

    // Reset first: it clears QSettings, including the consensus opt-in the frame
    // is about to read. Put the opt-in back to off before the window exists.
    if (!m_app.createOptionsModel(/*resetSettings=*/true)) return nullptr;
    QSettings().setValue(QStringLiteral("Desktop/ConsensusEnabled"), false);
    setupPlatformStyleOnce();
    static const std::unique_ptr<const NetworkStyle> style{NetworkStyle::instantiate(Params().GetChainType())};
    if (!style) return nullptr;
    m_app.createWindow(style.get());

    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* gui = qobject_cast<QuicksilverGUI*>(widget)) return gui;
    }
    return nullptr;
}

void AppTests::setupPlatformStyleOnce()
{
    if (m_platform_style_set) return;
    m_app.setupPlatformStyle();
    m_platform_style_set = true;
}

void AppTests::releaseConsensusOffWindow()
{
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* gui = qobject_cast<QuicksilverGUI*>(widget)) gui->hide();
    }
    // The member in QuicksilverApplication is assigned over on the next
    // createOptionsModel; that assignment does not dereference the old pointer.
    qDeleteAll(m_app.findChildren<OptionsModel*>(QString{}, Qt::FindDirectChildrenOnly));
    m_app.node().mapPort(false);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void AppTests::optionsOpensWithConsensusOff()
{
    QuicksilverGUI* window = consensusOffWindow();
    QVERIFY(window);
    QVERIFY(!window->findChild<ClientModel*>());
    window->show();
    QApplication::processEvents();

    QAction* options = PreferencesAction(window);
    QVERIFY(options);
    QVERIFY(options->isEnabled());
    options->trigger();

    auto* dialog = qobject_cast<OptionsDialog*>(QApplication::activeModalWidget());
    QVERIFY2(dialog, "optionsAction did not open an OptionsDialog");
    const QString box_problem = ConsensusBoxProblem(dialog, /*ticked=*/false);
    QVERIFY2(box_problem.isEmpty(), qPrintable(box_problem));
    dialog->reject();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(!QSettings().value(QStringLiteral("Desktop/ConsensusEnabled"), false).toBool());
}

void AppTests::optionsConsensusCancelChangesNothing()
{
    SavedConsensusGuard keep_off;
    CloseModalGuard close_modal;
    QuicksilverGUI* window = consensusOffWindow();
    QVERIFY(window);
    window->show();
    QApplication::processEvents();
    QAction* options = PreferencesAction(window);
    QVERIFY(options && options->isEnabled());

    options->trigger();
    auto* dialog = qobject_cast<OptionsDialog*>(QApplication::activeModalWidget());
    QVERIFY2(dialog, "optionsAction did not open an OptionsDialog");
    auto* box = dialog->findChild<QCheckBox*>(QStringLiteral("runConsensus"));
    QVERIFY(box);
    QVERIFY(!box->isChecked());
    box->click();
    QVERIFY(box->isChecked());
    dialog->findChild<QPushButton*>(QStringLiteral("cancelButton"))->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(!QSettings().value(QStringLiteral("Desktop/ConsensusEnabled"), false).toBool());

    // The box shows the saved setting. Live consensus on this window stays off.
    QSettings().setValue(QStringLiteral("Desktop/ConsensusEnabled"), true);
    options->trigger();
    dialog = qobject_cast<OptionsDialog*>(QApplication::activeModalWidget());
    QVERIFY2(dialog, "optionsAction did not reopen an OptionsDialog");
    const QString ticked = ConsensusBoxProblem(dialog, /*ticked=*/true);
    QVERIFY2(ticked.isEmpty(), qPrintable(ticked));
    dialog->findChild<QCheckBox*>(QStringLiteral("runConsensus"))->click();
    dialog->findChild<QPushButton*>(QStringLiteral("cancelButton"))->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(QSettings().value(QStringLiteral("Desktop/ConsensusEnabled"), false).toBool());
}

void AppTests::consensusOffOptionsTickOpensReview()
{
    CloseModalGuard close_modal;
    QuicksilverGUI* window = consensusOffWindow();
    QVERIFY(window);
    window->show();
    QApplication::processEvents();
    VaultFrame* frame = window->findChild<VaultFrame*>(QStringLiteral("vaultFrame"));
    QVERIFY(frame);
    QVERIFY(!frame->consensusEnabled());
    auto* stack = window->findChild<QStackedWidget*>(QStringLiteral("vaultFrameStack"));
    QVERIFY(stack);

    QAction* options = PreferencesAction(window);
    QVERIFY(options && options->isEnabled());
    options->trigger();
    auto* dialog = qobject_cast<OptionsDialog*>(QApplication::activeModalWidget());
    QVERIFY2(dialog, "optionsAction did not open an OptionsDialog");
    const QString box_problem = ConsensusBoxProblem(dialog, /*ticked=*/false);
    QVERIFY2(box_problem.isEmpty(), qPrintable(box_problem));
    dialog->findChild<QCheckBox*>(QStringLiteral("runConsensus"))->click();
    auto* ok = dialog->findChild<QPushButton*>(QStringLiteral("okButton"));
    QVERIFY(ok);
    QTRY_VERIFY(ok->isEnabled());
    ok->click();

    QCOMPARE(stack->currentWidget()->objectName(), QStringLiteral("consensusReviewPage"));
    QVERIFY(!QSettings().value(QStringLiteral("Desktop/ConsensusEnabled"), false).toBool());
    QVERIFY(!frame->consensusEnabled());
    // Continue on this window would start the node. Decline leaves consensus off.
    auto* decline = stack->currentWidget()->findChild<QPushButton*>(QStringLiteral("consensusDeclineButton"));
    QVERIFY(decline);
    decline->click();
    QVERIFY(!QSettings().value(QStringLiteral("Desktop/ConsensusEnabled"), false).toBool());
    QVERIFY(!frame->consensusEnabled());
    QCOMPARE(stack->currentWidget()->objectName(), QStringLiteral("desktopLaunchPage"));
}

void AppTests::consensusOnOptionsUntickWaitsForRestart()
{
    SavedConsensusGuard keep_off;
    CloseModalGuard close_modal;
    QSettings().setValue(QStringLiteral("Desktop/ConsensusEnabled"), false);
    OptionsModel* model = m_app.findChild<OptionsModel*>();
    QVERIFY(model);
    QScopedPointer<const PlatformStyle> platform_style(PlatformStyle::instantiate(QuicksilverGUI::DEFAULT_UIPLATFORM.c_str()));
    QScopedPointer<const NetworkStyle> network_style(NetworkStyle::instantiate(Params().GetChainType()));
    QVERIFY(platform_style);
    QVERIFY(network_style);
    // Not createWindow: that connects consensus activation to requestInitialize.
    QuicksilverGUI window(m_app.node(), platform_style.data(), network_style.data());
    window.setOptionsModel(model);
    window.show();
    QApplication::processEvents();

    VaultFrame* frame = window.findChild<VaultFrame*>(QStringLiteral("vaultFrame"));
    QVERIFY(frame);
    QVERIFY(!frame->consensusEnabled());
    auto* stack = window.findChild<QStackedWidget*>(QStringLiteral("vaultFrameStack"));
    QVERIFY(stack);
    frame->gotoConsensusReviewPage();
    auto* continue_button = stack->currentWidget()->findChild<QPushButton*>(QStringLiteral("consensusContinueButton"));
    QVERIFY(continue_button);
    continue_button->click();
    QVERIFY(frame->consensusEnabled());
    QVERIFY(QSettings().value(QStringLiteral("Desktop/ConsensusEnabled"), false).toBool());

    frame->gotoConsensusReviewPage();
    QCOMPARE(window.findChild<QLabel*>(QStringLiteral("consensusChoiceCopy"))->text(),
             QStringLiteral("You can review live status now. To turn consensus off, untick it in Settings > Options > Network. It stops when the client restarts."));

    QAction* options = PreferencesAction(&window);
    QVERIFY(options && options->isEnabled());
    options->trigger();
    auto* dialog = qobject_cast<OptionsDialog*>(QApplication::activeModalWidget());
    QVERIFY2(dialog, "optionsAction did not open an OptionsDialog");
    const QString ticked = ConsensusBoxProblem(dialog, /*ticked=*/true);
    QVERIFY2(ticked.isEmpty(), qPrintable(ticked));
    auto* consensus_box = dialog->findChild<QCheckBox*>(QStringLiteral("runConsensus"));
    QVERIFY(consensus_box);
    consensus_box->click();
    // Visible before OK. The saved setting stays true until OK.
    QCOMPARE(dialog->findChild<QLabel*>(QStringLiteral("statusLabel"))->text(), QStringLiteral("Client restart required to activate changes."));
    QVERIFY(QSettings().value(QStringLiteral("Desktop/ConsensusEnabled"), false).toBool());
    consensus_box->click();
    QCOMPARE(dialog->findChild<QLabel*>(QStringLiteral("statusLabel"))->text(), QString());
    QVERIFY(QSettings().value(QStringLiteral("Desktop/ConsensusEnabled"), false).toBool());
    consensus_box->click();
    auto* ok = dialog->findChild<QPushButton*>(QStringLiteral("okButton"));
    QVERIFY(ok);
    QTRY_VERIFY(ok->isEnabled());
    ok->click();

    QVERIFY(!QSettings().value(QStringLiteral("Desktop/ConsensusEnabled"), true).toBool());
    QCOMPARE(dialog->findChild<QLabel*>(QStringLiteral("statusLabel"))->text(), QStringLiteral("Client restart required to activate changes."));
    QVERIFY(frame->consensusEnabled());
    auto* sync = window.findChild<QLabel*>(QStringLiteral("benchStatusSync"));
    QVERIFY(sync);
    QVERIFY(sync->text() != QStringLiteral("Consensus off"));

    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    options->trigger();
    dialog = qobject_cast<OptionsDialog*>(QApplication::activeModalWidget());
    QVERIFY2(dialog, "optionsAction did not reopen an OptionsDialog");
    const QString unticked = ConsensusBoxProblem(dialog, /*ticked=*/false);
    QVERIFY2(unticked.isEmpty(), qPrintable(unticked));
    QCOMPARE(dialog->findChild<QLabel*>(QStringLiteral("statusLabel"))->text(), QStringLiteral("Client restart required to activate changes."));

    frame->gotoLaunchPage();
    QCOMPARE(stack->currentWidget()->objectName(), QStringLiteral("desktopLaunchPage"));
    dialog->findChild<QCheckBox*>(QStringLiteral("runConsensus"))->click();
    ok = dialog->findChild<QPushButton*>(QStringLiteral("okButton"));
    QVERIFY(ok);
    QTRY_VERIFY(ok->isEnabled());
    ok->click();
    QVERIFY(QSettings().value(QStringLiteral("Desktop/ConsensusEnabled"), false).toBool());
    QCOMPARE(dialog->findChild<QLabel*>(QStringLiteral("statusLabel"))->text(), QString());
    QCOMPARE(stack->currentWidget()->objectName(), QStringLiteral("desktopLaunchPage"));
    QVERIFY(frame->consensusEnabled());
    m_app.node().mapPort(false);
}

void AppTests::optionsSaveWithNoNode()
{
    AutostartFileGuard autostart;
    struct MapPortGuard {
        interfaces::Node& node;
        ~MapPortGuard() { node.mapPort(false); }
    } map_port{m_app.node()};

    QuicksilverGUI* window = consensusOffWindow();
    QVERIFY(window);
    QVERIFY(!window->findChild<ClientModel*>());
    window->show();
    QApplication::processEvents();

    QAction* options = PreferencesAction(window);
    QVERIFY(options);
    QVERIFY(options->isEnabled());
    options->trigger();
    auto* dialog = qobject_cast<OptionsDialog*>(QApplication::activeModalWidget());
    QVERIFY2(dialog, "optionsAction did not open an OptionsDialog");

    OptionsModel* model = m_app.findChild<OptionsModel*>();
    QVERIFY(model);

    const bool tray_available = QSystemTrayIcon::isSystemTrayAvailable();
#ifndef Q_OS_MACOS
    if (tray_available) {
        QVERIFY(model->setOption(OptionsModel::ShowTrayIcon, false));
        QVERIFY(model->setOption(OptionsModel::MinimizeToTray, false));
        dialog->reject();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        options->trigger();
        dialog = qobject_cast<OptionsDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog, "optionsAction did not reopen an OptionsDialog");
    }
#endif

    auto require = [](auto* widget, const char* name) {
        return widget ? QString{} : QStringLiteral("missing widget %1").arg(QLatin1String(name));
    };
    QStringList problems;

    auto* startup = dialog->findChild<QCheckBox*>(QStringLiteral("quicksilverAtStartup"));
    auto* threads = dialog->findChild<QSpinBox*>(QStringLiteral("threadsScriptVerif"));
    auto* dbcache = dialog->findChild<QSpinBox*>(QStringLiteral("databaseCache"));
    auto* gpu_path = dialog->findChild<QLineEdit*>(QStringLiteral("gpuSolverPath"));
    auto* cpu_mining = dialog->findChild<QCheckBox*>(QStringLiteral("allowCpuBlockMining"));
    auto* prune = dialog->findChild<QCheckBox*>(QStringLiteral("prune"));
    auto* prune_size = dialog->findChild<QSpinBox*>(QStringLiteral("pruneSize"));
    auto* spend_zero = dialog->findChild<QCheckBox*>(QStringLiteral("spendZeroConfChange"));
    auto* coin_control = dialog->findChild<QCheckBox*>(QStringLiteral("coinControlFeatures"));
    auto* signer = dialog->findChild<QLineEdit*>(QStringLiteral("externalSignerPath"));
    auto* psqt = dialog->findChild<QCheckBox*>(QStringLiteral("m_enable_psqt_controls"));
    auto* natpmp = dialog->findChild<QCheckBox*>(QStringLiteral("mapPortNatpmp"));
    auto* listen = dialog->findChild<QCheckBox*>(QStringLiteral("allowIncoming"));
    auto* server = dialog->findChild<QCheckBox*>(QStringLiteral("enableServer"));
    auto* proxy_use = dialog->findChild<QCheckBox*>(QStringLiteral("connectSocks"));
    auto* proxy_ip = dialog->findChild<QLineEdit*>(QStringLiteral("proxyIp"));
    auto* proxy_port = dialog->findChild<QLineEdit*>(QStringLiteral("proxyPort"));
    auto* tor_use = dialog->findChild<QCheckBox*>(QStringLiteral("connectSocksTor"));
    auto* tor_ip = dialog->findChild<QLineEdit*>(QStringLiteral("proxyIpTor"));
    auto* tor_port = dialog->findChild<QLineEdit*>(QStringLiteral("proxyPortTor"));
    auto* minimize_close = dialog->findChild<QCheckBox*>(QStringLiteral("minimizeOnClose"));
    auto* lang = dialog->findChild<QValueComboBox*>(QStringLiteral("lang"));
    auto* unit = dialog->findChild<QValueComboBox*>(QStringLiteral("unit"));
    auto* third_party = dialog->findChild<QLineEdit*>(QStringLiteral("thirdPartyTxUrls"));
    auto* gpu_cpu = dialog->findChild<QRadioButton*>(QStringLiteral("gpuModeCpu"));
    auto* ok = dialog->findChild<QPushButton*>(QStringLiteral("okButton"));

    const QWidget* widgets[] = {startup, threads, dbcache, gpu_path, cpu_mining, prune, prune_size, spend_zero,
        coin_control, signer, psqt, natpmp, listen, server, proxy_use, proxy_ip, proxy_port, tor_use, tor_ip,
        tor_port, lang, unit, third_party, gpu_cpu, ok};
    const char* names[] = {"quicksilverAtStartup", "threadsScriptVerif", "databaseCache", "gpuSolverPath",
        "allowCpuBlockMining", "prune", "pruneSize", "spendZeroConfChange", "coinControlFeatures",
        "externalSignerPath", "m_enable_psqt_controls", "mapPortNatpmp", "allowIncoming", "enableServer",
        "connectSocks", "proxyIp", "proxyPort", "connectSocksTor", "proxyIpTor", "proxyPortTor", "lang",
        "unit", "thirdPartyTxUrls", "gpuModeCpu", "okButton"};
    for (size_t i = 0; i < sizeof(widgets) / sizeof(widgets[0]); ++i) {
        if (!widgets[i]) problems << require(widgets[i], names[i]);
    }
#ifndef Q_OS_MACOS
    if (!minimize_close) problems << require(minimize_close, "minimizeOnClose");
    QCheckBox* show_tray = nullptr;
    QCheckBox* minimize_tray = nullptr;
    if (tray_available) {
        show_tray = dialog->findChild<QCheckBox*>(QStringLiteral("showTrayIcon"));
        minimize_tray = dialog->findChild<QCheckBox*>(QStringLiteral("minimizeToTray"));
        if (!show_tray) problems << require(show_tray, "showTrayIcon");
        if (!minimize_tray) problems << require(minimize_tray, "minimizeToTray");
    }
#endif
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QLatin1Char('\n'))));

    const bool want_startup = !startup->isChecked();
    startup->setChecked(want_startup);
    const int want_threads = ChangedSpin(threads);
    threads->setValue(want_threads);
    const int want_dbcache = ChangedSpin(dbcache);
    dbcache->setValue(want_dbcache);
    cpu_mining->setChecked(!cpu_mining->isChecked());
    const bool want_cpu_mining = cpu_mining->isChecked();
    prune->setChecked(true);
    const int want_prune_size = ChangedSpin(prune_size);
    prune_size->setValue(want_prune_size);
    const bool want_spend_zero = !spend_zero->isChecked();
    spend_zero->setChecked(want_spend_zero);
    const bool want_coin_control = !coin_control->isChecked();
    coin_control->setChecked(want_coin_control);
    const QString want_signer = QStringLiteral("/tmp/qs-f482-signer");
    signer->setText(want_signer);
    const bool want_psqt = !psqt->isChecked();
    psqt->setChecked(want_psqt);
    const bool want_natpmp = !natpmp->isChecked();
    natpmp->setChecked(want_natpmp);
    const bool want_listen = !listen->isChecked();
    listen->setChecked(want_listen);
    const bool want_server = !server->isChecked();
    server->setChecked(want_server);

    const QString want_proxy_ip = QStringLiteral("127.0.0.1");
    const QString want_proxy_port = QStringLiteral("1080");
    proxy_ip->setText(want_proxy_ip);
    proxy_port->setText(want_proxy_port);
    proxy_use->setChecked(true);
    const QString want_tor_port = QStringLiteral("9051");
    tor_ip->setText(want_proxy_ip);
    tor_port->setText(want_tor_port);
    tor_use->setChecked(true);

#ifndef Q_OS_MACOS
    const bool want_minimize_close = !minimize_close->isChecked();
    minimize_close->setChecked(want_minimize_close);
    if (tray_available) {
        show_tray->setChecked(true);
        minimize_tray->setChecked(true);
    }
#endif

    const bool language_mapped = lang && !lang->isHidden();
    QString want_language;
    if (language_mapped) {
        const int english = lang->findData(QStringLiteral("en"));
        if (english >= 0) lang->setCurrentIndex(english);
        want_language = lang->value().toString();
    }

    unit->setValue(QVariant::fromValue(QuicksilverUnit::HG));
    // The model still holds the pre-accept value. HG is 0, so a failed metatype
    // conversion would look like a successful save later; this pins the read.
    QCOMPARE(model->getOption(OptionsModel::DisplayUnit).value<QuicksilverUnit>(), QuicksilverUnit::HGS);
    const QString want_third_party = QStringLiteral("https://example.invalid/tx/%s");
    third_party->setText(want_third_party);

    // The dialog accepts a helper only if QFileInfo calls it an executable file
    // and `<helper> --list-devices` exits 0. A shell script fails both on
    // Windows, so use the stub binary the build ships beside this test, with
    // its exit code and delay left at their defaults (0).
    const QString helper_path{SolverProbeStubPath()};
    QVERIFY2(QFileInfo(helper_path).isExecutable(), qPrintable(helper_path));
    qunsetenv("QS_PROBE_STUB_EXIT");
    qunsetenv("QS_PROBE_STUB_SLEEP_MS");
    gpu_path->setText(helper_path);

    QVERIFY(ok);
    QTRY_VERIFY(ok->isEnabled());
    // After the probe's event-loop wait: a late graphics-card listing rewrites the
    // radios from the model. Set the CPU choice and accept before that can land.
    gpu_cpu->setChecked(true);
    ok->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    OptionsModel fresh(m_app.node());
    bilingual_str error;
    QVERIFY2(fresh.Init(error), error.original.c_str());

    auto mismatch = [&](const char* name, const QString& got, const QString& want) {
        if (got != want) problems << QStringLiteral("%1: got '%2', want '%3'").arg(QLatin1String(name), got, want);
    };
    mismatch("StartAtStartup", fresh.getOption(OptionsModel::StartAtStartup).toString(), want_startup ? QStringLiteral("true") : QStringLiteral("false"));
    mismatch("ThreadsScriptVerif", fresh.getOption(OptionsModel::ThreadsScriptVerif).toString(), QString::number(want_threads));
    mismatch("DatabaseCache", fresh.getOption(OptionsModel::DatabaseCache).toString(), QString::number(want_dbcache));
    mismatch("GpuSolverPath", fresh.getOption(OptionsModel::GpuSolverPath).toString(), helper_path);
    mismatch("AllowCpuBlockMining", fresh.getOption(OptionsModel::AllowCpuBlockMining).toString(), want_cpu_mining ? QStringLiteral("true") : QStringLiteral("false"));
    mismatch("Prune", fresh.getOption(OptionsModel::Prune).toString(), QStringLiteral("true"));
    mismatch("PruneSize", fresh.getOption(OptionsModel::PruneSize).toString(), QString::number(want_prune_size));
    mismatch("SpendZeroConfChange", fresh.getOption(OptionsModel::SpendZeroConfChange).toString(), want_spend_zero ? QStringLiteral("true") : QStringLiteral("false"));
    mismatch("CoinControlFeatures", fresh.getOption(OptionsModel::CoinControlFeatures).toString(), want_coin_control ? QStringLiteral("true") : QStringLiteral("false"));
    mismatch("ExternalSignerPath", fresh.getOption(OptionsModel::ExternalSignerPath).toString(), want_signer);
    mismatch("EnablePSQTControls", fresh.getOption(OptionsModel::EnablePSQTControls).toString(), want_psqt ? QStringLiteral("true") : QStringLiteral("false"));
    mismatch("MapPortNatpmp", fresh.getOption(OptionsModel::MapPortNatpmp).toString(), want_natpmp ? QStringLiteral("true") : QStringLiteral("false"));
    mismatch("Listen", fresh.getOption(OptionsModel::Listen).toString(), want_listen ? QStringLiteral("true") : QStringLiteral("false"));
    mismatch("Server", fresh.getOption(OptionsModel::Server).toString(), want_server ? QStringLiteral("true") : QStringLiteral("false"));
    mismatch("ProxyUse", fresh.getOption(OptionsModel::ProxyUse).toString(), QStringLiteral("true"));
    mismatch("ProxyIP", fresh.getOption(OptionsModel::ProxyIP).toString(), want_proxy_ip);
    mismatch("ProxyPort", fresh.getOption(OptionsModel::ProxyPort).toString(), want_proxy_port);
    mismatch("ProxyUseTor", fresh.getOption(OptionsModel::ProxyUseTor).toString(), QStringLiteral("true"));
    mismatch("ProxyIPTor", fresh.getOption(OptionsModel::ProxyIPTor).toString(), want_proxy_ip);
    mismatch("ProxyPortTor", fresh.getOption(OptionsModel::ProxyPortTor).toString(), want_tor_port);
#ifndef Q_OS_MACOS
    mismatch("MinimizeOnClose", fresh.getOption(OptionsModel::MinimizeOnClose).toString(), want_minimize_close ? QStringLiteral("true") : QStringLiteral("false"));
    if (tray_available) {
        mismatch("ShowTrayIcon", fresh.getOption(OptionsModel::ShowTrayIcon).toString(), QStringLiteral("true"));
        mismatch("MinimizeToTray", fresh.getOption(OptionsModel::MinimizeToTray).toString(), QStringLiteral("true"));
    }
#endif
    if (language_mapped) {
        mismatch("Language", fresh.getOption(OptionsModel::Language).toString(), want_language);
    }
    mismatch("DisplayUnit", QString::number(static_cast<int>(fresh.getOption(OptionsModel::DisplayUnit).value<QuicksilverUnit>())),
             QString::number(static_cast<int>(QuicksilverUnit::HG)));
    mismatch("ThirdPartyTxUrls", fresh.getOption(OptionsModel::ThirdPartyTxUrls).toString(), want_third_party);
    const QStringList devices = fresh.getOption(OptionsModel::GpuDevices).toStringList();
    mismatch("GpuDevices", devices.join(QLatin1Char(',')), QStringLiteral("cpu"));

    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QLatin1Char('\n'))));
}

void AppTests::networkPageOpensWithConsensusOff()
{
    QuicksilverGUI* window = consensusOffWindow();
    QVERIFY(window);
    VaultFrame* frame = window->findChild<VaultFrame*>(QStringLiteral("vaultFrame"));
    QVERIFY(frame);
    QVERIFY(!frame->consensusEnabled());

    frame->gotoNetworkPage();
    auto* stack = window->findChild<QStackedWidget*>(QStringLiteral("vaultFrameStack"));
    QVERIFY(stack);
    QCOMPARE(stack->currentWidget()->objectName(), QStringLiteral("networkPage"));

    QWidget* page = stack->currentWidget();
    QCOMPARE(page->findChild<QLabel*>(QStringLiteral("networkIntro"))->text(),
             QStringLiteral("Consensus is off, so this computer is not connected to any network."));
    QCOMPARE(page->findChild<QLabel*>(QStringLiteral("networkStatusValue"))->text(), QStringLiteral("Consensus off"));
    QCOMPARE(page->findChild<QLabel*>(QStringLiteral("peerCountValue"))->text(), QStringLiteral("Not running"));
    QCOMPARE(page->findChild<QLabel*>(QStringLiteral("syncStatusValue"))->text(), QStringLiteral("Not running"));

    QVERIFY(page->findChild<QWidget*>(QStringLiteral("developerNetworkMainCard")));
    QVERIFY(page->findChild<QWidget*>(QStringLiteral("developerNetworkPublicTestCard")));
    QVERIFY(page->findChild<QWidget*>(QStringLiteral("developerNetworkSandboxCard")));
    QPushButton* main_switch = page->findChild<QPushButton*>(QStringLiteral("developerNetworkMainCardButton"));
    QPushButton* public_switch = page->findChild<QPushButton*>(QStringLiteral("developerNetworkPublicTestCardButton"));
    QVERIFY(main_switch);
    QVERIFY(public_switch);
    QVERIFY(main_switch->isEnabled());
    QVERIFY(public_switch->isEnabled());
    QVERIFY(!page->findChild<QLineEdit*>(QStringLiteral("addPeerEdit"))->isEnabled());
    QVERIFY(!page->findChild<QPushButton*>(QStringLiteral("addPeerButton"))->isEnabled());

    QPushButton* review = page->findChild<QPushButton*>(QStringLiteral("reviewConsensusButton"));
    QVERIFY(review);
    QVERIFY(!review->isHidden());
    review->click();
    QCOMPARE(stack->currentWidget()->objectName(), QStringLiteral("consensusReviewPage"));

    frame->gotoLaunchPage();
    QPushButton* home_consensus = window->findChild<QPushButton*>(QStringLiteral("launchConsensusCardButton"));
    QVERIFY(home_consensus);
    QVERIFY(home_consensus->isEnabled());
    home_consensus->click();
    QCOMPARE(stack->currentWidget()->objectName(), QStringLiteral("consensusReviewPage"));

    QAction* mine = window->findChild<QAction*>(QStringLiteral("mineMintAction"));
    QVERIFY(mine);
    mine->trigger();
    QCOMPARE(stack->currentWidget()->objectName(), QStringLiteral("consensusReviewPage"));

    // Confirmed switch reaches launchPendingRestart before any node thread exists.
    // The launcher stands in for the process start. Production leaves it empty
    // and still shuts the node down before the relaunch; this test skips that
    // because the Qt classes share one node, and startShutdown is process-global.
    struct ClearRestartLauncher {
        QuicksilverApplication& app;
        ~ClearRestartLauncher() { app.setRestartLauncherForTesting({}); }
    } clear_launcher{m_app};
    QString seen_executable;
    QStringList seen_arguments;
    m_app.setRestartLauncherForTesting([&](const QString& executable, const QStringList& arguments) {
        seen_executable = executable;
        seen_arguments = arguments;
        return true;
    });
    frame->gotoNetworkPage();
    QVERIFY(public_switch->isEnabled());
    bool clicked = false;
    QTimer::singleShot(0, [&] {
        clicked = ClickNetworkRestartDialogButton(QStringLiteral("Switch and restart"), nullptr, nullptr);
    });
    public_switch->click();
    QTRY_VERIFY(clicked);
    QTRY_VERIFY(!seen_executable.isEmpty());
    QVERIFY(seen_arguments.contains(QStringLiteral("-chain=publictest")));
}

void AppTests::headerHidesTheCurrentPagesButton()
{
    struct ReleaseGuard {
        AppTests* tests;
        ~ReleaseGuard() { tests->releaseConsensusOffWindow(); }
    } release{this};

    QuicksilverGUI* window = consensusOffWindow();
    QVERIFY(window);
    window->show();
    QApplication::processEvents();

    auto* request = window->findChild<QPushButton*>(QStringLiteral("benchRequestButton"));
    auto* transfer = window->findChild<QPushButton*>(QStringLiteral("benchTransferButton"));
    auto* home = window->findChild<QAction*>(QStringLiteral("homeBootstrapAction"));
    auto* receive = window->findChild<QAction*>(QStringLiteral("receiveCoinsAction"));
    auto* send = window->findChild<QAction*>(QStringLiteral("sendCoinsAction"));
    QVERIFY(request);
    QVERIFY(transfer);
    QVERIFY(home);
    QVERIFY(receive);
    QVERIFY(send);

    home->setChecked(true);
    QVERIFY(request->isVisible());
    QVERIFY(transfer->isVisible());

    receive->setChecked(true);
    QVERIFY(!request->isVisible());
    QVERIFY(transfer->isVisible());
    const bool send_enabled = send->isEnabled();
    send->setEnabled(!send_enabled);
    QCOMPARE(transfer->isEnabled(), send->isEnabled());
    send->setEnabled(send_enabled);
    QCOMPARE(transfer->isEnabled(), send->isEnabled());

    send->setChecked(true);
    QVERIFY(request->isVisible());
    QVERIFY(!transfer->isVisible());
    const bool receive_enabled = receive->isEnabled();
    receive->setEnabled(!receive_enabled);
    QCOMPARE(request->isEnabled(), receive->isEnabled());
    receive->setEnabled(receive_enabled);
    QCOMPARE(request->isEnabled(), receive->isEnabled());

    home->setChecked(true);
    QVERIFY(request->isVisible());
    QVERIFY(transfer->isVisible());
}
