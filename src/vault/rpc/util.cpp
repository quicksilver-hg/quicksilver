// Copyright (c) 2011-present The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <vault/rpc/util.h>

#include <common/url.h>
#include <rpc/util.h>
#include <util/any.h>
#include <util/translation.h>
#include <vault/context.h>
#include <vault/vault.h>

#include <string_view>
#include <univalue.h>

namespace vault {
static const std::string VAULT_ENDPOINT_BASE = "/vault/";
const std::string HELP_REQUIRING_PASSPHRASE{"\nRequires vault passphrase to be set with vaultpassphrase call if vault is encrypted.\n"};

bool GetAvoidReuseFlag(const CVault& vault, const UniValue& param) {
    bool can_avoid_reuse = vault.IsVaultFlagSet(VAULT_FLAG_AVOID_REUSE);
    bool avoid_reuse = param.isNull() ? can_avoid_reuse : param.get_bool();

    if (avoid_reuse && !can_avoid_reuse) {
        throw JSONRPCError(RPC_VAULT_ERROR, "vault does not have the \"avoid reuse\" feature enabled");
    }

    return avoid_reuse;
}

bool GetVaultNameFromJSONRPCRequest(const JSONRPCRequest& request, std::string& vault_name)
{
    if (request.URI.starts_with(VAULT_ENDPOINT_BASE)) {
        // vault endpoint was used
        vault_name = UrlDecode(std::string_view{request.URI}.substr(VAULT_ENDPOINT_BASE.size()));
        return true;
    }
    return false;
}

std::shared_ptr<CVault> GetVaultForJSONRPCRequest(const JSONRPCRequest& request)
{
    CHECK_NONFATAL(request.mode == JSONRPCRequest::EXECUTE);
    VaultContext& context = EnsureVaultContext(request.context);

    std::string vault_name;
    if (GetVaultNameFromJSONRPCRequest(request, vault_name)) {
        std::shared_ptr<CVault> pvault = GetVault(context, vault_name);
        if (!pvault) throw JSONRPCError(RPC_VAULT_NOT_FOUND, "Requested vault does not exist or is not loaded");
        return pvault;
    }

    size_t count{0};
    auto vault = GetDefaultVault(context, count);
    if (vault) return vault;

    if (count == 0) {
        throw JSONRPCError(
            RPC_VAULT_NOT_FOUND, "No vault is loaded. Load a vault using loadvault or create a new one with createvault. (Note: A default vault is no longer automatically created)");
    }
    throw JSONRPCError(RPC_VAULT_NOT_SPECIFIED,
        "Multiple vaults are loaded. Please select which vault to use by requesting the RPC through the /vault/<vaultname> URI path.");
}

void EnsureVaultIsUnlocked(const CVault& vault)
{
    if (vault.IsLocked()) {
        throw JSONRPCError(RPC_VAULT_UNLOCK_NEEDED, "Error: Please enter the vault passphrase with vaultpassphrase first.");
    }
}

VaultContext& EnsureVaultContext(const std::any& context)
{
    auto vault_context = util::AnyPtr<VaultContext>(context);
    if (!vault_context) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Vault context not found");
    }
    return *vault_context;
}

std::string LabelFromValue(const UniValue& value)
{
    static const std::string empty_string;
    if (value.isNull()) return empty_string;

    const std::string& label{value.get_str()};
    if (label == "*")
        throw JSONRPCError(RPC_VAULT_INVALID_LABEL_NAME, "Invalid label name");
    return label;
}

void PushParentDescriptors(const CVault& vault, const CScript& script_pubkey, UniValue& entry)
{
    UniValue parent_descs(UniValue::VARR);
    for (const auto& desc: vault.GetVaultDescriptors(script_pubkey)) {
        parent_descs.push_back(desc.descriptor->ToString());
    }
    entry.pushKV("parent_descs", std::move(parent_descs));
}

void HandleVaultError(const std::shared_ptr<CVault> vault, DatabaseStatus& status, bilingual_str& error)
{
    if (!vault) {
        // Map bad format to not found, since bad format is returned when the
        // vault directory exists, but doesn't contain a data file.
        RPCErrorCode code = RPC_VAULT_ERROR;
        switch (status) {
            case DatabaseStatus::FAILED_NOT_FOUND:
            case DatabaseStatus::FAILED_BAD_FORMAT:
                code = RPC_VAULT_NOT_FOUND;
                break;
            case DatabaseStatus::FAILED_ALREADY_LOADED:
                code = RPC_VAULT_ALREADY_LOADED;
                break;
            case DatabaseStatus::FAILED_ALREADY_EXISTS:
                code = RPC_VAULT_ALREADY_EXISTS;
                break;
            case DatabaseStatus::FAILED_INVALID_BACKUP_FILE:
                code = RPC_INVALID_PARAMETER;
                break;
            default: // RPC_VAULT_ERROR is returned for all other cases.
                break;
        }
        throw JSONRPCError(code, error.original);
    }
}

void AppendLastProcessedBlock(UniValue& entry, const CVault& vault)
{
    AssertLockHeld(vault.cs_vault);
    UniValue lastprocessedblock{UniValue::VOBJ};
    lastprocessedblock.pushKV("hash", vault.GetLastBlockHash().GetHex());
    lastprocessedblock.pushKV("height", vault.GetLastBlockHeight());
    entry.pushKV("lastprocessedblock", std::move(lastprocessedblock));
}

} // namespace vault
