// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/messages.h>
#include <consensus/validation.h>
#include <core_io.h>
#include <key_io.h>
#include <node/types.h>
#include <policy/policy.h>
#include <rpc/rawtransaction_util.h>
#include <rpc/util.h>
#include <script/script.h>
#include <util/translation.h>
#include <util/vector.h>
#include <vault/coincontrol.h>
#include <vault/rpc/util.h>
#include <vault/spend.h>
#include <vault/vault.h>

#include <univalue.h>

using common::TransactionErrorString;
using node::TransactionError;

namespace vault {
std::vector<CRecipient> CreateRecipients(const std::vector<std::pair<CTxDestination, CAmount>>& outputs)
{
    std::vector<CRecipient> recipients;
    for (const auto& [destination, amount] : outputs) {
        CRecipient recipient{destination, amount};
        recipients.push_back(recipient);
    }
    return recipients;
}

static UniValue FinishTransaction(const std::shared_ptr<CVault> pvault, const UniValue& options, const CMutableTransaction& rawTx)
{
    // Make a blank psqt
    PartiallySignedQuicksilverTransaction psqtx(rawTx);

    // First fill transaction with our data without signing,
    // so external signers are not asked to sign more than once.
    bool complete;
    pvault->FillPSQT(psqtx, complete, SIGHASH_DEFAULT, /*sign=*/false, /*bip32derivs=*/true);
    const auto err{pvault->FillPSQT(psqtx, complete, SIGHASH_DEFAULT, /*sign=*/true, /*bip32derivs=*/false)};
    if (err) {
        throw JSONRPCPSQTError(*err);
    }

    CMutableTransaction mtx;
    complete = FinalizeAndExtractPSQT(psqtx, mtx);

    // Quicksilver: a transaction that never went through CreateTransaction arrives
    // here with no per-tx proof -- sendall builds its own body, so it produced
    // transactions the node refused to broadcast ("bad-txns-pow-anchor", anchor=0)
    // while this function happily reported {"txid": ..., "complete": true}.
    //
    // Grinding AFTER signing is deliberate and is what CreateTransaction's own
    // regrind path relies on: the signature hash covers neither nAnchorHeight nor
    // nCycle nor nPowNonce, and PowPreimage covers neither scriptSig nor witness, so
    // the proof and the signatures cannot invalidate each other. It is also strictly
    // better here -- the transaction is complete, so its serialized size is known
    // exactly rather than estimated, and there is no regrind.
    if (complete && mtx.nAnchorHeight == 0 &&
        std::all_of(mtx.nCycle.begin(), mtx.nCycle.end(), [](uint32_t e) { return e == 0; })) {
        // cs_vault covers the anchor lookup only, and is released before the grind.
        // The grind runs for minutes at mainnet edge bits and needs nothing from the
        // vault — it reads the chain and writes the proof fields on a local
        // transaction — so holding the lock across it would stall every other vault
        // operation, including this node's own RPC, for that whole time.
        int anchor_height{0};
        {
            LOCK(pvault->cs_vault);
            const auto anchor_height_result{vault::TxPowAnchorHeight(*pvault)};
            if (!anchor_height_result) {
                throw JSONRPCError(RPC_VAULT_ERROR, util::ErrorString(anchor_height_result).original);
            }
            anchor_height = *anchor_height_result;
        }
        const uint64_t final_bytes{static_cast<uint64_t>(::GetSerializeSize(TX_WITH_WITNESS(CTransaction(mtx))))};
        if (auto err{vault::GrindTransactionPow(*pvault, mtx, final_bytes, anchor_height)}) {
            throw JSONRPCError(RPC_VAULT_ERROR, err->original);
        }
    }

    UniValue result(UniValue::VOBJ);

    const bool psqt_opt_in{options.exists("psqt") && options["psqt"].get_bool()};
    bool add_to_vault{options.exists("add_to_vault") ? options["add_to_vault"].get_bool() : true};
    if (psqt_opt_in || !complete || !add_to_vault) {
        // Serialize the PSQT
        DataStream ssTx{};
        ssTx << psqtx;
        result.pushKV("psqt", EncodeBase64(ssTx.str()));
    }

    if (complete) {
        std::string hex{EncodeHexTx(CTransaction(mtx))};
        CTransactionRef tx(MakeTransactionRef(std::move(mtx)));
        result.pushKV("txid", tx->GetHash().GetHex());
        if (add_to_vault && !psqt_opt_in) {
            const CommitTransactionResult committed{pvault->CommitTransaction(tx, {}, /*orderForm=*/{})};
            if (!committed) {
                throw JSONRPCTransactionError(committed.error, committed.reject_reason);
            }
        } else {
            result.pushKV("hex", hex);
        }
    }
    result.pushKV("complete", complete);

    return result;
}

static void PreventOutdatedOptions(const UniValue& options)
{
    if (options.exists("changeAddress")) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Use change_address instead of changeAddress");
    }
    if (options.exists("changePosition")) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Use change_position instead of changePosition");
    }
    if (options.exists("lockUnspents")) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Use lock_unspents instead of lockUnspents");
    }
}

std::string SendMoney(CVault& vault, const CCoinControl &coin_control, std::vector<CRecipient> &recipients, mapValue_t map_value)
{
    EnsureVaultIsUnlocked(vault);

    // This function is only used by sendtoaddress and sendmany.
    // This should always try to sign, if we don't have private keys, don't try to do anything here.
    if (vault.IsVaultFlagSet(VAULT_FLAG_DISABLE_PRIVATE_KEYS)) {
        throw JSONRPCError(RPC_VAULT_ERROR, "Error: Private keys are disabled for this vault");
    }

    // Shuffle recipient list
    std::shuffle(recipients.begin(), recipients.end(), FastRandomContext());

    // Send
    auto res = CreateTransaction(vault, recipients, /*change_pos=*/std::nullopt, coin_control, true);
    if (!res) {
        throw JSONRPCError(RPC_VAULT_INSUFFICIENT_FUNDS, util::ErrorString(res).original);
    }
    const CTransactionRef& tx = res->tx;
    const CommitTransactionResult committed{vault.CommitTransaction(tx, std::move(map_value), /*orderForm=*/{})};
    if (!committed) {
        throw JSONRPCTransactionError(committed.error, committed.reject_reason);
    }
    return tx->GetHash().GetHex();
}


RPCHelpMan sendtoaddress()
{
    return RPCHelpMan{"sendtoaddress",
                "\nSend an amount to a given address." +
        HELP_REQUIRING_PASSPHRASE,
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The Quicksilver address to send to."},
                    {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The amount in " + CURRENCY_UNIT + " to send. eg 0.1"},
                    {"comment", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "A comment used to store what the transaction is for.\n"
                                         "This is not part of the transaction, just kept in your vault."},
                    {"comment_to", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "A comment to store the name of the person or organization\n"
                                         "to which you're sending the transaction. This is not part of the \n"
                                         "transaction, just kept in your vault."},
                    {"avoid_reuse", RPCArg::Type::BOOL, RPCArg::Default{true}, "(only available if avoid_reuse vault flag is set) Avoid spending from dirty addresses; addresses are considered\n"
                                         "dirty if they have previously been used in a transaction. If true, this also activates avoidpartialspends, grouping outputs by their addresses."},
                },
                RPCResult{
                    RPCResult::Type::STR_HEX, "txid", "The transaction id."
                },
                RPCExamples{
                    "\nSend 0.1 Hg\n"
                    + HelpExampleCli("sendtoaddress", "\"" + EXAMPLE_ADDRESS[0] + "\" 0.1") +
                    "\nSend 0.5 Hg using named arguments\n"
                    + HelpExampleCli("-named sendtoaddress", "address=\"" + EXAMPLE_ADDRESS[0] + "\" amount=0.5 avoid_reuse=true comment=\"invoice 12\" comment_to=\"jeremy\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CVault> const pvault = GetVaultForJSONRPCRequest(request);
    if (!pvault) return UniValue::VNULL;

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pvault->BlockUntilSyncedToCurrentChain();

    // Vault comments
    mapValue_t mapValue;
    if (!request.params[2].isNull() && !request.params[2].get_str().empty())
        mapValue["comment"] = request.params[2].get_str();
    if (!request.params[3].isNull() && !request.params[3].get_str().empty())
        mapValue["to"] = request.params[3].get_str();

    CCoinControl coin_control;
    // Scoped so the lock is NOT held across SendMoney: that grinds a proof-of-work
    // which runs for minutes at mainnet edge bits, and holding cs_vault across it
    // blocks every other vault operation for that whole time. Everything below
    // acquires the lock for itself — CommitTransaction included.
    {
        LOCK(pvault->cs_vault);
        coin_control.m_avoid_address_reuse = GetAvoidReuseFlag(*pvault, request.params[4]);
        // We also enable partial spend avoidance if reuse avoidance is set.
        coin_control.m_avoid_partial_spends |= coin_control.m_avoid_address_reuse;

        EnsureVaultIsUnlocked(*pvault);
    }

    UniValue address_amounts(UniValue::VOBJ);
    const std::string address = request.params[0].get_str();
    address_amounts.pushKV(address, request.params[1]);

    std::vector<CRecipient> recipients{CreateRecipients(ParseOutputs(address_amounts))};
    return SendMoney(*pvault, coin_control, recipients, mapValue);
},
    };
}

RPCHelpMan sendmany()
{
    return RPCHelpMan{"sendmany",
        "Send multiple times. Amounts are double-precision floating point numbers." +
        HELP_REQUIRING_PASSPHRASE,
                {
                    {"amounts", RPCArg::Type::OBJ_USER_KEYS, RPCArg::Optional::NO, "The addresses and amounts",
                        {
                            {"address", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The Quicksilver address is the key, the numeric amount (can be string) in " + CURRENCY_UNIT + " is the value"},
                        },
                    },
                    {"comment", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "A comment"},
                },
                RPCResult{
                    RPCResult::Type::STR_HEX, "txid", "The transaction id for the send. Only 1 transaction is created regardless of\n"
                "the number of addresses."
                },
                RPCExamples{
            "\nSend two amounts to two different addresses:\n"
            + HelpExampleCli("sendmany", "\"{\\\"" + EXAMPLE_ADDRESS[0] + "\\\":0.01,\\\"" + EXAMPLE_ADDRESS[1] + "\\\":0.02}\"") +
            "\nSend two amounts to two different addresses with a comment:\n"
            + HelpExampleCli("sendmany", "\"{\\\"" + EXAMPLE_ADDRESS[0] + "\\\":0.01,\\\"" + EXAMPLE_ADDRESS[1] + "\\\":0.02}\" \"testing\"") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("sendmany", "{\"" + EXAMPLE_ADDRESS[0] + "\":0.01,\"" + EXAMPLE_ADDRESS[1] + "\":0.02}, \"testing\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CVault> const pvault = GetVaultForJSONRPCRequest(request);
    if (!pvault) return UniValue::VNULL;

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pvault->BlockUntilSyncedToCurrentChain();

    // No cs_vault here: nothing between this point and SendMoney reads vault state —
    // it is argument parsing — and holding it would cover SendMoney's proof-of-work
    // grind, which blocks every other vault operation for minutes at mainnet edge
    // bits. SendMoney's callees take the lock for themselves.
    UniValue sendTo = request.params[0].get_obj();

    mapValue_t mapValue;
    if (!request.params[1].isNull() && !request.params[1].get_str().empty())
        mapValue["comment"] = request.params[1].get_str();

    CCoinControl coin_control;
    std::vector<CRecipient> recipients = CreateRecipients(ParseOutputs(sendTo));
    return SendMoney(*pvault, coin_control, recipients, std::move(mapValue));
},
    };
}

// Shared snake_case fund options documented by every funding RPC.
static std::vector<RPCArg> FundTxDoc(bool solving_data = true)
{
    std::vector<RPCArg> args = {};
    if (solving_data) {
        args.push_back({"solving_data", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "Keys and scripts needed for producing a final transaction with a dummy signature.\n"
        "Used for input sizing during coin selection.",
            {
                {
                    "pubkeys", RPCArg::Type::ARR, RPCArg::Default{UniValue::VARR}, "Public keys involved in this transaction.",
                    {
                        {"pubkey", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "A public key"},
                    }
                },
                {
                    "scripts", RPCArg::Type::ARR, RPCArg::Default{UniValue::VARR}, "Scripts involved in this transaction.",
                    {
                        {"script", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "A script"},
                    }
                },
                {
                    "descriptors", RPCArg::Type::ARR, RPCArg::Default{UniValue::VARR}, "Descriptors that provide solving data for this transaction.",
                    {
                        {"descriptor", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "A descriptor"},
                    }
                },
            }
        });
    }
    return args;
}

CreatedTransactionResult FundTransaction(CVault& vault, const CMutableTransaction& tx, const std::vector<CRecipient>& recipients, const UniValue& options, CCoinControl& coinControl)
{
    // We want to make sure tx.vout is not used now that we are passing outputs as a vector of recipients.
    // This sets us up to remove tx completely in a future PR in favor of passing the inputs directly.
    CHECK_NONFATAL(tx.vout.empty());
    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    vault.BlockUntilSyncedToCurrentChain();

    std::optional<unsigned int> change_position;
    bool lockUnspents = false;
    if (!options.isNull()) {
        PreventOutdatedOptions(options);
        RPCTypeCheckObj(options,
            {
                {"add_inputs", UniValueType(UniValue::VBOOL)},
                {"include_unsafe", UniValueType(UniValue::VBOOL)},
                {"add_to_vault", UniValueType(UniValue::VBOOL)},
                {"change_address", UniValueType(UniValue::VSTR)},
                {"change_position", UniValueType(UniValue::VNUM)},
                {"change_type", UniValueType(UniValue::VSTR)},
                {"inputs", UniValueType(UniValue::VARR)},
                {"lock_unspents", UniValueType(UniValue::VBOOL)},
                {"locktime", UniValueType(UniValue::VNUM)},
                {"psqt", UniValueType(UniValue::VBOOL)},
                {"solving_data", UniValueType(UniValue::VOBJ)},
                {"minconf", UniValueType(UniValue::VNUM)},
                {"maxconf", UniValueType(UniValue::VNUM)},
                {"input_weights", UniValueType(UniValue::VARR)},
                {"max_tx_weight", UniValueType(UniValue::VNUM)},
            },
            true, true);

        if (options.exists("add_inputs")) {
            coinControl.m_allow_other_inputs = options["add_inputs"].get_bool();
        }

        if (options.exists("change_address")) {
            const std::string change_address_str = options["change_address"].get_str();
            CTxDestination dest = DecodeDestination(change_address_str);

            if (!IsValidDestination(dest)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Change address must be a valid Quicksilver address");
            }

            coinControl.destChange = dest;
        }

        if (options.exists("change_position")) {
            int pos = options["change_position"].getInt<int>();
            if (pos < 0 || (unsigned int)pos > recipients.size()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "change_position out of bounds");
            }
            change_position = (unsigned int)pos;
        }

        if (options.exists("change_type")) {
            if (options.exists("change_address")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both change address and address type options");
            }
            if (std::optional<OutputType> parsed = ParseOutputType(options["change_type"].get_str())) {
                coinControl.m_change_type.emplace(parsed.value());
            } else {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Unknown change type '%s'", options["change_type"].get_str()));
            }
        }

        if (options.exists("lock_unspents")) {
            lockUnspents = options["lock_unspents"].get_bool();
        }

        if (options.exists("include_unsafe")) {
            coinControl.m_include_unsafe_inputs = options["include_unsafe"].get_bool();
        }

        if (options.exists("minconf")) {
            coinControl.m_min_depth = options["minconf"].getInt<int>();

            if (coinControl.m_min_depth < 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative minconf");
            }
        }

        if (options.exists("maxconf")) {
            coinControl.m_max_depth = options["maxconf"].getInt<int>();

            if (coinControl.m_max_depth < coinControl.m_min_depth) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("maxconf can't be lower than minconf: %d < %d", coinControl.m_max_depth, coinControl.m_min_depth));
            }
        }
    }

    if (options.exists("solving_data")) {
        const UniValue solving_data = options["solving_data"].get_obj();
        if (solving_data.exists("pubkeys")) {
            for (const UniValue& pk_univ : solving_data["pubkeys"].get_array().getValues()) {
                const CPubKey pubkey = HexToPubKey(pk_univ.get_str());
                coinControl.m_external_provider.pubkeys.emplace(pubkey.GetID(), pubkey);
            }
        }

        if (solving_data.exists("scripts")) {
            for (const UniValue& script_univ : solving_data["scripts"].get_array().getValues()) {
                const std::string& script_str = script_univ.get_str();
                if (!IsHex(script_str)) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("'%s' is not hex", script_str));
                }
                std::vector<unsigned char> script_data(ParseHex(script_str));
                const CScript script(script_data.begin(), script_data.end());
                coinControl.m_external_provider.scripts.emplace(CScriptID(script), script);
            }
        }

        if (solving_data.exists("descriptors")) {
            for (const UniValue& desc_univ : solving_data["descriptors"].get_array().getValues()) {
                const std::string& desc_str  = desc_univ.get_str();
                FlatSigningProvider desc_out;
                std::string error;
                std::vector<CScript> scripts_temp;
                auto descs = Parse(desc_str, desc_out, error, true);
                if (descs.empty()) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Unable to parse descriptor '%s': %s", desc_str, error));
                }
                for (auto& desc : descs) {
                    desc->Expand(0, desc_out, scripts_temp, desc_out);
                }
                coinControl.m_external_provider.Merge(std::move(desc_out));
            }
        }
    }

    if (options.exists("input_weights")) {
        for (const UniValue& input : options["input_weights"].get_array().getValues()) {
            Txid txid = Txid::FromUint256(ParseHashO(input, "txid"));

            const UniValue& vout_v = input.find_value("vout");
            if (!vout_v.isNum()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, missing vout key");
            }
            int vout = vout_v.getInt<int>();
            if (vout < 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, vout cannot be negative");
            }

            const UniValue& weight_v = input.find_value("weight");
            if (!weight_v.isNum()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, missing weight key");
            }
            int64_t weight = weight_v.getInt<int64_t>();
            const int64_t min_input_weight = GetTransactionInputWeight(CTxIn());
            CHECK_NONFATAL(min_input_weight == 165);
            if (weight < min_input_weight) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, weight cannot be less than 165 (41 bytes (size of outpoint + sequence + empty scriptSig) * 4 (witness scaling factor)) + 1 (empty witness)");
            }
            if (weight > MAX_STANDARD_TX_WEIGHT) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, weight cannot be greater than the maximum standard tx weight of %d", MAX_STANDARD_TX_WEIGHT));
            }

            coinControl.SetInputWeight(COutPoint(txid, vout), weight);
        }
    }

    if (options.exists("max_tx_weight")) {
        coinControl.m_max_tx_weight = options["max_tx_weight"].getInt<int>();
    }

    if (recipients.empty())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "TX must have at least one output");

    auto txr = FundTransaction(vault, tx, recipients, change_position, lockUnspents, coinControl);
    if (!txr) {
        throw JSONRPCError(RPC_VAULT_ERROR, ErrorString(txr).original);
    }
    return *txr;
}

static void SetOptionsInputWeights(const UniValue& inputs, UniValue& options)
{
    if (options.exists("input_weights")) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Input weights should be specified in inputs rather than in options.");
    }
    if (inputs.size() == 0) {
        return;
    }
    UniValue weights(UniValue::VARR);
    for (const UniValue& input : inputs.getValues()) {
        if (input.exists("weight")) {
            weights.push_back(input);
        }
    }
    options.pushKV("input_weights", std::move(weights));
}

RPCHelpMan fundrawtransaction()
{
    return RPCHelpMan{"fundrawtransaction",
                "\nIf the transaction has no inputs, they will be automatically selected to meet its out value.\n"
                "It will add at most one change output to the outputs.\n"
                "No existing outputs will be modified.\n"
                "Note that inputs which were signed may need to be resigned after completion since in/outputs have been added.\n"
                "The inputs added will not be signed, use signrawtransactionwithkey\n"
                "or signrawtransactionwithvault for that.\n"
                "All existing inputs must either have their previous output transaction be in the vault\n"
                "or be in the UTXO set. Solving data must be provided for non-vault inputs.\n"
                "Note that all inputs selected must be of standard form and P2SH scripts must be\n"
                "available through vault descriptors or the solving_data option.\n"
                "You can see whether this is the case by checking the \"solvable\" field in the listunspent output.\n",
                {
                    {"hexstring", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The hex string of the raw transaction"},
                    {"options", RPCArg::Type::OBJ_NAMED_PARAMS, RPCArg::Optional::OMITTED, "",
                        Cat<std::vector<RPCArg>>(
                        {
                            {"add_inputs", RPCArg::Type::BOOL, RPCArg::Default{true}, "For a transaction with existing inputs, automatically include more if they are not enough."},
                            {"include_unsafe", RPCArg::Type::BOOL, RPCArg::Default{false}, "Include inputs that are not safe to spend (unconfirmed transactions from outside keys and unconfirmed replacement transactions).\n"
                                                          "Warning: the resulting transaction may become invalid if one of the unsafe inputs disappears.\n"
                                                          "If that happens, you will need to fund the transaction with different inputs and republish it."},
                            {"minconf", RPCArg::Type::NUM, RPCArg::Default{0}, "If add_inputs is specified, require inputs with at least this many confirmations."},
                            {"maxconf", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "If add_inputs is specified, require inputs with at most this many confirmations."},
                            {"change_address", RPCArg::Type::STR, RPCArg::DefaultHint{"automatic"}, "The Quicksilver address to receive the change"},
                            {"change_position", RPCArg::Type::NUM, RPCArg::DefaultHint{"random"}, "The index of the change output"},
                            {"change_type", RPCArg::Type::STR, RPCArg::DefaultHint{"set by -changetype"}, "The output type to use. Only valid if change_address is not specified. Options are \"base58\", \"bech32\", and \"bech32m\"."},
                            {"lock_unspents", RPCArg::Type::BOOL, RPCArg::Default{false}, "Lock selected unspent outputs"},
                            {"input_weights", RPCArg::Type::ARR, RPCArg::Optional::OMITTED, "Inputs and their corresponding weights",
                                {
                                    {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                        {
                                            {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                                            {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output index"},
                                            {"weight", RPCArg::Type::NUM, RPCArg::Optional::NO, "The maximum weight for this input, "
                                                "including the weight of the outpoint and sequence number. "
                                                "Note that serialized signature sizes are not guaranteed to be consistent, "
                                                "so the maximum DER signatures size of 73 bytes should be used when considering ECDSA signatures."
                                                "Remember to convert serialized sizes to weight units when necessary."},
                                        },
                                    },
                                },
                             },
                            {"max_tx_weight", RPCArg::Type::NUM, RPCArg::Default{MAX_STANDARD_TX_WEIGHT}, "The maximum acceptable transaction weight.\n"
                                                          "Transaction building will fail if this can not be satisfied."},
                        },
                        FundTxDoc()),
                        RPCArgOptions{
                            .oneline_description = "options",
                        }},
                    {"iswitness", RPCArg::Type::BOOL, RPCArg::DefaultHint{"depends on heuristic tests"}, "Whether the transaction hex is a serialized witness transaction.\n"
                        "If iswitness is not present, heuristic tests will be used in decoding.\n"
                        "If true, only witness deserialization will be tried.\n"
                        "If false, only non-witness deserialization will be tried.\n"
                        "This boolean should reflect whether the transaction has inputs\n"
                        "(e.g. fully valid, or on-chain transactions), if known by the caller."
                    },
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "hex", "The resulting raw transaction (hex-encoded string)"},
                        {RPCResult::Type::NUM, "changepos", "The position of the added change output, or -1"},
                    }
                                },
                                RPCExamples{
                            "\nCreate a transaction with no inputs\n"
                            + HelpExampleCli("createrawtransaction", "\"[]\" \"[{\\\"myaddress\\\":0.01}]\"") +
                            "\nAdd sufficient unsigned inputs to meet the output value\n"
                            + HelpExampleCli("fundrawtransaction", "\"rawtransactionhex\"") +
                            "\nSign the transaction\n"
                            + HelpExampleCli("signrawtransactionwithvault", "\"fundedtransactionhex\"") +
                            "\nSend the transaction\n"
                            + HelpExampleCli("sendrawtransaction", "\"signedtransactionhex\"")
                                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CVault> const pvault = GetVaultForJSONRPCRequest(request);
    if (!pvault) return UniValue::VNULL;

    // parse hex string from parameter
    CMutableTransaction tx;
    bool try_witness = request.params[2].isNull() ? true : request.params[2].get_bool();
    bool try_no_witness = request.params[2].isNull() ? true : !request.params[2].get_bool();
    if (!DecodeHexTx(tx, request.params[0].get_str(), try_no_witness, try_witness)) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
    }
    UniValue options = request.params[1];
    std::vector<std::pair<CTxDestination, CAmount>> destinations;
    for (const auto& tx_out : tx.vout) {
        CTxDestination dest;
        ExtractDestination(tx_out.scriptPubKey, dest);
        destinations.emplace_back(dest, tx_out.nValue);
    }
    std::vector<CRecipient> recipients = CreateRecipients(destinations);
    CCoinControl coin_control;
    // Automatically select (additional) coins. Can be overridden by options.add_inputs.
    coin_control.m_allow_other_inputs = true;
    // Clear tx.vout since it is not meant to be used now that we are passing outputs directly.
    // This sets us up for a future PR to completely remove tx from the function signature in favor of passing inputs directly
    tx.vout.clear();
    auto txr = FundTransaction(*pvault, tx, recipients, options, coin_control);

    UniValue result(UniValue::VOBJ);
    result.pushKV("hex", EncodeHexTx(*txr.tx));
    result.pushKV("changepos", txr.change_pos ? (int)*txr.change_pos : -1);

    return result;
},
    };
}

RPCHelpMan signrawtransactionwithvault()
{
    return RPCHelpMan{"signrawtransactionwithvault",
                "\nSign inputs for raw transaction (serialized, hex-encoded).\n"
                "The second optional argument (may be null) is an array of previous transaction outputs that\n"
                "this transaction depends on but may not yet be in the block chain." +
        HELP_REQUIRING_PASSPHRASE,
                {
                    {"hexstring", RPCArg::Type::STR, RPCArg::Optional::NO, "The transaction hex string"},
                    {"prevtxs", RPCArg::Type::ARR, RPCArg::Optional::OMITTED, "The previous dependent transaction outputs",
                        {
                            {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                {
                                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                                    {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output number"},
                                    {"output_script", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The output script"},
                                    {"redeem_script", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "(required for P2SH) redeem script"},
                                    {"witness_script", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "(required for P2WSH) witness script"},
                                    {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "(required for Segwit inputs) the amount spent"},
                                },
                            },
                        },
                    },
                    {"sighashtype", RPCArg::Type::STR, RPCArg::Default{"DEFAULT for Taproot, ALL otherwise"}, "The signature hash type. Must be one of\n"
            "       \"DEFAULT\"\n"
            "       \"ALL\"\n"
            "       \"NONE\"\n"
            "       \"SINGLE\"\n"
            "       \"ALL|ANYONECANPAY\"\n"
            "       \"NONE|ANYONECANPAY\"\n"
            "       \"SINGLE|ANYONECANPAY\""},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "hex", "The hex-encoded raw transaction with signature(s)"},
                        {RPCResult::Type::BOOL, "complete", "If the transaction has a complete set of signatures"},
                        {RPCResult::Type::ARR, "errors", /*optional=*/true, "Script verification errors (if there are any)",
                        {
                            {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR_HEX, "txid", "The hash of the referenced, previous transaction"},
                                {RPCResult::Type::NUM, "vout", "The index of the output to spent and used as input"},
                                {RPCResult::Type::ARR, "witness", "",
                                {
                                    {RPCResult::Type::STR_HEX, "witness", ""},
                                }},
                                {RPCResult::Type::STR_HEX, "input_script", "The hex-encoded input script"},
                                {RPCResult::Type::NUM, "sequence", "Script sequence number"},
                                {RPCResult::Type::STR, "error", "Verification or signing error related to the input"},
                            }},
                        }},
                    }
                },
                RPCExamples{
                    HelpExampleCli("signrawtransactionwithvault", "\"myhex\"")
            + HelpExampleRpc("signrawtransactionwithvault", "\"myhex\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const std::shared_ptr<const CVault> pvault = GetVaultForJSONRPCRequest(request);
    if (!pvault) return UniValue::VNULL;

    CMutableTransaction mtx;
    if (!DecodeHexTx(mtx, request.params[0].get_str())) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed. Make sure the tx has at least one input.");
    }

    // Sign the transaction
    LOCK(pvault->cs_vault);
    EnsureVaultIsUnlocked(*pvault);

    // Fetch previous transactions (inputs):
    std::map<COutPoint, Coin> coins;
    for (const CTxIn& txin : mtx.vin) {
        coins[txin.prevout]; // Create empty map entry keyed by prevout.
    }
    pvault->chain().findCoins(coins);

    // Parse the prevtxs array
    ParsePrevouts(request.params[1], nullptr, coins);

    int nHashType = ParseSighashString(request.params[2]);

    // Script verification errors
    std::map<int, bilingual_str> input_errors;

    bool complete = pvault->SignTransaction(mtx, coins, nHashType, input_errors);
    UniValue result(UniValue::VOBJ);
    SignTransactionResultToJSON(mtx, complete, coins, input_errors, result);
    return result;
},
    };
}

// Definition of allowed formats of specifying transaction outputs in
// the `send` and `vaultcreatefundedpsqt` RPCs.
static std::vector<RPCArg> OutputsDoc()
{
    return
    {
        {"", RPCArg::Type::OBJ_USER_KEYS, RPCArg::Optional::OMITTED, "",
            {
                {"address", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "A key-value pair. The key (string) is the Quicksilver address,\n"
                         "the value (float or string) is the amount in " + CURRENCY_UNIT + ""},
            },
        },
        {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
            {
                {"data", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "A key-value pair. The key must be \"data\", the value is hex-encoded data"},
            },
        },
    };
}

RPCHelpMan send()
{
    return RPCHelpMan{"send",
        "\nEXPERIMENTAL warning: this call may be changed in future releases.\n"
        "\nSend a transaction.\n",
        {
            {"outputs", RPCArg::Type::ARR, RPCArg::Optional::NO, "The outputs specified as key-value pairs.\n"
                    "Each key may only appear once, i.e. there can only be one 'data' output, and no address may be duplicated.\n"
                    "At least one output of either type must be specified.",
                OutputsDoc()},
            {"options", RPCArg::Type::OBJ_NAMED_PARAMS, RPCArg::Optional::OMITTED, "",
                Cat<std::vector<RPCArg>>(
                {
                    {"add_inputs", RPCArg::Type::BOOL, RPCArg::DefaultHint{"false when \"inputs\" are specified, true otherwise"},"Automatically include coins from the vault to cover the target amount.\n"},
                    {"include_unsafe", RPCArg::Type::BOOL, RPCArg::Default{false}, "Include inputs that are not safe to spend (unconfirmed transactions from outside keys and unconfirmed replacement transactions).\n"
                                                          "Warning: the resulting transaction may become invalid if one of the unsafe inputs disappears.\n"
                                                          "If that happens, you will need to fund the transaction with different inputs and republish it."},
                    {"minconf", RPCArg::Type::NUM, RPCArg::Default{0}, "If add_inputs is specified, require inputs with at least this many confirmations."},
                    {"maxconf", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "If add_inputs is specified, require inputs with at most this many confirmations."},
                    {"add_to_vault", RPCArg::Type::BOOL, RPCArg::Default{true}, "When false, returns a serialized transaction which will not be added to the vault or broadcast"},
                    {"change_address", RPCArg::Type::STR, RPCArg::DefaultHint{"automatic"}, "The Quicksilver address to receive the change"},
                    {"change_position", RPCArg::Type::NUM, RPCArg::DefaultHint{"random"}, "The index of the change output"},
                    {"change_type", RPCArg::Type::STR, RPCArg::DefaultHint{"set by -changetype"}, "The output type to use. Only valid if change_address is not specified. Options are \"base58\", \"bech32\" and \"bech32m\"."},
                    {"inputs", RPCArg::Type::ARR, RPCArg::Default{UniValue::VARR}, "Specify inputs instead of adding them automatically.",
                        {
                          {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "", {
                            {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                            {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output number"},
                            {"sequence", RPCArg::Type::NUM, RPCArg::DefaultHint{"depends on the value of the 'locktime' argument"}, "The sequence number"},
                            {"weight", RPCArg::Type::NUM, RPCArg::DefaultHint{"Calculated from vault and solving data"}, "The maximum weight for this input, "
                                        "including the weight of the outpoint and sequence number. "
                                        "Note that signature sizes are not guaranteed to be consistent, "
                                        "so the maximum DER signatures size of 73 bytes should be used when considering ECDSA signatures."
                                        "Remember to convert serialized sizes to weight units when necessary."},
                          }},
                        },
                    },
                    {"locktime", RPCArg::Type::NUM, RPCArg::Default{0}, "Raw locktime. Non-0 value also locktime-activates inputs"},
                    {"lock_unspents", RPCArg::Type::BOOL, RPCArg::Default{false}, "Lock selected unspent outputs"},
                    {"psqt", RPCArg::Type::BOOL,  RPCArg::DefaultHint{"automatic"}, "Always return a PSQT, implies add_to_vault=false."},
                    {"max_tx_weight", RPCArg::Type::NUM, RPCArg::Default{MAX_STANDARD_TX_WEIGHT}, "The maximum acceptable transaction weight.\n"
                                                  "Transaction building will fail if this can not be satisfied."},
                },
                FundTxDoc()),
                RPCArgOptions{.oneline_description="options"}},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::BOOL, "complete", "If the transaction has a complete set of signatures"},
                    {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "The transaction id for the send. Only 1 transaction is created regardless of the number of addresses."},
                    {RPCResult::Type::STR_HEX, "hex", /*optional=*/true, "If add_to_vault is false, the hex-encoded raw transaction with signature(s)"},
                    {RPCResult::Type::STR, "psqt", /*optional=*/true, "If more signatures are needed, or if add_to_vault is false, the base64-encoded (partially) signed transaction"}
                }
        },
        RPCExamples{""
        "\nSend 0.1 Hg\n"
        + HelpExampleCli("send", "'[{\"" + EXAMPLE_ADDRESS[0] + "\": 0.1}]'\n") +
        "Create a transaction that should confirm the next block, with a specific input, and return result without adding to vault or broadcasting to the network\n"
        + HelpExampleCli("send", "'[{\"" + EXAMPLE_ADDRESS[0] + "\": 0.1}]' '{\"add_to_vault\": false, \"inputs\": [{\"txid\":\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\", \"vout\":1}]}'")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            std::shared_ptr<CVault> const pvault = GetVaultForJSONRPCRequest(request);
            if (!pvault) return UniValue::VNULL;

            UniValue options{request.params[1].isNull() ? UniValue::VOBJ : request.params[1]};
            PreventOutdatedOptions(options);


            UniValue outputs(UniValue::VOBJ);
            outputs = NormalizeOutputs(request.params[0]);
            std::vector<CRecipient> recipients = CreateRecipients(ParseOutputs(outputs));
            CMutableTransaction rawTx = ConstructTransaction(options["inputs"], request.params[0], options["locktime"]);
            CCoinControl coin_control;
            // Automatically select coins, unless at least one is manually selected. Can
            // be overridden by options.add_inputs.
            coin_control.m_allow_other_inputs = rawTx.vin.size() == 0;
            if (options.exists("max_tx_weight")) {
                coin_control.m_max_tx_weight = options["max_tx_weight"].getInt<int>();
            }
            SetOptionsInputWeights(options["inputs"], options);
            // Clear tx.vout since it is not meant to be used now that we are passing outputs directly.
            // This sets us up for a future PR to completely remove tx from the function signature in favor of passing inputs directly
            rawTx.vout.clear();
            auto txr = FundTransaction(*pvault, rawTx, recipients, options, coin_control);

            return FinishTransaction(pvault, options, CMutableTransaction(*txr.tx));
        }
    };
}

RPCHelpMan sendall()
{
    return RPCHelpMan{"sendall",
        "EXPERIMENTAL warning: this call may be changed in future releases.\n"
        "\nSpend the value of all (or specific) confirmed UTXOs and unconfirmed change in the vault to one or more recipients.\n"
        "Unconfirmed inbound UTXOs and locked UTXOs will not be spent. Sendall will respect the avoid_reuse vault flag.\n",
        {
            {"recipients", RPCArg::Type::ARR, RPCArg::Optional::NO, "The sendall destinations. Each address may only appear once.\n"
                "Optionally some recipients can be specified with an amount to perform payments, but at least one address must appear without a specified amount.\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "A Quicksilver address which receives an equal share of the unspecified amount."},
                    {"", RPCArg::Type::OBJ_USER_KEYS, RPCArg::Optional::OMITTED, "",
                        {
                            {"address", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "A key-value pair. The key (string) is the Quicksilver address, the value (float or string) is the amount in " + CURRENCY_UNIT + ""},
                        },
                    },
                },
            },
            {
                "options", RPCArg::Type::OBJ_NAMED_PARAMS, RPCArg::Optional::OMITTED, "",
                Cat<std::vector<RPCArg>>(
                    {
                        {"add_to_vault", RPCArg::Type::BOOL, RPCArg::Default{true}, "When false, returns the serialized transaction without broadcasting or adding it to the vault"},
                        {"inputs", RPCArg::Type::ARR, RPCArg::Default{UniValue::VARR}, "Use exactly the specified inputs to build the transaction. Specifying inputs is incompatible with the minconf and maxconf options.",
                            {
                                {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                    {
                                        {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                                        {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output number"},
                                        {"sequence", RPCArg::Type::NUM, RPCArg::DefaultHint{"depends on the value of the 'locktime' argument"}, "The sequence number"},
                                    },
                                },
                            },
                        },
                        {"locktime", RPCArg::Type::NUM, RPCArg::Default{0}, "Raw locktime. Non-0 value also locktime-activates inputs"},
                        {"lock_unspents", RPCArg::Type::BOOL, RPCArg::Default{false}, "Lock selected unspent outputs"},
                        {"psqt", RPCArg::Type::BOOL,  RPCArg::DefaultHint{"automatic"}, "Always return a PSQT, implies add_to_vault=false."},
                        {"minconf", RPCArg::Type::NUM, RPCArg::Default{0}, "Require inputs with at least this many confirmations."},
                        {"maxconf", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Require inputs with at most this many confirmations."},
                    },
                    FundTxDoc()
                ),
                RPCArgOptions{.oneline_description="options"}
            },
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::BOOL, "complete", "If the transaction has a complete set of signatures"},
                    {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "The transaction id for the send. Only 1 transaction is created regardless of the number of addresses."},
                    {RPCResult::Type::STR_HEX, "hex", /*optional=*/true, "If add_to_vault is false, the hex-encoded raw transaction with signature(s)"},
                    {RPCResult::Type::STR, "psqt", /*optional=*/true, "If more signatures are needed, or if add_to_vault is false, the base64-encoded (partially) signed transaction"}
                }
        },
        RPCExamples{""
        "\nSpend all UTXOs from the vault\n"
        + HelpExampleCli("-named sendall", "recipients='[\"" + EXAMPLE_ADDRESS[0] + "\"]'\n") +
        "Spend all UTXOs split into equal amounts to two addresses\n"
        + HelpExampleCli("sendall", "'[\"" + EXAMPLE_ADDRESS[0] + "\", \"" + EXAMPLE_ADDRESS[1] + "\"]'\n") +
        "Spend all UTXOs using named arguments and sending 0.25 " + CURRENCY_UNIT + " to another recipient\n"
        + HelpExampleCli("-named sendall", "recipients='[{\"" + EXAMPLE_ADDRESS[1] + "\": 0.25}, \""+ EXAMPLE_ADDRESS[0] + "\"]'\n")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            std::shared_ptr<CVault> const pvault{GetVaultForJSONRPCRequest(request)};
            if (!pvault) return UniValue::VNULL;
            // Make sure the results are valid at least up to the most recent block
            // the user could have gotten from another RPC command prior to now
            pvault->BlockUntilSyncedToCurrentChain();

            UniValue options{request.params[1].isNull() ? UniValue::VOBJ : request.params[1]};
            PreventOutdatedOptions(options);


            std::set<std::string> addresses_without_amount;
            UniValue recipient_key_value_pairs(UniValue::VARR);
            const UniValue& recipients{request.params[0]};
            for (unsigned int i = 0; i < recipients.size(); ++i) {
                const UniValue& recipient{recipients[i]};
                if (recipient.isStr()) {
                    UniValue rkvp(UniValue::VOBJ);
                    rkvp.pushKV(recipient.get_str(), 0);
                    recipient_key_value_pairs.push_back(std::move(rkvp));
                    addresses_without_amount.insert(recipient.get_str());
                } else {
                    recipient_key_value_pairs.push_back(recipient);
                }
            }

            if (addresses_without_amount.size() == 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Must provide at least one address without a specified amount");
            }

            CCoinControl coin_control;


            if (options.exists("minconf")) {
                if (options["minconf"].getInt<int>() < 0)
                {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid minconf (minconf cannot be negative): %s", options["minconf"].getInt<int>()));
                }

                coin_control.m_min_depth = options["minconf"].getInt<int>();
            }

            if (options.exists("maxconf")) {
                coin_control.m_max_depth = options["maxconf"].getInt<int>();

                if (coin_control.m_max_depth < coin_control.m_min_depth) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("maxconf can't be lower than minconf: %d < %d", coin_control.m_max_depth, coin_control.m_min_depth));
                }
            }

            CMutableTransaction rawTx{ConstructTransaction(options["inputs"], recipient_key_value_pairs, options["locktime"])};
            // Scoped: everything that reads vault state runs under the lock, which is
            // then released before FinishTransaction grinds the proof-of-work. It used
            // to stay held across the grind, stalling the whole vault for minutes.
            {
            LOCK(pvault->cs_vault);

            CAmount total_input_value(0);
            if (options.exists("inputs") && (options.exists("minconf") || options.exists("maxconf"))) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot combine minconf or maxconf with specific inputs.");
            } else if (options.exists("inputs")) {
                for (const CTxIn& input : rawTx.vin) {
                    if (pvault->IsSpent(input.prevout)) {
                        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Input not available. UTXO (%s:%d) was already spent.", input.prevout.hash.ToString(), input.prevout.n));
                    }
                    const CVaultTx* tx{pvault->GetVaultTx(input.prevout.hash)};
                    if (!tx || input.prevout.n >= tx->tx->vout.size() || !(pvault->IsMine(tx->tx->vout[input.prevout.n]) & ISMINE_SPENDABLE)) {
                        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Input not found. UTXO (%s:%d) is not part of vault.", input.prevout.hash.ToString(), input.prevout.n));
                    }
                    total_input_value += tx->tx->vout[input.prevout.n].nValue;
                }
            } else {
                CoinFilterParams coins_params;
                coins_params.min_amount = 0;
                for (const COutput& output : AvailableCoins(*pvault, &coin_control, coins_params).All()) {
                    CHECK_NONFATAL(output.input_bytes > 0);
                    CTxIn input(output.outpoint.hash, output.outpoint.n, CScript(), CTxIn::MAX_SEQUENCE_NONFINAL);
                    rawTx.vin.push_back(input);
                    total_input_value += output.txout.nValue;
                }
            }

            // estimate final size of tx
            const TxSize tx_size{CalculateMaximumSignedTxSize(CTransaction(rawTx), pvault.get())};
            CAmount effective_value = total_input_value;

            if (effective_value <= 0) {
                throw JSONRPCError(RPC_VAULT_INSUFFICIENT_FUNDS, "Total value of UTXO pool too low to pay requested outputs.");
            }

            // If this transaction is too large, e.g. because the vault has many UTXOs, it will be rejected by the node's relaypool.
            if (tx_size.weight > MAX_STANDARD_TX_WEIGHT) {
                throw JSONRPCError(RPC_VAULT_ERROR, "Transaction too large.");
            }

            CAmount output_amounts_claimed{0};
            for (const CTxOut& out : rawTx.vout) {
                output_amounts_claimed += out.nValue;
            }

            if (output_amounts_claimed > total_input_value) {
                throw JSONRPCError(RPC_VAULT_INSUFFICIENT_FUNDS, "Assigned more value to outputs than available funds.");
            }

            const CAmount remainder{effective_value - output_amounts_claimed};
            if (remainder < 0) {
                throw JSONRPCError(RPC_VAULT_INSUFFICIENT_FUNDS, "Assigned outputs exceed available funds after creating specified outputs.");
            }

            const CAmount per_output_without_amount{remainder / (long)addresses_without_amount.size()};

            bool gave_remaining_to_first{false};
            for (CTxOut& out : rawTx.vout) {
                CTxDestination dest;
                ExtractDestination(out.scriptPubKey, dest);
                std::string addr{EncodeDestination(dest)};
                // Quicksilver is feeless/exact-change: no dust threshold on output amounts.
                if (addresses_without_amount.count(addr) > 0) {
                    out.nValue = per_output_without_amount;
                    if (!gave_remaining_to_first) {
                        out.nValue += remainder % addresses_without_amount.size();
                        gave_remaining_to_first = true;
                    }
                }
            }

            const bool lock_unspents{options.exists("lock_unspents") ? options["lock_unspents"].get_bool() : false};
            if (lock_unspents) {
                for (const CTxIn& txin : rawTx.vin) {
                    pvault->LockCoin(txin.prevout);
                }
            }
            } // cs_vault released: FinishTransaction below grinds a proof-of-work.

            return FinishTransaction(pvault, options, rawTx);
        }
    };
}

RPCHelpMan vaultprocesspsqt()
{
    return RPCHelpMan{"vaultprocesspsqt",
                "\nUpdate a PSQT with input information from our vault and then sign inputs\n"
                "that we can sign for." +
        HELP_REQUIRING_PASSPHRASE,
                {
                    {"psqt", RPCArg::Type::STR, RPCArg::Optional::NO, "The transaction base64 string"},
                    {"sign", RPCArg::Type::BOOL, RPCArg::Default{true}, "Also sign the transaction when updating (requires vault to be unlocked)"},
                    {"sighashtype", RPCArg::Type::STR, RPCArg::Default{"DEFAULT for Taproot, ALL otherwise"}, "The signature hash type to sign with if not specified by the PSQT. Must be one of\n"
            "       \"DEFAULT\"\n"
            "       \"ALL\"\n"
            "       \"NONE\"\n"
            "       \"SINGLE\"\n"
            "       \"ALL|ANYONECANPAY\"\n"
            "       \"NONE|ANYONECANPAY\"\n"
            "       \"SINGLE|ANYONECANPAY\""},
                    {"bip32derivs", RPCArg::Type::BOOL, RPCArg::Default{true}, "Include BIP 32 derivation paths for public keys if we know them"},
                    {"finalize", RPCArg::Type::BOOL, RPCArg::Default{true}, "Also finalize inputs if possible"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "psqt", "The base64-encoded partially signed transaction"},
                        {RPCResult::Type::BOOL, "complete", "If the transaction has a complete set of signatures"},
                        {RPCResult::Type::STR_HEX, "hex", /*optional=*/true, "The hex-encoded network transaction if complete"},
                    }
                },
                RPCExamples{
                    HelpExampleCli("vaultprocesspsqt", "\"psqt\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const std::shared_ptr<const CVault> pvault = GetVaultForJSONRPCRequest(request);
    if (!pvault) return UniValue::VNULL;

    const CVault& vault{*pvault};
    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    vault.BlockUntilSyncedToCurrentChain();

    // Unserialize the transaction
    PartiallySignedQuicksilverTransaction psqtx;
    std::string error;
    if (!DecodeBase64PSQT(psqtx, request.params[0].get_str(), error)) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, strprintf("TX decode failed %s", error));
    }

    // Get the sighash type
    int nHashType = ParseSighashString(request.params[2]);

    // Fill transaction with our data and also sign
    bool sign = request.params[1].isNull() ? true : request.params[1].get_bool();
    bool bip32derivs = request.params[3].isNull() ? true : request.params[3].get_bool();
    bool finalize = request.params[4].isNull() ? true : request.params[4].get_bool();
    bool complete = true;

    if (sign) EnsureVaultIsUnlocked(*pvault);

    const auto err{vault.FillPSQT(psqtx, complete, nHashType, sign, bip32derivs, nullptr, finalize)};
    if (err) {
        throw JSONRPCPSQTError(*err);
    }

    UniValue result(UniValue::VOBJ);
    DataStream ssTx{};
    ssTx << psqtx;
    result.pushKV("psqt", EncodeBase64(ssTx.str()));
    result.pushKV("complete", complete);
    if (complete) {
        CMutableTransaction mtx;
        // Returns true if complete, which we already think it is.
        CHECK_NONFATAL(FinalizeAndExtractPSQT(psqtx, mtx));
        DataStream ssTx_final;
        ssTx_final << TX_WITH_WITNESS(mtx);
        result.pushKV("hex", HexStr(ssTx_final));
    }

    return result;
},
    };
}

RPCHelpMan vaultcreatefundedpsqt()
{
    return RPCHelpMan{"vaultcreatefundedpsqt",
                "\nCreates and funds a transaction in the Partially Signed Quicksilver Transaction (PSQT) format.\n"
                "Implements the Creator and Updater roles.\n"
                "All existing inputs must either have their previous output transaction be in the vault\n"
                "or be in the UTXO set. Solving data must be provided for non-vault inputs.\n",
                {
                    {"inputs", RPCArg::Type::ARR, RPCArg::Optional::OMITTED, "Leave empty to add inputs automatically. See add_inputs option.",
                        {
                            {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                {
                                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                                    {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output number"},
                                    {"sequence", RPCArg::Type::NUM, RPCArg::DefaultHint{"depends on the value of the 'locktime' argument"}, "The sequence number"},
                                    {"weight", RPCArg::Type::NUM, RPCArg::DefaultHint{"Calculated from vault and solving data"}, "The maximum weight for this input, "
                                        "including the weight of the outpoint and sequence number. "
                                        "Note that signature sizes are not guaranteed to be consistent, "
                                        "so the maximum DER signatures size of 73 bytes should be used when considering ECDSA signatures."
                                        "Remember to convert serialized sizes to weight units when necessary."},
                                },
                            },
                        },
                        },
                    {"outputs", RPCArg::Type::ARR, RPCArg::Optional::NO, "The outputs specified as key-value pairs.\n"
                            "Each key may only appear once, i.e. there can only be one 'data' output, and no address may be duplicated.\n"
                            "At least one output of either type must be specified.",
                        OutputsDoc()},
                    {"locktime", RPCArg::Type::NUM, RPCArg::Default{0}, "Raw locktime. Non-0 value also locktime-activates inputs"},
                    {"options", RPCArg::Type::OBJ_NAMED_PARAMS, RPCArg::Optional::OMITTED, "",
                        Cat<std::vector<RPCArg>>(
                        {
                            {"add_inputs", RPCArg::Type::BOOL, RPCArg::DefaultHint{"false when \"inputs\" are specified, true otherwise"}, "Automatically include coins from the vault to cover the target amount.\n"},
                            {"include_unsafe", RPCArg::Type::BOOL, RPCArg::Default{false}, "Include inputs that are not safe to spend (unconfirmed transactions from outside keys and unconfirmed replacement transactions).\n"
                                                          "Warning: the resulting transaction may become invalid if one of the unsafe inputs disappears.\n"
                                                          "If that happens, you will need to fund the transaction with different inputs and republish it."},
                            {"minconf", RPCArg::Type::NUM, RPCArg::Default{0}, "If add_inputs is specified, require inputs with at least this many confirmations."},
                            {"maxconf", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "If add_inputs is specified, require inputs with at most this many confirmations."},
                            {"change_address", RPCArg::Type::STR, RPCArg::DefaultHint{"automatic"}, "The Quicksilver address to receive the change"},
                            {"change_position", RPCArg::Type::NUM, RPCArg::DefaultHint{"random"}, "The index of the change output"},
                            {"change_type", RPCArg::Type::STR, RPCArg::DefaultHint{"set by -changetype"}, "The output type to use. Only valid if change_address is not specified. Options are \"base58\", \"bech32\", and \"bech32m\"."},
                            {"lock_unspents", RPCArg::Type::BOOL, RPCArg::Default{false}, "Lock selected unspent outputs"},
                            {"max_tx_weight", RPCArg::Type::NUM, RPCArg::Default{MAX_STANDARD_TX_WEIGHT}, "The maximum acceptable transaction weight.\n"
                                                          "Transaction building will fail if this can not be satisfied."},
                        },
                        FundTxDoc()),
                        RPCArgOptions{.oneline_description="options"}},
                    {"bip32derivs", RPCArg::Type::BOOL, RPCArg::Default{true}, "Include BIP 32 derivation paths for public keys if we know them"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "psqt", "The resulting raw transaction (base64-encoded string)"},
                        {RPCResult::Type::NUM, "changepos", "The position of the added change output, or -1"},
                    }
                                },
                                RPCExamples{
                            "\nCreate a transaction with no inputs\n"
                            + HelpExampleCli("vaultcreatefundedpsqt", "\"[{\\\"txid\\\":\\\"myid\\\",\\\"vout\\\":0}]\" \"[{\\\"data\\\":\\\"00010203\\\"}]\"")
                                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CVault> const pvault = GetVaultForJSONRPCRequest(request);
    if (!pvault) return UniValue::VNULL;

    CVault& vault{*pvault};
    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    vault.BlockUntilSyncedToCurrentChain();

    UniValue options{request.params[3].isNull() ? UniValue::VOBJ : request.params[3]};

    CMutableTransaction rawTx = ConstructTransaction(request.params[0], request.params[1], request.params[2]);
    UniValue outputs(UniValue::VOBJ);
    outputs = NormalizeOutputs(request.params[1]);
    std::vector<CRecipient> recipients = CreateRecipients(ParseOutputs(outputs));
    CCoinControl coin_control;
    // Automatically select coins, unless at least one is manually selected. Can
    // be overridden by options.add_inputs.
    coin_control.m_allow_other_inputs = rawTx.vin.size() == 0;
    SetOptionsInputWeights(request.params[0], options);
    // Clear tx.vout since it is not meant to be used now that we are passing outputs directly.
    // This sets us up for a future PR to completely remove tx from the function signature in favor of passing inputs directly
    rawTx.vout.clear();
    auto txr = FundTransaction(vault, rawTx, recipients, options, coin_control);

    // Make a blank psqt
    PartiallySignedQuicksilverTransaction psqtx(CMutableTransaction(*txr.tx));

    // Fill transaction with out data but don't sign
    bool bip32derivs = request.params[4].isNull() ? true : request.params[4].get_bool();
    bool complete = true;
    const auto err{vault.FillPSQT(psqtx, complete, 1, /*sign=*/false, /*bip32derivs=*/bip32derivs)};
    if (err) {
        throw JSONRPCPSQTError(*err);
    }

    // Serialize the PSQT
    DataStream ssTx{};
    ssTx << psqtx;

    UniValue result(UniValue::VOBJ);
    result.pushKV("psqt", EncodeBase64(ssTx.str()));
    result.pushKV("changepos", txr.change_pos ? (int)*txr.change_pos : -1);
    return result;
},
    };
}
} // namespace vault
