// Copyright (c) 2016-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <quicksilver-build-config.h> // IWYU pragma: keep

#include <vault/vaulttool.h>

#include <common/args.h>
#include <util/fs.h>
#include <util/translation.h>
#include <vault/dump.h>
#include <vault/vault.h>
#include <vault/vaultutil.h>

namespace vault {
namespace VaultTool {

// The standard vault deleter function blocks on the validation interface
// queue, which doesn't exist for the quicksilver-vault. Define our own
// deleter here.
static void VaultToolReleaseVault(CVault* vault)
{
    vault->VaultLogPrintf("down reason=released");
    vault->Close();
    delete vault;
}

static void VaultCreate(CVault* vault_instance, uint64_t vault_creation_flags)
{
    LOCK(vault_instance->cs_vault);

    vault_instance->SetMinVersion();
    if (!vault_instance->IsVaultFlagSet(vault_creation_flags)) {
        vault_instance->SetVaultFlag(vault_creation_flags);
    }

    vault_instance->SetupDescriptorScriptPubKeyMans();

    tfm::format(std::cout, "Topping up keypool...\n");
    vault_instance->TopUpKeyPool();
}

static std::shared_ptr<CVault> MakeVault(const std::string& name, const fs::path& path, DatabaseOptions options)
{
    DatabaseStatus status;
    bilingual_str error;
    std::unique_ptr<VaultDatabase> database = MakeDatabase(path, options, status, error);
    if (!database) {
        tfm::format(std::cerr, "%s\n", error.original);
        return nullptr;
    }

    // dummy chain interface
    std::shared_ptr<CVault> vault_instance{new CVault(/*chain=*/nullptr, name, std::move(database)), VaultToolReleaseVault};
    DBErrors load_vault_ret;
    try {
        load_vault_ret = vault_instance->LoadVault();
    } catch (const std::runtime_error&) {
        tfm::format(std::cerr, "Error loading %s. Is vault being used by another process?\n", name);
        return nullptr;
    }

    if (load_vault_ret != DBErrors::LOAD_OK) {
        if (load_vault_ret == DBErrors::CORRUPT) {
            tfm::format(std::cerr, "Error loading %s: Vault corrupted", name);
            return nullptr;
        } else if (load_vault_ret == DBErrors::NONCRITICAL_ERROR) {
            tfm::format(std::cerr, "Error reading %s! All keys read correctly, but transaction data"
                            " or address book entries might be missing or incorrect.",
                name);
        } else if (load_vault_ret == DBErrors::TOO_NEW) {
            tfm::format(std::cerr, "Error loading %s: Vault requires newer version of %s",
                name, CLIENT_NAME);
            return nullptr;
        } else if (load_vault_ret == DBErrors::NEED_RESCAN) {
            tfm::format(std::cerr, "Error reading %s! Some transaction data might be missing or"
                           " incorrect. Vault requires a rescan.",
                name);
        } else {
            tfm::format(std::cerr, "Error loading %s", name);
            return nullptr;
        }
    }

    if (options.require_create) VaultCreate(vault_instance.get(), options.create_flags);

    return vault_instance;
}

static void VaultShowInfo(CVault* vault_instance)
{
    LOCK(vault_instance->cs_vault);

    tfm::format(std::cout, "Vault info\n==========\n");
    tfm::format(std::cout, "Name: %s\n", vault_instance->GetName());
    tfm::format(std::cout, "Format: %s\n", vault_instance->GetDatabase().Format());
    tfm::format(std::cout, "Descriptors: yes\n");
    tfm::format(std::cout, "Encrypted: %s\n", vault_instance->IsCrypted() ? "yes" : "no");
    tfm::format(std::cout, "HD (hd seed available): %s\n", vault_instance->IsHDEnabled() ? "yes" : "no");
    tfm::format(std::cout, "Keypool Size: %u\n", vault_instance->GetKeyPoolSize());
    tfm::format(std::cout, "Transactions: %zu\n", vault_instance->mapVault.size());
    tfm::format(std::cout, "Address Book: %zu\n", vault_instance->m_address_book.size());
}

bool ExecuteVaultToolFunc(const ArgsManager& args, const std::string& command)
{
    if (args.IsArgSet("-format") && command != "createfromdump") {
        tfm::format(std::cerr, "The -format option can only be used with the \"createfromdump\" command.\n");
        return false;
    }
    if (args.IsArgSet("-dumpfile") && command != "dump" && command != "createfromdump") {
        tfm::format(std::cerr, "The -dumpfile option can only be used with the \"dump\" and \"createfromdump\" commands.\n");
        return false;
    }
    if (command == "create" && !args.IsArgSet("-vault")) {
        tfm::format(std::cerr, "Vault name must be provided when creating a new vault.\n");
        return false;
    }
    const std::string name = args.GetArg("-vault", "");
    const fs::path path = fsbridge::AbsPathJoin(GetVaultDir(), fs::PathFromString(name));

    if (command == "create") {
        DatabaseOptions options;
        ReadDatabaseArgs(args, options);
        options.require_create = true;
        options.create_flags |= VAULT_FLAG_DESCRIPTORS;
        options.require_format = DatabaseFormat::SQLITE;

        const std::shared_ptr<CVault> vault_instance = MakeVault(name, path, options);
        if (vault_instance) {
            VaultShowInfo(vault_instance.get());
            vault_instance->Close();
        }
    } else if (command == "info") {
        DatabaseOptions options;
        ReadDatabaseArgs(args, options);
        options.require_existing = true;
        options.read_only = true;
        const std::shared_ptr<CVault> vault_instance = MakeVault(name, path, options);
        if (!vault_instance) return false;
        VaultShowInfo(vault_instance.get());
        vault_instance->Close();
    } else if (command == "dump") {
        DatabaseOptions options;
        ReadDatabaseArgs(args, options);
        options.require_existing = true;
        DatabaseStatus status;

        bilingual_str error;
        std::unique_ptr<VaultDatabase> database = MakeDatabase(path, options, status, error);
        if (!database) {
            tfm::format(std::cerr, "%s\n", error.original);
            return false;
        }

        bool ret = DumpVault(args, *database, error);
        if (!ret && !error.empty()) {
            tfm::format(std::cerr, "%s\n", error.original);
            return ret;
        }
        tfm::format(std::cout, "The dumpfile may contain private keys. To ensure the safety of your coins, do not share the dumpfile.\n");
        return ret;
    } else if (command == "createfromdump") {
        bilingual_str error;
        std::vector<bilingual_str> warnings;
        bool ret = CreateFromDump(args, name, path, error, warnings);
        for (const auto& warning : warnings) {
            tfm::format(std::cout, "%s\n", warning.original);
        }
        if (!ret && !error.empty()) {
            tfm::format(std::cerr, "%s\n", error.original);
        }
        return ret;
    } else {
        tfm::format(std::cerr, "Invalid command: %s\n", command);
        return false;
    }

    return true;
}
} // namespace VaultTool
} // namespace vault
