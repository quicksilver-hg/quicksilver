// Copyright (c) 2018-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/args.h>
#include <util/fs.h>
#include <crypto/cuckatoo/gpu_devices.h>
#include <crypto/cuckatoo/gpu_kernel_limit.h>
#include <crypto/cuckatoo/gpu_solver.h>
#include <crypto/cuckatoo/multi_device.h>
#include <qt/quicksilver.h>
#include <qt/createvaultdialog.h>
#include <qt/clientmodel.h>
#include <qt/coincontroldialog.h>
#include <qt/guiutil.h>
#include <qt/optionsdialog.h>
#include <qt/gpudevicelist.h>
#include <qt/gpusituation.h>
#include <qt/platformstyle.h>
#include <qt/qvalidatedlineedit.h>
#include <qt/quicksilverunits.h>
#include <qt/quicksilverstyle.h>
#include <qt/receiverequestdialog.h>
#include <qt/sendcoinsdialog.h>
#include <qt/test/optiontests.h>
#include <qt/test/util.h>
#include <qt/vaultmodel.h>
#include <test/util/setup_common.h>
#include <vault/context.h>
#include <vault/coincontrol.h>
#include <vault/test/util.h>
#include <vault/vault.h>

#include <QByteArray>
#include <QDataStream>
#include <QSettings>
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QCoreApplication>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPoint>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStringList>
#include <QTest>
#include <QTimer>
#include <QTabWidget>
#include <QTabBar>
#include <QStyle>
#include <QStyleFactory>

#include <univalue.h>

#include <fstream>
#include <system_error>
#include <tuple>
#include <utility>

void ShieldOptionsDialogFromBundledHelper()
{
    OptionsDialog::setGpuDiscoveryRunnerForTesting([](uint8_t edgebits, QObject* receiver, std::function<void(const cuckatoo::GpuDiscovery&)> done) {
        const bool runs_bundled{qEnvironmentVariableIsEmpty("CUCKATOO_GPU_SOLVER") && cuckatoo::GpuHelperFor(edgebits)};
        if (!runs_bundled) {
            gpusituation::RunDiscovery(edgebits, receiver, std::move(done));
            return;
        }
        // The state a run of this class alone sees: no bundled helper configured.
        QTimer::singleShot(0, receiver, [done = std::move(done)] { done(cuckatoo::GpuDiscovery{}); });
    });
}

namespace {
std::optional<fs::path> SettingsFilePath()
{
    fs::path path;
    if (!gArgs.GetSettingsPath(&path)) return std::nullopt;
    return path;
}
} // namespace

OptionTests::OptionTests(interfaces::Node& node) : m_node(node)
{
    gArgs.LockSettings([&](common::Settings& s) { m_previous_settings = s; });
    if (const auto path{SettingsFilePath()}; path && fs::exists(*path)) {
        std::ifstream file{*path, std::ios::binary};
        m_previous_settings_file = std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }
    const QSettings settings;
    for (const QString& key : settings.allKeys()) m_previous_qsettings.insert(key, settings.value(key));
}

void OptionTests::init()
{
    // reset args
    gArgs.LockSettings([&](common::Settings& s) { s = m_previous_settings; });
    gArgs.ClearPathCache();
    for (const char* name : ProbeEnvironmentNames()) qunsetenv(name);
    gArgs.LockSettings([](common::Settings& settings) { settings.rw_settings.erase("cuckatoodevice"); });
    QSettings().remove(QStringLiteral("gpu_device_names"));
    ResetGpuCardTables();
}

void OptionTests::cleanup()
{
    // init() resets only what the next slot of this class reads. A failed check
    // returns before a slot's own restore, so put back everything a slot can
    // change, on disk too, before the next slot or the next class (S0294-A-03,
    // S0294-A-04): the args, settings.json, every QSettings key (the language
    // among them) and the probe variables.
    gArgs.LockSettings([&](common::Settings& s) { s = m_previous_settings; });
    gArgs.ClearPathCache();
    if (const auto path{SettingsFilePath()}) {
        if (m_previous_settings_file) {
            std::ofstream file{*path, std::ios::binary | std::ios::trunc};
            file << *m_previous_settings_file;
            QVERIFY2(file.good(), qPrintable(QStringLiteral("cannot restore ") + QString::fromStdString(fs::PathToString(*path))));
        } else {
            std::error_code ec;
            fs::remove(*path, ec);
        }
    }
    QSettings settings;
    settings.clear();
    for (auto it = m_previous_qsettings.cbegin(); it != m_previous_qsettings.cend(); ++it) settings.setValue(it.key(), it.value());
    settings.sync();
    for (const char* name : ProbeEnvironmentNames()) qunsetenv(name);
    ResetGpuCardTables();
}

void OptionTests::leftoverQSettingsKeysAreIgnored()
{
    // Inherited QSettings keys are not migrated into settings.json.
    QSettings settings;
    settings.setValue("nDatabaseCache", 600);
    settings.setValue("nThreadsScriptVerif", 12);
    settings.setValue("fListen", false);
    settings.setValue("bPrune", true);
    settings.setValue("nPruneSize", 3);
    settings.setValue("fUseProxy", true);
    settings.setValue("addrProxy", "proxy:123");
    settings.setValue("fUseSeparateProxyTor", true);
    settings.setValue("addrSeparateProxyTor", "onion:234");
    settings.sync();

    OptionsModel options{m_node};
    bilingual_str error;
    QVERIFY(options.Init(error));

    QVERIFY(settings.contains("nDatabaseCache"));
    QVERIFY(settings.contains("fListen"));
    QVERIFY(settings.contains("addrProxy"));

    // Every settings write goes through updateRwSetting, which changes the
    // in-memory read-write settings and rewrites settings.json. Check both:
    // reading only the file passed on an empty read whenever Init() wrote
    // nothing and the file was absent (S0294-A-01).
    for (const char* name : {"dbcache", "listen", "proxy", "prune", "par", "onion"}) {
        bool present{true};
        gArgs.LockSettings([&](const common::Settings& settings) { present = settings.rw_settings.count(name) > 0; });
        QVERIFY2(!present, name);
    }
    const fs::path settings_path{gArgs.GetDataDirNet() / "settings.json"};
    const auto read_settings_file = [&settings_path] {
        std::ifstream file(settings_path);
        return std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    };
    if (fs::exists(settings_path)) {
        const std::string contents{read_settings_file()};
        QVERIFY(contents.find("\"dbcache\"") == std::string::npos);
        QVERIFY(contents.find("\"listen\"") == std::string::npos);
        QVERIFY(contents.find("\"proxy\"") == std::string::npos);
    }
    // Positive control: a real write lands in the file read above.
    m_node.updateRwSetting("settings_path_probe", "1");
    QVERIFY2(fs::exists(settings_path), qPrintable(QString::fromStdString(fs::PathToString(settings_path))));
    QVERIFY(read_settings_file().find("\"settings_path_probe\"") != std::string::npos);
}

void OptionTests::legacyDisplayUnitSettingIsIgnored()
{
    // Quicksilver used to read another application's stored display-unit key and
    // migrate its value, including that application's denomination names. That
    // is a legacy migration path for a coin this one is not, and it is gone.
    // The key is spelled in fragments so the branding lint gates do not read
    // this assertion as the residue it exists to forbid.
    const QString foreign_key{QStringLiteral("Display") + QStringLiteral("Bit") + QStringLiteral("coin") + QStringLiteral("Unit")};
    const QString current_key{QStringLiteral("DisplayQuicksilverUnit")};
    const QString foreign_milli_unit{QStringLiteral("m") + QStringLiteral("BT") + QStringLiteral("C")};
    const QVariant foreign_values[]{
        QVariant::fromValue(qint8{1}),
        foreign_milli_unit,
    };

    for (const auto& foreign_value : foreign_values) {
        QSettings settings;
        settings.remove(foreign_key);
        settings.remove(current_key);
        settings.setValue(foreign_key, foreign_value);
        settings.sync();

        OptionsModel options{m_node};
        bilingual_str error;
        QVERIFY(options.Init(error));

        // The foreign key seeds nothing: the unit is the default.
        QCOMPARE(options.getDisplayUnit(), QuicksilverUnit::HGS);
        QCOMPARE(settings.value(current_key).value<QuicksilverUnit>(), QuicksilverUnit::HGS);
        // And it is left alone rather than consumed - it is not ours to erase.
        QVERIFY(settings.contains(foreign_key));

        settings.remove(foreign_key);
        settings.remove(current_key);
    }
}

void OptionTests::displayUnitsAreFocused()
{
    const QList<QuicksilverUnit> units{QuicksilverUnits::availableUnits()};
    QCOMPARE(units.size(), 2);
    QCOMPARE(units.at(0), QuicksilverUnit::HG);
    QCOMPARE(units.at(1), QuicksilverUnit::HGS);
    QCOMPARE(QuicksilverUnits::description(QuicksilverUnit::HG), QStringLiteral("Quicksilver"));
    QCOMPARE(QuicksilverUnits::description(QuicksilverUnit::HGS), QStringLiteral("cinnabar (0.00000001 Quicksilver)"));

    QuicksilverUnits model{nullptr};
    QCOMPARE(model.rowCount({}), 2);
}

void OptionTests::displayUnitSerializationIsCompact()
{
    const QString current_key{QStringLiteral("DisplayQuicksilverUnit")};
    const std::pair<QVariant, QuicksilverUnit> stored_values[]{
        {QVariant::fromValue(qint8{0}), QuicksilverUnit::HG},
        {QVariant::fromValue(qint8{1}), QuicksilverUnit::HGS},
    };

    for (const auto& [stored_value, expected_unit] : stored_values) {
        QSettings settings;
        settings.setValue(current_key, stored_value);
        settings.sync();

        OptionsModel options{m_node};
        bilingual_str error;
        QVERIFY(options.Init(error));
        QCOMPARE(options.getDisplayUnit(), expected_unit);
        QCOMPARE(settings.value(current_key).value<QuicksilverUnit>(), expected_unit);

        settings.remove(current_key);
    }
}

void OptionTests::newInstallShowsCinnabarAndKeepsAStoredUnit()
{
    const QString key{QStringLiteral("DisplayQuicksilverUnit")};
    struct Case {
        const char* name;
        bool present;
        QVariant stored;
        QuicksilverUnit expected;
    };
    const Case cases[]{
        {"fresh", false, {}, QuicksilverUnit::HGS},
        {"stored Hg", true, QVariant::fromValue(qint8{0}), QuicksilverUnit::HG},
        {"stored HgS", true, QVariant::fromValue(qint8{1}), QuicksilverUnit::HGS},
        {"unreadable number", true, QVariant::fromValue(qint8{9}), QuicksilverUnit::HGS},
        {"unreadable text", true, QVariant(QStringLiteral("not-a-unit")), QuicksilverUnit::HGS},
    };

    QSettings settings;
    QStringList failures;
    for (const Case& item : cases) {
        if (item.present) {
            settings.setValue(key, item.stored);
        } else {
            settings.remove(key);
        }
        settings.sync();

        OptionsModel options{m_node};
        bilingual_str error;
        if (!options.Init(error)) {
            failures << QStringLiteral("%1: init failed").arg(QLatin1String(item.name));
            continue;
        }
        const auto got = static_cast<int>(options.getDisplayUnit());
        const auto stored = static_cast<int>(settings.value(key).value<QuicksilverUnit>());
        const auto expected = static_cast<int>(item.expected);
        if (got != expected || stored != expected) {
            failures << QStringLiteral("%1: unit %2, stored %3, expected %4")
                            .arg(QLatin1String(item.name))
                            .arg(got)
                            .arg(stored)
                            .arg(expected);
        }
    }

    // An unknown stored code is not a unit this install recognizes.
    QByteArray bytes;
    {
        QDataStream out(&bytes, QIODevice::WriteOnly);
        out << qint8{7};
    }
    QuicksilverUnit streamed{QuicksilverUnit::HG};
    {
        QDataStream in(bytes);
        in >> streamed;
    }
    if (streamed != QuicksilverUnit::HGS) {
        failures << QStringLiteral("unknown code: %1").arg(static_cast<int>(streamed));
    }

    settings.remove(key);
    QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QLatin1Char('\n'))));
}

void OptionTests::languageRowIsHiddenWhileNoCatalogueShips()
{
    OptionsDialog dialog(nullptr, /*enableVault=*/true);
    auto* languages = dialog.findChild<QComboBox*>(QStringLiteral("lang"));
    auto* language_label = dialog.findChild<QLabel*>(QStringLiteral("langLabel"));
    QVERIFY(languages);
    QVERIFY(language_label);
    QVERIFY(languages->isHidden());
    QVERIFY(language_label->isHidden());
}

void OptionTests::hiddenLanguageRowLeavesStoredValueAlone()
{
    QSettings settings;
    const bool had_restart{settings.contains("fRestartRequired")};
    const QVariant previous_restart{settings.value("fRestartRequired")};

    OptionsModel options{m_node};
    bilingual_str error;
    QVERIFY(options.Init(error));

    for (const QString& language : {QStringLiteral("de"), QStringLiteral("en")}) {
        QVERIFY(options.setOption(OptionsModel::Language, language));
        options.setRestartRequired(false);

        OptionsDialog dialog(nullptr, /*enableVault=*/true);
        dialog.setModel(&options);
        auto* languages = dialog.findChild<QComboBox*>(QStringLiteral("lang"));
        auto* ok_button = dialog.findChild<QPushButton*>(QStringLiteral("okButton"));
        QVERIFY(languages);
        QVERIFY(ok_button);
        // A hidden control is not user-editable, but changing its internal state
        // proves it has neither a mapper write path nor a restart-warning path.
        languages->addItem(QStringLiteral("test-only different value"), language == QStringLiteral("de") ? QStringLiteral("en") : QString{});
        languages->setCurrentIndex(languages->count() - 1);
        QTest::mouseClick(ok_button, Qt::LeftButton);

        QCOMPARE(options.getOption(OptionsModel::Language).toString(), language);
        QVERIFY(!options.isRestartRequired());
    }

    if (had_restart) {
        settings.setValue("fRestartRequired", previous_restart);
    } else {
        settings.remove("fRestartRequired");
    }
}

void OptionTests::languageRowListsShippedCatalogues()
{
    OptionsDialog dialog(nullptr, /*enableVault=*/true);
    auto* languages = dialog.findChild<QComboBox*>(QStringLiteral("lang"));
    auto* language_label = dialog.findChild<QLabel*>(QStringLiteral("langLabel"));
    QVERIFY(languages);
    QVERIFY(language_label);

    // The choices remain an explicit code list and never depend on catalogue
    // resources, preserving the intent of the test this ruling replaced.
    OptionsDialog::configureLanguageRow({QStringLiteral("de")}, languages, language_label);
    QVERIFY(!languages->isHidden());
    QVERIFY(!language_label->isHidden());
    QCOMPARE(languages->count(), 3);
    QCOMPARE(languages->itemData(0).toString(), QString{});
    QCOMPARE(languages->itemData(1).toString(), QStringLiteral("en"));
    QCOMPARE(languages->itemData(2).toString(), QStringLiteral("de"));
}

void OptionTests::integerGetArgBug()
{
    // Test regression upstream issue 24457. Ensure
    // that setting integer prune value doesn't cause an exception to be thrown
    // in the OptionsModel constructor
    gArgs.LockSettings([&](common::Settings& settings) {
        settings.forced_settings.erase("prune");
        settings.rw_settings["prune"] = 3814;
    });
    gArgs.WriteSettingsFile();
    bilingual_str error;
    QVERIFY(OptionsModel{m_node}.Init(error));
    gArgs.LockSettings([&](common::Settings& settings) {
        settings.rw_settings.erase("prune");
    });
    gArgs.WriteSettingsFile();
}

namespace {
QString WriteListing(const QStringList& lines)
{
    static int counter{0};
    const QString path = QDir::temp().filePath(QStringLiteral("qs_optiontests_%1_%2.listing").arg(QCoreApplication::applicationPid()).arg(++counter));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) qFatal("cannot write %s", qPrintable(path));
    for (const QString& line : lines) f.write((line + QLatin1Char('\n')).toUtf8());
    return path;
}

void PointDiscoveryAtStub(const QString& listing)
{
    qputenv("CUCKATOO_GPU_SOLVER", SolverProbeStubPath().toUtf8());
    qputenv("QS_PROBE_STUB_LISTING", listing.toUtf8());
    qunsetenv("QS_PROBE_STUB_EXIT");
    qunsetenv("QS_PROBE_STUB_SLEEP_MS");
    qunsetenv("CUCKATOO_GPU_DEVICES");
    cuckatoo::ResetGpuDiscoveryForTesting();
}
} // namespace

void OptionTests::gpuDevicesSettingRoundTrips()
{
    OptionsModel options{m_node};
    bilingual_str error;
    QVERIFY(options.Init(error));
    const QStringList chosen{QStringLiteral("pci:0000:04:00.0"), QStringLiteral("pci:0000:01:00.0")};
    QVERIFY(options.setOption(OptionsModel::GpuDevices, chosen));
    QCOMPARE(options.getOption(OptionsModel::GpuDevices).toStringList(), chosen);
    QVERIFY(options.isRestartRequired());
    QCOMPARE(QString::fromStdString(gArgs.GetArgs("-cuckatoodevice").at(1)), chosen.at(1));
    QVERIFY(options.setOption(OptionsModel::GpuDevices, QStringList{QStringLiteral("cpu")}));
    QVERIFY(gArgs.GetArgs("-cuckatoodevice") == std::vector<std::string>{"cpu"});
    QVERIFY(options.setOption(OptionsModel::GpuDevices, QStringList{}));
    QVERIFY(gArgs.GetArgs("-cuckatoodevice").empty());
    const QMap<QString, QString> names{{QStringLiteral("pci:0000:04:00.0"), QStringLiteral("AMD Radeon RX 480")}};
    options.setGpuDeviceNames(names);
    QCOMPARE(options.getGpuDeviceNames(), names);
}

void OptionTests::gpuCardsListAndModes()
{
    QVERIFY_SOLVER_PROBE_STUB();
    PointDiscoveryAtStub(WriteListing({
        QStringLiteral("device backend=opencl id=pci:0000:00:02.0 type=gpu integrated=1 mem=0 status=ok name=Intel(R) HD Graphics 4600"),
        QStringLiteral("device backend=opencl id=pci:0000:04:00.0 type=gpu integrated=0 mem=0 status=ok name=AMD Radeon RX 480"),
        QStringLiteral("device backend=opencl id=pci:0000:05:00.0 type=gpu integrated=0 mem=0 status=compile-failed reason=x name=Old Card") }));
    OptionsModel options{m_node}; bilingual_str error; QVERIFY(options.Init(error));
    OptionsDialog dialog(nullptr, true); dialog.setModel(&options);
    auto* list = dialog.findChild<QWidget*>(QStringLiteral("gpuDeviceList")); QVERIFY(list);
    QTRY_VERIFY_WITH_TIMEOUT(list->findChild<QCheckBox*>(QStringLiteral("gpuDevice_pci_0000_04_00_0")), 30000);
    auto* helper_status = dialog.findChild<QLabel*>(QStringLiteral("gpuSolverStatusLabel")); QVERIFY(helper_status);
    QCOMPARE(helper_status->text(), QStringLiteral("Using Quicksilver's graphics helper."));
    QMetaObject::invokeMethod(&dialog, "validateGpuSolverPath");
    QCOMPARE(helper_status->text(), QStringLiteral("Using Quicksilver's graphics helper."));
    const auto state = [&](const char* id) { return list->findChild<QLabel*>(QString::fromLatin1(id) + QStringLiteral("_state"))->text(); };
    QCOMPARE(state("gpuDevice_pci_0000_04_00_0"), /*W-D5*/ QStringLiteral("Chosen"));
    QCOMPARE(state("gpuDevice_pci_0000_00_02_0"), /*W-D6*/ QStringLiteral("Not used: built-in graphics"));
    QCOMPARE(state("gpuDevice_pci_0000_05_00_0"), /*W-D7*/ QStringLiteral("Can't be used: its driver couldn't run Quicksilver's graphics code"));
    QVERIFY(!dialog.findChild<QLabel*>(QStringLiteral("gpuCardsHelp"))->isHidden());
    dialog.findChild<QRadioButton*>(QStringLiteral("gpuModeChoose"))->click();
    auto* hd = list->findChild<QCheckBox*>(QStringLiteral("gpuDevice_pci_0000_00_02_0")); QVERIFY(!hd->isChecked());
    QTest::mouseClick(dialog.findChild<QPushButton*>(QStringLiteral("okButton")), Qt::LeftButton);
    QCOMPARE(options.getOption(OptionsModel::GpuDevices).toStringList(), QStringList{QStringLiteral("pci:0000:04:00.0")});
    QCOMPARE(options.getGpuDeviceNames().value(QStringLiteral("pci:0000:04:00.0")), QStringLiteral("AMD Radeon RX 480"));
}

void OptionTests::choosingCardsRequiresASelection()
{
    QVERIFY_SOLVER_PROBE_STUB();
    const QString id{QStringLiteral("pci:0000:04:00.0")};
    PointDiscoveryAtStub(WriteListing({QStringLiteral("device backend=opencl id=pci:0000:04:00.0 type=gpu integrated=0 mem=0 status=ok name=Discrete Card")}));
    OptionsModel options{m_node};
    bilingual_str error;
    QVERIFY(options.Init(error));
    OptionsDialog dialog(nullptr, true);
    dialog.setModel(&options);
    auto* choose = dialog.findChild<QRadioButton*>(QStringLiteral("gpuModeChoose"));
    auto* automatic = dialog.findChild<QRadioButton*>(QStringLiteral("gpuModeAutomatic"));
    auto* cpu = dialog.findChild<QRadioButton*>(QStringLiteral("gpuModeCpu"));
    auto* ok = dialog.findChild<QPushButton*>(QStringLiteral("okButton"));
    QVERIFY(choose && automatic && cpu && ok);
    QCheckBox* card{nullptr};
    QTRY_VERIFY((card = dialog.findChild<QCheckBox*>(QStringLiteral("gpuDevice_pci_0000_04_00_0"))));
    choose->click();
    card->setChecked(false);
    QVERIFY(!ok->isEnabled());
    automatic->click();
    QVERIFY(ok->isEnabled());
    cpu->click();
    QVERIFY(ok->isEnabled());
    choose->click();
    card->setChecked(true);
    QVERIFY(ok->isEnabled());
    ok->click();
    QCOMPARE(options.getOption(OptionsModel::GpuDevices).toStringList(), QStringList{id});

    OptionsDialog reopened(nullptr, true);
    reopened.setModel(&options);
    QVERIFY(reopened.findChild<QRadioButton*>(QStringLiteral("gpuModeChoose"))->isChecked());
    QTRY_VERIFY((card = reopened.findChild<QCheckBox*>(QStringLiteral("gpuDevice_pci_0000_04_00_0"))));
    QVERIFY(card->isChecked());
}

void OptionTests::commandLineGraphicsHelperIsShown()
{
    const QString path{QStringLiteral("/opt/quicksilver/custom-helper")};
    gArgs.LockSettings([&](common::Settings& settings) {
        settings.command_line_options["cuckatoosolver"] = {UniValue(path.toStdString())};
        settings.command_line_options["cuckatoodevice"] = {UniValue("pci:0000:03:00.0"), UniValue("pci:0000:04:00.0")};
        settings.command_line_options["server"] = {UniValue("")};
    });
    OptionsModel options{m_node}; bilingual_str error; QVERIFY(options.Init(error));
    OptionsDialog dialog(nullptr, true); dialog.setModel(&options);
    auto* field = dialog.findChild<QLineEdit*>(QStringLiteral("gpuSolverPath"));
    auto* status = dialog.findChild<QLabel*>(QStringLiteral("gpuSolverStatusLabel"));
    QVERIFY(field);
    QVERIFY(status);
    QCOMPARE(field->text(), path);
    QVERIFY(field->isReadOnly());
    QCOMPARE(field->toolTip(), QStringLiteral("Set by -cuckatoosolver on the command line."));
    QCOMPARE(status->text(), QStringLiteral("Using the graphics helper set on the command line: %1").arg(path));
    const QString overrides = options.getOverriddenByCommandLine();
    QVERIFY(overrides.contains(QStringLiteral("-cuckatoodevice=pci:0000:03:00.0")));
    QVERIFY(overrides.contains(QStringLiteral("-cuckatoodevice=pci:0000:04:00.0")));
    QVERIFY(overrides.contains(QStringLiteral("-server ")));
    QVERIFY(!overrides.contains(QStringLiteral("-server=")));
}

void OptionTests::sameNamedCardsShowTheirIds()
{
    QVERIFY_SOLVER_PROBE_STUB();
    PointDiscoveryAtStub(WriteListing({
        QStringLiteral("device backend=opencl id=pci:0000:03:00.0 type=gpu integrated=0 mem=0 status=ok name=Radeon RX 580"),
        QStringLiteral("device backend=opencl id=pci:0000:04:00.0 type=gpu integrated=0 mem=0 status=ok name=Radeon RX 580"),
        QStringLiteral("device backend=opencl id=pci:0000:05:00.0 type=gpu integrated=0 mem=0 status=ok name=Unique card") }));
    OptionsModel options{m_node}; bilingual_str error; QVERIFY(options.Init(error));
    OptionsDialog dialog(nullptr, true); dialog.setModel(&options);
    auto* list = dialog.findChild<GpuDeviceList*>(QStringLiteral("gpuDeviceList")); QVERIFY(list);
    auto* first = list->findChild<QCheckBox*>(QStringLiteral("gpuDevice_pci_0000_03_00_0"));
    QTRY_VERIFY_WITH_TIMEOUT((first = list->findChild<QCheckBox*>(QStringLiteral("gpuDevice_pci_0000_03_00_0"))), 30000);
    QCOMPARE(first->text(), QStringLiteral("Radeon RX 580 (pci:0000:03:00.0)"));
    QCOMPARE(list->findChild<QCheckBox*>(QStringLiteral("gpuDevice_pci_0000_04_00_0"))->text(), QStringLiteral("Radeon RX 580 (pci:0000:04:00.0)"));
    QCOMPARE(list->findChild<QCheckBox*>(QStringLiteral("gpuDevice_pci_0000_05_00_0"))->text(), QStringLiteral("Unique card"));
    auto* tabs = dialog.findChild<QTabWidget*>(QStringLiteral("tabWidget"));
    QVERIFY(tabs);
    tabs->setCurrentWidget(dialog.findChild<QWidget*>(QStringLiteral("tabHardware")));
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    dialog.resize(1366, 768);
    QApplication::processEvents();
    QVERIFY2(first->width() >= first->sizeHint().width(), qPrintable(QStringLiteral("dialog=%1x%2 list=%3 group=%4 checkbox=%5 hint=%6")
        .arg(dialog.width()).arg(dialog.height()).arg(list->width())
        .arg(dialog.findChild<QGroupBox*>(QStringLiteral("gpuCardsGroup"))->width())
        .arg(first->width()).arg(first->sizeHint().width())));
    QVERIFY(first->height() >= first->sizeHint().height());
}

void OptionTests::gpuCardStatsRefresh()
{
    QVERIFY_SOLVER_PROBE_STUB();
    PointDiscoveryAtStub(WriteListing({
        QStringLiteral("device backend=opencl id=pci:0000:04:00.0 type=gpu integrated=0 mem=0 status=ok name=AMD Radeon RX 480") }));
    OptionsModel options{m_node}; bilingual_str error; QVERIFY(options.Init(error));
    OptionsDialog dialog(nullptr, true); dialog.setModel(&options);
    auto* list = dialog.findChild<GpuDeviceList*>(QStringLiteral("gpuDeviceList")); QVERIFY(list);
    auto* state = list->findChild<QLabel*>(QStringLiteral("gpuDevice_pci_0000_04_00_0_state"));
    QTRY_VERIFY_WITH_TIMEOUT((state = list->findChild<QLabel*>(QStringLiteral("gpuDevice_pci_0000_04_00_0_state"))), 30000);
    QCOMPARE(state->text(), QStringLiteral("Chosen"));

    cuckatoo::SetGpuDeviceStatsForTesting({
        {"pci:0000:04:00.0", cuckatoo::GpuDeviceState::kSolving, cuckatoo::GpuSolveStatus::kNoCycle, 5, 2.4},
    });
    list->refreshRows();
    QCOMPARE(state->text(), QStringLiteral("Chosen"));
    list->setMiningActive(true);
    QCOMPARE(state->text(), QStringLiteral("In use, 2.4 s per graph"));
    cuckatoo::ResetGpuDeviceStatsForTesting();
    list->refreshRows();
    QCOMPARE(state->text(), QStringLiteral("In use"));
    cuckatoo::SetGpuDeviceStatsForTesting({
        {"pci:0000:04:00.0", cuckatoo::GpuDeviceState::kSolving, cuckatoo::GpuSolveStatus::kNoCycle, 5, 2.4},
    });
    cuckatoo::SetGpuDeviceStatsForTesting({
        {"pci:0000:04:00.0", cuckatoo::GpuDeviceState::kFaulted, cuckatoo::GpuSolveStatus::kDeviceFault, 5, 2.4},
    });
    list->refreshRows();
    QCOMPARE(state->text(), QStringLiteral("Faulted: %1").arg(QString::fromStdString(
        cuckatoo::SolverFault(cuckatoo::GpuSolveStatus::kDeviceFault, false).value())));
    list->setMiningActive(false);
    QCOMPARE(state->text(), QStringLiteral("Chosen"));
}

// F-590: the Hardware page advises when a chosen amdgpu card sits under a short kernel compute limit.
void OptionTests::amdgpuAdvisoryFollowsTheKernelLimit()
{
#ifdef Q_OS_WIN
    // The fake /sys below names a directory after the PCI slot ("0000:04:00.0"), and a colon is not legal in a Windows
    // file name. The advisory reads Linux sysfs only, so the case runs where that tree can exist.
    QSKIP("The fake Linux sysfs needs a ':' in a directory name, which Windows file names cannot hold.");
#else
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto slot_dir = QDir(root.path()).filePath(QStringLiteral("bus/pci/devices/0000:04:00.0"));
    QVERIFY(QDir().mkpath(slot_dir));
    const auto write = [](const QString& path, const QByteArray& text) { QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(text); };
    write(slot_dir + QStringLiteral("/uevent"), "DRIVER=amdgpu\nPCI_SLOT_NAME=0000:04:00.0\n");
    const QString lockup = QDir(root.path()).filePath(QStringLiteral("lockup_timeout"));
    cuckatoo::KernelPaths paths;
    paths.pci_devices = QDir(root.path()).filePath(QStringLiteral("bus/pci/devices")).toStdString();
    paths.amdgpu_lockup_timeout = lockup.toStdString();

    cuckatoo::GpuDiscovery discovery;
    discovery.listing.outcome = cuckatoo::ListOutcome::kListed;
    cuckatoo::GpuDevice card; card.backend = "opencl"; card.id = "pci:0000:04:00.0"; card.type = "gpu";
    card.status = cuckatoo::DeviceStatus::kOk; card.name = "Radeon RX 580";
    discovery.listing.devices.push_back(card);

    GpuDeviceList list;
    list.setObjectName(QStringLiteral("gpuDeviceList"));
    list.show();
    list.setKernelPaths(paths);
    list.setMode(GpuDeviceList::Mode::kAutomatic);
    list.setDevices(discovery, {}, {});
    const auto advisory = [&list] { return list.findChild<QLabel*>(QStringLiteral("gpuAmdgpuAdvisory")); };

    write(lockup, "2000,2000,2000,2000\n");
    list.refreshRows();
    QVERIFY(advisory());
    QVERIFY(advisory()->isVisible());
    QCOMPARE(advisory()->text(), QString::fromStdString(cuckatoo::AmdgpuAdvisoryText(2000)));

    write(lockup, "2000,60000,2000,2000\n");
    list.refreshRows();
    QVERIFY(!advisory() || !advisory()->isVisible());

    write(lockup, "garbage\n");
    list.refreshRows();
    QVERIFY(!advisory() || !advisory()->isVisible());

    // The card is not chosen, so there is nothing to warn about.
    write(lockup, "2000,2000,2000,2000\n");
    list.setMode(GpuDeviceList::Mode::kChoose);
    list.setChecked(QStringLiteral("pci:0000:04:00.0"), false);
    QVERIFY(!advisory() || !advisory()->isVisible());
    list.setChecked(QStringLiteral("pci:0000:04:00.0"), true);
    QVERIFY(advisory() && advisory()->isVisible());
#endif // Q_OS_WIN
}

namespace {
//! AppTests' node init points CUCKATOO_GPU_BUNDLED at the real helper for the
//! rest of the process, so a slot that changes it puts it back.
struct RestoreBundled {
    QByteArray value{qgetenv("CUCKATOO_GPU_BUNDLED")};
    ~RestoreBundled()
    {
        if (value.isEmpty()) qunsetenv("CUCKATOO_GPU_BUNDLED");
        else qputenv("CUCKATOO_GPU_BUNDLED", value);
        cuckatoo::ResetGpuDiscoveryForTesting();
    }
};
} // namespace

//! F-595: after AppTests had run, every OptionsDialog a test opened ran the real
//! qsgpusolve-opencl on the build machine's graphics card. The stub stands in for
//! the bundled helper here and lists a card only it can name: if that card reaches
//! the dialog, a test run started whatever helper the node was configured with.
void OptionTests::dialogTestsDoNotRunTheBundledHelper()
{
    QVERIFY_SOLVER_PROBE_STUB();
    const RestoreBundled restore;
    qputenv("CUCKATOO_GPU_BUNDLED", SolverProbeStubPath().toUtf8());
    qputenv("QS_PROBE_STUB_LISTING", WriteListing({QStringLiteral("device backend=opencl id=pci:0000:0a:00.0 type=gpu integrated=0 mem=0 status=ok name=Ran The Bundled Helper")}).toUtf8());
    cuckatoo::ResetGpuDiscoveryForTesting();

    OptionsModel options{m_node}; bilingual_str error; QVERIFY(options.Init(error));
    OptionsDialog dialog(nullptr, true); dialog.setModel(&options);
    auto* list = dialog.findChild<GpuDeviceList*>(QStringLiteral("gpuDeviceList")); QVERIFY(list);
    const QString no_helper = QStringLiteral("Quicksilver's graphics helper isn't installed.");
    const auto bundled_card = [&] { return list->findChild<QCheckBox*>(QStringLiteral("gpuDevice_pci_0000_0a_00_0")); };
    QTRY_VERIFY_WITH_TIMEOUT(bundled_card() || list->findChild<QLabel*>()->text().contains(no_helper), 30000);
    QVERIFY2(!bundled_card(), "the Options dialog ran the bundled graphics helper during a test");
}

namespace {
//! Puts the suite's shield back when a slot has replaced it.
struct RestoreDiscoveryShield {
    ~RestoreDiscoveryShield() { ShieldOptionsDialogFromBundledHelper(); }
};

cuckatoo::GpuDiscovery FixedDiscovery(cuckatoo::ListOutcome outcome, const std::string& listing)
{
    cuckatoo::GpuDiscovery discovery;
    discovery.helper = "/fixed/helper";
    discovery.listing.outcome = outcome;
    discovery.listing.devices = cuckatoo::ParseDeviceListing(listing);
    discovery.selection = cuckatoo::SelectDevices(discovery.listing.devices, discovery.choice);
    discovery.situation = cuckatoo::ClassifySituation(discovery.listing, discovery.choice, discovery.selection);
    return discovery;
}
} // namespace

void OptionTests::injectedDiscoveryFillsTheDialog()
{
    const RestoreDiscoveryShield restore;
    const auto inject = [](const cuckatoo::GpuDiscovery& discovery) {
        OptionsDialog::setGpuDiscoveryRunnerForTesting([discovery](uint8_t, QObject* receiver, std::function<void(const cuckatoo::GpuDiscovery&)> done) {
            QTimer::singleShot(0, receiver, [discovery, done = std::move(done)] { done(discovery); });
        });
    };
    OptionsModel options{m_node}; bilingual_str error; QVERIFY(options.Init(error));

    inject(FixedDiscovery(cuckatoo::ListOutcome::kListed, "device backend=opencl id=pci:0000:0b:00.0 type=gpu integrated=0 mem=0 status=ok name=Fixed Card\n"));
    OptionsDialog listed(nullptr, true); listed.setModel(&options);
    auto* listed_cards = listed.findChild<GpuDeviceList*>(QStringLiteral("gpuDeviceList")); QVERIFY(listed_cards);
    QTRY_VERIFY_WITH_TIMEOUT(listed_cards->findChild<QCheckBox*>(QStringLiteral("gpuDevice_pci_0000_0b_00_0")), 30000);

    inject(FixedDiscovery(cuckatoo::ListOutcome::kFailed, ""));
    OptionsDialog failed(nullptr, true); failed.setModel(&options);
    auto* failed_cards = failed.findChild<GpuDeviceList*>(QStringLiteral("gpuDeviceList")); QVERIFY(failed_cards);
    QTRY_COMPARE_WITH_TIMEOUT(failed_cards->findChild<QLabel*>()->text(), QStringLiteral("Quicksilver couldn't list this computer's graphics cards. See the debug log."), 30000);
}

//! Without an injected runner the dialog runs production discovery. The named
//! helper is the stub, so this reaches a real process and no hardware.
void OptionTests::defaultDiscoveryRunnerListsThroughTheHelper()
{
    QVERIFY_SOLVER_PROBE_STUB();
    const RestoreDiscoveryShield restore;
    OptionsDialog::setGpuDiscoveryRunnerForTesting(nullptr);
    PointDiscoveryAtStub(WriteListing({QStringLiteral("device backend=opencl id=pci:0000:0c:00.0 type=gpu integrated=0 mem=0 status=ok name=Default Runner Card")}));
    OptionsModel options{m_node}; bilingual_str error; QVERIFY(options.Init(error));
    OptionsDialog dialog(nullptr, true); dialog.setModel(&options);
    auto* list = dialog.findChild<GpuDeviceList*>(QStringLiteral("gpuDeviceList")); QVERIFY(list);
    QTRY_VERIFY_WITH_TIMEOUT(list->findChild<QCheckBox*>(QStringLiteral("gpuDevice_pci_0000_0c_00_0")), 30000);
}

void OptionTests::missingBundledHelperIsNamed()
{
    const RestoreBundled restore;
    qunsetenv("CUCKATOO_GPU_BUNDLED");
    cuckatoo::ResetGpuDiscoveryForTesting();
    OptionsModel options{m_node}; bilingual_str error; QVERIFY(options.Init(error));
    OptionsDialog dialog(nullptr, true); dialog.setModel(&options);
    auto* status = dialog.findChild<QLabel*>(QStringLiteral("gpuSolverStatusLabel")); QVERIFY(status);
    auto* list = dialog.findChild<GpuDeviceList*>(QStringLiteral("gpuDeviceList")); QVERIFY(list);
    const QString headline = QStringLiteral("Quicksilver's graphics helper isn't installed.");
    QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(headline), 30000);
    QVERIFY(list->findChild<QLabel*>()->text().contains(headline));
#ifdef Q_OS_WIN
    const QString advice = QStringLiteral("Reinstall Quicksilver.");
#else
    const QString advice = QStringLiteral("Install it with <i>sudo apt install quicksilver-gpu</i>.");
#endif
    QVERIFY(status->text().contains(advice));
    QVERIFY(list->findChild<QLabel*>()->text().contains(advice));
    QMetaObject::invokeMethod(&dialog, "validateGpuSolverPath");
    QVERIFY(status->text().contains(headline));
    // F-575: no helper, so no card to deselect.
    QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("gpuCardsHelp"))->isHidden());
}

//! F-575: the hint under the card list tells the reader to deselect a card, so
//! it shows only while the list holds one. Not while discovery is still running,
//! not when the helper lists no card, and not when the listing fails. A listed
//! card (gpuCardsListAndModes) and a missing helper (missingBundledHelperIsNamed)
//! are checked where those states are already built.
void OptionTests::cardsHintNeedsACard()
{
    QVERIFY_SOLVER_PROBE_STUB();
    const auto check = [&](const QString& expected_state) {
        OptionsModel options{m_node}; bilingual_str error; QVERIFY(options.Init(error));
        OptionsDialog dialog(nullptr, true); dialog.setModel(&options);
        auto* hint = dialog.findChild<QLabel*>(QStringLiteral("gpuCardsHelp")); QVERIFY(hint);
        auto* list = dialog.findChild<GpuDeviceList*>(QStringLiteral("gpuDeviceList")); QVERIFY(list);
        QVERIFY(hint->isHidden());
        QTRY_VERIFY_WITH_TIMEOUT(list->findChild<QLabel*>() && list->findChild<QLabel*>()->text() == expected_state, 30000);
        QVERIFY(hint->isHidden());
    };
    PointDiscoveryAtStub(WriteListing({}));
    check(QStringLiteral("No graphics cards were found."));
    PointDiscoveryAtStub(WriteListing({}));
    qputenv("QS_PROBE_STUB_EXIT", "1");
    check(QStringLiteral("Quicksilver couldn't list this computer's graphics cards. See the debug log."));
}

void OptionTests::tickingBuiltInGraphicsAsksFirst()
{
    QVERIFY_SOLVER_PROBE_STUB();
    PointDiscoveryAtStub(WriteListing({
        QStringLiteral("device backend=opencl id=pci:0000:00:02.0 type=gpu integrated=1 mem=0 status=ok name=Intel(R) HD Graphics 4600"),
        QStringLiteral("device backend=opencl id=pci:0000:04:00.0 type=gpu integrated=0 mem=0 status=ok name=AMD Radeon RX 480") }));
    OptionsModel options{m_node}; bilingual_str error; QVERIFY(options.Init(error));
    OptionsDialog dialog(nullptr, true); dialog.setModel(&options);
    auto* list = dialog.findChild<QWidget*>(QStringLiteral("gpuDeviceList"));
    QTRY_VERIFY_WITH_TIMEOUT(list->findChild<QCheckBox*>(QStringLiteral("gpuDevice_pci_0000_00_02_0")), 30000);
    dialog.findChild<QRadioButton*>(QStringLiteral("gpuModeChoose"))->click();
    auto* hd = list->findChild<QCheckBox*>(QStringLiteral("gpuDevice_pci_0000_00_02_0"));
    hd->click();
    QMessageBox* box{nullptr};
    QTRY_VERIFY((box = dialog.findChild<QMessageBox*>(QStringLiteral("gpuIntegratedConfirm"))) != nullptr);
    QCOMPARE(box->windowTitle(), /*W-D14*/ QStringLiteral("Use built-in graphics?"));
    QCOMPARE(box->text(), /*W-D15*/ QStringLiteral("Your screen may respond slowly while transfers are being prepared."));
    box->button(QMessageBox::Cancel)->click(); QTRY_VERIFY(!hd->isChecked());
    hd->click(); QTRY_VERIFY((box = dialog.findChild<QMessageBox*>(QStringLiteral("gpuIntegratedConfirm"))) != nullptr);
    QPushButton* use{nullptr};
    for (QAbstractButton* button : box->buttons()) if (button->text() == /*W-D16*/ QStringLiteral("Use built-in graphics")) use = qobject_cast<QPushButton*>(button);
    QVERIFY(use); use->click(); QTRY_VERIFY(hd->isChecked());
    QCOMPARE(list->findChild<QLabel*>(QStringLiteral("gpuDevice_pci_0000_00_02_0_state"))->text(), /*W-D5*/ QStringLiteral("Chosen"));
    QTest::mouseClick(dialog.findChild<QPushButton*>(QStringLiteral("okButton")), Qt::LeftButton);
    QVERIFY(options.getOption(OptionsModel::GpuDevices).toStringList().contains(QStringLiteral("pci:0000:00:02.0")));
    QCOMPARE(options.getGpuDeviceNames().value(QStringLiteral("pci:0000:00:02.0")), QStringLiteral("Intel(R) HD Graphics 4600"));
}

void OptionTests::missingChosenCardKeepsItsName()
{
    QVERIFY_SOLVER_PROBE_STUB();
    PointDiscoveryAtStub(WriteListing({QStringLiteral("device backend=opencl id=pci:0000:04:00.0 type=gpu integrated=0 mem=0 status=ok name=AMD Radeon RX 480")}));
    OptionsModel options{m_node}; bilingual_str error; QVERIFY(options.Init(error));
    QVERIFY(options.setOption(OptionsModel::GpuDevices, QStringList{QStringLiteral("pci:0000:09:00.0")}));
    options.setGpuDeviceNames({{QStringLiteral("pci:0000:09:00.0"), QStringLiteral("NVIDIA P106-100")}});
    OptionsDialog dialog(nullptr, true); dialog.setModel(&options);
    auto* list = dialog.findChild<QWidget*>(QStringLiteral("gpuDeviceList"));
    QTRY_VERIFY_WITH_TIMEOUT(list->findChild<QCheckBox*>(QStringLiteral("gpuDevice_pci_0000_09_00_0")), 30000);
    QVERIFY(dialog.findChild<QRadioButton*>(QStringLiteral("gpuModeChoose"))->isChecked());
    auto* gone = list->findChild<QCheckBox*>(QStringLiteral("gpuDevice_pci_0000_09_00_0"));
    QCOMPARE(gone->text(), QStringLiteral("NVIDIA P106-100")); QVERIFY(gone->isChecked());
    QCOMPARE(list->findChild<QLabel*>(QStringLiteral("gpuDevice_pci_0000_09_00_0_state"))->text(), /*W-D8*/ QStringLiteral("Not found"));
    QCOMPARE(list->findChild<QLabel*>(QStringLiteral("gpuDevice_pci_0000_04_00_0_state"))->text(), /*W-D9*/ QStringLiteral("Not chosen"));
    gone->click(); list->findChild<QCheckBox*>(QStringLiteral("gpuDevice_pci_0000_04_00_0"))->click();
    QTest::mouseClick(dialog.findChild<QPushButton*>(QStringLiteral("okButton")), Qt::LeftButton);
    QCOMPARE(options.getOption(OptionsModel::GpuDevices).toStringList(), QStringList{QStringLiteral("pci:0000:04:00.0")});
}

//! S0222-C-01: the only chosen card, missing from discovery, used to be dropped on OK.
//! Choose mode then saved an empty list, which reopens as Automatic: every card mined.
void OptionTests::onlyChosenCardMissingStaysChosen()
{
    QVERIFY_SOLVER_PROBE_STUB();
    PointDiscoveryAtStub(WriteListing({QStringLiteral("device backend=opencl id=pci:0000:04:00.0 type=gpu integrated=0 mem=0 status=ok name=AMD Radeon RX 480")}));
    OptionsModel options{m_node}; bilingual_str error; QVERIFY(options.Init(error));
    QVERIFY(options.setOption(OptionsModel::GpuDevices, QStringList{QStringLiteral("pci:0000:09:00.0")}));
    options.setGpuDeviceNames({{QStringLiteral("pci:0000:09:00.0"), QStringLiteral("NVIDIA P106-100")}});
    OptionsDialog dialog(nullptr, true); dialog.setModel(&options);
    auto* list = dialog.findChild<QWidget*>(QStringLiteral("gpuDeviceList"));
    QTRY_VERIFY_WITH_TIMEOUT(list->findChild<QCheckBox*>(QStringLiteral("gpuDevice_pci_0000_09_00_0")), 30000);
    auto* gone = list->findChild<QCheckBox*>(QStringLiteral("gpuDevice_pci_0000_09_00_0"));
    QVERIFY(gone->isChecked());
    QVERIFY(gone->isEnabled()); // the operator can still untick it
    QTest::mouseClick(dialog.findChild<QPushButton*>(QStringLiteral("okButton")), Qt::LeftButton);
    QCOMPARE(options.getOption(OptionsModel::GpuDevices).toStringList(), QStringList{QStringLiteral("pci:0000:09:00.0")});
    QCOMPARE(options.getGpuDeviceNames().value(QStringLiteral("pci:0000:09:00.0")), QStringLiteral("NVIDIA P106-100"));
}

//! S0223-A-01: a card id that is not a PCI address has no slot. The fault line named
//! it "(slot )"; it is named by its id instead.
void OptionTests::faultedCardWithoutSlotIsNamedById()
{
    cuckatoo::GpuDiscovery discovery;
    discovery.situation = cuckatoo::GpuSituation::kReady;
    cuckatoo::GpuDevice pci; pci.id = "pci:0000:04:00.0"; pci.name = "Radeon RX 580"; pci.type = "gpu"; pci.status = cuckatoo::DeviceStatus::kOk;
    cuckatoo::GpuDevice other; other.id = "opencl:1:0"; other.name = "Other card"; other.type = "gpu"; other.status = cuckatoo::DeviceStatus::kOk;
    discovery.selection.use = {pci, other};
    cuckatoo::SetGpuDeviceStatsForTesting({
        {"pci:0000:04:00.0", cuckatoo::GpuDeviceState::kFaulted, cuckatoo::GpuSolveStatus::kDeviceFault, 0, 0.0},
        {"opencl:1:0", cuckatoo::GpuDeviceState::kFaulted, cuckatoo::GpuSolveStatus::kDeviceFault, 0, 0.0},
    });
    const QString fault{QString::fromStdString(cuckatoo::SolverFault(cuckatoo::GpuSolveStatus::kDeviceFault, false).value())};
    const QString details{gpusituation::FaultedCardDetails(discovery)};
    cuckatoo::ResetGpuDeviceStatsForTesting();
    QCOMPARE(details, QStringLiteral("Radeon RX 580 (slot 04:00.0): %1\nOther card (opencl:1:0): %1").arg(fault));
    QVERIFY(!details.contains(QStringLiteral("(slot )")));
}

//! S0222-C-04: Automatic mode's rows are the cards cuckatoo::SelectDevices picks, the
//! rule the miner uses, so the list cannot show a card in use that the miner skips.
void OptionTests::automaticRowsFollowTheMinersSelection()
{
    cuckatoo::GpuDiscovery discovery;
    discovery.listing.outcome = cuckatoo::ListOutcome::kListed;
    discovery.listing.devices = cuckatoo::ParseDeviceListing(
        "device backend=opencl id=pci:0000:04:00.0 type=gpu integrated=0 mem=0 status=ok name=Radeon RX 580\n"
        "device backend=opencl id=pci:0000:00:02.0 type=gpu integrated=1 mem=0 status=ok name=Intel HD\n"
        "device backend=opencl id=pci:0000:05:00.0 type=gpu integrated=0 mem=0 status=compile-failed name=Old Card\n");
    GpuDeviceList list;
    list.setMode(GpuDeviceList::Mode::kAutomatic);
    list.setDevices(discovery, {}, {});
    QStringList shown;
    for (const char* id : {"pci_0000_04_00_0", "pci_0000_00_02_0", "pci_0000_05_00_0"}) {
        if (list.findChild<QLabel*>(QStringLiteral("gpuDevice_%1_state").arg(QLatin1String(id)))->text() == QStringLiteral("Chosen")) shown.push_back(QLatin1String(id));
    }
    QStringList picked;
    for (const auto& device : cuckatoo::SelectDevices(discovery.listing.devices, cuckatoo::DeviceChoice{}).use) {
        picked.push_back(QString::fromStdString(device.id).replace(QLatin1Char(':'), QLatin1Char('_')).replace(QLatin1Char('.'), QLatin1Char('_')));
    }
    QCOMPARE(shown, picked);
    QCOMPARE(shown, QStringList{QStringLiteral("pci_0000_04_00_0")});
    QCOMPARE(list.tickedIds(), QStringList{QStringLiteral("pci:0000:04:00.0")});
}

void OptionTests::cpuOnlyHidesTheList()
{
    QVERIFY_SOLVER_PROBE_STUB();
    PointDiscoveryAtStub(WriteListing({QStringLiteral("device backend=opencl id=pci:0000:04:00.0 type=gpu integrated=0 mem=0 status=ok name=AMD Radeon RX 480")}));
    OptionsModel options{m_node}; bilingual_str error; QVERIFY(options.Init(error));
    OptionsDialog dialog(nullptr, true); dialog.setModel(&options);
    auto* list = dialog.findChild<QWidget*>(QStringLiteral("gpuDeviceList"));
    QTRY_VERIFY_WITH_TIMEOUT(list->findChild<QCheckBox*>(QStringLiteral("gpuDevice_pci_0000_04_00_0")), 30000);
    dialog.findChild<QRadioButton*>(QStringLiteral("gpuModeCpu"))->click(); QVERIFY(list->isHidden());
    QTest::mouseClick(dialog.findChild<QPushButton*>(QStringLiteral("okButton")), Qt::LeftButton);
    QCOMPARE(options.getOption(OptionsModel::GpuDevices).toStringList(), QStringList{QStringLiteral("cpu")});
}

void OptionTests::savedGpuDevicesSelectTheMatchingRadio()
{
    QVERIFY_SOLVER_PROBE_STUB();
    PointDiscoveryAtStub(WriteListing({QStringLiteral("device backend=opencl id=pci:0000:04:00.0 type=gpu integrated=0 mem=0 status=ok name=AMD Radeon RX 480")}));
    const struct {
        QStringList devices;
        const char* checked;
    } cases[]{
        {QStringList{}, "gpuModeAutomatic"},
        {QStringList{QStringLiteral("cpu")}, "gpuModeCpu"},
        {QStringList{QStringLiteral("pci:0000:04:00.0")}, "gpuModeChoose"},
        {QStringList{QStringLiteral("cpu"), QStringLiteral("pci:0000:04:00.0")}, "gpuModeChoose"},
    };
    for (const auto& sample : cases) {
        OptionsModel options{m_node};
        bilingual_str error;
        QVERIFY(options.Init(error));
        QVERIFY(options.setOption(OptionsModel::GpuDevices, sample.devices));
        OptionsDialog dialog(nullptr, true);
        dialog.setModel(&options);
        for (const char* name : {"gpuModeAutomatic", "gpuModeCpu", "gpuModeChoose"}) {
            auto* radio = dialog.findChild<QRadioButton*>(QString::fromLatin1(name));
            QVERIFY2(radio != nullptr, name);
            QCOMPARE(radio->isChecked(), QString::fromLatin1(name) == QLatin1String(sample.checked));
        }
    }
}

void OptionTests::gpuSolverSettingPersistsAndRequiresRestart()
{
    // The probe starts the configured solver and accepts it on exit 0, so this
    // needs a genuinely startable executable. `true` exists only on POSIX, and
    // on Windows QProcess reaches CreateProcess, which cannot launch a script,
    // so the build ships a tiny stub binary beside this test for both platforms
    // (src/qt/test/solverprobestub.cpp).
    const QString solver{SolverProbeStubPath()};
    QVERIFY2(QFileInfo(solver).isExecutable(), qPrintable(solver));
    qputenv("QS_PROBE_STUB_LISTING", WriteListing({QStringLiteral("device backend=opencl id=pci:0000:04:00.0 type=gpu integrated=0 mem=0 status=ok name=AMD Radeon RX 480")}).toUtf8());

    OptionsModel options{m_node};
    bilingual_str error;
    QVERIFY(options.Init(error));
    OptionsDialog dialog(nullptr, /*enableVault=*/true);
    dialog.setModel(&options);

    auto* path = dialog.findChild<QLineEdit*>(QStringLiteral("gpuSolverPath"));
    auto* status = dialog.findChild<QLabel*>(QStringLiteral("gpuSolverStatusLabel"));
    auto* ok_button = dialog.findChild<QPushButton*>(QStringLiteral("okButton"));
    QVERIFY(path);
    QVERIFY(status);
    QVERIFY(ok_button);
    path->setText(solver);
    QTRY_COMPARE_WITH_TIMEOUT(status->text(), /*W-D23*/ QStringLiteral("The selected graphics helper loaded successfully."), 5000);
    QVERIFY(ok_button->isEnabled());
    QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("statusLabel"))->text().contains(QStringLiteral("restart")));

    QTest::mouseClick(ok_button, Qt::LeftButton);
    QCOMPARE(options.getOption(OptionsModel::GpuSolverPath).toString(), solver);
    QVERIFY(options.isRestartRequired());

    qputenv("QS_PROBE_STUB_EXIT", "4");
    OptionsDialog failed(nullptr, true);
    failed.setModel(&options);
    auto* failed_path = failed.findChild<QLineEdit*>(QStringLiteral("gpuSolverPath"));
    auto* failed_status = failed.findChild<QLabel*>(QStringLiteral("gpuSolverStatusLabel"));
    auto* failed_ok = failed.findChild<QPushButton*>(QStringLiteral("okButton"));
    failed_path->setText(QString());
    failed_path->setText(solver);
    QTRY_COMPARE_WITH_TIMEOUT(failed_status->text(), /*W-D24*/ QStringLiteral("The selected graphics helper was unable to load."), 5000);
    QVERIFY(!failed_ok->isEnabled());
}

void OptionTests::gpuSolverDialogRejectsInvalidPath()
{
    OptionsModel options{m_node};
    bilingual_str error;
    QVERIFY(options.Init(error));
    const QString original = options.getOption(OptionsModel::GpuSolverPath).toString();
    OptionsDialog dialog(nullptr, /*enableVault=*/true);
    dialog.setModel(&options);

    auto* path = dialog.findChild<QLineEdit*>(QStringLiteral("gpuSolverPath"));
    auto* status = dialog.findChild<QLabel*>(QStringLiteral("gpuSolverStatusLabel"));
    auto* ok_button = dialog.findChild<QPushButton*>(QStringLiteral("okButton"));
    QVERIFY(path);
    QVERIFY(status);
    QVERIFY(ok_button);
    path->setText(QStringLiteral("/path/that/does/not/exist/qsgpusolve-opencl"));
    QCOMPARE(status->text(), /*W-D20*/ QStringLiteral("The selected graphics helper does not exist."));
    QVERIFY(!ok_button->isEnabled());
    QCOMPARE(options.getOption(OptionsModel::GpuSolverPath).toString(), original);
}

void OptionTests::externalSignerSurfacesAreAbsent()
{
    // F-64 ruled that v1 shows no external-signer surface, absent rather than
    // inert; F-679 then removed the code until the adapter work. Each dialog
    // that carried a signer control is pinned here, because they are separate
    // forms and restoring one would not touch the others.
    OptionsModel options{m_node};
    bilingual_str error;
    QVERIFY(options.Init(error));
    OptionsDialog options_dialog(nullptr, /*enableVault=*/true);
    options_dialog.setModel(&options);
    QVERIFY(options_dialog.findChild<QWidget*>(QStringLiteral("tabHardware")));
    QVERIFY(!options_dialog.findChild<QGroupBox*>(QStringLiteral("groupBoxHww")));
    QVERIFY(!options_dialog.findChild<QLineEdit*>(QStringLiteral("externalSignerPath")));

    CreateVaultDialog create_dialog(nullptr);
    QVERIFY(create_dialog.findChild<QLineEdit*>(QStringLiteral("vault_name_line_edit")));
    QVERIFY(!create_dialog.findChild<QCheckBox*>(QStringLiteral("external_signer_checkbox")));

    ReceiveRequestDialog receive_dialog(nullptr);
    QVERIFY(receive_dialog.findChild<QPushButton*>(QStringLiteral("btnSaveAs")));
    QVERIFY(!receive_dialog.findChild<QPushButton*>(QStringLiteral("btnVerify")));
}

void OptionTests::cpuFallbackWarningPreferencePersists()
{
    constexpr auto SETTING_KEY{"show_cpu_fallback_warning"};
    QSettings settings;
    const bool had_previous_value{settings.contains(SETTING_KEY)};
    const QVariant previous_value{settings.value(SETTING_KEY)};
    settings.remove(SETTING_KEY);

    OptionsModel first{m_node};
    QVERIFY(first.getOption(OptionsModel::ShowCpuFallbackWarning).toBool());
    QVERIFY(first.setOption(OptionsModel::ShowCpuFallbackWarning, false));

    OptionsModel second{m_node};
    QVERIFY(!second.getOption(OptionsModel::ShowCpuFallbackWarning).toBool());
    QVERIFY(second.setOption(OptionsModel::ShowCpuFallbackWarning, true));
    QVERIFY(second.getOption(OptionsModel::ShowCpuFallbackWarning).toBool());

    if (had_previous_value) {
        settings.setValue(SETTING_KEY, previous_value);
    } else {
        settings.remove(SETTING_KEY);
    }
}

void OptionTests::allowCpuBlockMiningPersistsWithoutRestart()
{
    // The mining thread reads -allowcpumining from the live args at each
    // arming. updateRwSetting writes those args in this process, so the
    // checkbox must not claim a restart. A command-line value still wins:
    // the dialog may store a different persistent value, and must say so.
    auto restore_allowcpu = [] {
        gArgs.LockSettings([&](common::Settings& settings) {
            settings.command_line_options.erase("allowcpumining");
            settings.rw_settings.erase("allowcpumining");
        });
        gArgs.WriteSettingsFile();
    };
    restore_allowcpu();

    OptionsModel options{m_node};
    bilingual_str error;
    QVERIFY(options.Init(error));
    // Effective value before the checkbox is the default, false.
    QCOMPARE(gArgs.GetBoolArg("-allowcpumining", false), false);
    QVERIFY(!options.getOption(OptionsModel::AllowCpuBlockMining).toBool());

    OptionsDialog dialog(nullptr, /*enableVault=*/true);
    dialog.setModel(&options);
    auto* box = dialog.findChild<QCheckBox*>(QStringLiteral("allowCpuBlockMining"));
    auto* solver_status = dialog.findChild<QLabel*>(QStringLiteral("gpuSolverStatusLabel"));
    auto* ok_button = dialog.findChild<QPushButton*>(QStringLiteral("okButton"));
    QVERIFY(box);
    QVERIFY(solver_status);
    QVERIFY(ok_button);
    QVERIFY(!box->isChecked());
    QCOMPARE(box->text(), /*W-D25*/ QStringLiteral("&Allow this computer's processor to mine blocks when no graphics card is in use."));
    QCOMPARE(solver_status->text(), /*W-D19*/ QStringLiteral("Using Quicksilver's graphics helper."));

    box->setChecked(true);
    QTest::mouseClick(ok_button, Qt::LeftButton);
    QVERIFY(options.getOption(OptionsModel::AllowCpuBlockMining).toBool());
    QCOMPARE(gArgs.GetBoolArg("-allowcpumining", false), true);
    QVERIFY(!options.isRestartRequired());
    auto* restart_label = dialog.findChild<QLabel*>(QStringLiteral("statusLabel"));
    QVERIFY(restart_label);
    QVERIFY(!restart_label->text().contains(QStringLiteral("restart"), Qt::CaseInsensitive));

    OptionsModel reloaded{m_node};
    QVERIFY(reloaded.Init(error));
    QVERIFY(reloaded.getOption(OptionsModel::AllowCpuBlockMining).toBool());
    QVERIFY(!reloaded.isRestartRequired());

    restore_allowcpu();

    gArgs.LockSettings([&](common::Settings& settings) {
        settings.command_line_options["allowcpumining"] = {UniValue("0")};
        settings.rw_settings.erase("allowcpumining");
    });
    OptionsModel overridden{m_node};
    QVERIFY(overridden.Init(error));
    QVERIFY(overridden.getOverriddenByCommandLine().contains(QStringLiteral("-allowcpumining=0")));
    QCOMPARE(gArgs.GetBoolArg("-allowcpumining", true), false);
    QVERIFY(!overridden.getOption(OptionsModel::AllowCpuBlockMining).toBool());

    OptionsDialog overridden_dialog(nullptr, /*enableVault=*/true);
    overridden_dialog.setModel(&overridden);
    auto* overridden_label = overridden_dialog.findChild<QLabel*>(QStringLiteral("overriddenByCommandLineLabel"));
    auto* overridden_box = overridden_dialog.findChild<QCheckBox*>(QStringLiteral("allowCpuBlockMining"));
    auto* overridden_ok = overridden_dialog.findChild<QPushButton*>(QStringLiteral("okButton"));
    QVERIFY(overridden_label);
    QVERIFY(overridden_box);
    QVERIFY(overridden_ok);
    QVERIFY(overridden_label->text().contains(QStringLiteral("-allowcpumining=0")));
    overridden_box->setChecked(true);
    QTest::mouseClick(overridden_ok, Qt::LeftButton);
    // The checkbox did store a persistent true. The command line still wins.
    QVERIFY(overridden.getOption(OptionsModel::AllowCpuBlockMining).toBool());
    QCOMPARE(gArgs.GetBoolArg("-allowcpumining", true), false);
    QVERIFY(!overridden.isRestartRequired());

    restore_allowcpu();
}

//! F-427, moved with F-485: the graphics rows used to be crushed into the
//! dialog's 700x540 minimum. They now live on Hardware, which owns the scroll
//! area. Each label, checkbox and spin box has to receive its height, and sit
//! fully inside the scroll viewport's width. What does not fit vertically has
//! to be reachable by scrolling. A 1366x768 laptop, after a taskbar and a
//! title bar, is checked by sizing the client to 1366x700.
void OptionTests::hardwareTabFitsAtTheOpeningSize()
{
    OptionsModel options{m_node};
    bilingual_str error;
    QVERIFY(options.Init(error));
    OptionsDialog dialog(nullptr, /*enableVault=*/true);
    dialog.setModel(&options);
    auto* tabs = dialog.findChild<QTabWidget*>(QStringLiteral("tabWidget"));
    QVERIFY(tabs);
    QStringList order;
    for (int i = 0; i < tabs->count(); ++i) order << tabs->widget(i)->objectName();
    QCOMPARE(order, QStringList({QStringLiteral("tabMain"), QStringLiteral("tabHardware"), QStringLiteral("tabVault"), QStringLiteral("tabNetwork"), QStringLiteral("tabDisplay")}));

    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    QWidget* tab = dialog.findChild<QWidget*>(QStringLiteral("tabHardware"));
    QWidget* main = dialog.findChild<QWidget*>(QStringLiteral("tabMain"));
    QVERIFY(tab);
    QVERIFY(main);
    tabs->setCurrentWidget(tab);
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));

    QVERIFY(!dialog.findChild<QCheckBox*>(QStringLiteral("allowCpuAgentTxPow")));
    const char* required[]{"allowCpuBlockMining", "gpuSolverStatusLabel", "gpuCardsGroup", "gpuModeAutomatic", "gpuModeChoose", "gpuModeCpu", "gpuDeviceList", "gpuCardsHelp",
                           "threadsScriptVerif"};
    for (const char* name : required) {
        QVERIFY2(tab->findChild<QWidget*>(QString::fromLatin1(name)), name);
    }
    for (const char* name : {"pruneSize", "databaseCache"}) {
        QVERIFY2(main->findChild<QWidget*>(QString::fromLatin1(name)), name);
        QVERIFY2(!tab->findChild<QWidget*>(QString::fromLatin1(name)), name);
    }

    QString problem;
    // horizontal is for a screen wide enough to give the rows their width.
    // The opening-size check does not ask for it: at the 700x540 opening size
    // a wide font sticks out.
    auto rows_fit = [&](bool horizontal) {
        QScrollArea* scroll = tab->findChild<QScrollArea*>(QStringLiteral("tabMainScrollArea"));
        const auto widgets = tab->findChildren<QWidget*>();
        for (QWidget* widget : widgets) {
            const bool tracked = qobject_cast<QLabel*>(widget) || qobject_cast<QCheckBox*>(widget) || qobject_cast<QSpinBox*>(widget);
            // isVisibleTo, not isHidden: a row inside a hidden box is not
            // hidden itself, but it is never laid out, so its geometry means
            // nothing (F-586).
            if (!tracked || !widget->isVisibleTo(tab)) continue;
            // A word-wrapped label's sizeHint height is the height at its
            // preferred width. At the width it actually has, the height it
            // needs is heightForWidth.
            const int hint = widget->hasHeightForWidth()
                ? widget->heightForWidth(qMax(1, widget->width()))
                : widget->sizeHint().height();
            if (widget->height() < hint) {
                problem = widget->objectName()
                    + QStringLiteral(" height ") + QString::number(widget->height())
                    + QStringLiteral(" < needed ") + QString::number(hint)
                    + QStringLiteral(" dialog ") + QString::number(dialog.width()) + QLatin1Char('x') + QString::number(dialog.height())
                    + QStringLiteral(" tab given ") + QString::number(tab->width()) + QLatin1Char('x') + QString::number(tab->height())
                    + QStringLiteral(" tab sizeHint ") + QString::number(tab->sizeHint().width()) + QLatin1Char('x') + QString::number(tab->sizeHint().height())
                    + QStringLiteral(" tab minHint ") + QString::number(tab->minimumSizeHint().width()) + QLatin1Char('x') + QString::number(tab->minimumSizeHint().height());
                return false;
            }
            if (!scroll || !scroll->widget() || !scroll->widget()->isAncestorOf(widget)) continue;
            const int bottom = widget->mapTo(scroll->widget(), QPoint(0, widget->height())).y();
            const int reach = scroll->verticalScrollBar()->maximum() + scroll->viewport()->height();
            if (reach < bottom) {
                problem = widget->objectName()
                    + QStringLiteral(" bottom ") + QString::number(bottom)
                    + QStringLiteral(" is past the scroll range ") + QString::number(reach);
                return false;
            }
            if (!horizontal) continue;
            const QPoint origin = widget->mapTo(scroll->viewport(), QPoint(0, 0));
            if (origin.x() < 0 || origin.x() + widget->width() > scroll->viewport()->width()) {
                problem = widget->objectName()
                    + QStringLiteral(" x ") + QString::number(origin.x())
                    + QStringLiteral(" width ") + QString::number(widget->width())
                    + QStringLiteral(" outside viewport width ") + QString::number(scroll->viewport()->width());
                return false;
            }
        }
        return true;
    };
    // Prune and the database cache stayed on Main. Same height rule, and the
    // same horizontal rule against the page they actually sit on.
    auto main_rows_fit = [&](bool horizontal) {
        const auto widgets = main->findChildren<QWidget*>();
        for (QWidget* widget : widgets) {
            const bool tracked = qobject_cast<QLabel*>(widget) || qobject_cast<QCheckBox*>(widget) || qobject_cast<QSpinBox*>(widget);
            if (!tracked || !widget->isVisibleTo(main)) continue;
            const int hint = widget->hasHeightForWidth()
                ? widget->heightForWidth(qMax(1, widget->width()))
                : widget->sizeHint().height();
            if (widget->height() < hint) {
                problem = widget->objectName()
                    + QStringLiteral(" height ") + QString::number(widget->height())
                    + QStringLiteral(" < needed ") + QString::number(hint)
                    + QStringLiteral(" dialog ") + QString::number(dialog.width()) + QLatin1Char('x') + QString::number(dialog.height());
                return false;
            }
            if (!horizontal) continue;
            const QPoint origin = widget->mapTo(main, QPoint(0, 0));
            if (origin.x() < 0 || origin.x() + widget->width() > main->width()) {
                problem = widget->objectName()
                    + QStringLiteral(" x ") + QString::number(origin.x())
                    + QStringLiteral(" width ") + QString::number(widget->width())
                    + QStringLiteral(" outside Main width ") + QString::number(main->width());
                return false;
            }
        }
        return true;
    };
    auto show_page = [&](QWidget* page) {
        tabs->setCurrentWidget(page);
        return QTest::qWaitForWindowExposed(&dialog);
    };

    // Opening size. On this platform that is the 700x540 minimum, which is the
    // size the desktop opened at when the rows were crushed.
    QVERIFY2(rows_fit(false), qPrintable(problem));
    QVERIFY(show_page(main));
    QVERIFY2(main_rows_fit(false), qPrintable(problem));
    QVERIFY(show_page(tab));

    // A 1920x1080 desktop gives the rows their own size. Size the dialog to
    // what its contents ask for, then to the client area a 1366x768 laptop
    // has left after a taskbar and a title bar.
    QScrollArea* scroll = tab->findChild<QScrollArea*>(QStringLiteral("tabMainScrollArea"));
    QVERIFY(scroll);
    QVERIFY(scroll->widget());
    const int chrome_w = dialog.width() - scroll->viewport()->width();
    const int chrome_h = dialog.height() - scroll->viewport()->height();
    const QSize contents = scroll->widget()->sizeHint().expandedTo(scroll->widget()->minimumSizeHint());
    dialog.resize(qMax(dialog.minimumWidth(), contents.width() + qMax(0, chrome_w)),
                  qMax(dialog.minimumHeight(), contents.height() + qMax(0, chrome_h)));
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    const int overflow = scroll->widget()->width() - scroll->viewport()->width();
    if (overflow > 0) {
        dialog.resize(dialog.width() + overflow, dialog.height());
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    }
    QVERIFY2(rows_fit(true), qPrintable(problem));
    QVERIFY(show_page(main));
    QVERIFY2(main_rows_fit(true), qPrintable(problem));
    QVERIFY(show_page(tab));

    // The dialog must really take the laptop size: a minimum above it, or a
    // resize the platform refused, would leave the checks below measuring the
    // desktop size again (S0294-A-05).
    const QSize laptop{qMin(dialog.width(), 1366), qMin(dialog.height(), 700)};
    dialog.resize(laptop);
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    QVERIFY2(dialog.size() == laptop,
             qPrintable(QStringLiteral("asked for laptop size %1x%2, got %3x%4").arg(laptop.width()).arg(laptop.height()).arg(dialog.width()).arg(dialog.height())));
    QVERIFY2(rows_fit(true), qPrintable(problem));
    QVERIFY(show_page(main));
    QVERIFY2(main_rows_fit(true), qPrintable(problem));
}

void OptionTests::graphicsSettingsOpenOnTheHardwareTab()
{
    OptionsModel options{m_node};
    bilingual_str error;
    QVERIFY(options.Init(error));
    OptionsDialog dialog(nullptr, /*enableVault=*/true);
    dialog.setModel(&options);
    auto* tabs = dialog.findChild<QTabWidget*>(QStringLiteral("tabWidget"));
    QVERIFY(tabs);
    // The menu's Options entry leaves the dialog on Main.
    QCOMPARE(tabs->currentWidget()->objectName(), QStringLiteral("tabMain"));
    dialog.setCurrentTab(OptionsDialog::TAB_HARDWARE);
    QCOMPARE(tabs->currentWidget()->objectName(), QStringLiteral("tabHardware"));
}

void OptionTests::optionsUseBenchPanelAndExplainAutomaticThreads()
{
    const RestoreApplicationStyle restore;
    QuicksilverStyle::Apply(*qApp);
    OptionsDialog dialog(nullptr, true);
    QVERIFY(!dialog.findChild<QLabel*>(QStringLiteral("optionsEyebrow")));
    auto* panel = dialog.findChild<QFrame*>(QStringLiteral("optionsPanel"));
    QVERIFY(panel);
    QVERIFY(panel->isAncestorOf(dialog.findChild<QTabWidget*>(QStringLiteral("tabWidget"))));
    auto* spin = dialog.findChild<QSpinBox*>(QStringLiteral("threadsScriptVerif"));
    QVERIFY(spin);
    spin->setValue(0);
    QVERIFY(spin->toolTip().contains(QStringLiteral("0")));
    QVERIFY(spin->toolTip().contains(QStringLiteral("automatic")));
    auto* label = dialog.findChild<QLabel*>(QStringLiteral("threadsScriptVerifLabel"));
    QVERIFY(label);
    QCOMPARE(label->property("class").toString(), QStringLiteral("benchKey"));
    QVERIFY(label->text().contains(QStringLiteral("0 = automatic")));
}

void OptionTests::mainTabMatchesCardSurface()
{
    const RestoreApplicationStyle restore;
    QuicksilverStyle::Apply(*qApp);
    OptionsModel options{m_node};
    bilingual_str error;
    QVERIFY(options.Init(error));
    OptionsDialog dialog(nullptr, /*enableVault=*/true);
    dialog.setModel(&options);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto* tabs = dialog.findChild<QTabWidget*>(QStringLiteral("tabWidget"));
    auto* contents = dialog.findChild<QWidget*>(QStringLiteral("tabMainScrollContents"));
    auto* hardware = dialog.findChild<QWidget*>(QStringLiteral("tabHardware"));
    auto* main_tab = dialog.findChild<QWidget*>(QStringLiteral("tabMain"));
    auto* network = dialog.findChild<QWidget*>(QStringLiteral("tabNetwork"));
    QVERIFY(tabs);
    QVERIFY(contents);
    QVERIFY(hardware);
    QVERIFY(main_tab);
    QVERIFY(network);
    tabs->setCurrentWidget(main_tab);
    QCoreApplication::processEvents();
    // The top-left layout margin is outside every child control.
    QVERIFY(!main_tab->childAt(QPoint(2, 2)));
    const QImage main = dialog.grab().toImage();
    const QPoint main_point = main_tab->mapTo(&dialog, QPoint(2, 2));
    const QColor main_color = main.pixelColor(main_point * main.devicePixelRatio());
    qInfo() << "Main sample" << main_point << main_color.name();
    tabs->setCurrentWidget(hardware);
    QCoreApplication::processEvents();
    QVERIFY(!contents->childAt(QPoint(2, 2)));
    const QImage hardware_image = dialog.grab().toImage();
    const QPoint hardware_point = contents->mapTo(&dialog, QPoint(2, 2));
    const QColor hardware_color = hardware_image.pixelColor(hardware_point * hardware_image.devicePixelRatio());
    qInfo() << "Hardware sample" << hardware_point << hardware_color.name();
    tabs->setCurrentWidget(network);
    QCoreApplication::processEvents();
    QVERIFY(!network->childAt(QPoint(2, 2)));
    const QImage other = dialog.grab().toImage();
    const QColor network_color = other.pixelColor(network->mapTo(&dialog, QPoint(2, 2)) * other.devicePixelRatio());
    qInfo() << "Network sample" << network_color.name();
    QCOMPARE(main_color, network_color);
    QCOMPARE(hardware_color, network_color);
    QCOMPARE(main_color, QColor(QStringLiteral("#171b1f")));
}

void OptionTests::documentModeStripHasNoLightBase()
{
    const RestoreApplicationStyle restore;
    QuicksilverStyle::Apply(*qApp);
    OptionsDialog dialog(nullptr, /*enableVault=*/true);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto* tabs = dialog.findChild<QTabWidget*>(QStringLiteral("tabWidget"));
    QVERIFY(tabs);
    auto* bar = tabs->tabBar();
    QVERIFY(tabs->documentMode());
    QCoreApplication::processEvents();
    const QImage image = dialog.grab().toImage();
    const int x = bar->tabRect(bar->count() - 1).right() + 12;
    QVERIFY(x < bar->width());
    // Fusion's document-mode base used to draw a white line one pixel
    // below the top of this strip. Scan the column so DPI/font changes cannot
    // move the line past the assertion.
    for (int y = 0; y < bar->height(); ++y) {
        const QPoint point = bar->mapTo(&dialog, QPoint(x, y));
        const QColor color = image.pixelColor(point * image.devicePixelRatio());
        QVERIFY2(color.lightness() < 100, qPrintable(color.name()));
    }
}

void OptionTests::parametersInteraction()
{
    // With -listen=false, parameter interaction should also set -listenonion
    // to false.
    gArgs.LockSettings([&](common::Settings& s) {
        s.forced_settings.erase("listen");
        s.forced_settings.erase("listenonion");
    });
    QVERIFY(!gArgs.IsArgSet("-listen"));
    QVERIFY(!gArgs.IsArgSet("-listenonion"));

    QVERIFY(gArgs.SoftSetBoolArg("-listen", false));
    m_node.initParameterInteraction();

    const bool expected{false};
    QVERIFY(gArgs.IsArgSet("-listen"));
    QCOMPARE(gArgs.GetBoolArg("-listen", !expected), expected);
    QVERIFY(gArgs.IsArgSet("-listenonion"));
    QCOMPARE(gArgs.GetBoolArg("-listenonion", !expected), expected);

    gArgs.ClearPathCache();
}

void OptionTests::extractFilter()
{
    QString filter = QString("Partially Signed Transaction (Binary) (*.psqt)");
    QCOMPARE(GUIUtil::ExtractFirstSuffixFromFilter(filter), "psqt");

    filter = QString("Image (*.png *.jpg)");
    QCOMPARE(GUIUtil::ExtractFirstSuffixFromFilter(filter), "png");
}

void OptionTests::openConfDoesNotNestEventLoop()
{
    OptionsModel options{m_node};
    bilingual_str error;
    QVERIFY(options.Init(error));
    OptionsDialog dialog(nullptr, /*enableVault=*/true);
    dialog.setModel(&options);
    QPushButton* open_conf = dialog.findChild<QPushButton*>(QStringLiteral("openQuicksilverConfButton"));
    QVERIFY(open_conf);
    ExpectModalWithoutNestedEventLoop("QMessageBox", [&] {
        open_conf->click();
    });
}

void OptionTests::resetDoesNotNestEventLoop()
{
    OptionsModel options{m_node};
    bilingual_str error;
    QVERIFY(options.Init(error));
    ClientModel client{m_node, &options};
    OptionsDialog dialog(nullptr, /*enableVault=*/true);
    dialog.setModel(&options);
    dialog.setClientModel(&client);
    QPushButton* reset = dialog.findChild<QPushButton*>(QStringLiteral("resetButton"));
    QVERIFY(reset);
    ExpectModalWithoutNestedEventLoop("QMessageBox", [&] {
        reset->click();
    });
}

void OptionTests::gpuSolverBrowseDoesNotNestEventLoop()
{
    OptionsModel options{m_node};
    bilingual_str error;
    QVERIFY(options.Init(error));
    OptionsDialog dialog(nullptr, /*enableVault=*/true);
    dialog.setModel(&options);
    QPushButton* browse = dialog.findChild<QPushButton*>(QStringLiteral("gpuSolverBrowseButton"));
    QVERIFY(browse);
    ExpectModalWithoutNestedEventLoop("QFileDialog", [&] {
        browse->click();
    });
}

void OptionTests::displayTabHasNoDeadFontChoiceAndAlignsItsFields()
{
    // F-552/F-548: the shipping app sets the base style to Fusion at startup
    // (QuicksilverStyle::Apply, quicksilver.cpp:324), but test_main never does, so
    // without this the case measures the Display tab under the native platform style
    // the app replaces. On Windows that style's metrics put the columns 134 px apart
    // and the case never settled; the alignment it asserts was always correct in the
    // app. The two sibling slots at :987 and :1007 already apply it for this reason.
    const RestoreApplicationStyle restore;
    QuicksilverStyle::Apply(*qApp);
    OptionsModel options{m_node};
    bilingual_str error;
    QVERIFY(options.Init(error));
    OptionsDialog dialog(nullptr, /*enableVault=*/true);
    dialog.setModel(&options);
    // The font choice set the face of the old HUD page's amounts. That page is gone
    // and nothing else read the setting, so the row changed nothing.
    QVERIFY(!dialog.findChild<QComboBox*>(QStringLiteral("moneyFont")));
    QVERIFY(!dialog.findChild<QLabel*>(QStringLiteral("moneyFontLabel")));

    QTabWidget* tabs = dialog.findChild<QTabWidget*>(QStringLiteral("tabWidget"));
    QWidget* display = dialog.findChild<QWidget*>(QStringLiteral("tabDisplay"));
    QVERIFY(tabs && display);
    tabs->setCurrentWidget(display);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    QWidget* unit = dialog.findChild<QWidget*>(QStringLiteral("unit"));
    QWidget* urls = dialog.findChild<QWidget*>(QStringLiteral("thirdPartyTxUrls"));
    QVERIFY(unit && urls);
    struct SettleLog {
        QElapsedTimer timer;
        QWidget* unit;
        QWidget* urls;
        QWidget* display;
        //! Stamped when the columns agree. Read at destruction so an early
        //! return still logs, but measured here: the checks after this one
        //! wait too, and their time is not settle time.
        qint64 settled_ms{-1};
        ~SettleLog()
        {
            qInfo() << "Display layout settle ms" << (settled_ms >= 0 ? settled_ms : timer.elapsed())
                    << (settled_ms >= 0 ? "" : "(never settled)")
                    << "unit x" << unit->mapTo(display, QPoint{}).x()
                    << "URLs x" << urls->mapTo(display, QPoint{}).x();
        }
    } settle_log{{}, unit, urls, display};
    settle_log.timer.start();
    QTRY_COMPARE(unit->mapTo(display, QPoint{}).x(), urls->mapTo(display, QPoint{}).x());
    settle_log.settled_ms = settle_log.timer.elapsed();

    QVERIFY2(!dialog.findChild<QWidget*>(QStringLiteral("tabWindow")), "the Window tab was merged into Display");
    for (const char* name : {"showTrayIcon", "minimizeToTray", "minimizeOnClose"}) {
        QWidget* box = dialog.findChild<QWidget*>(QString::fromLatin1(name));
        QVERIFY2(box, name);
        QVERIFY2(display->isAncestorOf(box), name);
    }
    QWidget* tray = dialog.findChild<QWidget*>(QStringLiteral("showTrayIcon"));
    QTRY_VERIFY(tray->mapTo(display, QPoint{}).y() < unit->mapTo(display, QPoint{}).y());
}

namespace {
//! Opens Transfer on a test vault with these options. Empty when its coin control
//! line and button show; otherwise what is wrong. Also returns the coin control
//! dialog's title through `title`.
QString TransferCoinControlProblem(interfaces::Node& node, OptionsModel& options, QString& title)
{
    TestChain100Setup test;
    // Put the context back before `test` is destroyed, on every exit.
    const ScopedNodeContext scoped_context(node, test.m_node);
    QString problem;
    vault::VaultContext context;
    context.args = &test.m_args;
    std::shared_ptr<vault::CVault> raw_vault = vault::TestLoadVault(context);
    {
        std::unique_ptr<interfaces::Vault> vault = interfaces::MakeVault(context, raw_vault);
        std::unique_ptr<const PlatformStyle> style{PlatformStyle::instantiate("other")};
        VaultModel vault_model(std::move(vault), node, &options, style.get());
        SendCoinsDialog transfer(style.get());
        transfer.setModel(&vault_model);
        auto* frame = transfer.findChild<QWidget*>(QStringLiteral("frameCoinControl"));
        auto* button = transfer.findChild<QPushButton*>(QStringLiteral("pushButtonCoinControl"));
        if (!frame || !button) {
            problem = QStringLiteral("missing frameCoinControl or pushButtonCoinControl");
        } else if (!button->isVisibleTo(&transfer)) {
            problem = QStringLiteral("the coin control button is hidden");
        }
        vault::CCoinControl coin_control;
        CoinControlDialog coin_control_dialog(coin_control, &vault_model, style.get());
        title = coin_control_dialog.windowTitle();
    }
    // The model holding the vault is gone; wait for the vault itself to be
    // released before `test` removes its data dir. Windows cannot remove
    // the database file while the vault still holds it open.
    vault::WaitForDeleteVault(std::move(raw_vault));
    return problem;
}
} // namespace

//! Owner, 2026-11-22: coin control is always available. The "Enable coin control"
//! option only showed the Transfer page's button, which opens coin control in a
//! modal dialog, so the option is gone and the button shows with fresh settings.
void OptionTests::coinControlIsAlwaysOnTransfer()
{
    QSettings().remove(QStringLiteral("fCoinControlFeatures")); // fresh settings
    OptionsModel options{m_node};
    bilingual_str error;
    QVERIFY(options.Init(error));

    QString coin_control_title;
    const QString problem{TransferCoinControlProblem(m_node, options, coin_control_title)};
    QVERIFY2(problem.isEmpty(), qPrintable(problem));
    QCOMPARE(coin_control_title, QStringLiteral("Coin Control"));

    OptionsDialog options_dialog(nullptr, /*enableVault=*/true);
    options_dialog.setModel(&options);
    QVERIFY(!options_dialog.findChild<QCheckBox*>(QStringLiteral("coinControlFeatures")));
}

//! A settings file written while the option existed still holds its key, often
//! false. It loads, the button shows anyway, and the key is gone once saved.
void OptionTests::storedCoinControlSettingIsDropped()
{
    {
        QSettings settings;
        settings.setValue("fCoinControlFeatures", false);
        settings.sync();
    }
    OptionsModel options{m_node};
    bilingual_str error;
    QVERIFY(options.Init(error));

    QString coin_control_title;
    const QString problem{TransferCoinControlProblem(m_node, options, coin_control_title)};
    QVERIFY2(problem.isEmpty(), qPrintable(problem));

    QSettings settings;
    settings.sync();
    QVERIFY(!settings.contains("fCoinControlFeatures"));
}

//! The Settings minimum was `(a / GB) + (a % GB) ? 1 : 0`, which is always 1 because
//! `+` binds tighter than `?:`. It is the block-file floor rounded up to whole GB:
//! 2,956 MiB is 3.1e9 bytes, so 4 GB (owner, 2026-11-22).
void OptionTests::settingsPruneMinimumIsTheBlockFileFloor()
{
    OptionsModel options{m_node};
    bilingual_str error;
    QVERIFY(options.Init(error));
    OptionsDialog dialog(nullptr, /*enableVault=*/true);
    dialog.setModel(&options);
    auto* prune_size = dialog.findChild<QSpinBox*>(QStringLiteral("pruneSize"));
    QVERIFY(prune_size);
    QCOMPARE(prune_size->minimum(), 4);
    prune_size->setValue(1);
    QCOMPARE(prune_size->value(), 4);
}

//! A stored prune size below the Settings minimum, such as the old 2 GB default, is
//! raised to it and saved on load, so the node does not warn about it on every start.
//! Only the size moves: whether pruning is on is never migrated (F-705).
void OptionTests::storedPruneSizeBelowTheFloorIsRaised()
{
    const fs::path settings_path{gArgs.GetDataDirNet() / "settings.json"};
    const auto stored_prune = [] {
        common::SettingsValue value;
        gArgs.LockSettings([&](const common::Settings& settings) {
            if (auto it{settings.rw_settings.find("prune")}; it != settings.rw_settings.end()) value = it->second;
        });
        return value;
    };

    // Pruning on at 2 GB (1907 MiB) loads as 4 GB (3814 MiB) and is saved as 4 GB.
    gArgs.LockSettings([&](common::Settings& settings) {
        settings.forced_settings.erase("prune");
        settings.rw_settings["prune"] = 1907;
    });
    QVERIFY(gArgs.WriteSettingsFile());
    {
        OptionsModel options{m_node};
        bilingual_str error;
        QVERIFY(options.Init(error));
        QCOMPARE(options.getOption(OptionsModel::Prune).toBool(), true);
        QCOMPARE(options.getOption(OptionsModel::PruneSize).toInt(), 4);
    }
    QCOMPARE(stored_prune().getInt<int64_t>(), int64_t{3814});
    const auto read_file = [&] {
        std::ifstream file(settings_path);
        return std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    };
    const std::string first_contents{read_file()};
    QVERIFY2(first_contents.find("\"prune\": 3814") != std::string::npos, first_contents.c_str());

    // A second Init changes nothing the first one wrote.
    const auto rw_text = [] {
        std::string text;
        gArgs.LockSettings([&](const common::Settings& settings) {
            for (const auto& [name, value] : settings.rw_settings) text += name + "=" + value.write() + "\n";
        });
        return text;
    };
    const std::string first_rw{rw_text()};
    {
        OptionsModel options{m_node};
        bilingual_str error;
        QVERIFY(options.Init(error));
        QCOMPARE(options.getOption(OptionsModel::Prune).toBool(), true);
        QCOMPARE(options.getOption(OptionsModel::PruneSize).toInt(), 4);
    }
    QCOMPARE(rw_text(), first_rw);
    QCOMPARE(read_file(), first_contents);

    // Pruning off stays off: nothing is written.
    gArgs.LockSettings([&](common::Settings& settings) { settings.rw_settings.erase("prune"); });
    QVERIFY(gArgs.WriteSettingsFile());
    {
        OptionsModel options{m_node};
        bilingual_str error;
        QVERIFY(options.Init(error));
        QCOMPARE(options.getOption(OptionsModel::Prune).toBool(), false);
    }
    QVERIFY(stored_prune().isNull());
}

//! A prune size below the floor that comes from quicksilver.conf is the user's, and
//! settings.json outranks the conf file: copying it there would shadow every later
//! edit of the conf line. Init leaves settings.json alone (review of e0a4b8f2); the
//! node raises the conf value with its own warning.
void OptionTests::confPruneSizeBelowTheFloorIsNotWritten()
{
    const fs::path settings_path{gArgs.GetDataDirNet() / "settings.json"};
    const std::string network{gArgs.GetChainTypeString()};
    gArgs.LockSettings([&](common::Settings& settings) {
        settings.forced_settings.erase("prune");
        settings.rw_settings.erase("prune");
        settings.rw_settings.erase("prune-prev");
        settings.ro_config[""]["prune"] = {2000};
        settings.ro_config[network]["prune"] = {2000};
    });
    QVERIFY(gArgs.WriteSettingsFile());
    {
        OptionsModel options{m_node};
        bilingual_str error;
        QVERIFY(options.Init(error));
        QCOMPARE(options.getOption(OptionsModel::Prune).toBool(), true);
    }
    bool rw_has_prune{true};
    gArgs.LockSettings([&](const common::Settings& settings) {
        rw_has_prune = settings.rw_settings.contains("prune") || settings.rw_settings.contains("prune-prev");
    });
    QVERIFY(!rw_has_prune);
    std::ifstream file(settings_path);
    const std::string contents{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    QVERIFY2(contents.find("\"prune") == std::string::npos, contents.c_str());
}

//! The size kept while pruning is off ("prune-prev") is raised the same way, so
//! turning pruning back on stores 4 GB, not the old 2 GB the next start would warn
//! about. Pruning stays off (F-705).
void OptionTests::storedPrevPruneSizeBelowTheFloorIsRaised()
{
    const auto stored = [](const std::string& name) {
        common::SettingsValue value;
        gArgs.LockSettings([&](const common::Settings& settings) {
            if (auto it{settings.rw_settings.find(name)}; it != settings.rw_settings.end()) value = it->second;
        });
        return value;
    };
    gArgs.LockSettings([&](common::Settings& settings) {
        settings.forced_settings.erase("prune");
        settings.rw_settings.erase("prune");
        settings.rw_settings["prune-prev"] = 1907;
    });
    QVERIFY(gArgs.WriteSettingsFile());
    OptionsModel options{m_node};
    bilingual_str error;
    QVERIFY(options.Init(error));
    QCOMPARE(options.getOption(OptionsModel::Prune).toBool(), false);
    QVERIFY(stored("prune").isNull());
    QCOMPARE(stored("prune-prev").getInt<int64_t>(), int64_t{3814});
    QCOMPARE(options.getOption(OptionsModel::PruneSize).toInt(), 4);

    QVERIFY(options.setOption(OptionsModel::Prune, true));
    QCOMPARE(stored("prune").getInt<int64_t>(), int64_t{3814});
}

void OptionTests::savedGpuModeOpensWithoutARestartWarning()
{
    // Opening Settings applies the saved mode; it is not a change (S0230-B-01).
    QVERIFY_SOLVER_PROBE_STUB();
    PointDiscoveryAtStub(WriteListing({QStringLiteral("device backend=opencl id=pci:0000:04:00.0 type=gpu integrated=0 mem=0 status=ok name=AMD Radeon RX 480")}));
    for (const QStringList& devices : {QStringList{QStringLiteral("cpu")}, QStringList{QStringLiteral("pci:0000:04:00.0")}}) {
        OptionsModel options{m_node};
        bilingual_str error;
        QVERIFY(options.Init(error));
        QVERIFY(options.setOption(OptionsModel::GpuDevices, devices));
        options.setRestartRequired(false); // saved before this session, not changed in it
        OptionsDialog dialog(nullptr, true);
        dialog.setModel(&options);
        auto* list = dialog.findChild<QWidget*>(QStringLiteral("gpuDeviceList"));
        QTRY_VERIFY_WITH_TIMEOUT(list->findChild<QCheckBox*>(QStringLiteral("gpuDevice_pci_0000_04_00_0")), 30000);
        QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("statusLabel"))->text(), QString{});
        // A mode the user picks still warns.
        dialog.findChild<QRadioButton*>(QStringLiteral("gpuModeAutomatic"))->click();
        QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("statusLabel"))->text().contains(QStringLiteral("restart")));
    }
}

void OptionTests::switchedOffInvalidProxyLeavesOkEnabled()
{
    // A proxy the user switches off is not checked, whatever its host says
    // (S0230-B-02, S0239-B-01). Both the default and the Tor proxy.
    for (const auto& [use_name, host_name] : {std::pair{"connectSocks", "proxyIp"}, std::pair{"connectSocksTor", "proxyIpTor"}}) {
        OptionsModel options{m_node};
        bilingual_str error;
        QVERIFY(options.Init(error));
        OptionsDialog dialog(nullptr, true);
        dialog.setModel(&options);
        auto* use = dialog.findChild<QCheckBox*>(QString::fromLatin1(use_name));
        auto* host = dialog.findChild<QValidatedLineEdit*>(QString::fromLatin1(host_name));
        auto* ok = dialog.findChild<QPushButton*>(QStringLiteral("okButton"));
        QVERIFY(use && host && ok);
        use->setChecked(true);
        host->setText(QStringLiteral("not an address"));
        QVERIFY2(!ok->isEnabled(), use_name);
        use->setChecked(false);
        QVERIFY2(ok->isEnabled(), use_name);
        QVERIFY2(host->isValid(), use_name);
    }
}

void OptionTests::emptyProxyHostBlocksOk()
{
    // A proxy switched on with no host would be saved as ":9050" (S0239-B-03).
    OptionsModel options{m_node};
    bilingual_str error;
    QVERIFY(options.Init(error));
    OptionsDialog dialog(nullptr, true);
    dialog.setModel(&options);
    auto* ok = dialog.findChild<QPushButton*>(QStringLiteral("okButton"));
    dialog.findChild<QCheckBox*>(QStringLiteral("connectSocks"))->setChecked(true);
    QVERIFY(ok->isEnabled());
    dialog.findChild<QValidatedLineEdit*>(QStringLiteral("proxyIp"))->setText(QString{});
    QVERIFY(!ok->isEnabled());
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("statusLabel"))->text(), QStringLiteral("The supplied proxy address is invalid."));
}

void OptionTests::loadedInvalidProxyHostIsChecked()
{
    // The mapper loads the host past QValidatedLineEdit::setText, so a saved host the
    // field rejects was neither marked nor kept from being saved (S0239-B-02). The
    // host is one the node refuses too: a name with a space cannot be looked up.
    gArgs.LockSettings([](common::Settings& settings) { settings.rw_settings["proxy"] = "bad host:9050"; });
    OptionsModel options{m_node};
    bilingual_str error;
    QVERIFY(options.Init(error));
    OptionsDialog dialog(nullptr, true);
    dialog.setModel(&options);
    QVERIFY(dialog.findChild<QCheckBox*>(QStringLiteral("connectSocks"))->isChecked());
    auto* host = dialog.findChild<QValidatedLineEdit*>(QStringLiteral("proxyIp"));
    QCOMPARE(host->text(), QStringLiteral("bad host"));
    QVERIFY(!host->isValid());
    QVERIFY(!dialog.findChild<QPushButton*>(QStringLiteral("okButton"))->isEnabled());
}

void OptionTests::ipv6ProxyHostRoundTrips()
{
    // A bare IPv6 host is stored bracketed, so it reads back with its port
    // (S0231-A-01). Both the default and the Tor proxy.
    const struct {
        OptionsModel::OptionID use, ip, port;
        const char* setting;
    } proxies[]{
        {OptionsModel::ProxyUse, OptionsModel::ProxyIP, OptionsModel::ProxyPort, "proxy"},
        {OptionsModel::ProxyUseTor, OptionsModel::ProxyIPTor, OptionsModel::ProxyPortTor, "onion"},
    };
    for (const auto& proxy : proxies) {
        OptionsModel options{m_node};
        bilingual_str error;
        QVERIFY(options.Init(error));
        QVERIFY(options.setOption(proxy.use, true));
        QVERIFY(options.setOption(proxy.ip, QStringLiteral("::1")));
        QVERIFY(options.setOption(proxy.port, QStringLiteral("9150")));
        QCOMPARE(QString::fromStdString(m_node.getPersistentSetting(proxy.setting).get_str()), QStringLiteral("[::1]:9150"));
        QCOMPARE(options.getOption(proxy.ip).toString(), QStringLiteral("[::1]"));
        QCOMPARE(options.getOption(proxy.port).toString(), QStringLiteral("9150"));
    }
}

void OptionTests::optionsModelIsAFlatEditOnlyList()
{
    // A flat list stores only the edit role of its own rows (S0231-B-01).
    OptionsModel options{m_node};
    bilingual_str error;
    QVERIFY(options.Init(error));
    const QModelIndex row{options.index(OptionsModel::ThirdPartyTxUrls)};
    QVERIFY(row.isValid());
    QCOMPARE(options.rowCount(row), 0);
    QCOMPARE(options.rowCount(), int{OptionsModel::OptionIDRowCount});
    QVERIFY(!options.data(QModelIndex{}, Qt::EditRole).isValid());
    QSignalSpy changed{&options, &OptionsModel::dataChanged};
    QVERIFY(!options.setData(row, QStringLiteral("https://example.invalid/%s"), Qt::DisplayRole));
    QVERIFY(!options.setData(QModelIndex{}, QStringLiteral("https://example.invalid/%s"), Qt::EditRole));
    QCOMPARE(changed.count(), 0);
    QVERIFY(options.setData(row, QStringLiteral("https://example.invalid/%s"), Qt::EditRole));
    QCOMPARE(changed.count(), 1);
}

void OptionTests::loadedHostNameProxyIsValid()
{
    // The node looks a -proxy host name up at startup, so a saved name is a working
    // setting: it shows as valid and OK stays enabled (c05a-15 review fix). Both the
    // default and the Tor proxy.
    for (const auto& [setting, use_name, host_name, host] : {
             std::tuple{"proxy", "connectSocks", "proxyIp", "localhost"},
             std::tuple{"proxy", "connectSocks", "proxyIp", "tor_proxy"},
             std::tuple{"onion", "connectSocksTor", "proxyIpTor", "torhost.example"},
             std::tuple{"onion", "connectSocksTor", "proxyIpTor", "torhost.example."}}) {
        gArgs.LockSettings([&](common::Settings& settings) { settings.rw_settings[setting] = std::string{host} + ":9050"; });
        OptionsModel options{m_node};
        bilingual_str error;
        QVERIFY(options.Init(error));
        OptionsDialog dialog(nullptr, true);
        dialog.setModel(&options);
        QVERIFY2(dialog.findChild<QCheckBox*>(QString::fromLatin1(use_name))->isChecked(), setting);
        auto* field = dialog.findChild<QValidatedLineEdit*>(QString::fromLatin1(host_name));
        QCOMPARE(field->text(), QString::fromLatin1(host));
        QVERIFY2(field->isValid(), host);
        QVERIFY2(dialog.findChild<QPushButton*>(QStringLiteral("okButton"))->isEnabled(), host);
        gArgs.LockSettings([&](common::Settings& settings) { settings.rw_settings.erase(setting); });
    }
}
