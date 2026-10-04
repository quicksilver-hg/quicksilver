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

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

const std::function<void(const std::string&)> G_TEST_LOG_FUN{};

const std::function<std::vector<const char*>()> G_TEST_COMMAND_LINE_ARGUMENTS{};

const std::function<std::string()> G_TEST_GET_FULL_NAME{};

namespace {
[[noreturn]] void TestTerminateHandler() noexcept
{
    std::fputs("test_quicksilver-qt: std::terminate", stderr);
    try {
        if (const auto exception = std::current_exception()) std::rethrow_exception(exception);
    } catch (const std::exception& exception) {
        std::fputs(": ", stderr);
        std::fputs(exception.what(), stderr);
    } catch (...) {
    }
    std::fputc('\n', stderr);
    std::fflush(stdout);
    std::fflush(stderr);
    std::abort();
}
} // namespace

// This is all you need to run all the tests
int main(int argc, char* argv[])
{
    // F-382: keep piped Qt test output readable if the process aborts, including under MSVC.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::set_terminate(TestTerminateHandler);

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

        struct TestClass {
            const QMetaObject* meta_object;
            std::function<std::unique_ptr<QObject>()> create;
        };
        const TestClass test_classes[]{
            {&QuicksilverStyleTests::staticMetaObject, [&] { return std::make_unique<QuicksilverStyleTests>(); }},
            {&GUIUtilTests::staticMetaObject, [&] { return std::make_unique<GUIUtilTests>(); }},
            {&IntroTests::staticMetaObject, [&] { return std::make_unique<IntroTests>(); }},
            {&MaturityTests::staticMetaObject, [&] { return std::make_unique<MaturityTests>(); }},
            {&StorageCostsTests::staticMetaObject, [&] { return std::make_unique<StorageCostsTests>(); }},
            {&AppTests::staticMetaObject, [&] { return std::make_unique<AppTests>(app); }},
            {&OptionTests::staticMetaObject, [&] { return std::make_unique<OptionTests>(app.node()); }},
            {&URITests::staticMetaObject, [&] { return std::make_unique<URITests>(); }},
            {&RPCNestedTests::staticMetaObject, [&] { return std::make_unique<RPCNestedTests>(app.node()); }},
#ifdef ENABLE_VAULT
            {&VaultSummaryTests::staticMetaObject, [&] { return std::make_unique<VaultSummaryTests>(); }},
            {&VaultTests::staticMetaObject, [&] { return std::make_unique<VaultTests>(app.node()); }},
            {&AddressBookTests::staticMetaObject, [&] { return std::make_unique<AddressBookTests>(app.node()); }},
#endif
        };

        const TestClass* selected{nullptr};
        if (argc > 1) {
            const std::string_view class_name{argv[1]};
            for (const auto& test_class : test_classes) {
                if (class_name == test_class.meta_object->className()) {
                    selected = &test_class;
                    break;
                }
            }
            if (!selected) {
                std::fputs("Usage: ", stderr);
                std::fputs(argv[0], stderr);
                std::fputs(" [<TestClass> [<QTest args...>]]\n", stderr);
                for (const auto& test_class : test_classes) {
                    std::fputs(test_class.meta_object->className(), stderr);
                    std::fputc('\n', stderr);
                }
                return EXIT_FAILURE;
            }
        }

        // Keep objects alive through the run, and construct each just before its tests.
        std::vector<std::unique_ptr<QObject>> test_objects;
        std::vector<char*> test_args;
        if (selected) {
            test_args.push_back(argv[0]);
            test_args.insert(test_args.end(), argv + 2, argv + argc);
            test_args.push_back(nullptr);
        }
        for (const auto& test_class : test_classes) {
            if (selected && selected != &test_class) continue;
            test_objects.push_back(test_class.create());
            num_test_failures += selected
                ? QTest::qExec(test_objects.back().get(), argc - 1, test_args.data())
                : QTest::qExec(test_objects.back().get());
        }

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
