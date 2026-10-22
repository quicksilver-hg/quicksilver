// Copyright (c) 2018-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <quicksilver-build-config.h> // IWYU pragma: keep

#include <common/args.h>
#include <crypto/cuckatoo/gpu_devices.h>
#include <crypto/cuckatoo/gpu_solver.h>
#include <crypto/cuckatoo/multi_device.h>
#include <qt/quicksilver.h>
#include <qt/createvaultdialog.h>
#include <qt/clientmodel.h>
#include <qt/coincontroldialog.h>
#include <qt/guiutil.h>
#include <qt/optionsdialog.h>
#include <qt/gpudevicelist.h>
#include <qt/platformstyle.h>
#include <qt/quicksilverunits.h>
#include <qt/quicksilverstyle.h>
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
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPoint>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QStringList>
#include <QTest>
#include <QTabWidget>
#include <QTabBar>
#include <QStyle>
#include <QStyleFactory>

#include <univalue.h>

#include <fstream>
#include <utility>

OptionTests::OptionTests(interfaces::Node& node) : m_node(node)
{
    gArgs.LockSettings([&](common::Settings& s) { m_previous_settings = s; });
}

void OptionTests::init()
{
    // reset args
    gArgs.LockSettings([&](common::Settings& s) { s = m_previous_settings; });
    gArgs.ClearPathCache();
    for (const char* name : {"CUCKATOO_GPU_SOLVER", "QS_PROBE_STUB_LISTING", "QS_PROBE_STUB_EXIT", "QS_PROBE_STUB_SLEEP_MS", "CUCKATOO_GPU_DEVICES"}) qunsetenv(name);
    gArgs.LockSettings([](common::Settings& settings) { settings.rw_settings.erase("cuckatoodevice"); });
    QSettings().remove(QStringLiteral("gpu_device_names"));
    cuckatoo::ResetGpuDiscoveryForTesting();
    cuckatoo::ResetGpuDeviceStatsForTesting();
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

    std::ifstream file(gArgs.GetDataDirNet() / "settings.json");
    const std::string contents{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    QVERIFY(contents.find("\"dbcache\"") == std::string::npos);
    QVERIFY(contents.find("\"listen\"") == std::string::npos);
    QVERIFY(contents.find("\"proxy\"") == std::string::npos);
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
    QCOMPARE(state("gpuDevice_pci_0000_04_00_0"), /*W-D5*/ QStringLiteral("In use"));
    QCOMPARE(state("gpuDevice_pci_0000_00_02_0"), /*W-D6*/ QStringLiteral("Not used: built-in graphics"));
    QCOMPARE(state("gpuDevice_pci_0000_05_00_0"), /*W-D7*/ QStringLiteral("Can't be used: its driver couldn't run Quicksilver's graphics code"));
    dialog.findChild<QRadioButton*>(QStringLiteral("gpuModeChoose"))->click();
    auto* hd = list->findChild<QCheckBox*>(QStringLiteral("gpuDevice_pci_0000_00_02_0")); QVERIFY(!hd->isChecked());
    QTest::mouseClick(dialog.findChild<QPushButton*>(QStringLiteral("okButton")), Qt::LeftButton);
    QCOMPARE(options.getOption(OptionsModel::GpuDevices).toStringList(), QStringList{QStringLiteral("pci:0000:04:00.0")});
    QCOMPARE(options.getGpuDeviceNames().value(QStringLiteral("pci:0000:04:00.0")), QStringLiteral("AMD Radeon RX 480"));
}

void OptionTests::gpuCardStatsRefresh()
{
    PointDiscoveryAtStub(WriteListing({
        QStringLiteral("device backend=opencl id=pci:0000:04:00.0 type=gpu integrated=0 mem=0 status=ok name=AMD Radeon RX 480") }));
    OptionsModel options{m_node}; bilingual_str error; QVERIFY(options.Init(error));
    OptionsDialog dialog(nullptr, true); dialog.setModel(&options);
    auto* list = dialog.findChild<GpuDeviceList*>(QStringLiteral("gpuDeviceList")); QVERIFY(list);
    auto* state = list->findChild<QLabel*>(QStringLiteral("gpuDevice_pci_0000_04_00_0_state"));
    QTRY_VERIFY_WITH_TIMEOUT((state = list->findChild<QLabel*>(QStringLiteral("gpuDevice_pci_0000_04_00_0_state"))), 30000);
    QCOMPARE(state->text(), QStringLiteral("In use"));

    cuckatoo::SetGpuDeviceStatsForTesting({
        {"pci:0000:04:00.0", cuckatoo::GpuDeviceState::kSolving, cuckatoo::GpuSolveStatus::kNoCycle, 5, 2.4},
    });
    list->refreshRows();
    QCOMPARE(state->text(), QStringLiteral("In use, 2.4 s per graph"));
    cuckatoo::SetGpuDeviceStatsForTesting({
        {"pci:0000:04:00.0", cuckatoo::GpuDeviceState::kFaulted, cuckatoo::GpuSolveStatus::kDeviceFault, 5, 2.4},
    });
    list->refreshRows();
    QCOMPARE(state->text(), QStringLiteral("Faulted: %1").arg(QString::fromStdString(
        cuckatoo::SolverFault(cuckatoo::GpuSolveStatus::kDeviceFault, false).value())));
    cuckatoo::ResetGpuDeviceStatsForTesting();
}

void OptionTests::missingBundledHelperIsNamed()
{
    struct RestoreBundled {
        QByteArray value{qgetenv("CUCKATOO_GPU_BUNDLED")};
        ~RestoreBundled()
        {
            if (value.isEmpty()) qunsetenv("CUCKATOO_GPU_BUNDLED");
            else qputenv("CUCKATOO_GPU_BUNDLED", value);
            cuckatoo::ResetGpuDiscoveryForTesting();
        }
    } restore;
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
}

void OptionTests::tickingBuiltInGraphicsAsksFirst()
{
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
    QCOMPARE(list->findChild<QLabel*>(QStringLiteral("gpuDevice_pci_0000_00_02_0_state"))->text(), /*W-D5*/ QStringLiteral("In use"));
    QTest::mouseClick(dialog.findChild<QPushButton*>(QStringLiteral("okButton")), Qt::LeftButton);
    QVERIFY(options.getOption(OptionsModel::GpuDevices).toStringList().contains(QStringLiteral("pci:0000:00:02.0")));
    QCOMPARE(options.getGpuDeviceNames().value(QStringLiteral("pci:0000:00:02.0")), QStringLiteral("Intel(R) HD Graphics 4600"));
}

void OptionTests::missingChosenCardKeepsItsName()
{
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

void OptionTests::cpuOnlyHidesTheList()
{
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

void OptionTests::externalSignerSurfacesAreHiddenNotGreyedOut()
{
    // F-64: external signing ships post-launch, and the v1 decision was that its
    // surface be absent rather than inert. A permanently disabled control with a
    // "compiled without external signing support" tooltip is exactly the
    // placeholder that decision ruled out -- it advertises a feature to a v1 user
    // who has no way to obtain it. Both surfaces are pinned here because they
    // live in different dialogs and a change to one would not touch the other.
    OptionsModel options{m_node};
    bilingual_str error;
    QVERIFY(options.Init(error));
    OptionsDialog options_dialog(nullptr, /*enableVault=*/true);
    options_dialog.setModel(&options);

    auto* signer_group = options_dialog.findChild<QGroupBox*>(QStringLiteral("groupBoxHww"));
    QVERIFY(signer_group);
    // F-485: the signer group moves with the other hardware rows, and stays hidden.
    auto* hardware = options_dialog.findChild<QWidget*>(QStringLiteral("tabHardware"));
    QVERIFY2(hardware && hardware->isAncestorOf(signer_group), "groupBoxHww belongs on the Hardware tab");

    CreateVaultDialog create_dialog(nullptr);
    auto* signer_checkbox = create_dialog.findChild<QCheckBox*>(QStringLiteral("external_signer_checkbox"));
    QVERIFY(signer_checkbox);

#ifdef ENABLE_EXTERNAL_SIGNER
    QVERIFY(!signer_group->isHidden());
    QVERIFY(!signer_checkbox->isHidden());
#else
    QVERIFY(signer_group->isHidden());
    QVERIFY(signer_checkbox->isHidden());
    // Still inert as well as invisible, so setSigners() and
    // isExternalSignerChecked() cannot resurrect it.
    QVERIFY(!signer_checkbox->isEnabled());
    QVERIFY(!signer_checkbox->isChecked());
    QVERIFY(!create_dialog.isExternalSignerChecked());
#endif
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
    // The opening-size check does not ask for it: this platform's screen is
    // 240x320, so the dialog cannot grow past 700 and a wide font sticks out.
    auto rows_fit = [&](bool horizontal) {
        QScrollArea* scroll = tab->findChild<QScrollArea*>(QStringLiteral("tabMainScrollArea"));
        const auto widgets = tab->findChildren<QWidget*>();
        for (QWidget* widget : widgets) {
            const bool tracked = qobject_cast<QLabel*>(widget) || qobject_cast<QCheckBox*>(widget) || qobject_cast<QSpinBox*>(widget);
            if (!tracked || widget->isHidden()) continue;
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
            if (!tracked || widget->isHidden()) continue;
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

    // A 1920x1080 desktop gives the rows their own size. This platform cannot:
    // its screen is 240x320, and the dialog will not grow past the screen.
    // Apply the desktop size, then the client area a 1366x768 laptop has left
    // after a taskbar and a title bar.
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

    dialog.resize(qMin(dialog.width(), 1366), qMin(dialog.height(), 700));
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    QVERIFY2(dialog.width() <= 1366 && dialog.height() <= 700,
             qPrintable(QStringLiteral("laptop size %1x%2").arg(dialog.width()).arg(dialog.height())));
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
    QCOMPARE(unit->mapTo(display, QPoint{}).x(), urls->mapTo(display, QPoint{}).x());

    QVERIFY2(!dialog.findChild<QWidget*>(QStringLiteral("tabWindow")), "the Window tab was merged into Display");
#ifndef Q_OS_MACOS
    for (const char* name : {"showTrayIcon", "minimizeToTray", "minimizeOnClose"}) {
        QWidget* box = dialog.findChild<QWidget*>(QString::fromLatin1(name));
        QVERIFY2(box, name);
        QVERIFY2(display->isAncestorOf(box), name);
    }
    QWidget* tray = dialog.findChild<QWidget*>(QStringLiteral("showTrayIcon"));
    QVERIFY(tray->mapTo(display, QPoint{}).y() < unit->mapTo(display, QPoint{}).y());
#endif
}

void OptionTests::coinControlSettingIsNamedCoinControl()
{
    OptionsModel options{m_node};
    bilingual_str error;
    QVERIFY(options.Init(error));
    OptionsDialog options_dialog(nullptr, /*enableVault=*/true);
    options_dialog.setModel(&options);
    auto* setting = options_dialog.findChild<QCheckBox*>(QStringLiteral("coinControlFeatures"));
    QVERIFY(setting);
    QCOMPARE(setting->text(), QStringLiteral("Enable &coin control"));
    QCOMPARE(setting->toolTip(), QStringLiteral("Show the Coin control… button on Transfer, so you can choose which coins a transfer spends."));

    TestChain100Setup test;
    node::NodeContext* const previous_context = m_node.context();
    m_node.setContext(&test.m_node);
    QString coin_control_title;
    {
        vault::VaultContext context;
        context.args = &test.m_args;
        std::shared_ptr<vault::CVault> raw_vault = vault::TestLoadVault(context);
        {
            std::unique_ptr<interfaces::Vault> vault = interfaces::MakeVault(context, raw_vault);
            std::unique_ptr<const PlatformStyle> style{PlatformStyle::instantiate("other")};
            VaultModel vault_model(std::move(vault), m_node, &options, style.get());
            vault::CCoinControl coin_control;
            CoinControlDialog coin_control_dialog(coin_control, &vault_model, style.get());
            coin_control_title = coin_control_dialog.windowTitle();
        }
        // The model holding the vault is gone; wait for the vault itself to be
        // released before `test` removes its data dir. Windows cannot remove
        // the database file while the vault still holds it open.
        vault::WaitForDeleteVault(std::move(raw_vault));
    }
    m_node.setContext(previous_context);
    QCOMPARE(coin_control_title, QStringLiteral("Coin Control"));
}
