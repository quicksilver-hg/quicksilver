// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <quicksilver-build-config.h> // IWYU pragma: keep

#include <core_io.h>
#include <key_io.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <util/translation.h>
#include <vault/context.h>
#include <vault/receive.h>
#include <vault/rpc/vault.h>
#include <vault/rpc/util.h>
#include <vault/vault.h>
#include <vault/vaultutil.h>

#include <optional>

#include <univalue.h>


namespace vault {

static const std::map<uint64_t, std::string> VAULT_FLAG_CAVEATS{
    {VAULT_FLAG_AVOID_REUSE,
     "You need to rescan the blockchain in order to correctly mark used "
     "destinations in the past. Until this is done, some destinations may "
     "be considered unused, even if the opposite is the case."},
};

/** Checks if a CKey is in the given CVault compressed or otherwise*/
bool HaveKey(const SigningProvider& vault, const CKey& key)
{
    CKey key2;
    key2.Set(key.begin(), key.end(), !key.IsCompressed());
    return vault.HaveKey(key.GetPubKey().GetID()) || vault.HaveKey(key2.GetPubKey().GetID());
}

static RPCHelpMan getvaultinfo()
{
    return RPCHelpMan{"getvaultinfo",
                "Returns an object containing various vault state info.\n",
                {},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {
                        {RPCResult::Type::STR, "vaultname", "the vault name"},
                        {RPCResult::Type::NUM, "vaultversion", "the vault version"},
                        {RPCResult::Type::NUM, "txcount", "the total number of transactions in the vault"},
                        {RPCResult::Type::NUM_TIME, "keypoololdest", /*optional=*/true, "the " + UNIX_EPOCH_TIME + " of the oldest pre-generated key in the key pool."},
                        {RPCResult::Type::NUM, "keypoolsize", "how many new keys are pre-generated (only counts external keys)"},
                        {RPCResult::Type::NUM, "keypoolsize_hd_internal", "how many new keys are pre-generated for internal use (used for change outputs)"},
                        {RPCResult::Type::NUM_TIME, "unlocked_until", /*optional=*/true, "the " + UNIX_EPOCH_TIME + " until which the vault is unlocked for transfers, or 0 if the vault is locked (only present for passphrase-encrypted vaults)"},
                        {RPCResult::Type::BOOL, "private_keys_enabled", "false if private keys are disabled for this vault"},
                        {RPCResult::Type::BOOL, "avoid_reuse", "whether this vault tracks clean/dirty coins in terms of reuse"},
                        {RPCResult::Type::OBJ, "scanning", "current scanning details, or false if no scan is in progress",
                        {
                            {RPCResult::Type::NUM, "duration", "elapsed seconds since scan start"},
                            {RPCResult::Type::NUM, "progress", "scanning progress percentage [0.0, 1.0]"},
                        }, /*skip_type_check=*/true},
                        {RPCResult::Type::BOOL, "external_signer", "whether this vault is configured to use an external signer such as a hardware signer"},
                        {RPCResult::Type::BOOL, "blank", "Whether this vault intentionally does not contain any keys, scripts, or descriptors"},
                        {RPCResult::Type::NUM_TIME, "birthtime", /*optional=*/true, "The start time for blocks scanning. It could be modified by (re)importing any descriptor with an earlier timestamp."},
                        RESULT_LAST_PROCESSED_BLOCK,
                    }},
                },
                RPCExamples{
                    HelpExampleCli("getvaultinfo", "")
            + HelpExampleRpc("getvaultinfo", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const std::shared_ptr<const CVault> pvault = GetVaultForJSONRPCRequest(request);
    if (!pvault) return UniValue::VNULL;

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pvault->BlockUntilSyncedToCurrentChain();

    LOCK(pvault->cs_vault);

    UniValue obj(UniValue::VOBJ);

    size_t kpExternalSize = pvault->KeypoolCountExternalKeys();
    obj.pushKV("vaultname", pvault->GetName());
    obj.pushKV("vaultversion", pvault->GetVersion());
    obj.pushKV("txcount",       (int)pvault->mapVault.size());
    const auto kp_oldest = pvault->GetOldestKeyPoolTime();
    if (kp_oldest.has_value()) {
        obj.pushKV("keypoololdest", kp_oldest.value());
    }
    obj.pushKV("keypoolsize", (int64_t)kpExternalSize);
    obj.pushKV("keypoolsize_hd_internal", (int64_t)(pvault->GetKeyPoolSize() - kpExternalSize));
    if (pvault->IsCrypted()) {
        obj.pushKV("unlocked_until", pvault->nRelockTime);
    }
    obj.pushKV("private_keys_enabled", !pvault->IsVaultFlagSet(VAULT_FLAG_DISABLE_PRIVATE_KEYS));
    obj.pushKV("avoid_reuse", pvault->IsVaultFlagSet(VAULT_FLAG_AVOID_REUSE));
    if (pvault->IsScanning()) {
        UniValue scanning(UniValue::VOBJ);
        scanning.pushKV("duration", Ticks<std::chrono::seconds>(pvault->ScanningDuration()));
        scanning.pushKV("progress", pvault->ScanningProgress());
        obj.pushKV("scanning", std::move(scanning));
    } else {
        obj.pushKV("scanning", false);
    }
    obj.pushKV("external_signer", pvault->IsVaultFlagSet(VAULT_FLAG_EXTERNAL_SIGNER));
    obj.pushKV("blank", pvault->IsVaultFlagSet(VAULT_FLAG_BLANK_VAULT));
    if (int64_t birthtime = pvault->GetBirthTime(); birthtime != UNKNOWN_TIME) {
        obj.pushKV("birthtime", birthtime);
    }

    AppendLastProcessedBlock(obj, *pvault);
    return obj;
},
    };
}

static RPCHelpMan listvaultdir()
{
    return RPCHelpMan{"listvaultdir",
                "Returns a list of vaults in the vault directory.\n",
                {},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::ARR, "vaults", "",
                        {
                            {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR, "name", "The vault name"},
                            }},
                        }},
                    }
                },
                RPCExamples{
                    HelpExampleCli("listvaultdir", "")
            + HelpExampleRpc("listvaultdir", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    UniValue vaults(UniValue::VARR);
    for (const auto& [path, _] : ListDatabases(GetVaultDir())) {
        UniValue vault(UniValue::VOBJ);
        vault.pushKV("name", path.utf8string());
        vaults.push_back(std::move(vault));
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("vaults", std::move(vaults));
    return result;
},
    };
}

static RPCHelpMan listvaults()
{
    return RPCHelpMan{"listvaults",
                "Returns a list of currently loaded vault.\n"
                "For full information on the vault, use \"getvaultinfo\"\n",
                {},
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {
                        {RPCResult::Type::STR, "vaultname", "the vault name"},
                    }
                },
                RPCExamples{
                    HelpExampleCli("listvaults", "")
            + HelpExampleRpc("listvaults", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    UniValue obj(UniValue::VARR);

    VaultContext& context = EnsureVaultContext(request.context);
    for (const std::shared_ptr<CVault>& vault : GetVaults(context)) {
        LOCK(vault->cs_vault);
        obj.push_back(vault->GetName());
    }

    return obj;
},
    };
}

static RPCHelpMan loadvault()
{
    return RPCHelpMan{"loadvault",
                "\nLoads a vault from a vault file or directory."
                "\nNote that all vault command-line options used when starting quicksilverd will be"
                "\napplied to the new vault.\n",
                {
                    {"filename", RPCArg::Type::STR, RPCArg::Optional::NO, "The path to the directory of the vault to be loaded, either absolute or relative to the \"vaults\" directory. The \"vaults\" directory is set by the -vaultdir option and defaults to the \"vaults\" folder within the data directory."},
                    {"load_on_startup", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Save vault name to persistent settings and load on startup. True to add vault to startup list, false to remove, null to leave unchanged."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "name", "The vault name if loaded successfully."},
                        {RPCResult::Type::ARR, "warnings", /*optional=*/true, "Warning messages, if any, related to loading the vault.",
                        {
                            {RPCResult::Type::STR, "", ""},
                        }},
                    }
                },
                RPCExamples{
                    "\nLoad vault from the vault dir:\n"
                    + HelpExampleCli("loadvault", "\"vaultname\"")
                    + HelpExampleRpc("loadvault", "\"vaultname\"")
                    + "\nLoad vault using absolute path (Unix):\n"
                    + HelpExampleCli("loadvault", "\"/path/to/vaultname/\"")
                    + HelpExampleRpc("loadvault", "\"/path/to/vaultname/\"")
                    + "\nLoad vault using absolute path (Windows):\n"
                    + HelpExampleCli("loadvault", "\"DriveLetter:\\path\\to\\vaultname\\\"")
                    + HelpExampleRpc("loadvault", "\"DriveLetter:\\path\\to\\vaultname\\\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    VaultContext& context = EnsureVaultContext(request.context);
    const std::string name(request.params[0].get_str());

    DatabaseOptions options;
    DatabaseStatus status;
    ReadDatabaseArgs(*context.args, options);
    options.require_existing = true;
    bilingual_str error;
    std::vector<bilingual_str> warnings;
    std::optional<bool> load_on_start = request.params[1].isNull() ? std::nullopt : std::optional<bool>(request.params[1].get_bool());

    {
        LOCK(context.vaults_mutex);
        if (std::any_of(context.vaults.begin(), context.vaults.end(), [&name](const auto& vault) { return vault->GetName() == name; })) {
            throw JSONRPCError(RPC_VAULT_ALREADY_LOADED, "Vault \"" + name + "\" is already loaded.");
        }
    }

    std::shared_ptr<CVault> const vault = LoadVault(context, name, load_on_start, options, status, error, warnings);

    HandleVaultError(vault, status, error);

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("name", vault->GetName());
    PushWarnings(warnings, obj);

    return obj;
},
    };
}

static RPCHelpMan setvaultflag()
{
            std::string flags;
            for (auto& it : VAULT_FLAG_MAP)
                if (it.second & MUTABLE_VAULT_FLAGS)
                    flags += (flags == "" ? "" : ", ") + it.first;

    return RPCHelpMan{"setvaultflag",
                "\nChange the state of the given vault flag for a vault.\n",
                {
                    {"flag", RPCArg::Type::STR, RPCArg::Optional::NO, "The name of the flag to change. Current available flags: " + flags},
                    {"value", RPCArg::Type::BOOL, RPCArg::Default{true}, "The new state."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "flag_name", "The name of the flag that was modified"},
                        {RPCResult::Type::BOOL, "flag_state", "The new state of the flag"},
                        {RPCResult::Type::STR, "warnings", /*optional=*/true, "Any warnings associated with the change"},
                    }
                },
                RPCExamples{
                    HelpExampleCli("setvaultflag", "avoid_reuse")
                  + HelpExampleRpc("setvaultflag", "\"avoid_reuse\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CVault> const pvault = GetVaultForJSONRPCRequest(request);
    if (!pvault) return UniValue::VNULL;

    std::string flag_str = request.params[0].get_str();
    bool value = request.params[1].isNull() || request.params[1].get_bool();

    if (!VAULT_FLAG_MAP.count(flag_str)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Unknown vault flag: %s", flag_str));
    }

    auto flag = VAULT_FLAG_MAP.at(flag_str);

    if (!(flag & MUTABLE_VAULT_FLAGS)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Vault flag is immutable: %s", flag_str));
    }

    UniValue res(UniValue::VOBJ);

    if (pvault->IsVaultFlagSet(flag) == value) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Vault flag is already set to %s: %s", value ? "true" : "false", flag_str));
    }

    res.pushKV("flag_name", flag_str);
    res.pushKV("flag_state", value);

    if (value) {
        pvault->SetVaultFlag(flag);
    } else {
        pvault->UnsetVaultFlag(flag);
    }

    if (flag && value && VAULT_FLAG_CAVEATS.count(flag)) {
        res.pushKV("warnings", VAULT_FLAG_CAVEATS.at(flag));
    }

    return res;
},
    };
}

static RPCHelpMan createvault()
{
    return RPCHelpMan{
        "createvault",
        "\nCreates and loads a new vault.\n",
        {
            {"vault_name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name for the new vault. If this is a path, the vault will be created at the path location."},
            {"disable_private_keys", RPCArg::Type::BOOL, RPCArg::Default{false}, "Disable the possibility of private keys (the vault can only track addresses, not spend from them)."},
            {"blank", RPCArg::Type::BOOL, RPCArg::Default{false}, "Create a blank vault. A blank vault has no keys or HD seed until descriptors are imported."},
            {"passphrase", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Encrypt the vault with this passphrase."},
            {"avoid_reuse", RPCArg::Type::BOOL, RPCArg::Default{false}, "Keep track of coin reuse, and treat dirty and clean coins differently with privacy considerations in mind."},
            {"load_on_startup", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Save vault name to persistent settings and load on startup. True to add vault to startup list, false to remove, null to leave unchanged."},
            {"external_signer", RPCArg::Type::BOOL, RPCArg::Default{false}, "Use an external signer such as a hardware signer. Requires -signer to be configured. Vault creation will fail if keys cannot be fetched. Requires disable_private_keys set to true."},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "name", "The vault name if created successfully. If the vault was created using a full path, the vault_name will be the full path."},
                {RPCResult::Type::ARR, "warnings", /*optional=*/true, "Warning messages, if any, related to creating and loading the vault.",
                {
                    {RPCResult::Type::STR, "", ""},
                }},
            }
        },
        RPCExamples{
            HelpExampleCli("createvault", "\"testvault\"")
            + HelpExampleRpc("createvault", "\"testvault\"")
            + HelpExampleCliNamed("createvault", {{"vault_name", "myvault"}, {"avoid_reuse", true}, {"load_on_startup", true}})
            + HelpExampleRpcNamed("createvault", {{"vault_name", "myvault"}, {"avoid_reuse", true}, {"load_on_startup", true}})
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    VaultContext& context = EnsureVaultContext(request.context);
    uint64_t flags = 0;
    if (!request.params[1].isNull() && request.params[1].get_bool()) {
        flags |= VAULT_FLAG_DISABLE_PRIVATE_KEYS;
    }

    if (!request.params[2].isNull() && request.params[2].get_bool()) {
        flags |= VAULT_FLAG_BLANK_VAULT;
    }
    SecureString passphrase;
    passphrase.reserve(100);
    std::vector<bilingual_str> warnings;
    if (!request.params[3].isNull()) {
        passphrase = std::string_view{request.params[3].get_str()};
        if (passphrase.empty()) {
            // Empty string means unencrypted
            warnings.emplace_back(Untranslated("Empty string given as passphrase, vault will not be encrypted."));
        }
    }

    if (!request.params[4].isNull() && request.params[4].get_bool()) {
        flags |= VAULT_FLAG_AVOID_REUSE;
    }
#ifndef USE_SQLITE
    throw JSONRPCError(RPC_VAULT_ERROR, "Compiled without sqlite support (required for vaults)");
#endif
    flags |= VAULT_FLAG_DESCRIPTORS;
    if (!request.params[6].isNull() && request.params[6].get_bool()) {
#ifdef ENABLE_EXTERNAL_SIGNER
        flags |= VAULT_FLAG_EXTERNAL_SIGNER;
#else
        throw JSONRPCError(RPC_VAULT_ERROR, "Compiled without external signing support (required for external signing)");
#endif
    }

    DatabaseOptions options;
    DatabaseStatus status;
    ReadDatabaseArgs(*context.args, options);
    options.require_create = true;
    options.create_flags = flags;
    options.create_passphrase = passphrase;
    bilingual_str error;
    std::optional<bool> load_on_start = request.params[5].isNull() ? std::nullopt : std::optional<bool>(request.params[5].get_bool());
    const std::shared_ptr<CVault> vault = CreateVault(context, request.params[0].get_str(), load_on_start, options, status, error, warnings);
    if (!vault) {
        RPCErrorCode code = status == DatabaseStatus::FAILED_ENCRYPT ? RPC_VAULT_ENCRYPTION_FAILED : RPC_VAULT_ERROR;
        throw JSONRPCError(code, error.original);
    }

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("name", vault->GetName());
    PushWarnings(warnings, obj);

    return obj;
},
    };
}

static RPCHelpMan unloadvault()
{
    return RPCHelpMan{"unloadvault",
                "Unloads the vault referenced by the request endpoint, otherwise unloads the vault specified in the argument.\n"
                "Specifying the vault name on a vault endpoint is invalid.",
                {
                    {"vault_name", RPCArg::Type::STR, RPCArg::DefaultHint{"the vault name from the RPC endpoint"}, "The name of the vault to unload. If provided both here and in the RPC endpoint, the two must be identical."},
                    {"load_on_startup", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Save vault name to persistent settings and load on startup. True to add vault to startup list, false to remove, null to leave unchanged."},
                },
                RPCResult{RPCResult::Type::OBJ, "", "", {
                    {RPCResult::Type::ARR, "warnings", /*optional=*/true, "Warning messages, if any, related to unloading the vault.",
                    {
                        {RPCResult::Type::STR, "", ""},
                    }},
                }},
                RPCExamples{
                    HelpExampleCli("unloadvault", "vault_name")
            + HelpExampleRpc("unloadvault", "vault_name")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::string vault_name;
    if (GetVaultNameFromJSONRPCRequest(request, vault_name)) {
        if (!(request.params[0].isNull() || request.params[0].get_str() == vault_name)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "RPC endpoint vault and vault_name parameter specify different vaults");
        }
    } else {
        vault_name = request.params[0].get_str();
    }

    VaultContext& context = EnsureVaultContext(request.context);
    std::shared_ptr<CVault> vault = GetVault(context, vault_name);
    if (!vault) {
        throw JSONRPCError(RPC_VAULT_NOT_FOUND, "Requested vault does not exist or is not loaded");
    }

    std::vector<bilingual_str> warnings;
    {
        VaultRescanReserver reserver(*vault);
        if (!reserver.reserve()) {
            throw JSONRPCError(RPC_VAULT_ERROR, "Vault is currently rescanning. Abort existing rescan or wait.");
        }

        // Release the "main" shared pointer and prevent further notifications.
        // Note that any attempt to load the same vault would fail until the vault
        // is destroyed (see CheckUniqueFileid).
        std::optional<bool> load_on_start{self.MaybeArg<bool>("load_on_startup")};
        if (!RemoveVault(context, vault, load_on_start, warnings)) {
            throw JSONRPCError(RPC_MISC_ERROR, "Requested vault already unloaded");
        }
    }

    WaitForDeleteVault(std::move(vault));

    UniValue result(UniValue::VOBJ);
    PushWarnings(warnings, result);

    return result;
},
    };
}

RPCHelpMan simulaterawtransaction()
{
    return RPCHelpMan{"simulaterawtransaction",
        "\nCalculate the balance change resulting in the signing and broadcasting of the given transaction(s).\n",
        {
            {"rawtxs", RPCArg::Type::ARR, RPCArg::Optional::OMITTED, "An array of hex strings of raw transactions.\n",
                {
                    {"rawtx", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, ""},
                },
            },
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR_AMOUNT, "balance_change", "The vault balance change (negative means decrease)."},
            }
        },
        RPCExamples{
            HelpExampleCli("simulaterawtransaction", "[\"myhex\"]")
            + HelpExampleRpc("simulaterawtransaction", "[\"myhex\"]")
        },
    [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const std::shared_ptr<const CVault> rpc_vault = GetVaultForJSONRPCRequest(request);
    if (!rpc_vault) return UniValue::VNULL;
    const CVault& vault = *rpc_vault;

    LOCK(vault.cs_vault);

    const isminefilter filter = ISMINE_SPENDABLE;

    const auto& txs = request.params[0].get_array();
    CAmount changes{0};
    std::map<COutPoint, CAmount> new_utxos; // UTXO:s that were made available in transaction array
    std::set<COutPoint> spent;

    for (size_t i = 0; i < txs.size(); ++i) {
        CMutableTransaction mtx;
        if (!DecodeHexTx(mtx, txs[i].get_str(), /* try_no_witness */ true, /* try_witness */ true)) {
            throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Transaction hex string decoding failure.");
        }

        // Fetch previous transactions (inputs)
        std::map<COutPoint, Coin> coins;
        for (const CTxIn& txin : mtx.vin) {
            coins[txin.prevout]; // Create empty map entry keyed by prevout.
        }
        vault.chain().findCoins(coins);

        // Fetch debit; we are *spending* these; if the transaction is signed and
        // broadcast, we will lose everything in these
        for (const auto& txin : mtx.vin) {
            const auto& outpoint = txin.prevout;
            if (spent.count(outpoint)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Transaction(s) are spending the same output more than once");
            }
            if (new_utxos.count(outpoint)) {
                changes -= new_utxos.at(outpoint);
                new_utxos.erase(outpoint);
            } else {
                if (coins.at(outpoint).IsSpent()) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "One or more transaction inputs are missing or have been spent already");
                }
                changes -= vault.GetDebit(txin, filter);
            }
            spent.insert(outpoint);
        }

        // Iterate over outputs; we are *receiving* these, if the vault considers
        // them "mine"; if the transaction is signed and broadcast, we will receive
        // everything in these
        // Also populate new_utxos in case these are spent in later transactions

        const auto& hash = mtx.GetHash();
        for (size_t i = 0; i < mtx.vout.size(); ++i) {
            const auto& txout = mtx.vout[i];
            bool is_mine = 0 < (vault.IsMine(txout) & filter);
            changes += new_utxos[COutPoint(hash, i)] = is_mine ? txout.nValue : 0;
        }
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("balance_change", ValueFromAmount(changes));

    return result;
}
    };
}

RPCHelpMan gethdkeys()
{
    return RPCHelpMan{
        "gethdkeys",
        "\nList all BIP 32 HD keys in the vault and which descriptors use them.\n",
        {
            {"options", RPCArg::Type::OBJ_NAMED_PARAMS, RPCArg::Optional::OMITTED, "", {
                {"active_only", RPCArg::Type::BOOL, RPCArg::Default{false}, "Show the keys for only active descriptors"},
                {"private", RPCArg::Type::BOOL, RPCArg::Default{false}, "Show private keys"}
            }},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {
            {
                {RPCResult::Type::OBJ, "", "", {
                    {RPCResult::Type::STR, "qpub", "The extended public key"},
                    {RPCResult::Type::BOOL, "has_private", "Whether the vault has the private key for this key"},
                    {RPCResult::Type::STR, "qprv", /*optional=*/true, "The extended private key if \"private\" is true"},
                    {RPCResult::Type::ARR, "descriptors", "Array of descriptor objects that use this HD key",
                    {
                        {RPCResult::Type::OBJ, "", "", {
                            {RPCResult::Type::STR, "desc", "Descriptor string representation"},
                            {RPCResult::Type::BOOL, "active", "Whether this descriptor is currently used to generate new addresses"},
                        }},
                    }},
                }},
            }
        }},
        RPCExamples{
            HelpExampleCli("gethdkeys", "") + HelpExampleRpc("gethdkeys", "")
            + HelpExampleCliNamed("gethdkeys", {{"active_only", "true"}, {"private", "true"}}) + HelpExampleRpcNamed("gethdkeys", {{"active_only", "true"}, {"private", "true"}})
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            const std::shared_ptr<const CVault> vault = GetVaultForJSONRPCRequest(request);
            if (!vault) return UniValue::VNULL;

            CHECK_NONFATAL(vault->IsVaultFlagSet(VAULT_FLAG_DESCRIPTORS));

            LOCK(vault->cs_vault);

            UniValue options{request.params[0].isNull() ? UniValue::VOBJ : request.params[0]};
            const bool active_only{options.exists("active_only") ? options["active_only"].get_bool() : false};
            const bool priv{options.exists("private") ? options["private"].get_bool() : false};
            if (priv) {
                EnsureVaultIsUnlocked(*vault);
            }


            std::set<ScriptPubKeyMan*> spkms;
            if (active_only) {
                spkms = vault->GetActiveScriptPubKeyMans();
            } else {
                spkms = vault->GetAllScriptPubKeyMans();
            }

            std::map<CExtPubKey, std::set<std::tuple<std::string, bool, bool>>> vault_xpubs;
            std::map<CExtPubKey, CExtKey> vault_xprvs;
            for (auto* spkm : spkms) {
                auto* desc_spkm{dynamic_cast<DescriptorScriptPubKeyMan*>(spkm)};
                CHECK_NONFATAL(desc_spkm);
                LOCK(desc_spkm->cs_desc_man);
                VaultDescriptor w_desc = desc_spkm->GetVaultDescriptor();

                // Retrieve the pubkeys from the descriptor
                std::set<CPubKey> desc_pubkeys;
                std::set<CExtPubKey> desc_xpubs;
                w_desc.descriptor->GetPubKeys(desc_pubkeys, desc_xpubs);
                for (const CExtPubKey& xpub : desc_xpubs) {
                    std::string desc_str;
                    bool ok = desc_spkm->GetDescriptorString(desc_str, false);
                    CHECK_NONFATAL(ok);
                    vault_xpubs[xpub].emplace(desc_str, vault->IsActiveScriptPubKeyMan(*spkm), desc_spkm->HasPrivKey(xpub.pubkey.GetID()));
                    if (std::optional<CKey> key = priv ? desc_spkm->GetKey(xpub.pubkey.GetID()) : std::nullopt) {
                        vault_xprvs[xpub] = CExtKey(xpub, *key);
                    }
                }
            }

            UniValue response(UniValue::VARR);
            for (const auto& [xpub, descs] : vault_xpubs) {
                bool has_xprv = false;
                UniValue descriptors(UniValue::VARR);
                for (const auto& [desc, active, has_priv] : descs) {
                    UniValue d(UniValue::VOBJ);
                    d.pushKV("desc", desc);
                    d.pushKV("active", active);
                    has_xprv |= has_priv;

                    descriptors.push_back(std::move(d));
                }
                UniValue xpub_info(UniValue::VOBJ);
                xpub_info.pushKV("qpub", EncodeExtPubKey(xpub));
                xpub_info.pushKV("has_private", has_xprv);
                if (priv) {
                    xpub_info.pushKV("qprv", EncodeExtKey(vault_xprvs.at(xpub)));
                }
                xpub_info.pushKV("descriptors", std::move(descriptors));

                response.push_back(std::move(xpub_info));
            }

            return response;
        },
    };
}

static RPCHelpMan createvaultdescriptor()
{
    return RPCHelpMan{"createvaultdescriptor",
        "Creates the vault's descriptor for the given address type. "
        "The address type must be one that the vault does not already have a descriptor for."
        + HELP_REQUIRING_PASSPHRASE,
        {
            {"type", RPCArg::Type::STR, RPCArg::Optional::NO, "The address type the descriptor will produce. Options are \"base58\", \"bech32\", and \"bech32m\"."},
            {"options", RPCArg::Type::OBJ_NAMED_PARAMS, RPCArg::Optional::OMITTED, "", {
                {"internal", RPCArg::Type::BOOL, RPCArg::DefaultHint{"Both external and internal will be generated unless this parameter is specified"}, "Whether to only make one descriptor that is internal (if parameter is true) or external (if parameter is false)"},
                {"hdkey", RPCArg::Type::STR, RPCArg::DefaultHint{"The HD key used by all other active descriptors"}, "The HD key that the vault knows the private key of, listed using 'gethdkeys', to use for this descriptor's key"},
            }},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::ARR, "descs", "The public descriptors that were added to the vault",
                    {{RPCResult::Type::STR, "", ""}}
                }
            },
        },
        RPCExamples{
            HelpExampleCli("createvaultdescriptor", "bech32m")
            + HelpExampleRpc("createvaultdescriptor", "bech32m")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            std::shared_ptr<CVault> const pvault = GetVaultForJSONRPCRequest(request);
            if (!pvault) return UniValue::VNULL;

            CHECK_NONFATAL(pvault->IsVaultFlagSet(VAULT_FLAG_DESCRIPTORS));

            std::optional<OutputType> output_type = ParseOutputType(request.params[0].get_str());
            if (!output_type) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Unknown address type '%s'", request.params[0].get_str()));
            }

            UniValue options{request.params[1].isNull() ? UniValue::VOBJ : request.params[1]};
            UniValue internal_only{options["internal"]};
            UniValue hdkey{options["hdkey"]};

            std::vector<bool> internals;
            if (internal_only.isNull()) {
                internals.push_back(false);
                internals.push_back(true);
            } else {
                internals.push_back(internal_only.get_bool());
            }

            LOCK(pvault->cs_vault);
            EnsureVaultIsUnlocked(*pvault);

            CExtPubKey xpub;
            if (hdkey.isNull()) {
                std::set<CExtPubKey> active_xpubs = pvault->GetActiveHDPubKeys();
                if (active_xpubs.size() != 1) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Unable to determine which HD key to use from active descriptors. Please specify with 'hdkey'");
                }
                xpub = *active_xpubs.begin();
            } else {
                xpub = DecodeExtPubKey(hdkey.get_str());
                if (!xpub.pubkey.IsValid()) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Unable to parse HD key. Please provide a valid qpub");
                }
            }

            std::optional<CKey> key = pvault->GetKey(xpub.pubkey.GetID());
            if (!key) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Private key for %s is not known", EncodeExtPubKey(xpub)));
            }
            CExtKey active_hdkey(xpub, *key);

            std::vector<std::reference_wrapper<DescriptorScriptPubKeyMan>> spkms;
            VaultBatch batch{pvault->GetDatabase()};
            for (bool internal : internals) {
                VaultDescriptor w_desc = GenerateVaultDescriptor(xpub, *output_type, internal);
                uint256 w_id = DescriptorID(*w_desc.descriptor);
                if (!pvault->GetScriptPubKeyMan(w_id)) {
                    spkms.emplace_back(pvault->SetupDescriptorScriptPubKeyMan(batch, active_hdkey, *output_type, internal));
                }
            }
            if (spkms.empty()) {
                throw JSONRPCError(RPC_VAULT_ERROR, "Descriptor already exists");
            }

            // Fetch each descspkm from the vault in order to get the descriptor strings
            UniValue descs{UniValue::VARR};
            for (const auto& spkm : spkms) {
                std::string desc_str;
                bool ok = spkm.get().GetDescriptorString(desc_str, false);
                CHECK_NONFATAL(ok);
                descs.push_back(desc_str);
            }
            UniValue out{UniValue::VOBJ};
            out.pushKV("descs", std::move(descs));
            return out;
        }
    };
}

// addresses
RPCHelpMan getaddressinfo();
RPCHelpMan getnewaddress();
RPCHelpMan getrawchangeaddress();
RPCHelpMan setlabel();
RPCHelpMan listaddressgroupings();
RPCHelpMan keypoolrefill();
RPCHelpMan getaddressesbylabel();
RPCHelpMan listlabels();
#ifdef ENABLE_EXTERNAL_SIGNER
RPCHelpMan vaultdisplayaddress();
#endif // ENABLE_EXTERNAL_SIGNER

// backup and descriptor imports
RPCHelpMan importprunedfunds();
RPCHelpMan removeprunedfunds();
RPCHelpMan importdescriptors();
RPCHelpMan listdescriptors();
RPCHelpMan exportvaultarchive();
RPCHelpMan importvaultarchive();
RPCHelpMan backupvault();
RPCHelpMan restorevault();

// coins
RPCHelpMan getreceivedbyaddress();
RPCHelpMan getreceivedbylabel();
RPCHelpMan getbalance();
RPCHelpMan lockunspent();
RPCHelpMan listlockunspent();
RPCHelpMan getbalances();
RPCHelpMan listunspent();

// encryption
RPCHelpMan vaultpassphrase();
RPCHelpMan vaultpassphrasechange();
RPCHelpMan vaultlock();
RPCHelpMan encryptvault();

// spend
RPCHelpMan sendtoaddress();
RPCHelpMan sendmany();
RPCHelpMan fundrawtransaction();
RPCHelpMan send();
RPCHelpMan sendall();
RPCHelpMan vaultprocesspsqt();
RPCHelpMan vaultcreatefundedpsqt();
RPCHelpMan signrawtransactionwithvault();

// signmessage
RPCHelpMan signmessage();

// transactions
RPCHelpMan listreceivedbyaddress();
RPCHelpMan listreceivedbylabel();
RPCHelpMan listtransactions();
RPCHelpMan listsinceblock();
RPCHelpMan gettransaction();
RPCHelpMan abandontransaction();
RPCHelpMan rescanblockchain();
RPCHelpMan abortrescan();

Span<const CRPCCommand> GetVaultRPCCommands()
{
    static const CRPCCommand commands[]{
        {"rawtransactions", &fundrawtransaction},
        {"vault", &abandontransaction},
        {"vault", &abortrescan},
        {"vault", &backupvault},
        {"vault", &createvault},
        {"vault", &createvaultdescriptor},
        {"vault", &restorevault},
        {"vault", &encryptvault},
        {"vault", &getaddressesbylabel},
        {"vault", &getaddressinfo},
        {"vault", &getbalance},
        {"vault", &gethdkeys},
        {"vault", &getnewaddress},
        {"vault", &getrawchangeaddress},
        {"vault", &getreceivedbyaddress},
        {"vault", &getreceivedbylabel},
        {"vault", &gettransaction},
        {"vault", &getbalances},
        {"vault", &getvaultinfo},
        {"vault", &exportvaultarchive},
        {"vault", &importvaultarchive},
        {"vault", &importdescriptors},
        {"vault", &importprunedfunds},
        {"vault", &keypoolrefill},
        {"vault", &listaddressgroupings},
        {"vault", &listdescriptors},
        {"vault", &listlabels},
        {"vault", &listlockunspent},
        {"vault", &listreceivedbyaddress},
        {"vault", &listreceivedbylabel},
        {"vault", &listsinceblock},
        {"vault", &listtransactions},
        {"vault", &listunspent},
        {"vault", &listvaultdir},
        {"vault", &listvaults},
        {"vault", &loadvault},
        {"vault", &lockunspent},
        {"vault", &removeprunedfunds},
        {"vault", &rescanblockchain},
        {"vault", &send},
        {"vault", &sendmany},
        {"vault", &sendtoaddress},
        {"vault", &setlabel},
        {"vault", &setvaultflag},
        {"vault", &signmessage},
        {"vault", &signrawtransactionwithvault},
        {"vault", &simulaterawtransaction},
        {"vault", &sendall},
        {"vault", &unloadvault},
        {"vault", &vaultcreatefundedpsqt},
#ifdef ENABLE_EXTERNAL_SIGNER
        {"vault", &vaultdisplayaddress},
#endif // ENABLE_EXTERNAL_SIGNER
        {"vault", &vaultlock},
        {"vault", &vaultpassphrase},
        {"vault", &vaultpassphrasechange},
        {"vault", &vaultprocesspsqt},
    };
    return commands;
}
} // namespace vault
