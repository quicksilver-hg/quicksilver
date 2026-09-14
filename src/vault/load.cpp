// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <vault/load.h>

#include <common/args.h>
#include <interfaces/chain.h>
#include <scheduler.h>
#include <util/check.h>
#include <util/fs.h>
#include <util/string.h>
#include <util/translation.h>
#include <vault/context.h>
#include <vault/spend.h>
#include <vault/vault.h>
#include <vault/vaultdb.h>

#include <univalue.h>

#include <system_error>

using util::Join;

namespace vault {
bool VerifyVaults(VaultContext& context)
{
    interfaces::Chain& chain = *context.chain;
    ArgsManager& args = *Assert(context.args);

    if (args.IsArgSet("-vaultdir")) {
        const fs::path vault_dir{args.GetPathArg("-vaultdir")};
        std::error_code error;
        // The canonical path lets the fs::exists and fs::is_directory checks below pass on windows, since they return false
        // if a path has trailing slashes, and it strips trailing slashes.
        fs::path canonical_vault_dir = fs::canonical(vault_dir, error);
        if (error || !fs::exists(canonical_vault_dir)) {
            chain.initError(strprintf(_("Specified -vaultdir \"%s\" does not exist"), fs::PathToString(vault_dir)));
            return false;
        } else if (!fs::is_directory(canonical_vault_dir)) {
            chain.initError(strprintf(_("Specified -vaultdir \"%s\" is not a directory"), fs::PathToString(vault_dir)));
            return false;
        // The canonical path transforms relative paths into absolute ones, so we check the non-canonical version
        } else if (!vault_dir.is_absolute()) {
            chain.initError(strprintf(_("Specified -vaultdir \"%s\" is a relative path"), fs::PathToString(vault_dir)));
            return false;
        }
        args.ForceSetArg("-vaultdir", fs::PathToString(canonical_vault_dir));
    }

    LogInfo(HgLog::VAULT, "ready vaultdir=%s\n", fs::PathToString(GetVaultDir()));

    chain.initMessage(_("Verifying vault…"));

    // Keep track of each vault absolute path to detect duplicates.
    std::set<fs::path> vault_paths;

    for (const auto& vault : chain.getSettingsList("vault")) {
        if (!vault.isStr()) {
            chain.initError(_("Invalid value detected for '-vault' or '-novault'. "
                              "'-vault' requires a string value, while '-novault' accepts only '1' to disable all vaults"));
            return false;
        }
        const auto& vault_file = vault.get_str();
        const fs::path path = fsbridge::AbsPathJoin(GetVaultDir(), fs::PathFromString(vault_file));

        if (!vault_paths.insert(path).second) {
            chain.initWarning(strprintf(_("Ignoring duplicate -vault %s."), vault_file));
            continue;
        }

        DatabaseOptions options;
        DatabaseStatus status;
        ReadDatabaseArgs(args, options);
        options.require_existing = true;
        options.verify = true;
        bilingual_str error_string;
        if (!MakeVaultDatabase(vault_file, options, status, error_string)) {
            if (status == DatabaseStatus::FAILED_NOT_FOUND) {
                chain.initWarning(Untranslated(strprintf("Skipping -vault path that doesn't exist. %s", error_string.original)));
            } else {
                chain.initError(error_string);
                return false;
            }
        }
    }

    return true;
}

bool LoadVaults(VaultContext& context)
{
    interfaces::Chain& chain = *context.chain;
    try {
        std::set<fs::path> vault_paths;
        for (const auto& vault : chain.getSettingsList("vault")) {
            if (!vault.isStr()) {
                chain.initError(_("Invalid value detected for '-vault' or '-novault'. "
                                  "'-vault' requires a string value, while '-novault' accepts only '1' to disable all vaults"));
                return false;
            }
            const auto& name = vault.get_str();
            if (!vault_paths.insert(fs::PathFromString(name)).second) {
                continue;
            }
            DatabaseOptions options;
            DatabaseStatus status;
            ReadDatabaseArgs(*context.args, options);
            options.require_existing = true;
            options.verify = false; // No need to verify, assuming verified earlier in VerifyVaults()
            bilingual_str error;
            std::vector<bilingual_str> warnings;
            std::unique_ptr<VaultDatabase> database = MakeVaultDatabase(name, options, status, error);
            if (!database && status == DatabaseStatus::FAILED_NOT_FOUND) {
                continue;
            }
            chain.initMessage(_("Loading vault…"));
            std::shared_ptr<CVault> pvault = database ? CVault::Create(context, name, std::move(database), options.create_flags, error, warnings) : nullptr;
            if (!warnings.empty()) chain.initWarning(Join(warnings, Untranslated("\n")));
            if (!pvault) {
                chain.initError(error);
                return false;
            }

            NotifyVaultLoaded(context, pvault);
            AddVault(context, pvault);
        }
        return true;
    } catch (const std::runtime_error& e) {
        chain.initError(Untranslated(e.what()));
        return false;
    }
}

void StartVaults(VaultContext& context)
{
    for (const std::shared_ptr<CVault>& pvault : GetVaults(context)) {
        pvault->postInitProcess();
    }

    // Schedule periodic vault flushes and tx rebroadcasts
    if (context.args->GetBoolArg("-flushvault", DEFAULT_FLUSHVAULT)) {
        context.scheduler->scheduleEvery([&context] { MaybeCompactVaultDB(context); }, 500ms);
    }
    context.scheduler->scheduleEvery([&context] { MaybeResendVaultTxs(context); }, 1min);
}

void FlushVaults(VaultContext& context)
{
    for (const std::shared_ptr<CVault>& pvault : GetVaults(context)) {
        pvault->Flush();
    }
}

void StopVaults(VaultContext& context)
{
    for (const std::shared_ptr<CVault>& pvault : GetVaults(context)) {
        pvault->Close();
    }
}

void UnloadVaults(VaultContext& context)
{
    auto vaults = GetVaults(context);
    while (!vaults.empty()) {
        auto vault = vaults.back();
        vaults.pop_back();
        std::vector<bilingual_str> warnings;
        RemoveVault(context, vault, /* load_on_start= */ std::nullopt, warnings);
        WaitForDeleteVault(std::move(vault));
    }
}
} // namespace vault
