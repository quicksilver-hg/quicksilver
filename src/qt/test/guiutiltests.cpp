// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <quicksilver-build-config.h> // IWYU pragma: keep

#include <qt/test/guiutiltests.h>

#include <qt/guiutil.h>
#include <qt/notificator.h>

#include <QRegularExpression>
#ifdef USE_DBUS
#include <QDBusConnection>
#endif
#include <QAbstractButton>
#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>

#include <string>

//! formatNiceTimeOffset() feeds the Home Node card's last-block age. It used to
//! compose that text with Qt's "%n unit(s)" numerus idiom, which only a translation
//! catalogue resolves -- and this application installs none of its own (see
//! initTranslations() in qt/quicksilver.cpp). The age therefore read
//! "3 minute(s)" for everyone, in every locale.
void GUIUtilTests::timeOffsetReadsGrammaticallyWithNoCatalogue()
{
    // One value from every branch, plus both sides of each boundary.
    const qint64 samples[] = {0, 1, 45, 59, 60, 61, 3600, 7199, 7200, 10800,
                              172799, 172800, 259200, 1209599, 1209600, 1814400,
                              31556951, 31556952, 33371352, 63718704, 315569520};
    for (const qint64 secs : samples) {
        const QString text = GUIUtil::formatNiceTimeOffset(secs);
        QVERIFY2(!text.isEmpty(), qPrintable(QString::number(secs)));
        // The tell of an unresolved numerus form.
        QVERIFY2(!text.contains(QStringLiteral("(s)")), qPrintable(text));
        QVERIFY2(!text.contains(QStringLiteral("%n")), qPrintable(text));
        // And every placeholder must have been consumed: QString::arg leaves an
        // unmatched marker in place rather than failing.
        QVERIFY2(!text.contains(QRegularExpression(QStringLiteral("%[0-9]"))), qPrintable(text));
    }
}

//! A count of one is the case the numerus idiom existed to get right, so it is the
//! case worth pinning: "1 seconds" is as wrong as "1 second(s)".
//!
//! Only seconds, minutes and years can actually reach a count of one. The unit
//! thresholds are deliberately 2x -- the hour branch does not begin until 7200s --
//! so hours, days and the standalone week branch can never render fewer than two.
//! Their singular forms are kept for the day a threshold changes; the trailing week
//! of the year branch does reach one and is covered by timeOffsetPairsYearsWithWeeks.
void GUIUtilTests::timeOffsetSingularWhereReachable()
{
    QCOMPARE(GUIUtil::formatNiceTimeOffset(1), QStringLiteral("1 second"));
    QCOMPARE(GUIUtil::formatNiceTimeOffset(60), QStringLiteral("1 minute"));
    QCOMPARE(GUIUtil::formatNiceTimeOffset(119), QStringLiteral("1 minute"));
}

void GUIUtilTests::timeOffsetPluralAtEveryUnit()
{
    QCOMPARE(GUIUtil::formatNiceTimeOffset(0), QStringLiteral("0 seconds"));
    QCOMPARE(GUIUtil::formatNiceTimeOffset(45), QStringLiteral("45 seconds"));
    QCOMPARE(GUIUtil::formatNiceTimeOffset(600), QStringLiteral("10 minutes"));
    QCOMPARE(GUIUtil::formatNiceTimeOffset(10800), QStringLiteral("3 hours"));
    QCOMPARE(GUIUtil::formatNiceTimeOffset(259200), QStringLiteral("3 days"));
    QCOMPARE(GUIUtil::formatNiceTimeOffset(1814400), QStringLiteral("3 weeks"));
}

//! The year branch is the only optionally composed string, and the only place a
//! count of one reaches the week form.
void GUIUtilTests::timeOffsetPairsYearsWithWeeks()
{
    // Suppress a zero-valued suffix throughout the first partial week.
    QCOMPARE(GUIUtil::formatNiceTimeOffset(31556952), QStringLiteral("1 year"));
    QCOMPARE(GUIUtil::formatNiceTimeOffset(31556952 + 604799), QStringLiteral("1 year"));
    // 1 year + 3 weeks.
    QCOMPARE(GUIUtil::formatNiceTimeOffset(31556952 + 3 * 604800),
             QStringLiteral("1 year and 3 weeks"));
    QCOMPARE(GUIUtil::formatNiceTimeOffset(2 * 31556952), QStringLiteral("2 years"));
    // 2 years + 1 week: both halves inflect independently.
    QCOMPARE(GUIUtil::formatNiceTimeOffset(2 * 31556952 + 604800),
             QStringLiteral("2 years and 1 week"));
}

//! Each branch is picked by a strict `<`, so an off-by-one in a threshold silently
//! changes the unit rather than failing. Pin both sides of every boundary.
void GUIUtilTests::timeOffsetBoundariesPickTheIntendedUnit()
{
    QCOMPARE(GUIUtil::formatNiceTimeOffset(59), QStringLiteral("59 seconds"));
    QCOMPARE(GUIUtil::formatNiceTimeOffset(60), QStringLiteral("1 minute"));

    QCOMPARE(GUIUtil::formatNiceTimeOffset(7199), QStringLiteral("119 minutes"));
    QCOMPARE(GUIUtil::formatNiceTimeOffset(7200), QStringLiteral("2 hours"));

    QCOMPARE(GUIUtil::formatNiceTimeOffset(172799), QStringLiteral("47 hours"));
    QCOMPARE(GUIUtil::formatNiceTimeOffset(172800), QStringLiteral("2 days"));

    QCOMPARE(GUIUtil::formatNiceTimeOffset(1209599), QStringLiteral("13 days"));
    QCOMPARE(GUIUtil::formatNiceTimeOffset(1209600), QStringLiteral("2 weeks"));

    QCOMPARE(GUIUtil::formatNiceTimeOffset(31556951), QStringLiteral("52 weeks"));
    QCOMPARE(GUIUtil::formatNiceTimeOffset(31556952), QStringLiteral("1 year"));
}

//! A save dialog asks before replacing the file it will write. Typing "name" where "name.dat" exists must raise the
//! dialog's own "replace it?" question, and the name handed back is "name.dat". The filter's suffix used to be
//! appended after the dialog had accepted, so the dialog checked "name", found nothing, and the caller then wrote
//! over "name.dat" without asking (found in GX send-back 1, for the vault backup).
//! The question checked here is Qt's own, which only Qt's widget dialog asks: on a native dialog (the "windows"
//! platform the Windows tests run on) QFileDialog::accept() hands back the selection without asking, and the system
//! dialog asks instead (Qt sets FOS_OVERWRITEPROMPT and passes the default suffix to SetDefaultExtension). A test
//! cannot answer a system dialog, so it uses Qt's dialog on every platform.
void GUIUtilTests::saveDialogAsksBeforeReplacingTheSuffixedFile()
{
    const bool native_was_off{QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs)};
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    struct RestoreNative {
        bool off;
        ~RestoreNative() { QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, off); }
    } restore_native{native_was_off};
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString existing{dir.filePath(QStringLiteral("name.dat"))};
    {
        QFile file(existing);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("old"), qint64{3});
    }
    QWidget parent;
    QString chosen;
    bool done{false};
    GUIUtil::getSaveFileName(&parent, QStringLiteral("Save"), dir.path(), QStringLiteral("Data (*.dat)"),
        [&](const QString& filename) {
            chosen = filename;
            done = true;
        });
    QFileDialog* save{nullptr};
    for (QFileDialog* child : parent.findChildren<QFileDialog*>(QStringLiteral("saveFileDialog"))) {
        if (child->isVisible()) save = child;
    }
    QVERIFY(save);
    save->selectFile(dir.filePath(QStringLiteral("name")));
    bool asked{false};
    QTimer::singleShot(0, save, [&asked]() {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box) return;
        if (QAbstractButton* yes = box->button(QMessageBox::Yes)) {
            asked = true;
            yes->click();
        }
    });
    static_cast<QDialog*>(save)->accept();
    QTRY_VERIFY(done);
    QVERIFY(asked);
    QCOMPARE(chosen, QDir::toNativeSeparators(existing));
}

//! A message box's answer is handed over while the box, and so the clicked button, still exists. The box used to be
//! deleted on close before the deferred callback ran, so the button it received could already be freed.
void GUIUtilTests::messageBoxHandsOverALiveButton()
{
    auto* box = new QMessageBox(QMessageBox::Question, QStringLiteral("Question"), QStringLiteral("Proceed?"), QMessageBox::NoButton);
    QPushButton* proceed{box->addButton(QStringLiteral("Proceed"), QMessageBox::AcceptRole)};
    box->addButton(QMessageBox::Cancel);
    const QPointer<QPushButton> watch{proceed};
    bool done{false};
    bool alive{false};
    QAbstractButton* clicked_button{nullptr};
    GUIUtil::ShowModalMessageBoxAsynchronously(box, [&](int, QAbstractButton* clicked) {
        done = true;
        alive = !watch.isNull();
        clicked_button = clicked;
    });
    proceed->click();
    // Whether a pending delete-on-close runs before the deferred callback is up to the event loop. Run it first,
    // the order in which the button dangled.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QTRY_VERIFY(done);
    QVERIFY(alive);
    QCOMPARE(clicked_button, static_cast<QAbstractButton*>(proceed));
    // And the box is still freed once the answer is in.
    QTRY_VERIFY(watch.isNull());
}

#if defined(Q_OS_LINUX)
//! Each character the desktop-entry spec reserves is escaped in the autostart Exec argument: the path is quoted
//! (a space stays inside one argument), '"', '`' and '$' get a backslash that the string rule then doubles, a
//! backslash becomes four, and '%' is doubled because Exec reads it as a field code.
void GUIUtilTests::autostartExecArgumentEscapesReservedCharacters_data()
{
    QTest::addColumn<QString>("path");
    QTest::addColumn<QString>("expected");
    QTest::newRow("space") << QStringLiteral("/opt/my apps/qs") << QStringLiteral(R"("/opt/my apps/qs")");
    QTest::newRow("double quote") << QStringLiteral(R"(/opt/a"b/qs)") << QStringLiteral(R"("/opt/a\\"b/qs")");
    QTest::newRow("backtick") << QStringLiteral("/opt/a`b/qs") << QStringLiteral(R"("/opt/a\\`b/qs")");
    QTest::newRow("dollar") << QStringLiteral("/opt/a$b/qs") << QStringLiteral(R"("/opt/a\\$b/qs")");
    QTest::newRow("backslash") << QStringLiteral(R"(/opt/a\b/qs)") << QStringLiteral(R"("/opt/a\\\\b/qs")");
    QTest::newRow("percent") << QStringLiteral("/opt/a%b/qs") << QStringLiteral(R"("/opt/a%%b/qs")");
}

void GUIUtilTests::autostartExecArgumentEscapesReservedCharacters()
{
    QFETCH(QString, path);
    QFETCH(QString, expected);
    QCOMPARE(QString::fromStdString(GUIUtil::DesktopExecArgument(path.toStdString())), expected);
}

//! The Linux autostart entry quotes the executable, so an install path with a space stays one argument.
void GUIUtilTests::autostartEntryQuotesTheExecutable()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const bool had_config_home{qEnvironmentVariableIsSet("XDG_CONFIG_HOME")};
    const QByteArray old_config_home{qgetenv("XDG_CONFIG_HOME")};
    struct RestoreConfigHome {
        bool had;
        QByteArray value;
        ~RestoreConfigHome()
        {
            if (had) {
                qputenv("XDG_CONFIG_HOME", value);
            } else {
                qunsetenv("XDG_CONFIG_HOME");
            }
        }
    } restore{had_config_home, old_config_home};
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(dir.path()));

    QVERIFY(GUIUtil::SetStartOnSystemStartup(true));
    QVERIFY(GUIUtil::GetStartOnSystemStartup());
    const QDir autostart{dir.filePath(QStringLiteral("autostart"))};
    const QStringList entries{autostart.entryList({QStringLiteral("*.desktop")}, QDir::Files)};
    QCOMPARE(entries.size(), 1);
    QFile entry{autostart.filePath(entries.front())};
    QVERIFY(entry.open(QIODevice::ReadOnly | QIODevice::Text));
    QString exec;
    for (const QString& line : QString::fromUtf8(entry.readAll()).split(QLatin1Char('\n'))) {
        if (line.startsWith(QStringLiteral("Exec="))) exec = line;
    }
    const QString exe{QFileInfo(QStringLiteral("/proc/self/exe")).symLinkTarget()};
    QVERIFY(!exe.isEmpty());
    QVERIFY2(exec.startsWith(QStringLiteral("Exec=\"%1\" -min -chain=").arg(exe)), qPrintable(exec));

    QVERIFY(GUIUtil::SetStartOnSystemStartup(false));
    QVERIFY(!GUIUtil::GetStartOnSystemStartup());
}
#endif

#ifdef USE_DBUS
//! The first desktop notification on a Linux session bus makes Qt compute the
//! image type's D-Bus signature, which marshals a default-constructed
//! FreedesktopImage. Its scalar fields were left uninitialized, so UBSan
//! reported an invalid bool load (F-516). No bus is needed to reach that path.
void GUIUtilTests::notificationImageMarshalsInitializedFields()
{
    QCOMPARE(FreedesktopImageSignatureForTesting(), QStringLiteral("(iiibiiay)"));
}

//! F-572: the minimal platform isolates the display, not the session bus.
//! Notificator prefers org.freedesktop.Notifications whenever the bus answers,
//! so a test that connected a mining reward posted it to the developer's
//! desktop. test_main points the bus at a socket that does not exist.
void GUIUtilTests::testsCannotReachTheSessionBus()
{
    QVERIFY(!QDBusConnection::sessionBus().isConnected());
}
#endif
