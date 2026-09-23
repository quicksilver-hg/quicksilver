// Copyright (c) 2009-present The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <clientversion.h>
#include <core_io.h>
#include <hash.h>
#include <interfaces/chain.h>
#include <key_io.h>
#include <merkleblock.h>
#include <rpc/util.h>
#include <script/descriptor.h>
#include <script/script.h>
#include <script/solver.h>
#include <sync.h>
#include <uint256.h>
#include <util/bip32.h>
#include <util/fs.h>
#include <util/time.h>
#include <util/translation.h>
#include <vault/archive.h>
#include <vault/rpc/util.h>
#include <vault/vault.h>

#include <cstdint>
#include <fstream>
#include <optional>
#include <tuple>
#include <string>

#include <univalue.h>



using interfaces::FoundBlock;
using util::SplitString;

namespace vault {
RPCHelpMan importprunedfunds()
{
    return RPCHelpMan{"importprunedfunds",
                "\nImports funds without rescan. Corresponding address or script must previously be included in vault. Aimed towards pruned vault. The end-user is responsible to import additional transactions that subsequently spend the imported outputs or rescan after the point in the blockchain the transaction is included.\n",
                {
                    {"rawtransaction", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "A raw transaction in hex funding an already-existing address in vault"},
                    {"txoutproof", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The hex output from gettxoutproof that contains the transaction"},
                },
                RPCResult{RPCResult::Type::NONE, "", ""},
                RPCExamples{""},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CVault> const pvault = GetVaultForJSONRPCRequest(request);
    if (!pvault) return UniValue::VNULL;

    CMutableTransaction tx;
    if (!DecodeHexTx(tx, request.params[0].get_str())) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed. Make sure the tx has at least one input.");
    }
    uint256 hashTx = tx.GetHash();

    DataStream ssMB{ParseHexV(request.params[1], "proof")};
    CMerkleBlock merkleBlock;
    ssMB >> merkleBlock;

    //Search partial merkle tree in proof for our transaction and index in valid block
    std::vector<uint256> vMatch;
    std::vector<unsigned int> vIndex;
    if (merkleBlock.txn.ExtractMatches(vMatch, vIndex) != merkleBlock.header.hashMerkleRoot) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Something wrong with merkleblock");
    }

    LOCK(pvault->cs_vault);
    int height;
    if (!pvault->chain().findAncestorByHash(pvault->GetLastBlockHash(), merkleBlock.header.GetHash(), FoundBlock().height(height))) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found in chain");
    }

    std::vector<uint256>::const_iterator it;
    if ((it = std::find(vMatch.begin(), vMatch.end(), hashTx)) == vMatch.end()) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction given doesn't exist in proof");
    }

    unsigned int txnIndex = vIndex[it - vMatch.begin()];

    CTransactionRef tx_ref = MakeTransactionRef(tx);
    if (pvault->IsMine(*tx_ref)) {
        pvault->AddToVault(std::move(tx_ref), TxStateConfirmed{merkleBlock.header.GetHash(), height, static_cast<int>(txnIndex)});
        return UniValue::VNULL;
    }

    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No addresses in vault correspond to included transaction");
},
    };
}

RPCHelpMan removeprunedfunds()
{
    return RPCHelpMan{"removeprunedfunds",
                "\nDeletes the specified transaction from the vault. Meant for use with pruned vault and as a companion to importprunedfunds. This will affect vault balances.\n",
                {
                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The hex-encoded id of the transaction you are deleting"},
                },
                RPCResult{RPCResult::Type::NONE, "", ""},
                RPCExamples{
                    HelpExampleCli("removeprunedfunds", "\"a8d0c0184dde994a09ec054286f1ce581bebf46446a512166eae7628734ea0a5\"") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("removeprunedfunds", "\"a8d0c0184dde994a09ec054286f1ce581bebf46446a512166eae7628734ea0a5\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CVault> const pvault = GetVaultForJSONRPCRequest(request);
    if (!pvault) return UniValue::VNULL;

    LOCK(pvault->cs_vault);

    uint256 hash(ParseHashV(request.params[0], "txid"));
    std::vector<uint256> vHash;
    vHash.push_back(hash);
    if (auto res = pvault->RemoveTxs(vHash); !res) {
        throw JSONRPCError(RPC_VAULT_ERROR, util::ErrorString(res).original);
    }

    return UniValue::VNULL;
},
    };
}

static int64_t GetImportTimestamp(const UniValue& data, int64_t now)
{
    if (data.exists("timestamp")) {
        const UniValue& timestamp = data["timestamp"];
        if (timestamp.isNum()) {
            return timestamp.getInt<int64_t>();
        } else if (timestamp.isStr() && timestamp.get_str() == "now") {
            return now;
        }
        throw JSONRPCError(RPC_TYPE_ERROR, strprintf("Expected number or \"now\" timestamp value for key. got type %s", uvTypeName(timestamp.type())));
    }
    throw JSONRPCError(RPC_TYPE_ERROR, "Missing required timestamp field for key");
}

static UniValue ProcessDescriptorImport(CVault& vault, const UniValue& data, const int64_t timestamp) EXCLUSIVE_LOCKS_REQUIRED(vault.cs_vault)
{
    UniValue warnings(UniValue::VARR);
    UniValue result(UniValue::VOBJ);

    try {
        if (!data.exists("desc")) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Descriptor not found.");
        }

        const std::string& descriptor = data["desc"].get_str();
        const bool active = data.exists("active") ? data["active"].get_bool() : false;
        const std::string label{LabelFromValue(data["label"])};

        FlatSigningProvider keys;
        std::string error;
        auto parsed_descs = Parse(descriptor, keys, error, /*require_checksum=*/true);
        if (parsed_descs.empty()) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, error);
        }
        std::optional<bool> internal;
        if (data.exists("internal")) {
            if (parsed_descs.size() > 1) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Cannot have multipath descriptor while also specifying 'internal'");
            }
            internal = data["internal"].get_bool();
        }

        int64_t range_start = 0, range_end = 1, next_index = 0;
        if (!parsed_descs.at(0)->IsRange() && data.exists("range")) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Range should not be specified for an un-ranged descriptor");
        } else if (parsed_descs.at(0)->IsRange()) {
            if (data.exists("range")) {
                auto range = ParseDescriptorRange(data["range"]);
                range_start = range.first;
                range_end = range.second + 1;
            } else {
                warnings.push_back("Range not given, using default keypool range");
                range_start = 0;
                range_end = vault.m_keypool_size;
            }
            next_index = range_start;

            if (data.exists("next_index")) {
                next_index = data["next_index"].getInt<int64_t>();
                if (next_index < range_start || next_index >= range_end) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "next_index is out of range");
                }
            }
        }

        if (active && !parsed_descs.at(0)->IsRange()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Active descriptors must be ranged");
        }
        if (parsed_descs.size() > 1 && data.exists("label")) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Multipath descriptors should not have a label");
        }
        if (data.exists("range") && data.exists("label")) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Ranged descriptors should not have a label");
        }
        if (internal && data.exists("label")) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Internal addresses should not have a label");
        }
        if (active && !parsed_descs.at(0)->IsSingleType()) {
            throw JSONRPCError(RPC_VAULT_ERROR, "Combo descriptors cannot be set to active");
        }
        if (vault.IsVaultFlagSet(VAULT_FLAG_DISABLE_PRIVATE_KEYS) && !keys.keys.empty()) {
            throw JSONRPCError(RPC_VAULT_ERROR, "Cannot import private keys to a vault with private keys disabled");
        }

        for (size_t j = 0; j < parsed_descs.size(); ++j) {
            auto parsed_desc = std::move(parsed_descs[j]);
            bool desc_internal = internal.has_value() && internal.value();
            if (parsed_descs.size() == 2) {
                desc_internal = j == 1;
            } else if (parsed_descs.size() > 2) {
                CHECK_NONFATAL(!desc_internal);
            }

            FlatSigningProvider expand_keys;
            std::vector<CScript> scripts;
            if (!parsed_desc->Expand(0, keys, scripts, expand_keys)) {
                throw JSONRPCError(RPC_VAULT_ERROR, "Cannot expand descriptor. Probably because of hardened derivations without private keys provided");
            }
            parsed_desc->ExpandPrivate(0, keys, expand_keys);

            bool have_all_privkeys = !expand_keys.keys.empty();
            for (const auto& entry : expand_keys.origins) {
                const CKeyID& key_id = entry.first;
                CKey key;
                if (!expand_keys.GetKey(key_id, key)) {
                    have_all_privkeys = false;
                    break;
                }
            }

            if (!vault.IsVaultFlagSet(VAULT_FLAG_DISABLE_PRIVATE_KEYS)) {
               if (keys.keys.empty()) {
                    throw JSONRPCError(RPC_VAULT_ERROR, "Cannot import descriptor without private keys to a vault with private keys enabled");
               }
               if (!have_all_privkeys) {
                   warnings.push_back("Not all private keys provided. Some vault functionality may return unexpected errors");
               }
            }

            VaultDescriptor w_desc(std::move(parsed_desc), timestamp, range_start, range_end, next_index);
            auto existing_spk_manager = vault.GetDescriptorScriptPubKeyMan(w_desc);
            if (existing_spk_manager && !existing_spk_manager->CanUpdateToVaultDescriptor(w_desc, error)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, error);
            }

            auto spk_manager = vault.AddVaultDescriptor(w_desc, keys, label, desc_internal);
            if (spk_manager == nullptr) {
                throw JSONRPCError(RPC_VAULT_ERROR, strprintf("Could not add descriptor '%s'", descriptor));
            }

            if (active) {
                if (!w_desc.descriptor->GetOutputType()) {
                    warnings.push_back("Unknown output type, cannot set descriptor to active.");
                } else {
                    vault.AddActiveScriptPubKeyMan(spk_manager->GetID(), *w_desc.descriptor->GetOutputType(), desc_internal);
                }
            } else if (w_desc.descriptor->GetOutputType()) {
                vault.DeactivateScriptPubKeyMan(spk_manager->GetID(), *w_desc.descriptor->GetOutputType(), desc_internal);
            }
        }

        result.pushKV("success", UniValue(true));
    } catch (const UniValue& e) {
        result.pushKV("success", UniValue(false));
        result.pushKV("error", e);
    }
    PushWarnings(warnings, result);
    return result;
}

RPCHelpMan importdescriptors()
{
    return RPCHelpMan{"importdescriptors",
                "\nImport descriptors. This will trigger a rescan of the blockchain based on the earliest timestamp of all descriptors being imported. Requires a new vault backup.\n"
            "When importing descriptors with multipath key expressions, if the multipath specifier contains exactly two elements, the descriptor produced from the second elements will be imported as an internal descriptor.\n"
            "\nNote: This call can take over an hour to complete if using an early timestamp; during that time, other rpc calls\n"
            "may report that the imported keys, addresses or scripts exist but related transactions are still missing.\n"
            "The rescan is significantly faster if block filters are available (using startup option \"-blockfilterindex=1\").\n",
                {
                    {"requests", RPCArg::Type::ARR, RPCArg::Optional::NO, "Data to be imported",
                        {
                            {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                {
                                    {"desc", RPCArg::Type::STR, RPCArg::Optional::NO, "Descriptor to import."},
                                    {"active", RPCArg::Type::BOOL, RPCArg::Default{false}, "Set this descriptor to be the active descriptor for the corresponding output type/externality"},
                                    {"range", RPCArg::Type::RANGE, RPCArg::Optional::OMITTED, "If a ranged descriptor is used, this specifies the end or the range (in the form [begin,end]) to import"},
                                    {"next_index", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "If a ranged descriptor is set to active, this specifies the next index to generate addresses from"},
                                    {"timestamp", RPCArg::Type::NUM, RPCArg::Optional::NO, "Time from which to start rescanning the blockchain for this descriptor, in " + UNIX_EPOCH_TIME + "\n"
                                        "Use the string \"now\" to substitute the current synced blockchain time.\n"
                                        "\"now\" can be specified to bypass scanning, for outputs which are known to never have been used, and\n"
                                        "0 can be specified to scan the entire blockchain. Blocks up to 2 hours before the earliest timestamp\n"
                                        "of all descriptors being imported will be scanned as well as the relay pool.",
                                        RPCArgOptions{.type_str={"timestamp | \"now\"", "integer / string"}}
                                    },
                                    {"internal", RPCArg::Type::BOOL, RPCArg::Default{false}, "Whether matching outputs should be treated as not incoming payments (e.g. change)"},
                                    {"label", RPCArg::Type::STR, RPCArg::Default{""}, "Label to assign to the address, only allowed with internal=false. Disabled for ranged descriptors"},
                                },
                            },
                        },
                        RPCArgOptions{.oneline_description="requests"}},
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "Response is an array with the same size as the input that has the execution result",
                    {
                        {RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::BOOL, "success", ""},
                            {RPCResult::Type::ARR, "warnings", /*optional=*/true, "",
                            {
                                {RPCResult::Type::STR, "", ""},
                            }},
                            {RPCResult::Type::OBJ, "error", /*optional=*/true, "",
                            {
                                {RPCResult::Type::ELISION, "", "JSONRPC error"},
                            }},
                        }},
                    }
                },
                RPCExamples{
                    HelpExampleCli("importdescriptors", "'[{ \"desc\": \"<my descriptor>\", \"timestamp\":1455191478, \"internal\": true }, "
                                          "{ \"desc\": \"<my descriptor 2>\", \"label\": \"example 2\", \"timestamp\": 1455191480 }]'") +
                    HelpExampleCli("importdescriptors", "'[{ \"desc\": \"<my descriptor>\", \"timestamp\":1455191478, \"active\": true, \"range\": [0,100], \"label\": \"<my bech32 vault>\" }]'")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& main_request) -> UniValue
{
    std::shared_ptr<CVault> const pvault = GetVaultForJSONRPCRequest(main_request);
    if (!pvault) return UniValue::VNULL;
    CVault& vault{*pvault};

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    vault.BlockUntilSyncedToCurrentChain();

    CHECK_NONFATAL(pvault->IsVaultFlagSet(VAULT_FLAG_DESCRIPTORS));

    VaultRescanReserver reserver(*pvault);
    if (!reserver.reserve(/*with_passphrase=*/true)) {
        throw JSONRPCError(RPC_VAULT_ERROR, "Vault is currently rescanning. Abort existing rescan or wait.");
    }

    // Ensure that the vault is not locked for the remainder of this RPC, as
    // the passphrase is used to top up the keypool.
    LOCK(pvault->m_relock_mutex);

    const UniValue& requests = main_request.params[0];
    const int64_t minimum_timestamp = 1;
    int64_t now = 0;
    int64_t lowest_timestamp = 0;
    bool rescan = false;
    UniValue response(UniValue::VARR);
    {
        LOCK(pvault->cs_vault);
        EnsureVaultIsUnlocked(*pvault);

        CHECK_NONFATAL(pvault->chain().findBlock(pvault->GetLastBlockHash(), FoundBlock().time(lowest_timestamp).mtpTime(now)));

        // Get all timestamps and extract the lowest timestamp
        for (const UniValue& request : requests.getValues()) {
            // This throws an error if "timestamp" doesn't exist
            const int64_t timestamp = std::max(GetImportTimestamp(request, now), minimum_timestamp);
            const UniValue result = ProcessDescriptorImport(*pvault, request, timestamp);
            response.push_back(result);

            if (lowest_timestamp > timestamp ) {
                lowest_timestamp = timestamp;
            }

            // If we know the chain tip, and at least one request was successful then allow rescan
            if (!rescan && result["success"].get_bool()) {
                rescan = true;
            }
        }
        pvault->ConnectScriptPubKeyManNotifiers();
    }

    // Rescan the blockchain using the lowest timestamp
    if (rescan) {
        int64_t scanned_time = pvault->RescanFromTime(lowest_timestamp, reserver, /*update=*/true);
        pvault->ResubmitVaultTransactions(/*relay=*/false, /*force=*/true);

        if (pvault->IsAbortingRescan()) {
            throw JSONRPCError(RPC_MISC_ERROR, "Rescan aborted by user.");
        }
        if (scanned_time > lowest_timestamp && pvault->chain().shutdownRequested()) {
            // Same string rescanblockchain uses for USER_ABORT. "by user" is
            // false when the cause is a shutdown.
            throw JSONRPCError(RPC_MISC_ERROR, "Rescan aborted.");
        }

        if (scanned_time > lowest_timestamp) {
            std::vector<UniValue> results = response.getValues();
            response.clear();
            response.setArray();

            // Compose the response
            for (unsigned int i = 0; i < requests.size(); ++i) {
                const UniValue& request = requests.getValues().at(i);

                // If the descriptor timestamp is within the successfully scanned
                // range, or if the import result already has an error set, let
                // the result stand unmodified. Otherwise replace the result
                // with an error message.
                if (scanned_time <= GetImportTimestamp(request, now) || results.at(i).exists("error")) {
                    response.push_back(results.at(i));
                } else {
                    std::string error_msg{strprintf("Rescan failed for descriptor with timestamp %d. There "
                            "was an error reading a block from time %d, which is after or within %d seconds "
                            "of key creation, and could contain transactions pertaining to the desc. As a "
                            "result, transactions and coins using this desc may not appear in the vault.",
                            GetImportTimestamp(request, now), scanned_time - TIMESTAMP_WINDOW - 1, TIMESTAMP_WINDOW)};
                    if (pvault->chain().havePruned()) {
                        error_msg += strprintf(" This error could be caused by pruning or data corruption "
                                "(see quicksilverd log for details) and could be dealt with by downloading and "
                                "rescanning the relevant blocks (see -reindex option and rescanblockchain RPC).");
                    } else {
                        error_msg += strprintf(" This error could potentially caused by data corruption. If "
                                "the issue persists you may want to reindex (see -reindex option).");
                    }

                    UniValue result = UniValue(UniValue::VOBJ);
                    result.pushKV("success", UniValue(false));
                    result.pushKV("error", JSONRPCError(RPC_MISC_ERROR, error_msg));
                    response.push_back(std::move(result));
                }
            }
        }
    }

    return response;
},
    };
}

RPCHelpMan listdescriptors()
{
    return RPCHelpMan{
        "listdescriptors",
        "\nList all descriptors present in a descriptor-enabled vault.\n",
        {
            {"private", RPCArg::Type::BOOL, RPCArg::Default{false}, "Show private descriptors."}
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::STR, "vault_name", "Name of vault this operation was performed on"},
            {RPCResult::Type::ARR, "descriptors", "Array of descriptor objects (sorted by descriptor string representation)",
            {
                {RPCResult::Type::OBJ, "", "", {
                    {RPCResult::Type::STR, "desc", "Descriptor string representation"},
                    {RPCResult::Type::NUM, "timestamp", "The creation time of the descriptor"},
                    {RPCResult::Type::BOOL, "active", "Whether this descriptor is currently used to generate new addresses"},
                    {RPCResult::Type::BOOL, "internal", /*optional=*/true, "True if this descriptor is used to generate change addresses. False if this descriptor is used to generate receiving addresses; defined only for active descriptors"},
                    {RPCResult::Type::ARR_FIXED, "range", /*optional=*/true, "Defined only for ranged descriptors", {
                        {RPCResult::Type::NUM, "", "Range start inclusive"},
                        {RPCResult::Type::NUM, "", "Range end inclusive"},
                    }},
                    {RPCResult::Type::NUM, "next_index", /*optional=*/true, "The next index to generate addresses from; defined only for ranged descriptors"},
                }},
            }}
        }},
        RPCExamples{
            HelpExampleCli("listdescriptors", "") + HelpExampleRpc("listdescriptors", "")
            + HelpExampleCli("listdescriptors", "true") + HelpExampleRpc("listdescriptors", "true")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const std::shared_ptr<const CVault> vault = GetVaultForJSONRPCRequest(request);
    if (!vault) return UniValue::VNULL;

    CHECK_NONFATAL(vault->IsVaultFlagSet(VAULT_FLAG_DESCRIPTORS));

    const bool priv = !request.params[0].isNull() && request.params[0].get_bool();
    if (priv) {
        EnsureVaultIsUnlocked(*vault);
    }

    LOCK(vault->cs_vault);

    const auto active_spk_mans = vault->GetActiveScriptPubKeyMans();

    struct VaultDescInfo {
        std::string descriptor;
        uint64_t creation_time;
        bool active;
        std::optional<bool> internal;
        std::optional<std::pair<int64_t,int64_t>> range;
        int64_t next_index;
    };

    std::vector<VaultDescInfo> vault_descriptors;
    for (const auto& spk_man : vault->GetAllScriptPubKeyMans()) {
        const auto desc_spk_man = dynamic_cast<DescriptorScriptPubKeyMan*>(spk_man);
        if (!desc_spk_man) {
            throw JSONRPCError(RPC_VAULT_ERROR, "Unexpected ScriptPubKey manager type.");
        }
        LOCK(desc_spk_man->cs_desc_man);
        const auto& vault_descriptor = desc_spk_man->GetVaultDescriptor();
        std::string descriptor;
        if (!desc_spk_man->GetDescriptorString(descriptor, priv)) {
            throw JSONRPCError(RPC_VAULT_ERROR, "Can't get descriptor string.");
        }
        const bool is_range = vault_descriptor.descriptor->IsRange();
        vault_descriptors.push_back({
            descriptor,
            vault_descriptor.creation_time,
            active_spk_mans.count(desc_spk_man) != 0,
            vault->IsInternalScriptPubKeyMan(desc_spk_man),
            is_range ? std::optional(std::make_pair(vault_descriptor.range_start, vault_descriptor.range_end)) : std::nullopt,
            vault_descriptor.next_index
        });
    }

    std::sort(vault_descriptors.begin(), vault_descriptors.end(), [](const auto& a, const auto& b) {
        return a.descriptor < b.descriptor;
    });

    UniValue descriptors(UniValue::VARR);
    for (const VaultDescInfo& info : vault_descriptors) {
        UniValue spk(UniValue::VOBJ);
        spk.pushKV("desc", info.descriptor);
        spk.pushKV("timestamp", info.creation_time);
        spk.pushKV("active", info.active);
        if (info.internal.has_value()) {
            spk.pushKV("internal", info.internal.value());
        }
        if (info.range.has_value()) {
            UniValue range(UniValue::VARR);
            range.push_back(info.range->first);
            range.push_back(info.range->second - 1);
            spk.pushKV("range", std::move(range));
            spk.pushKV("next_index", info.next_index);
        }
        descriptors.push_back(std::move(spk));
    }

    UniValue response(UniValue::VOBJ);
    response.pushKV("vault_name", vault->GetName());
    response.pushKV("descriptors", std::move(descriptors));

    return response;
},
    };
}

RPCHelpMan exportvaultarchive()
{
    return RPCHelpMan{"exportvaultarchive",
                "\nWrites an encrypted archive of this vault's transaction history.\n"
                "\nA thin vault cannot rescan, so the vault file is otherwise the only record of\n"
                "what was sent, to whom, and when. A current vault-file backup preserves both\n"
                "spending keys and history; scantxoutset cannot recover keys or past activity.\n"
                "\nThe archive holds descriptors in their PUBLIC form, the vault birth time, and\n"
                "the full transaction history. It holds no private keys.\n",
                {
                    {"destination", RPCArg::Type::STR, RPCArg::Optional::NO, "The file to write the archive to"},
                    {"passphrase", RPCArg::Type::STR, RPCArg::Optional::NO, "The passphrase the archive is encrypted with. Losing it loses the history."},
                },
                RPCResult{RPCResult::Type::OBJ, "", "", {
                    {RPCResult::Type::STR, "path", "The archive that was written"},
                    {RPCResult::Type::NUM, "descriptors", "Number of descriptors written"},
                    {RPCResult::Type::NUM, "addresses", "Number of address book entries written"},
                    {RPCResult::Type::NUM, "transactions", "Number of transactions written"},
                }},
                RPCExamples{
                    HelpExampleCli("exportvaultarchive", "\"history.qsva\" \"a long passphrase\"")
            + HelpExampleRpc("exportvaultarchive", "\"history.qsva\", \"a long passphrase\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const std::shared_ptr<const CVault> pvault = GetVaultForJSONRPCRequest(request);
    if (!pvault) return UniValue::VNULL;

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pvault->BlockUntilSyncedToCurrentChain();

    SecureString passphrase;
    passphrase.reserve(100);
    passphrase = std::string_view{request.params[1].get_str()};
    if (passphrase.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "A vault archive must be protected by a passphrase.");
    }

    auto archive{BuildVaultArchive(*pvault)};
    if (!archive) {
        throw JSONRPCError(RPC_VAULT_ERROR, util::ErrorString(archive).original);
    }

    const fs::path destination{fs::PathFromString(request.params[0].get_str())};
    auto written{WriteVaultArchive(*archive, destination, passphrase)};
    if (!written) {
        throw JSONRPCError(RPC_VAULT_ERROR, util::ErrorString(written).original);
    }

    UniValue result{UniValue::VOBJ};
    result.pushKV("path", fs::PathToString(destination));
    result.pushKV("descriptors", static_cast<uint64_t>(archive->descriptors.size()));
    result.pushKV("addresses", static_cast<uint64_t>(archive->addresses.size()));
    result.pushKV("transactions", static_cast<uint64_t>(archive->transactions.size()));
    return result;
},
    };
}

RPCHelpMan importvaultarchive()
{
    return RPCHelpMan{"importvaultarchive",
                "\nRestores transaction history from an encrypted vault archive into this vault.\n"
                "\nDescriptors and transactions already present are left alone, so importing the\n"
                "same archive twice is harmless. This restores HISTORY: spendable balance comes\n"
                "from the keys, which an archive does not contain.\n",
                {
                    {"source", RPCArg::Type::STR, RPCArg::Optional::NO, "The archive file to read"},
                    {"passphrase", RPCArg::Type::STR, RPCArg::Optional::NO, "The passphrase the archive was encrypted with"},
                },
                RPCResult{RPCResult::Type::OBJ, "", "", {
                    {RPCResult::Type::NUM, "descriptors_imported", "Descriptors added to this vault"},
                    {RPCResult::Type::NUM, "addresses_imported", "Address book entries restored"},
                    {RPCResult::Type::NUM, "transactions_imported", "Transactions added to this vault"},
                    {RPCResult::Type::NUM, "transactions_skipped", "Transactions this vault already had"},
                }},
                RPCExamples{
                    HelpExampleCli("importvaultarchive", "\"history.qsva\" \"a long passphrase\"")
            + HelpExampleRpc("importvaultarchive", "\"history.qsva\", \"a long passphrase\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const std::shared_ptr<CVault> pvault = GetVaultForJSONRPCRequest(request);
    if (!pvault) return UniValue::VNULL;

    EnsureVaultIsUnlocked(*pvault);

    SecureString passphrase;
    passphrase.reserve(100);
    passphrase = std::string_view{request.params[1].get_str()};

    auto archive{ReadVaultArchive(fs::PathFromString(request.params[0].get_str()), passphrase)};
    if (!archive) {
        throw JSONRPCError(RPC_VAULT_ERROR, util::ErrorString(archive).original);
    }

    auto imported{ImportVaultArchive(*pvault, *archive)};
    if (!imported) {
        throw JSONRPCError(RPC_VAULT_ERROR, util::ErrorString(imported).original);
    }

    UniValue result{UniValue::VOBJ};
    result.pushKV("descriptors_imported", static_cast<uint64_t>(imported->descriptors_imported));
    result.pushKV("addresses_imported", static_cast<uint64_t>(imported->addresses_imported));
    result.pushKV("transactions_imported", static_cast<uint64_t>(imported->transactions_imported));
    result.pushKV("transactions_skipped", static_cast<uint64_t>(imported->transactions_skipped));
    return result;
},
    };
}

RPCHelpMan backupvault()
{
    return RPCHelpMan{"backupvault",
                "\nSafely copies the current vault file to the specified destination, which can either be a directory or a path with a filename.\n",
                {
                    {"destination", RPCArg::Type::STR, RPCArg::Optional::NO, "The destination directory or file"},
                },
                RPCResult{RPCResult::Type::NONE, "", ""},
                RPCExamples{
                    HelpExampleCli("backupvault", "\"backup.dat\"")
            + HelpExampleRpc("backupvault", "\"backup.dat\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const std::shared_ptr<const CVault> pvault = GetVaultForJSONRPCRequest(request);
    if (!pvault) return UniValue::VNULL;

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pvault->BlockUntilSyncedToCurrentChain();

    LOCK(pvault->cs_vault);

    std::string strDest = request.params[0].get_str();
    if (!pvault->BackupVault(strDest)) {
        throw JSONRPCError(RPC_VAULT_ERROR, "Error: Vault backup failed!");
    }

    return UniValue::VNULL;
},
    };
}


RPCHelpMan restorevault()
{
    return RPCHelpMan{
        "restorevault",
        "\nRestores and loads a vault from backup.\n"
        "\nThe rescan is significantly faster if block filters are available"
        "\n(using startup option \"-blockfilterindex=1\").\n",
        {
            {"vault_name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name that will be applied to the restored vault"},
            {"backup_file", RPCArg::Type::STR, RPCArg::Optional::NO, "The backup file that will be used to restore the vault."},
            {"load_on_startup", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Save vault name to persistent settings and load on startup. True to add vault to startup list, false to remove, null to leave unchanged."},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "name", "The vault name if restored successfully."},
                {RPCResult::Type::ARR, "warnings", /*optional=*/true, "Warning messages, if any, related to restoring and loading the vault.",
                {
                    {RPCResult::Type::STR, "", ""},
                }},
            }
        },
        RPCExamples{
            HelpExampleCli("restorevault", "\"testvault\" \"home\\backups\\backup-file.bak\"")
            + HelpExampleRpc("restorevault", "\"testvault\" \"home\\backups\\backup-file.bak\"")
            + HelpExampleCliNamed("restorevault", {{"vault_name", "testvault"}, {"backup_file", "home\\backups\\backup-file.bak\""}, {"load_on_startup", true}})
            + HelpExampleRpcNamed("restorevault", {{"vault_name", "testvault"}, {"backup_file", "home\\backups\\backup-file.bak\""}, {"load_on_startup", true}})
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{

    VaultContext& context = EnsureVaultContext(request.context);

    auto backup_file = fs::u8path(request.params[1].get_str());

    std::string vault_name = request.params[0].get_str();

    std::optional<bool> load_on_start = request.params[2].isNull() ? std::nullopt : std::optional<bool>(request.params[2].get_bool());

    DatabaseStatus status;
    bilingual_str error;
    std::vector<bilingual_str> warnings;

    const std::shared_ptr<CVault> vault = RestoreVault(context, backup_file, vault_name, load_on_start, status, error, warnings);

    HandleVaultError(vault, status, error);

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("name", vault->GetName());
    PushWarnings(warnings, obj);

    return obj;

},
    };
}
} // namespace vault
