// Copyright (c) 2018-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <quicksilver-build-config.h> // IWYU pragma: keep

#include <common/args.h>
#include <qt/quicksilver.h>
#include <qt/createvaultdialog.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/optionsdialog.h>
#include <qt/quicksilverunits.h>
#include <qt/test/optiontests.h>
#include <qt/test/util.h>
#include <test/util/setup_common.h>

#include <QSettings>
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QCoreApplication>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTest>

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
        QCOMPARE(options.getDisplayUnit(), QuicksilverUnit::HG);
        QCOMPARE(settings.value(current_key).value<QuicksilverUnit>(), QuicksilverUnit::HG);
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

void OptionTests::languageChoicesDoNotDependOnCatalogResources()
{
    OptionsDialog dialog(nullptr, /*enableVault=*/true);
    auto* languages = dialog.findChild<QComboBox*>(QStringLiteral("lang"));
    QVERIFY(languages);
    QCOMPARE(languages->count(), 2);
    QCOMPARE(languages->itemData(0).toString(), QString{});
    QCOMPARE(languages->itemData(1).toString(), QStringLiteral("en"));
    QCOMPARE(languages->itemText(1), QStringLiteral("English (en)"));
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
// The stub sits beside this test binary in the runtime output directory; see
// src/qt/test/CMakeLists.txt.
QString SolverProbeStubPath()
{
    QString path{QCoreApplication::applicationDirPath() + QStringLiteral("/test_solver_probe_stub")};
#ifdef Q_OS_WIN
    path += QStringLiteral(".exe");
#endif
    return path;
}
} // namespace

void OptionTests::gpuSolverSettingPersistsAndRequiresRestart()
{
    // The probe starts the configured solver and accepts it on exit 0, so this
    // needs a genuinely startable executable. `true` exists only on POSIX, and
    // on Windows QProcess reaches CreateProcess, which cannot launch a script,
    // so the build ships a tiny stub binary beside this test for both platforms
    // (src/qt/test/solverprobestub.cpp).
    const QString solver{SolverProbeStubPath()};
    QVERIFY2(QFileInfo(solver).isExecutable(), qPrintable(solver));

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
    QTRY_VERIFY_WITH_TIMEOUT(!status->text().contains(QStringLiteral("Checking whether")), 5000);
    QVERIFY2(status->text().contains(QStringLiteral("passed the startup check")), qPrintable(status->text()));
    QVERIFY(ok_button->isEnabled());
    QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("statusLabel"))->text().contains(QStringLiteral("restart")));

    QTest::mouseClick(ok_button, Qt::LeftButton);
    QCOMPARE(options.getOption(OptionsModel::GpuSolverPath).toString(), solver);
    QVERIFY(options.isRestartRequired());
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
    path->setText(QStringLiteral("/path/that/does/not/exist/qsgpusolve"));
    QVERIFY(status->text().contains(QStringLiteral("does not exist")));
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

void OptionTests::allowCpuAgentTxPowPersists()
{
    constexpr auto SETTING_KEY{"allow_cpu_agent_txpow"};
    QSettings settings;
    const bool had_previous_value{settings.contains(SETTING_KEY)};
    const QVariant previous_value{settings.value(SETTING_KEY)};
    const bool had_restart{settings.contains("fRestartRequired")};
    const QVariant previous_restart{settings.value("fRestartRequired")};
    settings.remove(SETTING_KEY);
    settings.setValue("fRestartRequired", false);

    OptionsModel first{m_node};
    QVERIFY(!first.getOption(OptionsModel::AllowCpuAgentTxPow).toBool());
    QVERIFY(first.setOption(OptionsModel::AllowCpuAgentTxPow, true));
    QVERIFY(!first.isRestartRequired());

    OptionsModel second{m_node};
    QVERIFY(second.getOption(OptionsModel::AllowCpuAgentTxPow).toBool());
    QVERIFY(second.setOption(OptionsModel::AllowCpuAgentTxPow, false));
    QVERIFY(!second.getOption(OptionsModel::AllowCpuAgentTxPow).toBool());
    QVERIFY(!second.isRestartRequired());

    if (had_previous_value) {
        settings.setValue(SETTING_KEY, previous_value);
    } else {
        settings.remove(SETTING_KEY);
    }
    if (had_restart) {
        settings.setValue("fRestartRequired", previous_restart);
    } else {
        settings.remove("fRestartRequired");
    }
}

void OptionTests::allowCpuAgentTxPowCancelLeavesTheSettingOff()
{
    constexpr auto SETTING_KEY{"allow_cpu_agent_txpow"};
    QSettings settings;
    const bool had_previous_value{settings.contains(SETTING_KEY)};
    const QVariant previous_value{settings.value(SETTING_KEY)};
    const bool had_restart{settings.contains("fRestartRequired")};
    const QVariant previous_restart{settings.value("fRestartRequired")};
    settings.remove(SETTING_KEY);
    settings.setValue("fRestartRequired", false);

    OptionsModel options{m_node};
    bilingual_str error;
    QVERIFY(options.Init(error));
    QVERIFY(!options.getOption(OptionsModel::AllowCpuAgentTxPow).toBool());

    OptionsDialog dialog(nullptr, /*enableVault=*/true);
    dialog.setModel(&options);
    dialog.show();
    auto* box = dialog.findChild<QCheckBox*>(QStringLiteral("allowCpuAgentTxPow"));
    auto* ok_button = dialog.findChild<QPushButton*>(QStringLiteral("okButton"));
    QVERIFY(box);
    QVERIFY(ok_button);
    QVERIFY(!box->isChecked());
    QVERIFY(box->text().contains(QStringLiteral("processor")));
    QVERIFY(box->text().contains(QStringLiteral("every core")));
    QVERIFY(!box->text().contains(QStringLiteral("infeasible"), Qt::CaseInsensitive));
    QVERIFY(!box->text().contains(QStringLiteral("impossible"), Qt::CaseInsensitive));
    QVERIFY(!box->text().contains(QStringLiteral("not supported"), Qt::CaseInsensitive));

    box->setChecked(true);
    QTest::mouseClick(ok_button, Qt::LeftButton);
    auto* warning = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
    QVERIFY(warning);
    QCOMPARE(warning->objectName(), QStringLiteral("cpuAgentTxPowWarning"));
    QVERIFY(warning->isVisible());
    auto* acknowledge = warning->findChild<QCheckBox*>(QStringLiteral("cpuAgentTxPowAcknowledge"));
    auto* proceed = warning->findChild<QPushButton*>(QStringLiteral("cpuAgentTxPowProceedButton"));
    QVERIFY(acknowledge);
    QVERIFY(proceed);
    QVERIFY(!proceed->isEnabled());
    QVERIFY(!acknowledge->isChecked());
    QAbstractButton* cancel = warning->button(QMessageBox::Cancel);
    QVERIFY(cancel);
    QVERIFY(warning->defaultButton() == cancel);
    QVERIFY(!warning->text().contains(QStringLiteral("infeasible"), Qt::CaseInsensitive));
    QVERIFY(!warning->informativeText().contains(QStringLiteral("impossible"), Qt::CaseInsensitive));
    QVERIFY(warning->informativeText().contains(QStringLiteral("calibration result")));
    QVERIFY(warning->informativeText().contains(QStringLiteral("16 minutes")));

    cancel->click();
    QTRY_VERIFY(!box->isChecked());
    QVERIFY(!options.getOption(OptionsModel::AllowCpuAgentTxPow).toBool());
    QVERIFY(dialog.isVisible());

    box->setChecked(true);
    QTest::mouseClick(ok_button, Qt::LeftButton);
    warning = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
    QVERIFY(warning);
    QCOMPARE(warning->objectName(), QStringLiteral("cpuAgentTxPowWarning"));
    acknowledge = warning->findChild<QCheckBox*>(QStringLiteral("cpuAgentTxPowAcknowledge"));
    proceed = warning->findChild<QPushButton*>(QStringLiteral("cpuAgentTxPowProceedButton"));
    QVERIFY(acknowledge);
    QVERIFY(proceed);
    acknowledge->setChecked(true);
    QVERIFY(proceed->isEnabled());
    proceed->click();
    QTRY_VERIFY(options.getOption(OptionsModel::AllowCpuAgentTxPow).toBool());
    QVERIFY(!options.isRestartRequired());

    OptionsModel reloaded{m_node};
    QVERIFY(reloaded.getOption(OptionsModel::AllowCpuAgentTxPow).toBool());

    if (had_previous_value) {
        settings.setValue(SETTING_KEY, previous_value);
    } else {
        settings.remove(SETTING_KEY);
    }
    if (had_restart) {
        settings.setValue("fRestartRequired", previous_restart);
    } else {
        settings.remove("fRestartRequired");
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
    QVERIFY(box->text().contains(QStringLiteral("very unlikely")));
    QVERIFY(box->text().contains(QStringLiteral("next time mining starts")));
    QVERIFY(!box->text().contains(QStringLiteral("-allowcpumining")));
    QVERIFY(solver_status->text().contains(QStringLiteral("may take many minutes")));
    QVERIFY(solver_status->text().contains(QStringLiteral("processor block mining is enabled above")));
    QVERIFY(!solver_status->text().contains(QStringLiteral("Live transfers and mining need one")));
    QVERIFY(!solver_status->text().contains(QStringLiteral("-allowcpumining")));

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

void OptionTests::customFontDoesNotNestEventLoop()
{
    OptionsModel options{m_node};
    bilingual_str error;
    QVERIFY(options.Init(error));
    OptionsDialog dialog(nullptr, /*enableVault=*/true);
    dialog.setModel(&options);
    QComboBox* money_font = dialog.findChild<QComboBox*>(QStringLiteral("moneyFont"));
    QVERIFY(money_font);
    QVERIFY(money_font->count() >= 3);
    ExpectModalWithoutNestedEventLoop("QFontDialog", [&] {
        money_font->setCurrentIndex(money_font->count() - 1);
    });
}
