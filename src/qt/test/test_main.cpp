// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <quicksilver-build-config.h> // IWYU pragma: keep

#include <interfaces/init.h>
#include <interfaces/node.h>
#include <qt/quicksilver.h>
#include <qt/guiconstants.h>
#include <qt/test/apptests.h>
#include <qt/test/guiutiltests.h>
#include <qt/test/introtests.h>
#include <qt/test/maturitytests.h>
#include <qt/test/optiontests.h>
#include <qt/test/quicksilverstyletests.h>
#include <qt/test/rpcnestedtests.h>
#include <qt/test/storagecoststests.h>
#include <qt/test/uritests.h>
#include <test/util/setup_common.h>
#include <util/chaintype.h>

#ifdef ENABLE_VAULT
#include <qt/test/addressbooktests.h>
#include <qt/test/vaultsummarytests.h>
#include <qt/test/vaulttests.h>
#endif // ENABLE_VAULT

#include <QApplication>
#include <QDebug>
#include <QObject>
#include <QSettings>
#include <QTest>

#include <functional>

const std::function<void(const std::string&)> G_TEST_LOG_FUN{};

const std::function<std::vector<const char*>()> G_TEST_COMMAND_LINE_ARGUMENTS{};

const std::function<std::string()> G_TEST_GET_FULL_NAME{};

// This is all you need to run all the tests
int main(int argc, char* argv[])
{
    // Initialize persistent globals with the testing setup state for sanity.
    // E.g. -datadir in gArgs is set to a temp directory dummy value (instead
    // of defaulting to the default datadir), or globalChainParams is set to
    // sandbox params.
    //
    // All tests must use their own testing setup (if needed).
    fs::create_directories([] {
        BasicTestingSetup dummy{ChainType::SANDBOX};
        return gArgs.GetDataDirNet() / "blocks";
    }());

    std::unique_ptr<interfaces::Init> init = interfaces::MakeGuiInit();
    gArgs.ForceSetArg("-listen", "0");
    gArgs.ForceSetArg("-listenonion", "0");
    gArgs.ForceSetArg("-discover", "0");
    gArgs.ForceSetArg("-dnsseed", "0");
    gArgs.ForceSetArg("-fixedseeds", "0");
    gArgs.ForceSetArg("-natpmp", "0");

    std::string error;
    if (!gArgs.ReadConfigFiles(error, true)) qWarning() << error.c_str();

    // Prefer the "minimal" platform for the test instead of the normal default
    // platform ("xcb", "windows", or "cocoa") so tests can't unintentionally
    // interfere with any background GUIs and don't require extra resources.
    #if defined(WIN32)
        if (getenv("QT_QPA_PLATFORM") == nullptr) _putenv_s("QT_QPA_PLATFORM", "minimal");
    #else
        setenv("QT_QPA_PLATFORM", "minimal", 0 /* overwrite */);
    #endif


    QCoreApplication::setOrganizationName(QAPP_ORG_NAME);
    QCoreApplication::setApplicationName(QAPP_APP_NAME_DEFAULT "-test");

    int num_test_failures{0};

    {
        QuicksilverApplication app;
        app.createNode(*init);

        QuicksilverStyleTests style_tests;
        num_test_failures += QTest::qExec(&style_tests);

        GUIUtilTests guiutil_tests;
        num_test_failures += QTest::qExec(&guiutil_tests);

        IntroTests intro_tests;
        num_test_failures += QTest::qExec(&intro_tests);

        MaturityTests maturity_tests;
        num_test_failures += QTest::qExec(&maturity_tests);

        StorageCostsTests storage_costs_tests;
        num_test_failures += QTest::qExec(&storage_costs_tests);

        AppTests app_tests(app);
        num_test_failures += QTest::qExec(&app_tests);

        OptionTests options_tests(app.node());
        num_test_failures += QTest::qExec(&options_tests);

        URITests test1;
        num_test_failures += QTest::qExec(&test1);

        RPCNestedTests test3(app.node());
        num_test_failures += QTest::qExec(&test3);

#ifdef ENABLE_VAULT
        VaultSummaryTests vault_summary_tests;
        num_test_failures += QTest::qExec(&vault_summary_tests);

        VaultTests test5(app.node());
        num_test_failures += QTest::qExec(&test5);

        AddressBookTests test6(app.node());
        num_test_failures += QTest::qExec(&test6);
#endif

        if (num_test_failures) {
            qWarning("\nFailed tests: %d\n", num_test_failures);
        } else {
            qDebug("\nAll tests passed.\n");
        }
    }

    QSettings settings;
    settings.clear();

    return num_test_failures;
}
