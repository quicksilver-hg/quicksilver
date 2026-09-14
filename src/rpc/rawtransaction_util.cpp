// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpc/rawtransaction_util.h>

#include <coins.h>
#include <consensus/amount.h>
#include <core_io.h>
#include <key_io.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <rpc/request.h>
#include <rpc/util.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <tinyformat.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <util/translation.h>

void AddInputs(CMutableTransaction& rawTx, const UniValue& inputs_in)
{
    UniValue inputs;
    if (inputs_in.isNull()) {
        inputs = UniValue::VARR;
    } else {
        inputs = inputs_in.get_array();
    }

    for (unsigned int idx = 0; idx < inputs.size(); idx++) {
        const UniValue& input = inputs[idx];
        const UniValue& o = input.get_obj();

        Txid txid = Txid::FromUint256(ParseHashO(o, "txid"));

        const UniValue& vout_v = o.find_value("vout");
        if (!vout_v.isNum())
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, missing vout key");
        int nOutput = vout_v.getInt<int>();
        if (nOutput < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, vout cannot be negative");

        // Default to non-final so nLockTime binds. Never SEQUENCE_FINAL by default;
        // callers that want a final input set `sequence` explicitly just below.
        uint32_t nSequence = CTxIn::MAX_SEQUENCE_NONFINAL;

        // set the sequence number if passed in the parameters object
        const UniValue& sequenceObj = o.find_value("sequence");
        if (sequenceObj.isNum()) {
            int64_t seqNr64 = sequenceObj.getInt<int64_t>();
            if (seqNr64 < 0 || seqNr64 > CTxIn::SEQUENCE_FINAL) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, sequence number is out of range");
            } else {
                nSequence = (uint32_t)seqNr64;
            }
        }

        CTxIn in(COutPoint(txid, nOutput), CScript(), nSequence);

        rawTx.vin.push_back(in);
    }
}

UniValue NormalizeOutputs(const UniValue& outputs_in)
{
    if (outputs_in.isNull()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, output argument must be non-null");
    }
    if (!outputs_in.isArray()) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid parameter, outputs must be an array of key-value pairs");
    }

    // Translate array of key-value pairs into a dict for ParseOutputs.
    UniValue outputs_dict = UniValue(UniValue::VOBJ);
    for (size_t i = 0; i < outputs_in.size(); ++i) {
        const UniValue& output = outputs_in[i];
        if (!output.isObject()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, key-value pair not an object as expected");
        }
        if (output.size() != 1) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, key-value pair must contain exactly one key");
        }
        outputs_dict.pushKVs(output);
    }
    return outputs_dict;
}

std::vector<std::pair<CTxDestination, CAmount>> ParseOutputs(const UniValue& outputs)
{
    // Duplicate checking
    std::set<CTxDestination> destinations;
    std::vector<std::pair<CTxDestination, CAmount>> parsed_outputs;
    bool has_data{false};
    for (const std::string& name_ : outputs.getKeys()) {
        if (name_ == "data") {
            if (has_data) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, duplicate key: data");
            }
            has_data = true;
            std::vector<unsigned char> data = ParseHexV(outputs[name_].getValStr(), "Data");
            CTxDestination destination{CNoDestination{CScript() << OP_RETURN << data}};
            CAmount amount{0};
            parsed_outputs.emplace_back(destination, amount);
        } else {
            CTxDestination destination{DecodeDestination(name_)};
            CAmount amount{AmountFromValue(outputs[name_])};
            if (!IsValidDestination(destination)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Quicksilver address: ") + name_);
            }

            if (!destinations.insert(destination).second) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid parameter, duplicated address: ") + name_);
            }
            parsed_outputs.emplace_back(destination, amount);
        }
    }
    return parsed_outputs;
}

void AddOutputs(CMutableTransaction& rawTx, const UniValue& outputs_in)
{
    UniValue outputs(UniValue::VOBJ);
    outputs = NormalizeOutputs(outputs_in);

    std::vector<std::pair<CTxDestination, CAmount>> parsed_outputs = ParseOutputs(outputs);
    for (const auto& [destination, nAmount] : parsed_outputs) {
        CScript scriptPubKey = GetScriptForDestination(destination);

        CTxOut out(nAmount, scriptPubKey);
        rawTx.vout.push_back(out);
    }
}

CMutableTransaction ConstructTransaction(const UniValue& inputs_in, const UniValue& outputs_in, const UniValue& locktime)
{
    CMutableTransaction rawTx;

    if (!locktime.isNull()) {
        int64_t nLockTime = locktime.getInt<int64_t>();
        if (nLockTime < 0 || nLockTime > LOCKTIME_MAX)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, locktime out of range");
        rawTx.nLockTime = nLockTime;
    }

    AddInputs(rawTx, inputs_in);
    AddOutputs(rawTx, outputs_in);

    return rawTx;
}

/** Pushes a JSON object for script verification or signing errors to vErrorsRet. */
static void TxInErrorToJSON(const CTxIn& txin, UniValue& vErrorsRet, const std::string& strMessage)
{
    UniValue entry(UniValue::VOBJ);
    entry.pushKV("txid", txin.prevout.hash.ToString());
    entry.pushKV("vout", (uint64_t)txin.prevout.n);
    UniValue witness(UniValue::VARR);
    for (unsigned int i = 0; i < txin.scriptWitness.stack.size(); i++) {
        witness.push_back(HexStr(txin.scriptWitness.stack[i]));
    }
    entry.pushKV("witness", std::move(witness));
    entry.pushKV("input_script", HexStr(txin.scriptSig));
    entry.pushKV("sequence", (uint64_t)txin.nSequence);
    entry.pushKV("error", strMessage);
    vErrorsRet.push_back(std::move(entry));
}

void ParsePrevouts(const UniValue& prevTxsUnival, FlatSigningProvider* keystore, std::map<COutPoint, Coin>& coins)
{
    if (!prevTxsUnival.isNull()) {
        const UniValue& prevTxs = prevTxsUnival.get_array();
        for (unsigned int idx = 0; idx < prevTxs.size(); ++idx) {
            const UniValue& p = prevTxs[idx];
            if (!p.isObject()) {
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "expected object with {\"txid\",\"vout\",\"output_script\"}");
            }

            const UniValue& prevOut = p.get_obj();

            if (prevOut.exists("scriptPubKey")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Use output_script instead of scriptPubKey");
            }
            if (prevOut.exists("redeemScript")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Use redeem_script instead of redeemScript");
            }
            if (prevOut.exists("witnessScript")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Use witness_script instead of witnessScript");
            }

            RPCTypeCheckObj(prevOut,
                {
                    {"txid", UniValueType(UniValue::VSTR)},
                    {"vout", UniValueType(UniValue::VNUM)},
                    {"output_script", UniValueType(UniValue::VSTR)},
                });

            Txid txid = Txid::FromUint256(ParseHashO(prevOut, "txid"));

            int nOut = prevOut.find_value("vout").getInt<int>();
            if (nOut < 0) {
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "vout cannot be negative");
            }

            COutPoint out(txid, nOut);
            std::vector<unsigned char> pkData(ParseHexO(prevOut, "output_script"));
            CScript scriptPubKey(pkData.begin(), pkData.end());

            {
                auto coin = coins.find(out);
                if (coin != coins.end() && !coin->second.IsSpent() && coin->second.out.scriptPubKey != scriptPubKey) {
                    std::string err("Previous output_script mismatch:\n");
                    err = err + ScriptToAsmStr(coin->second.out.scriptPubKey) + "\nvs:\n"+
                        ScriptToAsmStr(scriptPubKey);
                    throw JSONRPCError(RPC_DESERIALIZATION_ERROR, err);
                }
                Coin newcoin;
                newcoin.out.scriptPubKey = scriptPubKey;
                newcoin.out.nValue = MAX_MONEY;
                if (prevOut.exists("amount")) {
                    newcoin.out.nValue = AmountFromValue(prevOut.find_value("amount"));
                }
                newcoin.nHeight = 1;
                coins[out] = std::move(newcoin);
            }

            // If scripts and private keys were given, add the script needed to
            // solve this native P2SH or P2WSH output.
            const bool is_p2sh = scriptPubKey.IsPayToScriptHash();
            const bool is_p2wsh = scriptPubKey.IsPayToWitnessScriptHash();
            if (keystore && (is_p2sh || is_p2wsh)) {
                RPCTypeCheckObj(prevOut,
                    {
                        {"redeem_script", UniValueType(UniValue::VSTR)},
                        {"witness_script", UniValueType(UniValue::VSTR)},
                    }, true);
                const UniValue& rs{prevOut.find_value("redeem_script")};
                const UniValue& ws{prevOut.find_value("witness_script")};

                if (is_p2sh) {
                    if (rs.isNull()) {
                        throw JSONRPCError(RPC_INVALID_PARAMETER, "Missing redeem_script");
                    }
                    if (!ws.isNull()) {
                        throw JSONRPCError(RPC_INVALID_PARAMETER, "witness_script is not valid for a P2SH input");
                    }
                    const std::vector<unsigned char> script_data{ParseHexV(rs, "redeem_script")};
                    const CScript script{script_data.begin(), script_data.end()};
                    int witness_version;
                    std::vector<unsigned char> witness_program;
                    if (script.IsWitnessProgram(witness_version, witness_program)) {
                        throw JSONRPCError(RPC_INVALID_PARAMETER, "P2SH-wrapped witness programs are not supported");
                    }
                    const CTxDestination p2sh{ScriptHash(script)};
                    if (scriptPubKey != GetScriptForDestination(p2sh)) {
                        throw JSONRPCError(RPC_INVALID_PARAMETER, "redeem_script does not match output_script");
                    }
                    keystore->scripts.emplace(CScriptID(script), script);
                } else if (is_p2wsh) {
                    if (ws.isNull()) {
                        throw JSONRPCError(RPC_INVALID_PARAMETER, "Missing witness_script");
                    }
                    if (!rs.isNull()) {
                        throw JSONRPCError(RPC_INVALID_PARAMETER, "redeem_script is not valid for a P2WSH input");
                    }
                    const std::vector<unsigned char> script_data{ParseHexV(ws, "witness_script")};
                    const CScript script{script_data.begin(), script_data.end()};
                    const CTxDestination p2wsh{WitnessV0ScriptHash(script)};
                    if (scriptPubKey != GetScriptForDestination(p2wsh)) {
                        throw JSONRPCError(RPC_INVALID_PARAMETER, "witness_script does not match output_script");
                    }
                    keystore->scripts.emplace(CScriptID(script), script);
                }
            }
        }
    }
}

void SignTransaction(CMutableTransaction& mtx, const SigningProvider* keystore, const std::map<COutPoint, Coin>& coins, const UniValue& hashType, UniValue& result)
{
    int nHashType = ParseSighashString(hashType);

    // Script verification errors
    std::map<int, bilingual_str> input_errors;

    bool complete = SignTransaction(mtx, keystore, coins, nHashType, input_errors);
    SignTransactionResultToJSON(mtx, complete, coins, input_errors, result);
}

void SignTransactionResultToJSON(CMutableTransaction& mtx, bool complete, const std::map<COutPoint, Coin>& coins, const std::map<int, bilingual_str>& input_errors, UniValue& result)
{
    // Make errors UniValue
    UniValue vErrors(UniValue::VARR);
    for (const auto& err_pair : input_errors) {
        if (err_pair.second.original == "Missing amount") {
            // This particular error needs to be an exception for some reason
            throw JSONRPCError(RPC_TYPE_ERROR, strprintf("Missing amount for %s", coins.at(mtx.vin.at(err_pair.first).prevout).out.ToString()));
        }
        TxInErrorToJSON(mtx.vin.at(err_pair.first), vErrors, err_pair.second.original);
    }

    result.pushKV("hex", EncodeHexTx(CTransaction(mtx)));
    result.pushKV("complete", complete);
    if (!vErrors.empty()) {
        if (result.exists("errors")) {
            vErrors.push_backV(result["errors"].getValues());
        }
        result.pushKV("errors", std::move(vErrors));
    }
}
