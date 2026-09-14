// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/args.h>
#include <rpc/client.h>
#include <tinyformat.h>

#include <set>
#include <stdint.h>
#include <string>
#include <string_view>

class CRPCConvertParam
{
public:
    std::string methodName; //!< method whose params want conversion
    int paramIdx;           //!< 0-based idx of param to convert
    std::string paramName;  //!< parameter name
};

// clang-format off
/**
 * Specify a (method, idx, name) here if the argument is a non-string RPC
 * argument and needs to be converted from JSON.
 *
 * @note Parameter indexes start from 0.
 */
static const CRPCConvertParam vRPCConvertParams[] =
{
    { "setmocktime", 0, "timestamp" },
    { "mockscheduler", 0, "delta_time" },
    { "utxoupdatepsqt", 1, "descriptors" },
    { "generatetoaddress", 0, "nblocks" },
    { "generatetoaddress", 2, "maxtries" },
    { "generatetodescriptor", 0, "num_blocks" },
    { "generatetodescriptor", 2, "maxtries" },
    { "generateblock", 1, "transactions" },
    { "generateblock", 2, "submit" },
    { "getnetworkworkps", 0, "nblocks" },
    { "getnetworkworkps", 1, "height" },
    { "sendtoaddress", 1, "amount" },
    { "sendtoaddress", 4, "avoid_reuse" },
    { "getreceivedbyaddress", 1, "minconf" },
    { "getreceivedbyaddress", 2, "include_immature_coinbase" },
    { "getreceivedbylabel", 1, "minconf" },
    { "getreceivedbylabel", 2, "include_immature_coinbase" },
    { "listreceivedbyaddress", 0, "minconf" },
    { "listreceivedbyaddress", 1, "include_empty" },
    { "listreceivedbyaddress", 3, "include_immature_coinbase" },
    { "listreceivedbylabel", 0, "minconf" },
    { "listreceivedbylabel", 1, "include_empty" },
    { "listreceivedbylabel", 2, "include_immature_coinbase" },
    { "getbalance", 0, "minconf" },
    { "getbalance", 1, "avoid_reuse" },
    { "getblockfrompeer", 1, "peer_id" },
    { "getblockhash", 0, "height" },
    { "waitforblockheight", 0, "height" },
    { "waitforblockheight", 1, "timeout" },
    { "waitforblock", 1, "timeout" },
    { "waitfornewblock", 0, "timeout" },
    { "listtransactions", 1, "count" },
    { "listtransactions", 2, "skip" },
    { "vaultpassphrase", 1, "timeout" },
    { "getblocktemplate", 0, "template_request" },
    { "listsinceblock", 1, "target_confirmations" },
    { "listsinceblock", 2, "include_removed" },
    { "listsinceblock", 3, "include_change" },
    { "sendmany", 0, "amounts" },
    { "deriveaddresses", 1, "range" },
    { "scanblocks", 1, "scanobjects" },
    { "scanblocks", 2, "start_height" },
    { "scanblocks", 3, "stop_height" },
    { "scanblocks", 5, "options" },
    { "scanblocks", 5, "filter_false_positives" },
    { "getdescriptoractivity", 0, "blockhashes" },
    { "getdescriptoractivity", 1, "scanobjects" },
    { "getdescriptoractivity", 2, "include_relaypool" },
    { "scantxoutset", 1, "scanobjects" },
    { "createmultisig", 0, "nrequired" },
    { "createmultisig", 1, "keys" },
    { "listunspent", 0, "minconf" },
    { "listunspent", 1, "maxconf" },
    { "listunspent", 2, "addresses" },
    { "listunspent", 3, "include_unsafe" },
    { "listunspent", 4, "query_options" },
    { "listunspent", 4, "minimum_amount" },
    { "listunspent", 4, "maximum_amount" },
    { "listunspent", 4, "maximum_count" },
    { "listunspent", 4, "minimum_sum_amount" },
    { "listunspent", 4, "include_immature_coinbase" },
    { "getblock", 1, "verbosity" },
    { "getblockheader", 1, "verbose" },
    { "getchaintxstats", 0, "nblocks" },
    { "gettransaction", 1, "verbose" },
    { "getrawtransaction", 1, "verbosity" },
    { "createrawtransaction", 0, "inputs" },
    { "createrawtransaction", 1, "outputs" },
    { "createrawtransaction", 2, "locktime" },
    { "decoderawtransaction", 1, "iswitness" },
    { "signrawtransactionwithkey", 1, "privkeys" },
    { "signrawtransactionwithkey", 2, "prevtxs" },
    { "signrawtransactionwithvault", 1, "prevtxs" },
    { "sendrawtransaction", 1, "maxburnamount" },
    { "testrelaypoolaccept", 0, "rawtxs" },
    { "submitpackage", 0, "package" },
    { "submitpackage", 1, "maxburnamount" },
    { "combinerawtransaction", 0, "txs" },
    { "fundrawtransaction", 1, "options" },
    { "fundrawtransaction", 1, "add_inputs"},
    { "fundrawtransaction", 1, "include_unsafe"},
    { "fundrawtransaction", 1, "minconf"},
    { "fundrawtransaction", 1, "maxconf"},
    { "fundrawtransaction", 1, "change_position"},
    { "fundrawtransaction", 1, "lock_unspents"},
    { "fundrawtransaction", 1, "input_weights"},
    { "fundrawtransaction", 1, "solving_data"},
    { "fundrawtransaction", 1, "max_tx_weight"},
    { "fundrawtransaction", 2, "iswitness" },
    { "vaultcreatefundedpsqt", 0, "inputs" },
    { "vaultcreatefundedpsqt", 1, "outputs" },
    { "vaultcreatefundedpsqt", 2, "locktime" },
    { "vaultcreatefundedpsqt", 3, "options" },
    { "vaultcreatefundedpsqt", 3, "add_inputs"},
    { "vaultcreatefundedpsqt", 3, "include_unsafe"},
    { "vaultcreatefundedpsqt", 3, "minconf"},
    { "vaultcreatefundedpsqt", 3, "maxconf"},
    { "vaultcreatefundedpsqt", 3, "change_position"},
    { "vaultcreatefundedpsqt", 3, "lock_unspents"},
    { "vaultcreatefundedpsqt", 3, "solving_data"},
    { "vaultcreatefundedpsqt", 3, "max_tx_weight"},
    { "vaultcreatefundedpsqt", 4, "bip32derivs" },
    { "vaultprocesspsqt", 1, "sign" },
    { "vaultprocesspsqt", 3, "bip32derivs" },
    { "vaultprocesspsqt", 4, "finalize" },
    { "descriptorprocesspsqt", 1, "descriptors"},
    { "descriptorprocesspsqt", 3, "bip32derivs" },
    { "descriptorprocesspsqt", 4, "finalize" },
    { "createpsqt", 0, "inputs" },
    { "createpsqt", 1, "outputs" },
    { "createpsqt", 2, "locktime" },
    { "combinepsqt", 0, "txs"},
    { "joinpsqts", 0, "txs"},
    { "finalizepsqt", 1, "extract"},
    { "converttopsqt", 1, "permitsigdata"},
    { "converttopsqt", 2, "iswitness"},
    { "gettxout", 1, "n" },
    { "gettxout", 2, "include_relaypool" },
    { "gettxoutproof", 0, "txids" },
    { "gettxoutsetinfo", 1, "hash_or_height" },
    { "gettxoutsetinfo", 2, "use_index"},
    { "lockunspent", 0, "unlock" },
    { "lockunspent", 1, "transactions" },
    { "lockunspent", 2, "persistent" },
    { "send", 0, "outputs" },
    { "send", 1, "options" },
    { "send", 1, "add_inputs"},
    { "send", 1, "include_unsafe"},
    { "send", 1, "minconf"},
    { "send", 1, "maxconf"},
    { "send", 1, "add_to_vault"},
    { "send", 1, "change_position"},
    { "send", 1, "inputs"},
    { "send", 1, "locktime"},
    { "send", 1, "lock_unspents"},
    { "send", 1, "psqt"},
    { "send", 1, "solving_data"},
    { "send", 1, "max_tx_weight"},
    { "sendall", 0, "recipients" },
    { "sendall", 1, "options" },
    { "sendall", 1, "add_to_vault"},
    { "sendall", 1, "inputs"},
    { "sendall", 1, "locktime"},
    { "sendall", 1, "lock_unspents"},
    { "sendall", 1, "psqt"},
    { "sendall", 1, "minconf"},
    { "sendall", 1, "maxconf"},
    { "sendall", 1, "solving_data"},
    { "simulaterawtransaction", 0, "rawtxs" },
    { "importrelaypool", 1, "options" },
    { "importrelaypool", 1, "use_current_time" },
    { "importrelaypool", 1, "apply_unbroadcast_set" },
    { "importdescriptors", 0, "requests" },
    { "listdescriptors", 0, "private" },
    { "verifychain", 0, "checklevel" },
    { "verifychain", 1, "nblocks" },
    { "getblockstats", 0, "hash_or_height" },
    { "getblockstats", 1, "stats" },
    { "pruneblockchain", 0, "height" },
    { "keypoolrefill", 0, "newsize" },
    { "getrawrelaypool", 0, "verbose" },
    { "getrawrelaypool", 1, "relaypool_sequence" },
    { "getorphantxs", 0, "verbosity" },
    { "setban", 2, "bantime" },
    { "setban", 3, "absolute" },
    { "setnetworkactive", 0, "state" },
    { "setvaultflag", 1, "value" },
    { "getrelaypoolancestors", 1, "verbose" },
    { "getrelaypooldescendants", 1, "verbose" },
    { "gettxspendingprevout", 0, "outputs" },
    { "logging", 0, "include" },
    { "logging", 1, "exclude" },
    { "disconnectnode", 1, "nodeid" },
    { "gethdkeys", 0, "active_only" },
    { "gethdkeys", 0, "options" },
    { "gethdkeys", 0, "private" },
    { "createvaultdescriptor", 1, "options" },
    { "createvaultdescriptor", 1, "internal" },
    // Echo with conversion (For testing only)
    { "echojson", 0, "arg0" },
    { "echojson", 1, "arg1" },
    { "echojson", 2, "arg2" },
    { "echojson", 3, "arg3" },
    { "echojson", 4, "arg4" },
    { "echojson", 5, "arg5" },
    { "echojson", 6, "arg6" },
    { "echojson", 7, "arg7" },
    { "echojson", 8, "arg8" },
    { "echojson", 9, "arg9" },
    { "rescanblockchain", 0, "start_height"},
    { "rescanblockchain", 1, "stop_height"},
    { "createvault", 1, "disable_private_keys"},
    { "createvault", 2, "blank"},
    { "createvault", 4, "avoid_reuse"},
    { "createvault", 5, "load_on_startup"},
    { "createvault", 6, "external_signer"},
    { "restorevault", 2, "load_on_startup"},
    { "loadvault", 1, "load_on_startup"},
    { "unloadvault", 1, "load_on_startup"},
    { "getnodeaddresses", 0, "count"},
    { "addpeeraddress", 1, "port"},
    { "addpeeraddress", 2, "tried"},
    { "sendmsgtopeer", 0, "peer_id" },
    { "stop", 0, "wait" },
    { "addnode", 2, "v2transport" },
    { "addconnection", 2, "v2transport" },
};
// clang-format on

/** Parse string to UniValue or throw runtime_error if string contains invalid JSON */
static UniValue Parse(std::string_view raw)
{
    UniValue parsed;
    if (!parsed.read(raw)) throw std::runtime_error(tfm::format("Error parsing JSON: %s", raw));
    return parsed;
}

class CRPCConvertTable
{
private:
    std::set<std::pair<std::string, int>> members;
    std::set<std::pair<std::string, std::string>> membersByName;

public:
    CRPCConvertTable();

    /** Return arg_value as UniValue, and first parse it if it is a non-string parameter */
    UniValue ArgToUniValue(std::string_view arg_value, const std::string& method, int param_idx)
    {
        return members.count({method, param_idx}) > 0 ? Parse(arg_value) : arg_value;
    }

    /** Return arg_value as UniValue, and first parse it if it is a non-string parameter */
    UniValue ArgToUniValue(std::string_view arg_value, const std::string& method, const std::string& param_name)
    {
        return membersByName.count({method, param_name}) > 0 ? Parse(arg_value) : arg_value;
    }
};

CRPCConvertTable::CRPCConvertTable()
{
    for (const auto& cp : vRPCConvertParams) {
        members.emplace(cp.methodName, cp.paramIdx);
        membersByName.emplace(cp.methodName, cp.paramName);
    }
}

static CRPCConvertTable rpcCvtTable;

UniValue RPCConvertValues(const std::string &strMethod, const std::vector<std::string> &strParams)
{
    UniValue params(UniValue::VARR);

    for (unsigned int idx = 0; idx < strParams.size(); idx++) {
        std::string_view value{strParams[idx]};
        params.push_back(rpcCvtTable.ArgToUniValue(value, strMethod, idx));
    }

    return params;
}

UniValue RPCConvertNamedValues(const std::string &strMethod, const std::vector<std::string> &strParams)
{
    UniValue params(UniValue::VOBJ);
    UniValue positional_args{UniValue::VARR};

    for (std::string_view s: strParams) {
        size_t pos = s.find('=');
        if (pos == std::string::npos) {
            positional_args.push_back(rpcCvtTable.ArgToUniValue(s, strMethod, positional_args.size()));
            continue;
        }

        std::string name{s.substr(0, pos)};
        std::string_view value{s.substr(pos+1)};

        // Intentionally overwrite earlier named values with later ones as a
        // convenience for scripts and command line users that want to merge
        // options.
        params.pushKV(name, rpcCvtTable.ArgToUniValue(value, strMethod, name));
    }

    if (!positional_args.empty()) {
        // Use pushKVEnd instead of pushKV to avoid overwriting an explicit
        // "args" value with an implicit one. Let the RPC server handle the
        // request as given.
        params.pushKVEnd("args", std::move(positional_args));
    }

    return params;
}
