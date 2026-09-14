// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/guiutil.h>

#include <qt/quicksilveraddressvalidator.h>
#include <qt/quicksilverunits.h>
#include <qt/platformstyle.h>
#include <qt/qvalidatedlineedit.h>
#include <qt/sendcoinsrecipient.h>

#include <addresstype.h>
#include <base58.h>
#include <chainparams.h>
#include <common/args.h>
#include <key_io.h>
#include <logging.h>
#include <primitives/transaction.h>
#include <protocol.h>
#include <script/script.h>
#include <util/chaintype.h>
#include <util/exception.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/time.h>

#ifdef WIN32
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#endif

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDoubleValidator>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFontDialog>
#include <QFontMetrics>
#include <QInputDialog>
#include <QGuiApplication>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLatin1String>
#include <QLineEdit>
#include <QList>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QPluginLoader>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QSettings>
#include <QShortcut>
#include <QSize>
#include <QStandardPaths>
#include <QString>
#include <QTextDocument> // for Qt::mightBeRichText
#include <QThread>
#include <QTimer>
#include <QUrlQuery>
#include <QtGlobal>

#include <cassert>
#include <chrono>
#include <exception>
#include <fstream>
#include <string>
#include <vector>

#if defined(Q_OS_MACOS)

#include <QProcess>

void ForceActivation();
#endif

using namespace std::chrono_literals;

namespace GUIUtil {

QString dateTimeStr(const QDateTime &date)
{
    return QLocale::system().toString(date.date(), QLocale::ShortFormat) + QString(" ") + date.toString("hh:mm");
}

QString dateTimeStr(qint64 nTime)
{
    return dateTimeStr(QDateTime::fromSecsSinceEpoch(nTime));
}

QFont fixedPitchFont(bool use_embedded_font)
{
    if (use_embedded_font) {
        return {"Roboto Mono"};
    }
    return QFontDatabase::systemFont(QFontDatabase::FixedFont);
}

// Return a pre-generated dummy bech32m address (P2TR) with invalid checksum.
static std::string DummyAddress(const CChainParams &params)
{
    std::string addr;
    switch (params.GetChainType()) {
    case ChainType::MAIN:
        addr = "hg1p35yvjel7srp783ztf8v6jdra7dhfzk5jaun8xz2qp6ws7z80n4tq2jku9f";
        break;
    case ChainType::PUBLIC_TEST:
        addr = "phg1p35yvjel7srp783ztf8v6jdra7dhfzk5jaun8xz2qp6ws7z80n4tqa6qnlg";
        break;
    case ChainType::SANDBOX:
        addr = "shg1p35yvjel7srp783ztf8v6jdra7dhfzk5jaun8xz2qp6ws7z80n4tqsr2427";
        break;
    } // no default case, so the compiler can warn about missing cases
    assert(!addr.empty());

    if (Assume(!IsValidDestinationString(addr))) return addr;
    return {};
}

void setupAddressWidget(QValidatedLineEdit *widget, QWidget *parent)
{
    parent->setFocusProxy(widget);

    widget->setFont(fixedPitchFont());
    // We don't want translators to use own addresses in translations
    // and this is the only place, where this address is supplied.
    widget->setPlaceholderText(QObject::tr("Enter a Quicksilver address (e.g. %1)").arg(
        QString::fromStdString(DummyAddress(Params()))));
    widget->setValidator(new QuicksilverAddressEntryValidator(parent));
    widget->setCheckValidator(new QuicksilverAddressCheckValidator(parent));
}

void AddButtonShortcut(QAbstractButton* button, const QKeySequence& shortcut)
{
    QObject::connect(new QShortcut(shortcut, button), &QShortcut::activated, [button]() { button->animateClick(); });
}

bool parseQuicksilverURI(const QUrl &uri, SendCoinsRecipient *out)
{
    // return if URI is not valid or is no quicksilver: URI
    if(!uri.isValid() || uri.scheme() != QString("quicksilver"))
        return false;

    SendCoinsRecipient rv;
    rv.address = uri.path();
    // Trim any following forward slash which may have been added by the OS
    if (rv.address.endsWith("/")) {
        rv.address.truncate(rv.address.length() - 1);
    }
    rv.amount = 0;

    QUrlQuery uriQuery(uri);
    QList<QPair<QString, QString> > items = uriQuery.queryItems();
    for (QList<QPair<QString, QString> >::iterator i = items.begin(); i != items.end(); i++)
    {
        bool fShouldReturnFalse = false;
        if (i->first.startsWith("req-"))
        {
            i->first.remove(0, 4);
            fShouldReturnFalse = true;
        }

        if (i->first == "label")
        {
            rv.label = i->second;
            fShouldReturnFalse = false;
        }
        if (i->first == "message")
        {
            rv.message = i->second;
            fShouldReturnFalse = false;
        }
        else if (i->first == "amount")
        {
            if(!i->second.isEmpty())
            {
                if (!QuicksilverUnits::parse(QuicksilverUnit::HG, i->second, &rv.amount)) {
                    return false;
                }
            }
            fShouldReturnFalse = false;
        }

        if (fShouldReturnFalse)
            return false;
    }
    if(out)
    {
        *out = rv;
    }
    return true;
}

bool parseQuicksilverURI(QString uri, SendCoinsRecipient *out)
{
    QUrl uriInstance(uri);
    return parseQuicksilverURI(uriInstance, out);
}

QString formatQuicksilverURI(const SendCoinsRecipient &info)
{
    bool bech_32 = info.address.startsWith(QString::fromStdString(Params().Bech32HRP() + "1"));

    QString ret = QString("quicksilver:%1").arg(bech_32 ? info.address.toUpper() : info.address);
    int paramCount = 0;

    if (info.amount)
    {
        ret += QString("?amount=%1").arg(QuicksilverUnits::format(QuicksilverUnit::HG, info.amount, false, QuicksilverUnits::SeparatorStyle::NEVER));
        paramCount++;
    }

    if (!info.label.isEmpty())
    {
        QString lbl(QUrl::toPercentEncoding(info.label));
        ret += QString("%1label=%2").arg(paramCount == 0 ? "?" : "&").arg(lbl);
        paramCount++;
    }

    if (!info.message.isEmpty())
    {
        QString msg(QUrl::toPercentEncoding(info.message));
        ret += QString("%1message=%2").arg(paramCount == 0 ? "?" : "&").arg(msg);
        paramCount++;
    }

    return ret;
}

QString HtmlEscape(const QString& str, bool fMultiLine)
{
    QString escaped = str.toHtmlEscaped();
    if(fMultiLine)
    {
        escaped = escaped.replace("\n", "<br>\n");
    }
    return escaped;
}

QString HtmlEscape(const std::string& str, bool fMultiLine)
{
    return HtmlEscape(QString::fromStdString(str), fMultiLine);
}

void copyEntryData(const QAbstractItemView *view, int column, int role)
{
    if(!view || !view->selectionModel())
        return;
    QModelIndexList selection = view->selectionModel()->selectedRows(column);

    if(!selection.isEmpty())
    {
        // Copy first item
        setClipboard(selection.at(0).data(role).toString());
    }
}

QList<QModelIndex> getEntryData(const QAbstractItemView *view, int column)
{
    if(!view || !view->selectionModel())
        return QList<QModelIndex>();
    return view->selectionModel()->selectedRows(column);
}

bool hasEntryData(const QAbstractItemView *view, int column, int role)
{
    QModelIndexList selection = getEntryData(view, column);
    if (selection.isEmpty()) return false;
    return !selection.at(0).data(role).toString().isEmpty();
}

void LoadFont(const QString& file_name)
{
    const int id = QFontDatabase::addApplicationFont(file_name);
    assert(id != -1);
}

QString getDefaultDataDirectory()
{
    return PathToQString(GetDefaultDataDir());
}

QString ExtractFirstSuffixFromFilter(const QString& filter)
{
    QRegularExpression filter_re(QStringLiteral(".* \\(\\*\\.(.*)[ \\)]"), QRegularExpression::InvertedGreedinessOption);
    QString suffix;
    QRegularExpressionMatch m = filter_re.match(filter);
    if (m.hasMatch()) {
        suffix = m.captured(1);
    }
    return suffix;
}

namespace {
QString FileDialogStartDir(const QString& dir)
{
    if (dir.isEmpty()) return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return dir;
}

void PresentFileDialog(QFileDialog* dialog, bool append_suffix, std::function<void(const QString& filename)> done)
{
    dialog->setOption(QFileDialog::DontUseNativeDialog);
    QObject::connect(dialog, &QFileDialog::finished, dialog, [dialog, append_suffix, done = std::move(done)](int result) {
        QString filename;
        if (result == QDialog::Accepted && !dialog->selectedFiles().isEmpty()) {
            filename = QDir::toNativeSeparators(dialog->selectedFiles().front());
            if (append_suffix) {
                const QString selectedSuffix = ExtractFirstSuffixFromFilter(dialog->selectedNameFilter());
                QFileInfo info(filename);
                if (info.suffix().isEmpty() && !selectedSuffix.isEmpty()) {
                    if (!filename.endsWith(".")) filename.append(".");
                    filename.append(selectedSuffix);
                }
            }
        }
        QTimer::singleShot(0, [done, filename]() {
            if (done) done(filename);
        });
    });
    ShowModalDialogAsynchronously(dialog);
}
} // namespace

void getSaveFileName(QWidget *parent, const QString &caption, const QString &dir,
    const QString &filter,
    std::function<void(const QString& filename)> done)
{
    auto* dialog = new QFileDialog(parent, caption, FileDialogStartDir(dir), filter);
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->setObjectName(QStringLiteral("saveFileDialog"));
    PresentFileDialog(dialog, /*append_suffix=*/true, std::move(done));
}

void getOpenFileName(QWidget *parent, const QString &caption, const QString &dir,
    const QString &filter,
    std::function<void(const QString& filename)> done)
{
    auto* dialog = new QFileDialog(parent, caption, FileDialogStartDir(dir), filter);
    dialog->setAcceptMode(QFileDialog::AcceptOpen);
    dialog->setFileMode(QFileDialog::ExistingFile);
    dialog->setObjectName(QStringLiteral("openFileDialog"));
    PresentFileDialog(dialog, /*append_suffix=*/false, std::move(done));
}

void getFont(QWidget *parent, const QFont &initial, std::function<void(const QFont& font, bool ok)> done)
{
    auto* dialog = new QFontDialog(initial, parent);
    dialog->setOption(QFontDialog::DontUseNativeDialog);
    dialog->setObjectName(QStringLiteral("fontDialog"));
    QObject::connect(dialog, &QFontDialog::finished, dialog, [dialog, done = std::move(done)](int result) {
        const bool ok = result == QDialog::Accepted;
        const QFont font = dialog->currentFont();
        QTimer::singleShot(0, [done, font, ok]() {
            if (done) done(font, ok);
        });
    });
    ShowModalDialogAsynchronously(dialog);
}

void getText(QWidget *parent, const QString &title, const QString &label,
    std::function<void(const QString& text, bool ok)> done)
{
    auto* dialog = new QInputDialog(parent);
    dialog->setWindowTitle(title);
    dialog->setLabelText(label);
    dialog->setTextEchoMode(QLineEdit::Normal);
    dialog->setObjectName(QStringLiteral("textInputDialog"));
    QObject::connect(dialog, &QInputDialog::finished, dialog, [dialog, done = std::move(done)](int result) {
        const bool ok = result == QDialog::Accepted;
        const QString text = dialog->textValue();
        QTimer::singleShot(0, [done, text, ok]() {
            if (done) done(text, ok);
        });
    });
    ShowModalDialogAsynchronously(dialog);
}

Qt::ConnectionType blockingGUIThreadConnection()
{
    if(QThread::currentThread() != qApp->thread())
    {
        return Qt::BlockingQueuedConnection;
    }
    else
    {
        return Qt::DirectConnection;
    }
}

bool checkPoint(const QPoint &p, const QWidget *w)
{
    QWidget *atW = QApplication::widgetAt(w->mapToGlobal(p));
    if (!atW) return false;
    return atW->window() == w;
}

bool isObscured(QWidget *w)
{
    return !(checkPoint(QPoint(0, 0), w)
        && checkPoint(QPoint(w->width() - 1, 0), w)
        && checkPoint(QPoint(0, w->height() - 1), w)
        && checkPoint(QPoint(w->width() - 1, w->height() - 1), w)
        && checkPoint(QPoint(w->width() / 2, w->height() / 2), w));
}

void bringToFront(QWidget* w)
{
    if (w) {
        if (QGuiApplication::platformName() == "wayland") {
            auto flags = w->windowFlags();
            w->setWindowFlags(flags|Qt::WindowStaysOnTopHint);
            w->show();
            w->setWindowFlags(flags);
            w->show();
        } else {
#ifdef Q_OS_MACOS
            ForceActivation();
#endif
            // activateWindow() (sometimes) helps with keyboard focus on Windows
            if (w->isMinimized()) {
                w->showNormal();
            } else {
                w->show();
            }
            w->activateWindow();
            w->raise();
        }
    }
}

void handleCloseWindowShortcut(QWidget* w)
{
    QObject::connect(new QShortcut(QKeySequence(QObject::tr("Ctrl+W")), w), &QShortcut::activated, w, &QWidget::close);
}

void openDebugLogfile()
{
    fs::path pathDebug = gArgs.GetDataDirNet() / "debug.log";

    /* Open debug.log with the associated application */
    if (fs::exists(pathDebug))
        QDesktopServices::openUrl(QUrl::fromLocalFile(PathToQString(pathDebug)));
}

bool openQuicksilverConf()
{
    fs::path pathConfig = gArgs.GetConfigFilePath();

    /* Create the file */
    std::ofstream configFile{pathConfig, std::ios_base::app};

    if (!configFile.good())
        return false;

    configFile.close();

    /* Open quicksilver.conf with the associated application */
    bool res = QDesktopServices::openUrl(QUrl::fromLocalFile(PathToQString(pathConfig)));
#ifdef Q_OS_MACOS
    // Workaround for macOS-specific behavior; see #15409.
    if (!res) {
        res = QProcess::startDetached("/usr/bin/open", QStringList{"-t", PathToQString(pathConfig)});
    }
#endif

    return res;
}

ToolTipToRichTextFilter::ToolTipToRichTextFilter(int _size_threshold, QObject *parent) :
    QObject(parent),
    size_threshold(_size_threshold)
{

}

bool ToolTipToRichTextFilter::eventFilter(QObject *obj, QEvent *evt)
{
    if(evt->type() == QEvent::ToolTipChange)
    {
        QWidget *widget = static_cast<QWidget*>(obj);
        QString tooltip = widget->toolTip();
        if(tooltip.size() > size_threshold && !tooltip.startsWith("<qt") && !Qt::mightBeRichText(tooltip))
        {
            // Envelop with <qt></qt> to make sure Qt detects this as rich text
            // Escape the current message as HTML and replace \n by <br>
            tooltip = "<qt>" + HtmlEscape(tooltip, true) + "</qt>";
            widget->setToolTip(tooltip);
            return true;
        }
    }
    return QObject::eventFilter(obj, evt);
}

LabelOutOfFocusEventFilter::LabelOutOfFocusEventFilter(QObject* parent)
    : QObject(parent)
{
}

bool LabelOutOfFocusEventFilter::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::FocusOut) {
        auto focus_out = static_cast<QFocusEvent*>(event);
        if (focus_out->reason() != Qt::PopupFocusReason) {
            auto label = qobject_cast<QLabel*>(watched);
            if (label) {
                auto flags = label->textInteractionFlags();
                label->setTextInteractionFlags(Qt::NoTextInteraction);
                label->setTextInteractionFlags(flags);
            }
        }
    }

    return QObject::eventFilter(watched, event);
}

#ifdef WIN32
fs::path static StartupShortcutPath()
{
    ChainType chain = gArgs.GetChainType();
    if (chain == ChainType::MAIN)
        return GetSpecialFolderPath(CSIDL_STARTUP) / "Quicksilver.lnk";
    return GetSpecialFolderPath(CSIDL_STARTUP) / fs::u8path(strprintf("Quicksilver (%s).lnk", ChainTypeToString(chain)));
}

bool GetStartOnSystemStartup()
{
    // check for Quicksilver*.lnk
    return fs::exists(StartupShortcutPath());
}

bool SetStartOnSystemStartup(bool fAutoStart)
{
    // If the shortcut exists already, remove it for updating
    fs::remove_quiet(StartupShortcutPath());

    if (fAutoStart)
    {
        CoInitialize(nullptr);

        // Get a pointer to the IShellLink interface.
        IShellLinkW* psl = nullptr;
        HRESULT hres = CoCreateInstance(CLSID_ShellLink, nullptr,
            CLSCTX_INPROC_SERVER, IID_IShellLinkW,
            reinterpret_cast<void**>(&psl));

        if (SUCCEEDED(hres))
        {
            // Get the current executable path
            WCHAR pszExePath[MAX_PATH];
            GetModuleFileNameW(nullptr, pszExePath, ARRAYSIZE(pszExePath));

            // Start client minimized
            QString strArgs = "-min";
            // Set the selected chain option
            strArgs += QString::fromStdString(strprintf(" -chain=%s", gArgs.GetChainTypeString()));

            // Set the path to the shortcut target
            psl->SetPath(pszExePath);
            PathRemoveFileSpecW(pszExePath);
            psl->SetWorkingDirectory(pszExePath);
            psl->SetShowCmd(SW_SHOWMINNOACTIVE);
            psl->SetArguments(strArgs.toStdWString().c_str());

            // Query IShellLink for the IPersistFile interface for
            // saving the shortcut in persistent storage.
            IPersistFile* ppf = nullptr;
            hres = psl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&ppf));
            if (SUCCEEDED(hres))
            {
                // Save the link by calling IPersistFile::Save.
                hres = ppf->Save(StartupShortcutPath().wstring().c_str(), TRUE);
                ppf->Release();
                psl->Release();
                CoUninitialize();
                return true;
            }
            psl->Release();
        }
        CoUninitialize();
        return false;
    }
    return true;
}
#elif defined(Q_OS_LINUX)

// Follow the Desktop Application Autostart Spec:
// https://specifications.freedesktop.org/autostart-spec/autostart-spec-latest.html

fs::path static GetAutostartDir()
{
    char* pszConfigHome = getenv("XDG_CONFIG_HOME");
    if (pszConfigHome) return fs::path(pszConfigHome) / "autostart";
    char* pszHome = getenv("HOME");
    if (pszHome) return fs::path(pszHome) / ".config" / "autostart";
    return fs::path();
}

fs::path static GetAutostartFilePath()
{
    ChainType chain = gArgs.GetChainType();
    if (chain == ChainType::MAIN)
        return GetAutostartDir() / "quicksilver.desktop";
    return GetAutostartDir() / fs::u8path(strprintf("quicksilver-%s.desktop", ChainTypeToString(chain)));
}

bool GetStartOnSystemStartup()
{
    std::ifstream optionFile{GetAutostartFilePath()};
    if (!optionFile.good())
        return false;
    // Scan through file for "Hidden=true":
    std::string line;
    while (!optionFile.eof())
    {
        getline(optionFile, line);
        if (line.find("Hidden") != std::string::npos &&
            line.find("true") != std::string::npos)
            return false;
    }
    optionFile.close();

    return true;
}

bool SetStartOnSystemStartup(bool fAutoStart)
{
    if (!fAutoStart)
        fs::remove_quiet(GetAutostartFilePath());
    else
    {
        char pszExePath[MAX_PATH+1];
        ssize_t r = readlink("/proc/self/exe", pszExePath, sizeof(pszExePath));
        if (r == -1 || r > MAX_PATH) {
            return false;
        }
        pszExePath[r] = '\0';

        fs::create_directories(GetAutostartDir());

        std::ofstream optionFile{GetAutostartFilePath(), std::ios_base::out | std::ios_base::trunc};
        if (!optionFile.good())
            return false;
        ChainType chain = gArgs.GetChainType();
        // Write a quicksilver.desktop file to the autostart directory:
        optionFile << "[Desktop Entry]\n";
        optionFile << "Type=Application\n";
        if (chain == ChainType::MAIN)
            optionFile << "Name=Quicksilver\n";
        else
            optionFile << strprintf("Name=Quicksilver (%s)\n", ChainTypeToString(chain));
        optionFile << "Exec=" << pszExePath << strprintf(" -min -chain=%s\n", ChainTypeToString(chain));
        optionFile << "Terminal=false\n";
        optionFile << "Hidden=false\n";
        optionFile.close();
    }
    return true;
}

#else

bool GetStartOnSystemStartup() { return false; }
bool SetStartOnSystemStartup(bool fAutoStart) { return false; }

#endif

void setClipboard(const QString& str)
{
    QClipboard* clipboard = QApplication::clipboard();
    clipboard->setText(str, QClipboard::Clipboard);
    if (clipboard->supportsSelection()) {
        clipboard->setText(str, QClipboard::Selection);
    }
}

fs::path QStringToPath(const QString &path)
{
    return fs::u8path(path.toStdString());
}

QString PathToQString(const fs::path &path)
{
    return QString::fromStdString(path.utf8string());
}

QString NetworkToQString(Network net)
{
    switch (net) {
    case NET_UNROUTABLE: return QObject::tr("Unroutable");
    //: Name of IPv4 network in peer info
    case NET_IPV4: return QObject::tr("IPv4", "network name");
    //: Name of IPv6 network in peer info
    case NET_IPV6: return QObject::tr("IPv6", "network name");
    //: Name of Tor network in peer info
    case NET_ONION: return QObject::tr("Onion", "network name");
    //: Name of I2P network in peer info
    case NET_I2P: return QObject::tr("I2P", "network name");
    //: Name of CJDNS network in peer info
    case NET_CJDNS: return QObject::tr("CJDNS", "network name");
    case NET_INTERNAL: return "Internal";  // should never actually happen
    case NET_MAX: assert(false);
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

QString ConnectionTypeToQString(ConnectionType conn_type, bool prepend_direction)
{
    QString prefix;
    if (prepend_direction) {
        prefix = (conn_type == ConnectionType::INBOUND) ?
                     /*: An inbound connection from a peer. An inbound connection
                         is a connection initiated by a peer. */
                     QObject::tr("Inbound") :
                     /*: An outbound connection to a peer. An outbound connection
                         is a connection initiated by us. */
                     QObject::tr("Outbound") + " ";
    }
    switch (conn_type) {
    case ConnectionType::INBOUND: return prefix;
    //: Peer connection type that relays all network information.
    case ConnectionType::OUTBOUND_FULL_RELAY: return prefix + QObject::tr("Full Relay");
    /*: Peer connection type that relays network information about
        blocks and not transactions or addresses. */
    case ConnectionType::BLOCK_RELAY: return prefix + QObject::tr("Block Relay");
    //: Peer connection type established manually through one of several methods.
    case ConnectionType::MANUAL: return prefix + QObject::tr("Manual");
    //: Short-lived peer connection type that tests the aliveness of known addresses.
    case ConnectionType::FEELER: return prefix + QObject::tr("Feeler");
    //: Short-lived peer connection type that solicits known addresses from a peer.
    case ConnectionType::ADDR_FETCH: return prefix + QObject::tr("Address Fetch");
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

QString formatDurationStr(std::chrono::seconds dur)
{
    const auto d{std::chrono::duration_cast<std::chrono::days>(dur)};
    const auto h{std::chrono::duration_cast<std::chrono::hours>(dur - d)};
    const auto m{std::chrono::duration_cast<std::chrono::minutes>(dur - d - h)};
    const auto s{std::chrono::duration_cast<std::chrono::seconds>(dur - d - h - m)};
    QStringList str_list;
    if (auto d2{d.count()}) str_list.append(QObject::tr("%1 d").arg(d2));
    if (auto h2{h.count()}) str_list.append(QObject::tr("%1 h").arg(h2));
    if (auto m2{m.count()}) str_list.append(QObject::tr("%1 m").arg(m2));
    const auto s2{s.count()};
    if (s2 || str_list.empty()) str_list.append(QObject::tr("%1 s").arg(s2));
    return str_list.join(" ");
}

QString FormatPeerAge(std::chrono::seconds time_connected)
{
    const auto time_now{GetTime<std::chrono::seconds>()};
    const auto age{time_now - time_connected};
    if (age >= 24h) return QObject::tr("%1 d").arg(age / 24h);
    if (age >= 1h) return QObject::tr("%1 h").arg(age / 1h);
    if (age >= 1min) return QObject::tr("%1 m").arg(age / 1min);
    return QObject::tr("%1 s").arg(age / 1s);
}

QString formatServicesStr(quint64 mask)
{
    QStringList strList;

    for (const auto& flag : serviceFlagsToStr(mask)) {
        strList.append(QString::fromStdString(flag));
    }

    if (strList.size())
        return strList.join(", ");
    else
        return QObject::tr("None");
}

QString formatPingTime(std::chrono::microseconds ping_time)
{
    return (ping_time == std::chrono::microseconds::max() || ping_time == 0us) ?
        QObject::tr("N/A") :
        QObject::tr("%1 ms").arg(QString::number((int)(count_microseconds(ping_time) / 1000), 10));
}

QString formatTimeOffset(int64_t time_offset)
{
  return QObject::tr("%1 s").arg(QString::number((int)time_offset, 10));
}

namespace {
/**
 * Pick between a singular string and a plural format taking the count.
 *
 * Both forms are separate translatable strings rather than Qt's "%n unit(s)"
 * numerus idiom, for the reason set out in qt/maturity.cpp: %n is only resolved
 * by a catalogue, and this application installs no translator of its own (see
 * initTranslations() in qt/quicksilver.cpp -- only Qt's own base catalogues are
 * loaded, so that native dialog buttons stay localized). With nothing to resolve
 * it, Qt substitutes the number and leaves the literal "(s)" in place, so the
 * status bar read "3 minute(s) behind" for every user, in every locale.
 */
QString Plural(qint64 count, const QString& singular, const QString& plural_format)
{
    return count == 1 ? singular : plural_format.arg(count);
}
} // namespace

QString formatNiceTimeOffset(qint64 secs)
{
    // Represent time from last generated block in human readable text
    QString timeBehindText;
    const int HOUR_IN_SECONDS = 60*60;
    const int DAY_IN_SECONDS = 24*60*60;
    const int WEEK_IN_SECONDS = 7*24*60*60;
    const int YEAR_IN_SECONDS = 31556952; // Average length of year in Gregorian calendar
    if(secs < 60)
    {
        timeBehindText = Plural(secs, QObject::tr("1 second"), QObject::tr("%1 seconds"));
    }
    else if(secs < 2*HOUR_IN_SECONDS)
    {
        timeBehindText = Plural(secs/60, QObject::tr("1 minute"), QObject::tr("%1 minutes"));
    }
    else if(secs < 2*DAY_IN_SECONDS)
    {
        timeBehindText = Plural(secs/HOUR_IN_SECONDS, QObject::tr("1 hour"), QObject::tr("%1 hours"));
    }
    else if(secs < 2*WEEK_IN_SECONDS)
    {
        timeBehindText = Plural(secs/DAY_IN_SECONDS, QObject::tr("1 day"), QObject::tr("%1 days"));
    }
    else if(secs < YEAR_IN_SECONDS)
    {
        timeBehindText = Plural(secs/WEEK_IN_SECONDS, QObject::tr("1 week"), QObject::tr("%1 weeks"));
    }
    else
    {
        const qint64 years = secs / YEAR_IN_SECONDS;
        const qint64 weeks = (secs % YEAR_IN_SECONDS) / WEEK_IN_SECONDS;
        const QString years_text = Plural(years, QObject::tr("1 year"), QObject::tr("%1 years"));
        timeBehindText = weeks == 0
            ? years_text
            : QObject::tr("%1 and %2")
                  .arg(years_text)
                  .arg(Plural(weeks, QObject::tr("1 week"), QObject::tr("%1 weeks")));
    }
    return timeBehindText;
}

QString formatBytes(uint64_t bytes)
{
    if (bytes < 1'000)
        return QObject::tr("%1 B").arg(bytes);
    if (bytes < 1'000'000)
        return QObject::tr("%1 kB").arg(bytes / 1'000);
    if (bytes < 1'000'000'000)
        return QObject::tr("%1 MB").arg(bytes / 1'000'000);

    return QObject::tr("%1 GB").arg(bytes / 1'000'000'000);
}

qreal calculateIdealFontSize(int width, const QString& text, QFont font, qreal minPointSize, qreal font_size) {
    while(font_size >= minPointSize) {
        font.setPointSizeF(font_size);
        QFontMetrics fm(font);
        if (TextWidth(fm, text) < width) {
            break;
        }
        font_size -= 0.5;
    }
    return font_size;
}

ThemedLabel::ThemedLabel(const PlatformStyle* platform_style, QWidget* parent)
    : QLabel{parent}, m_platform_style{platform_style}
{
    assert(m_platform_style);
}

void ThemedLabel::setThemedPixmap(const QString& image_filename, int width, int height)
{
    m_image_filename = image_filename;
    m_pixmap_width = width;
    m_pixmap_height = height;
    updateThemedPixmap();
}

void ThemedLabel::changeEvent(QEvent* e)
{
    if (e->type() == QEvent::PaletteChange) {
        updateThemedPixmap();
    }

    QLabel::changeEvent(e);
}

void ThemedLabel::updateThemedPixmap()
{
    setPixmap(m_platform_style->SingleColorIcon(m_image_filename).pixmap(m_pixmap_width, m_pixmap_height));
}

ClickableLabel::ClickableLabel(const PlatformStyle* platform_style, QWidget* parent)
    : ThemedLabel{platform_style, parent}
{
}

void ClickableLabel::mouseReleaseEvent(QMouseEvent *event)
{
    Q_EMIT clicked(event->pos());
}

void ClickableProgressBar::mouseReleaseEvent(QMouseEvent *event)
{
    Q_EMIT clicked(event->pos());
}

bool ItemDelegate::eventFilter(QObject *object, QEvent *event)
{
    if (event->type() == QEvent::KeyPress) {
        if (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            Q_EMIT keyEscapePressed();
        }
    }
    return QItemDelegate::eventFilter(object, event);
}

void PolishProgressDialog(QProgressDialog* dialog)
{
#ifdef Q_OS_MACOS
    // Workaround for macOS-only Qt bug; see: QTBUG-65750, QTBUG-70357.
    const int margin = TextWidth(dialog->fontMetrics(), ("X"));
    dialog->resize(dialog->width() + 2 * margin, dialog->height());
#endif
    // QProgressDialog estimates the time the operation will take (based on time
    // for steps), and only shows itself if that estimate is beyond minimumDuration.
    // The default minimumDuration value is 4 seconds, and it could make users
    // think that the GUI is frozen.
    dialog->setMinimumDuration(0);
}

int TextWidth(const QFontMetrics& fm, const QString& text)
{
    return fm.horizontalAdvance(text);
}

void LogQtInfo()
{
#ifdef QT_STATIC
    const std::string qt_link{"static"};
#else
    const std::string qt_link{"dynamic"};
#endif
    LogInfo(HgLog::QT, "Qt %s (%s), plugin=%s\n", qVersion(), qt_link, QGuiApplication::platformName().toStdString());
    const auto static_plugins = QPluginLoader::staticPlugins();
    if (static_plugins.empty()) {
        LogInfo(HgLog::QT, "No static plugins.\n");
    } else {
        LogInfo(HgLog::QT, "Static plugins:\n");
        for (const QStaticPlugin& p : static_plugins) {
            QJsonObject meta_data = p.metaData();
            const std::string plugin_class = meta_data.take(QString("className")).toString().toStdString();
            const int plugin_version = meta_data.take(QString("version")).toInt();
            LogInfo(HgLog::QT, " %s, version %d\n", plugin_class, plugin_version);
        }
    }

    LogInfo(HgLog::QT, "Style: %s / %s\n", QApplication::style()->objectName().toStdString(), QApplication::style()->metaObject()->className());
    LogInfo(HgLog::QT, "System: %s, %s\n", QSysInfo::prettyProductName().toStdString(), QSysInfo::buildAbi().toStdString());
    for (const QScreen* s : QGuiApplication::screens()) {
        LogInfo(HgLog::QT, "Screen: %s %dx%d, pixel ratio=%.1f\n", s->name().toStdString(), s->size().width(), s->size().height(), s->devicePixelRatio());
    }
}

void PopupMenu(QMenu* menu, const QPoint& point, QAction* at_action)
{
    // The qminimal plugin does not provide window system integration.
    if (QApplication::platformName() == "minimal") return;
    menu->popup(point, at_action);
}

QDateTime StartOfDay(const QDate& date)
{
#if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
    return date.startOfDay();
#else
    return QDateTime(date);
#endif
}

bool HasPixmap(const QLabel* label)
{
#if (QT_VERSION >= QT_VERSION_CHECK(5, 15, 0))
    return !label->pixmap(Qt::ReturnByValue).isNull();
#else
    return label->pixmap() != nullptr;
#endif
}

QImage GetImage(const QLabel* label)
{
    if (!HasPixmap(label)) {
        return QImage();
    }

#if (QT_VERSION >= QT_VERSION_CHECK(5, 15, 0))
    return label->pixmap(Qt::ReturnByValue).toImage();
#else
    return label->pixmap()->toImage();
#endif
}

QString MakeHtmlLink(const QString& source, const QString& link)
{
    return QString(source).replace(
        link,
        QLatin1String("<a href=\"") + link + QLatin1String("\">") + link + QLatin1String("</a>"));
}

void PrintSlotException(
    const std::exception* exception,
    const QObject* sender,
    const QObject* receiver)
{
    std::string description = sender->metaObject()->className();
    description += "->";
    description += receiver->metaObject()->className();
    PrintExceptionContinue(exception, description);
}

void ShowModalDialogAsynchronously(QDialog* dialog)
{
    if (auto* box = qobject_cast<QMessageBox*>(dialog)) {
        if (!box->defaultButton()) {
            if (auto* ok = qobject_cast<QPushButton*>(box->button(QMessageBox::Ok))) {
                box->setDefaultButton(ok);
            }
        }
    }
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::ApplicationModal);
    dialog->show();
}

void ShowModalMessageBoxAsynchronously(QMessageBox* box, std::function<void(int result, QAbstractButton* clicked)> done)
{
    QObject::connect(box, &QMessageBox::finished, box, [box, done = std::move(done)](int result) {
        QAbstractButton* clicked = box->clickedButton();
        QTimer::singleShot(0, [done, result, clicked]() {
            if (done) done(result, clicked);
        });
    });
    ShowModalDialogAsynchronously(box);
}

QString VaultDisplayName(const QString& name)
{
    return name.isEmpty() ? "[" + QObject::tr("default vault") + "]" : name;
}

QString VaultDisplayName(const std::string& name)
{
    return VaultDisplayName(QString::fromStdString(name));
}
} // namespace GUIUtil
