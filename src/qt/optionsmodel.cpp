// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <quicksilver-build-config.h> // IWYU pragma: keep

#include <qt/optionsmodel.h>

#include <qt/quicksilverunits.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>

#include <common/args.h>
#include <interfaces/node.h>
#include <mapport.h>
#include <net.h>
#include <netbase.h>
#include <node/caches.h>
#include <node/chainstatemanager_args.h>
#include <util/string.h>
#include <validation.h>    // For DEFAULT_SCRIPTCHECK_THREADS
#include <vault/vault.h> // For DEFAULT_SPEND_ZEROCONF_CHANGE

#include <QDebug>
#include <QLatin1Char>
#include <QSettings>
#include <QStringList>
#include <QVariant>

#include <univalue.h>

const char *DEFAULT_GUI_PROXY_HOST = "127.0.0.1";

static QString GetDefaultProxyAddress();

static QString CurrentDisplayUnitKey()
{
    return QStringLiteral("DisplayQuicksilverUnit");
}

static QuicksilverUnit NormalizeDisplayUnit(QuicksilverUnit unit)
{
    switch (unit) {
    case QuicksilverUnit::HG:
    case QuicksilverUnit::HGS:
        return unit;
    }
    return QuicksilverUnit::HG;
}

static bool UnitFromStoredNumber(int value, QuicksilverUnit& unit)
{
    switch (value) {
    case 0: unit = QuicksilverUnit::HG; return true;
    case 1: unit = QuicksilverUnit::HGS; return true;
    }
    return false;
}

static bool UnitFromStoredName(const QString& value, QuicksilverUnit& unit)
{
    if (value == QStringLiteral("Hg") || value == QStringLiteral("HG")) {
        unit = QuicksilverUnit::HG;
        return true;
    }
    if (value == QStringLiteral("HgS") || value == QStringLiteral("HGS")) {
        unit = QuicksilverUnit::HGS;
        return true;
    }
    return false;
}

static QuicksilverUnit DisplayUnitFromSetting(const QVariant& value)
{
    bool ok{false};
    const int stored_number{value.toInt(&ok)};
    QuicksilverUnit unit{QuicksilverUnit::HG};
    if (ok && UnitFromStoredNumber(stored_number, unit)) {
        return NormalizeDisplayUnit(unit);
    }
    if (UnitFromStoredName(value.toString(), unit)) {
        return NormalizeDisplayUnit(unit);
    }
    if (value.canConvert<QuicksilverUnit>()) {
        return NormalizeDisplayUnit(value.value<QuicksilverUnit>());
    }
    return QuicksilverUnit::HG;
}

/** Map GUI option ID to node setting name. */
static const char* SettingName(OptionsModel::OptionID option)
{
    switch (option) {
    case OptionsModel::DatabaseCache: return "dbcache";
    case OptionsModel::GpuSolverPath: return "cuckatoosolver";
    case OptionsModel::AllowCpuBlockMining: return "allowcpumining";
    case OptionsModel::ThreadsScriptVerif: return "par";
    case OptionsModel::SpendZeroConfChange: return "spendzeroconfchange";
    case OptionsModel::ExternalSignerPath: return "signer";
    case OptionsModel::MapPortNatpmp: return "natpmp";
    case OptionsModel::Listen: return "listen";
    case OptionsModel::Server: return "server";
    case OptionsModel::PruneSize: return "prune";
    case OptionsModel::Prune: return "prune";
    case OptionsModel::ProxyIP: return "proxy";
    case OptionsModel::ProxyPort: return "proxy";
    case OptionsModel::ProxyUse: return "proxy";
    case OptionsModel::ProxyIPTor: return "onion";
    case OptionsModel::ProxyPortTor: return "onion";
    case OptionsModel::ProxyUseTor: return "onion";
    case OptionsModel::Language: return "lang";
    default: throw std::logic_error(strprintf("GUI option %i has no corresponding node setting.", option));
    }
}

static void UpdateRwSetting(interfaces::Node& node, OptionsModel::OptionID option, const std::string& suffix, const common::SettingsValue& value)
{
    node.updateRwSetting(SettingName(option) + suffix, value);
}

//! Convert enabled/size values to Quicksilver -prune setting.
static common::SettingsValue PruneSetting(bool prune_enabled, int prune_size_gb)
{
    assert(!prune_enabled || prune_size_gb >= 1); // PruneSizeGB and ParsePruneSizeGB never return less
    return prune_enabled ? PruneGBtoMiB(prune_size_gb) : 0;
}

//! Get pruning enabled value to show in GUI from Quicksilver -prune setting.
static bool PruneEnabled(const common::SettingsValue& prune_setting)
{
    // -prune=1 setting is manual pruning mode, so disabled for purposes of the gui
    return SettingToInt(prune_setting, 0) > 1;
}

//! Get pruning size value to show in GUI from Quicksilver -prune setting. If
//! pruning is not enabled, just show default recommended pruning size (2GB).
static int PruneSizeGB(const common::SettingsValue& prune_setting)
{
    int value = SettingToInt(prune_setting, 0);
    return value > 1 ? PruneMiBtoGB(value) : DEFAULT_PRUNE_TARGET_GB;
}

//! Parse pruning size value provided by user in GUI or loaded from QSettings
//! (windows registry key or qt .conf file). Smallest value that the GUI can
//! display is 1 GB, so round up if anything less is parsed.
static int ParsePruneSizeGB(const QVariant& prune_size)
{
    return std::max(1, prune_size.toInt());
}

struct ProxySetting {
    bool is_set;
    QString ip;
    QString port;
};
static ProxySetting ParseProxyString(const std::string& proxy);
static std::string ProxyString(bool is_set, QString ip, QString port);

static const QLatin1String fontchoice_str_embedded{"embedded"};
static const QLatin1String fontchoice_str_best_system{"best_system"};
static const QString fontchoice_str_custom_prefix{QStringLiteral("custom, ")};

QString OptionsModel::FontChoiceToString(const OptionsModel::FontChoice& f)
{
    if (std::holds_alternative<FontChoiceAbstract>(f)) {
        if (f == UseBestSystemFont) {
            return fontchoice_str_best_system;
        } else {
            return fontchoice_str_embedded;
        }
    }
    return fontchoice_str_custom_prefix + std::get<QFont>(f).toString();
}

OptionsModel::FontChoice OptionsModel::FontChoiceFromString(const QString& s)
{
    if (s == fontchoice_str_best_system) {
        return FontChoiceAbstract::BestSystemFont;
    } else if (s == fontchoice_str_embedded) {
        return FontChoiceAbstract::EmbeddedFont;
    } else if (s.startsWith(fontchoice_str_custom_prefix)) {
        QFont f;
        f.fromString(s.mid(fontchoice_str_custom_prefix.size()));
        return f;
    } else {
        return FontChoiceAbstract::EmbeddedFont;  // default
    }
}

OptionsModel::OptionsModel(interfaces::Node& node, QObject *parent) :
    QAbstractListModel(parent), m_node{node}
{
}

void OptionsModel::addOverriddenOption(const std::string &option)
{
    strOverriddenByCommandLine += QString::fromStdString(option) + "=" + QString::fromStdString(gArgs.GetArg(option, "")) + " ";
}

// Writes all missing QSettings with their default values
bool OptionsModel::Init(bilingual_str& error)
{
    // Initialize display settings from stored settings.
    language = QString::fromStdString(SettingToString(node().getPersistentSetting("lang"), ""));

    QSettings settings;

    // Ensure restart flag is unset on client startup
    setRestartRequired(false);

    // These are Qt-only settings:

    // Window
    if (!settings.contains("fHideTrayIcon")) {
        settings.setValue("fHideTrayIcon", false);
    }
    m_show_tray_icon = !settings.value("fHideTrayIcon").toBool();
    Q_EMIT showTrayIconChanged(m_show_tray_icon);

    if (!settings.contains("fMinimizeToTray"))
        settings.setValue("fMinimizeToTray", false);
    fMinimizeToTray = settings.value("fMinimizeToTray").toBool() && m_show_tray_icon;

    if (!settings.contains("fMinimizeOnClose"))
        settings.setValue("fMinimizeOnClose", false);
    fMinimizeOnClose = settings.value("fMinimizeOnClose").toBool();

    // Display
    const QString current_display_unit_key{CurrentDisplayUnitKey()};
    if (!settings.contains(current_display_unit_key)) {
        m_display_quicksilver_unit = QuicksilverUnit::HG;
        settings.setValue(current_display_unit_key, QVariant::fromValue(m_display_quicksilver_unit));
    } else {
        m_display_quicksilver_unit = DisplayUnitFromSetting(settings.value(current_display_unit_key));
        settings.setValue(current_display_unit_key, QVariant::fromValue(m_display_quicksilver_unit));
    }

    if (!settings.contains("strThirdPartyTxUrls"))
        settings.setValue("strThirdPartyTxUrls", "");
    strThirdPartyTxUrls = settings.value("strThirdPartyTxUrls", "").toString();

    if (!settings.contains("fCoinControlFeatures"))
        settings.setValue("fCoinControlFeatures", false);
    fCoinControlFeatures = settings.value("fCoinControlFeatures", false).toBool();

    if (!settings.contains("enable_psqt_controls")) {
        settings.setValue("enable_psqt_controls", false);
    }
    m_enable_psqt_controls = settings.value("enable_psqt_controls", false).toBool();

    // These are shared with the core or have a command-line parameter
    // and we want command-line parameters to overwrite the GUI settings.
    for (OptionID option : {DatabaseCache, ThreadsScriptVerif, SpendZeroConfChange, GpuSolverPath, AllowCpuBlockMining, ExternalSignerPath,
                            MapPortNatpmp, Listen, Server, Prune, ProxyUse, ProxyUseTor, Language}) {
        std::string setting = SettingName(option);
        if (node().isSettingIgnored(setting)) addOverriddenOption("-" + setting);
        try {
            getOption(option);
        } catch (const std::exception& e) {
            // This handles exceptions thrown by univalue that can happen if
            // settings in settings.json don't have the expected types.
            error.original = strprintf("Could not read setting \"%s\", %s.", setting, e.what());
            error.translated = tr("Could not read setting \"%1\", %2.").arg(QString::fromStdString(setting), e.what()).toStdString();
            return false;
        }
    }

    // If setting doesn't exist create it with defaults.

    // Main
    if (!settings.contains("strDataDir"))
        settings.setValue("strDataDir", GUIUtil::getDefaultDataDirectory());

    // Display
    if (settings.contains("FontForMoney")) {
        m_font_money = FontChoiceFromString(settings.value("FontForMoney").toString());
    }
    Q_EMIT fontForMoneyChanged(getFontForMoney());

    m_mask_values = settings.value("mask_values", false).toBool();

    return true;
}

/** Helper function to copy contents from one QSettings to another.
 * By using allKeys this also covers nested settings in a hierarchy.
 */
static void CopySettings(QSettings& dst, const QSettings& src)
{
    for (const QString& key : src.allKeys()) {
        dst.setValue(key, src.value(key));
    }
}

/** Back up a QSettings to an ini-formatted file. */
static void BackupSettings(const fs::path& filename, const QSettings& src)
{
    qInfo() << "Backing up GUI settings to" << GUIUtil::PathToQString(filename);
    QSettings dst(GUIUtil::PathToQString(filename), QSettings::IniFormat);
    dst.clear();
    CopySettings(dst, src);
}

void OptionsModel::Reset()
{
    // Backup and reset settings.json
    node().resetSettings();

    QSettings settings;

    // Backup old settings to chain-specific datadir for troubleshooting
    BackupSettings(gArgs.GetDataDirNet() / "guisettings.ini.bak", settings);

    // Save the strDataDir setting
    QString dataDir = GUIUtil::getDefaultDataDirectory();
    dataDir = settings.value("strDataDir", dataDir).toString();

    // Remove all entries from our QSettings object
    settings.clear();

    // Set strDataDir
    settings.setValue("strDataDir", dataDir);

    // Set that this was reset
    settings.setValue("fReset", true);

    // default setting for OptionsModel::StartAtStartup - disabled
    if (GUIUtil::GetStartOnSystemStartup())
        GUIUtil::SetStartOnSystemStartup(false);
}

int OptionsModel::rowCount(const QModelIndex & parent) const
{
    return OptionIDRowCount;
}

static ProxySetting ParseProxyString(const QString& proxy)
{
    static const ProxySetting default_val = {false, DEFAULT_GUI_PROXY_HOST, QString("%1").arg(DEFAULT_GUI_PROXY_PORT)};
    // Handle the case that the setting is not set at all
    if (proxy.isEmpty()) {
        return default_val;
    }
    uint16_t port{0};
    std::string hostname;
    if (SplitHostPort(proxy.toStdString(), port, hostname) && port != 0) {
        // Valid and port within the valid range
        // Check if the hostname contains a colon, indicating an IPv6 address
        if (hostname.find(':') != std::string::npos) {
            hostname = "[" + hostname + "]"; // Wrap IPv6 address in brackets
        }
        return {true, QString::fromStdString(hostname), QString::number(port)};
    } else { // Invalid: return default
        return default_val;
    }
}

static ProxySetting ParseProxyString(const std::string& proxy)
{
    return ParseProxyString(QString::fromStdString(proxy));
}

static std::string ProxyString(bool is_set, QString ip, QString port)
{
    return is_set ? QString(ip + ":" + port).toStdString() : "";
}

static QString GetDefaultProxyAddress()
{
    return QString("%1:%2").arg(DEFAULT_GUI_PROXY_HOST).arg(DEFAULT_GUI_PROXY_PORT);
}

void OptionsModel::SetPruneTargetGB(int prune_target_gb)
{
    const common::SettingsValue cur_value = node().getPersistentSetting("prune");
    const common::SettingsValue new_value = PruneSetting(prune_target_gb > 0, prune_target_gb);

    // Force setting to take effect. It is still safe to change the value at
    // this point because this function is only called after the intro screen is
    // shown, before the node starts.
    node().forceSetting("prune", new_value);

    // Update settings.json if value configured in intro screen is different
    // from saved value. Avoid writing settings.json if quicksilver.conf value
    // doesn't need to be overridden.
    if (PruneEnabled(cur_value) != PruneEnabled(new_value) ||
        PruneSizeGB(cur_value) != PruneSizeGB(new_value)) {
        // Call UpdateRwSetting() instead of setOption() to avoid setting
        // RestartRequired flag
        UpdateRwSetting(node(), Prune, "", new_value);
    }

    // Keep previous pruning size, if pruning was disabled.
    if (PruneEnabled(cur_value)) {
        UpdateRwSetting(node(), Prune, "-prev", PruneEnabled(new_value) ? common::SettingsValue{} : cur_value);
    }
}

// read QSettings values and return them
QVariant OptionsModel::data(const QModelIndex & index, int role) const
{
    if(role == Qt::EditRole)
    {
        return getOption(OptionID(index.row()));
    }
    return QVariant();
}

// write QSettings values
bool OptionsModel::setData(const QModelIndex & index, const QVariant & value, int role)
{
    bool successful = true; /* set to false on parse error */
    if(role == Qt::EditRole)
    {
        successful = setOption(OptionID(index.row()), value);
    }

    Q_EMIT dataChanged(index, index);

    return successful;
}

// NOLINTNEXTLINE(misc-no-recursion)
QVariant OptionsModel::getOption(OptionID option, const std::string& suffix) const
{
    auto setting = [&]{ return node().getPersistentSetting(SettingName(option) + suffix); };

    QSettings settings;
    switch (option) {
    case StartAtStartup:
        return GUIUtil::GetStartOnSystemStartup();
    case ShowTrayIcon:
        return m_show_tray_icon;
    case MinimizeToTray:
        return fMinimizeToTray;
    case MapPortNatpmp:
        return SettingToBool(setting(), DEFAULT_NATPMP);
    case MinimizeOnClose:
        return fMinimizeOnClose;

    // default proxy
    case ProxyUse:
    case ProxyUseTor:
        return ParseProxyString(SettingToString(setting(), "")).is_set;
    case ProxyIP:
    case ProxyIPTor: {
        ProxySetting proxy = ParseProxyString(SettingToString(setting(), ""));
        if (proxy.is_set) {
            return proxy.ip;
        } else if (suffix.empty()) {
            return getOption(option, "-prev");
        } else {
            return ParseProxyString(GetDefaultProxyAddress().toStdString()).ip;
        }
    }
    case ProxyPort:
    case ProxyPortTor: {
        ProxySetting proxy = ParseProxyString(SettingToString(setting(), ""));
        if (proxy.is_set) {
            return proxy.port;
        } else if (suffix.empty()) {
            return getOption(option, "-prev");
        } else {
            return ParseProxyString(GetDefaultProxyAddress().toStdString()).port;
        }
    }

#ifdef ENABLE_VAULT
    case SpendZeroConfChange:
        return SettingToBool(setting(), vault::DEFAULT_SPEND_ZEROCONF_CHANGE);
    case ExternalSignerPath:
        return QString::fromStdString(SettingToString(setting(), ""));
#endif
    case DisplayUnit:
        return QVariant::fromValue(m_display_quicksilver_unit);
    case ThirdPartyTxUrls:
        return strThirdPartyTxUrls;
    case Language:
        return QString::fromStdString(SettingToString(setting(), ""));
    case FontForMoney:
        return QVariant::fromValue(m_font_money);
    case CoinControlFeatures:
        return fCoinControlFeatures;
    case EnablePSQTControls:
        return settings.value("enable_psqt_controls");
    case Prune:
        return PruneEnabled(setting());
    case PruneSize:
        return PruneEnabled(setting()) ? PruneSizeGB(setting()) :
               suffix.empty()          ? getOption(option, "-prev") :
                                         DEFAULT_PRUNE_TARGET_GB;
    case DatabaseCache:
        return qlonglong(SettingToInt(setting(), DEFAULT_DB_CACHE >> 20));
    case GpuSolverPath:
        return QString::fromStdString(SettingToString(setting(), ""));
    case AllowCpuBlockMining:
        return SettingToBool(setting(), false);
    case ShowCpuFallbackWarning:
        return settings.value("show_cpu_fallback_warning", true).toBool();
    case ThreadsScriptVerif:
        return qlonglong(SettingToInt(setting(), DEFAULT_SCRIPTCHECK_THREADS));
    case Listen:
        return SettingToBool(setting(), DEFAULT_LISTEN);
    case Server:
        return SettingToBool(setting(), false);
    case MaskValues:
        return m_mask_values;
    default:
        return QVariant();
    }
}

QFont OptionsModel::getFontForChoice(const FontChoice& fc)
{
    QFont f;
    if (std::holds_alternative<FontChoiceAbstract>(fc)) {
        f = GUIUtil::fixedPitchFont(fc != UseBestSystemFont);
        f.setWeight(QFont::Bold);
    } else {
        f = std::get<QFont>(fc);
    }
    return f;
}

QFont OptionsModel::getFontForMoney() const
{
    return getFontForChoice(m_font_money);
}

// NOLINTNEXTLINE(misc-no-recursion)
bool OptionsModel::setOption(OptionID option, const QVariant& value, const std::string& suffix)
{
    auto changed = [&] { return value.isValid() && value != getOption(option, suffix); };
    auto update = [&](const common::SettingsValue& value) { return UpdateRwSetting(node(), option, suffix, value); };

    bool successful = true; /* set to false on parse error */
    QSettings settings;

    switch (option) {
    case StartAtStartup:
        successful = GUIUtil::SetStartOnSystemStartup(value.toBool());
        break;
    case ShowTrayIcon:
        m_show_tray_icon = value.toBool();
        settings.setValue("fHideTrayIcon", !m_show_tray_icon);
        Q_EMIT showTrayIconChanged(m_show_tray_icon);
        break;
    case MinimizeToTray:
        fMinimizeToTray = value.toBool();
        settings.setValue("fMinimizeToTray", fMinimizeToTray);
        break;
    case MapPortNatpmp: // core option - can be changed on-the-fly
        if (changed()) {
            update(value.toBool());
            node().mapPort(value.toBool());
        }
        break;
    case MinimizeOnClose:
        fMinimizeOnClose = value.toBool();
        settings.setValue("fMinimizeOnClose", fMinimizeOnClose);
        break;

    // default proxy
    case ProxyUse:
        if (changed()) {
            if (suffix.empty() && !value.toBool()) setOption(option, true, "-prev");
            update(ProxyString(value.toBool(), getOption(ProxyIP).toString(), getOption(ProxyPort).toString()));
            if (suffix.empty() && value.toBool()) UpdateRwSetting(node(), option, "-prev", {});
            if (suffix.empty()) setRestartRequired(true);
        }
        break;
    case ProxyIP:
        if (changed()) {
            if (suffix.empty() && !getOption(ProxyUse).toBool()) {
                setOption(option, value, "-prev");
            } else {
                update(ProxyString(true, value.toString(), getOption(ProxyPort).toString()));
            }
            if (suffix.empty() && getOption(ProxyUse).toBool()) setRestartRequired(true);
        }
        break;
    case ProxyPort:
        if (changed()) {
            if (suffix.empty() && !getOption(ProxyUse).toBool()) {
                setOption(option, value, "-prev");
            } else {
                update(ProxyString(true, getOption(ProxyIP).toString(), value.toString()));
            }
            if (suffix.empty() && getOption(ProxyUse).toBool()) setRestartRequired(true);
        }
        break;

    // separate Tor proxy
    case ProxyUseTor:
        if (changed()) {
            if (suffix.empty() && !value.toBool()) setOption(option, true, "-prev");
            update(ProxyString(value.toBool(), getOption(ProxyIPTor).toString(), getOption(ProxyPortTor).toString()));
            if (suffix.empty() && value.toBool()) UpdateRwSetting(node(), option, "-prev", {});
            if (suffix.empty()) setRestartRequired(true);
        }
        break;
    case ProxyIPTor:
        if (changed()) {
            if (suffix.empty() && !getOption(ProxyUseTor).toBool()) {
                setOption(option, value, "-prev");
            } else {
                update(ProxyString(true, value.toString(), getOption(ProxyPortTor).toString()));
            }
            if (suffix.empty() && getOption(ProxyUseTor).toBool()) setRestartRequired(true);
        }
        break;
    case ProxyPortTor:
        if (changed()) {
            if (suffix.empty() && !getOption(ProxyUseTor).toBool()) {
                setOption(option, value, "-prev");
            } else {
                update(ProxyString(true, getOption(ProxyIPTor).toString(), value.toString()));
            }
            if (suffix.empty() && getOption(ProxyUseTor).toBool()) setRestartRequired(true);
        }
        break;

#ifdef ENABLE_VAULT
    case SpendZeroConfChange:
        if (changed()) {
            update(value.toBool());
            setRestartRequired(true);
        }
        break;
    case ExternalSignerPath:
        if (changed()) {
            update(value.toString().toStdString());
            setRestartRequired(true);
        }
        break;
#endif
    case DisplayUnit:
        setDisplayUnit(value);
        break;
    case ThirdPartyTxUrls:
        if (strThirdPartyTxUrls != value.toString()) {
            strThirdPartyTxUrls = value.toString();
            settings.setValue("strThirdPartyTxUrls", strThirdPartyTxUrls);
            setRestartRequired(true);
        }
        break;
    case Language:
        if (changed()) {
            update(value.toString().toStdString());
            setRestartRequired(true);
        }
        break;
    case FontForMoney:
    {
        const auto& new_font = value.value<FontChoice>();
        if (m_font_money == new_font) break;
        settings.setValue("FontForMoney", FontChoiceToString(new_font));
        m_font_money = new_font;
        Q_EMIT fontForMoneyChanged(getFontForMoney());
        break;
    }
    case CoinControlFeatures:
        fCoinControlFeatures = value.toBool();
        settings.setValue("fCoinControlFeatures", fCoinControlFeatures);
        Q_EMIT coinControlFeaturesChanged(fCoinControlFeatures);
        break;
    case EnablePSQTControls:
        m_enable_psqt_controls = value.toBool();
        settings.setValue("enable_psqt_controls", m_enable_psqt_controls);
        break;
    case Prune:
        if (changed()) {
            if (suffix.empty() && !value.toBool()) setOption(option, true, "-prev");
            update(PruneSetting(value.toBool(), getOption(PruneSize).toInt()));
            if (suffix.empty() && value.toBool()) UpdateRwSetting(node(), option, "-prev", {});
            if (suffix.empty()) setRestartRequired(true);
        }
        break;
    case PruneSize:
        if (changed()) {
            if (suffix.empty() && !getOption(Prune).toBool()) {
                setOption(option, value, "-prev");
            } else {
                update(PruneSetting(true, ParsePruneSizeGB(value)));
            }
            if (suffix.empty() && getOption(Prune).toBool()) setRestartRequired(true);
        }
        break;
    case DatabaseCache:
        if (changed()) {
            update(static_cast<int64_t>(value.toLongLong()));
            setRestartRequired(true);
        }
        break;
    case GpuSolverPath:
        if (changed()) {
            update(value.toString().trimmed().toStdString());
            setRestartRequired(true);
        }
        break;
    case AllowCpuBlockMining:
        // Not restart-required. MiningService::Run reads -allowcpumining from
        // the live args once, at the start of each mining thread, and
        // updateRwSetting writes those args in this process. The next arming
        // observes the new value; a thread already running does not. A restart
        // warning here would be the false one.
        if (changed()) {
            update(value.toBool());
        }
        break;
    case ShowCpuFallbackWarning:
        settings.setValue("show_cpu_fallback_warning", value.toBool());
        break;
    case ThreadsScriptVerif:
        if (changed()) {
            update(static_cast<int64_t>(value.toLongLong()));
            setRestartRequired(true);
        }
        break;
    case Listen:
    case Server:
        if (changed()) {
            update(value.toBool());
            setRestartRequired(true);
        }
        break;
    case MaskValues:
        m_mask_values = value.toBool();
        settings.setValue("mask_values", m_mask_values);
        break;
    default:
        break;
    }

    return successful;
}

void OptionsModel::setDisplayUnit(const QVariant& new_unit)
{
    if (new_unit.isNull()) return;
    const QuicksilverUnit normalized_unit{NormalizeDisplayUnit(new_unit.value<QuicksilverUnit>())};
    if (normalized_unit == m_display_quicksilver_unit) return;
    m_display_quicksilver_unit = normalized_unit;
    QSettings settings;
    settings.setValue(CurrentDisplayUnitKey(), QVariant::fromValue(m_display_quicksilver_unit));
    Q_EMIT displayUnitChanged(m_display_quicksilver_unit);
}

void OptionsModel::setRestartRequired(bool fRequired)
{
    QSettings settings;
    return settings.setValue("fRestartRequired", fRequired);
}

bool OptionsModel::isRestartRequired() const
{
    QSettings settings;
    return settings.value("fRestartRequired", false).toBool();
}

bool OptionsModel::hasSigner()
{
    return gArgs.GetArg("-signer", "") != "";
}
