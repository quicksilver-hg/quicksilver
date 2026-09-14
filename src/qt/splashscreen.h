// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_SPLASHSCREEN_H
#define QUICKSILVER_QT_SPLASHSCREEN_H

#include <QWidget>

#include <memory>

class NetworkStyle;

namespace interfaces {
class Handler;
class Node;
class Vault;
};

/** Class for the splashscreen with information of the running client.
 *
 * @note this is intentionally not a QSplashScreen. Quicksilver initialization
 * can take a long time, and in that case a progress window that cannot be
 * moved around and minimized has turned out to be frustrating to the user.
 */
class SplashScreen : public QWidget
{
    Q_OBJECT

public:
    explicit SplashScreen(const NetworkStyle *networkStyle);
    ~SplashScreen();
    void setNode(interfaces::Node& node);

protected:
    void paintEvent(QPaintEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

public Q_SLOTS:
    /** Show message and progress */
    void showMessage(const QString &message, int alignment, const QColor &color);

    /** Handle vault load notifications. */
    void handleLoadVault();

protected:
    bool eventFilter(QObject * obj, QEvent * ev) override;

private:
    /** Connect core signals to splash screen */
    void subscribeToCoreSignals();
    /** Disconnect core signals to splash screen */
    void unsubscribeFromCoreSignals();
    /** Initiate shutdown */
    void shutdown();

    QPixmap pixmap;
    QString curMessage;
    QColor curColor;
    int curAlignment{0};

    interfaces::Node* m_node = nullptr;
    bool m_shutdown = false;
    std::unique_ptr<interfaces::Handler> m_handler_init_message;
    std::unique_ptr<interfaces::Handler> m_handler_show_progress;
    std::unique_ptr<interfaces::Handler> m_handler_init_vault;
    std::unique_ptr<interfaces::Handler> m_handler_load_vault;
    std::list<std::unique_ptr<interfaces::Vault>> m_connected_vaults;
    std::list<std::unique_ptr<interfaces::Handler>> m_connected_vault_handlers;
};

#endif // QUICKSILVER_QT_SPLASHSCREEN_H
