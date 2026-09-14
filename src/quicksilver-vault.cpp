// Copyright (c) 2016-present The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <quicksilver-build-config.h> // IWYU pragma: keep

#include <chainparams.h>
#include <chainparamsbase.h>
#include <clientversion.h>
#include <common/args.h>
#include <common/system.h>
#include <compat/compat.h>
#include <key.h>
#include <logging.h>
#include <pubkey.h>
#include <random.h>
#include <tinyformat.h>
#include <util/exception.h>
#include <util/translation.h>
#include <vault/vaulttool.h>

#include <exception>
#include <functional>
#include <string>
#include <tuple>

using util::Join;

const TranslateFn G_TRANSLATION_FUN{nullptr};

static void SetupVaultToolArgs(ArgsManager& argsman)
{
    SetupHelpOptions(argsman);
    SetupChainParamsBaseOptions(argsman);

    argsman.AddArg("-version", "Print version and exit", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-datadir=<dir>", "Specify data directory", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::OPTIONS);
    argsman.AddArg("-vault=<vault-name>", "Specify vault name", ArgsManager::ALLOW_ANY | ArgsManager::NETWORK_ONLY, OptionsCategory::OPTIONS);
    argsman.AddArg("-dumpfile=<file name>", "When used with 'dump', writes out the records to this file. When used with 'createfromdump', loads the records into a new vault.", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::OPTIONS);
    argsman.AddArg("-debug=<category>", "Output debugging information (default: 0).", ArgsManager::ALLOW_ANY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-format=<format>", "The format of the vault file to create. Only \"sqlite\" is supported. Only used with 'createfromdump'", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-printtoconsole", "Send trace/debug info to console (default: 1 when no -debug is true, 0 otherwise).", ArgsManager::ALLOW_ANY, OptionsCategory::DEBUG_TEST);

    argsman.AddCommand("info", "Get vault info");
    argsman.AddCommand("create", "Create new vault file");
    argsman.AddCommand("dump", "Print out all of the vault key-value records");
    argsman.AddCommand("createfromdump", "Create new vault file from dumped records");
}

static std::optional<int> VaultAppInit(ArgsManager& args, int argc, char* argv[])
{
    SetupVaultToolArgs(args);
    std::string error_message;
    if (!args.ParseParameters(argc, argv, error_message)) {
        tfm::format(std::cerr, "Error parsing command line arguments: %s\n", error_message);
        return EXIT_FAILURE;
    }
    const bool missing_args{argc < 2};
    if (missing_args || HelpRequested(args) || args.GetBoolArg("-version", false)) {
        std::string strUsage = "quicksilver-vault version " + FormatFullVersion() + "\n";

        if (args.GetBoolArg("-version", false)) {
            strUsage += FormatParagraph(LicenseInfo());
        } else {
            strUsage += "\n"
                "quicksilver-vault is an offline tool for creating and interacting with " CLIENT_NAME " vault files.\n\n"
                "By default quicksilver-vault will act on vaults in the default mainnet vault directory in the datadir.\n\n"
                "To change the target vault, use the -datadir, -vault and (test)chain selection arguments.\n"
                "\n"
                "Usage: quicksilver-vault [options] <command>\n"
                "\n";
            strUsage += "\n" + args.GetHelpMessage();
        }
        tfm::format(std::cout, "%s", strUsage);
        if (missing_args) {
            tfm::format(std::cerr, "Error: too few parameters\n");
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    // check for printtoconsole, allow -debug
    LogInstance().m_print_to_console = args.GetBoolArg("-printtoconsole", args.GetBoolArg("-debug", false));

    if (!CheckDataDirOption(args)) {
        tfm::format(std::cerr, "Error: Specified data directory \"%s\" does not exist.\n", args.GetArg("-datadir", ""));
        return EXIT_FAILURE;
    }
    // Check for chain settings (Params() calls are only valid after this clause)
    SelectParams(args.GetChainType());

    return std::nullopt;
}

MAIN_FUNCTION
{
    ArgsManager& args = gArgs;
#ifdef WIN32
    common::WinCmdLineArgs winArgs;
    std::tie(argc, argv) = winArgs.get();
#endif

    SetupEnvironment();
    RandomInit();
    try {
        if (const auto maybe_exit{VaultAppInit(args, argc, argv)}) return *maybe_exit;
    } catch (const std::exception& e) {
        PrintExceptionContinue(&e, "VaultAppInit()");
        return EXIT_FAILURE;
    } catch (...) {
        PrintExceptionContinue(nullptr, "VaultAppInit()");
        return EXIT_FAILURE;
    }

    const auto command = args.GetCommand();
    if (!command) {
        tfm::format(std::cerr, "No method provided. Run `quicksilver-vault -help` for valid methods.\n");
        return EXIT_FAILURE;
    }
    if (command->args.size() != 0) {
        tfm::format(std::cerr, "Error: Additional arguments provided (%s). Methods do not take arguments. Please refer to `-help`.\n", Join(command->args, ", "));
        return EXIT_FAILURE;
    }

    ECC_Context ecc_context{};
    if (!vault::VaultTool::ExecuteVaultToolFunc(args, command->command)) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
