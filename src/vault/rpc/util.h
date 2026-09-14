// Copyright (c) 2017-present The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_VAULT_RPC_UTIL_H
#define QUICKSILVER_VAULT_RPC_UTIL_H

#include <rpc/util.h>
#include <script/script.h>
#include <vault/vault.h>

#include <any>
#include <memory>
#include <string>
#include <vector>

class JSONRPCRequest;
class UniValue;
struct bilingual_str;

namespace vault {
enum class DatabaseStatus;
struct VaultContext;

extern const std::string HELP_REQUIRING_PASSPHRASE;

static const RPCResult RESULT_LAST_PROCESSED_BLOCK { RPCResult::Type::OBJ, "lastprocessedblock", "hash and height of the block this information was generated on",{
    {RPCResult::Type::STR_HEX, "hash", "hash of the block this information was generated on"},
    {RPCResult::Type::NUM, "height", "height of the block this information was generated on"}}
};

/**
 * Figures out what vault, if any, to use for a JSONRPCRequest.
 *
 * @param[in] request JSONRPCRequest that wishes to access a vault
 * @return nullptr if no vault should be used, or a pointer to the CVault
 */
std::shared_ptr<CVault> GetVaultForJSONRPCRequest(const JSONRPCRequest& request);
bool GetVaultNameFromJSONRPCRequest(const JSONRPCRequest& request, std::string& vault_name);

void EnsureVaultIsUnlocked(const CVault&);
VaultContext& EnsureVaultContext(const std::any& context);

bool GetAvoidReuseFlag(const CVault& vault, const UniValue& param);
std::string LabelFromValue(const UniValue& value);
//! Fetch parent descriptors of this scriptPubKey.
void PushParentDescriptors(const CVault& vault, const CScript& script_pubkey, UniValue& entry);

void HandleVaultError(const std::shared_ptr<CVault> vault, DatabaseStatus& status, bilingual_str& error);
void AppendLastProcessedBlock(UniValue& entry, const CVault& vault) EXCLUSIVE_LOCKS_REQUIRED(vault.cs_vault);
} //  namespace vault

#endif // QUICKSILVER_VAULT_RPC_UTIL_H
