// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_NOTIFICATOR_H
#define QUICKSILVER_QT_NOTIFICATOR_H

#include <quicksilver-build-config.h> // IWYU pragma: keep

#include <QIcon>
#include <QObject>

#include <cstdint>
#include <optional>

QT_BEGIN_NAMESPACE
class QSystemTrayIcon;

#ifdef USE_DBUS
class QDBusInterface;
#endif
QT_END_NAMESPACE

/** Timestamp-driven policy for coalescing transaction notifications. */
class NotificationBurst
{
public:
    static constexpr int64_t INTERVAL_MILLIS{10'000};

    struct Decision {
        bool notify_transaction{false};
        int summary_count{0};
    };

    Decision record(int64_t now_millis);
    int takeSummary(int64_t now_millis);
    std::optional<int64_t> millisUntilNext(int64_t now_millis) const;

private:
    void begin(int64_t now_millis);
    void advanceBoundary(int64_t now_millis);

    bool m_active{false};
    int64_t m_last_event{0};
    int64_t m_next_summary{0};
    int m_pending{0};
};

/** Cross-platform desktop notification client. */
class Notificator: public QObject
{
    Q_OBJECT

public:
    /** Create a new notificator.
       @note Ownership of trayIcon is not transferred to this object.
    */
    Notificator(const QString &programName, QSystemTrayIcon *trayIcon, QWidget *parent);
    ~Notificator();

    // Message class
    enum Class
    {
        Information,    /**< Informational message */
        Warning,        /**< Notify user of potential problem */
        Critical        /**< An error occurred */
    };

public Q_SLOTS:
    /** Show notification message.
       @param[in] cls    general message class
       @param[in] title  title shown with message
       @param[in] text   message content
       @param[in] icon   optional icon to show with message
       @param[in] millisTimeout notification timeout in milliseconds (defaults to 10 seconds)
       @note Platform implementations are free to ignore any of the provided fields except for \a text.
     */
    void notify(Class cls, const QString &title, const QString &text,
                const QIcon &icon = QIcon(), int millisTimeout = 10000);

Q_SIGNALS:
    /** Counting seam for tests; emitted before platform delivery is attempted. */
    void notified(Class cls, const QString& title, const QString& text);

private:
    QWidget *parent;
    enum Mode {
        None,                       /**< Ignore informational notifications, and show a modal pop-up dialog for Critical notifications. */
        Freedesktop,                /**< Use DBus org.freedesktop.Notifications */
        QSystemTray,                /**< Use QSystemTrayIcon::showMessage() */
        UserNotificationCenter      /**< Use the 10.8+ User Notification Center (Mac only) */
    };
    QString programName;
    Mode mode{None};
    QSystemTrayIcon *trayIcon;
#ifdef USE_DBUS
    QDBusInterface* interface{nullptr};

    void notifyDBus(Class cls, const QString &title, const QString &text, const QIcon &icon, int millisTimeout);
#endif
    void notifySystray(Class cls, const QString &title, const QString &text, int millisTimeout);
#ifdef Q_OS_MACOS
    void notifyMacUserNotificationCenter(const QString &title, const QString &text);
#endif
};

#endif // QUICKSILVER_QT_NOTIFICATOR_H
