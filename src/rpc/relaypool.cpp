// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpc/blockchain.h>

#include <node/relaypool_persist.h>

#include <chainparams.h>
#include <consensus/validation.h>
#include <core_io.h>
#include <kernel/relaypool_entry.h>
#include <net_processing.h>
#include <node/relaypool_persist_args.h>
#include <node/types.h>
#include <policy/settings.h>
#include <primitives/transaction.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <txrelaypool.h>
#include <univalue.h>
#include <util/fs.h>
#include <util/moneystr.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <util/vector.h>

#include <utility>

using node::DumpRelayPool;

using node::DEFAULT_MAX_BURN_AMOUNT;
using node::RelayPoolPath;
using node::NodeContext;
using node::TransactionError;
using util::ToString;

static RPCHelpMan sendrawtransaction()
{
    return RPCHelpMan{"sendrawtransaction",
        "\nSubmit a raw transaction (serialized, hex-encoded) to local node and network.\n"
        "\nThe transaction will be sent unconditionally to all peers, so using sendrawtransaction\n"
        "for manual rebroadcast may degrade privacy by leaking the transaction's origin, as\n"
        "nodes will normally not rebroadcast non-vault transactions already in their relay pool.\n"
        "\nA specific exception, RPC_VERIFY_ALREADY_IN_UTXO_SET, may be returned if the transaction cannot be added to the relay pool.\n"
        "\nRelated RPCs: createrawtransaction, signrawtransactionwithkey\n",
        {
            {"hexstring", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The hex string of the raw transaction"},
            {"maxburnamount", RPCArg::Type::AMOUNT, RPCArg::Default{FormatMoney(DEFAULT_MAX_BURN_AMOUNT)},
             "Reject transactions with provably unspendable outputs (e.g. 'datacarrier' outputs that use the OP_RETURN opcode) greater than the specified value, expressed in " + CURRENCY_UNIT + ".\n"
             "If burning funds through unspendable outputs is desired, increase this value.\n"
             "This check is based on heuristics and does not guarantee spendability of outputs.\n"},
        },
        RPCResult{
            RPCResult::Type::STR_HEX, "", "The transaction hash in hex"
        },
        RPCExamples{
            "\nCreate a transaction\n"
            + HelpExampleCli("createrawtransaction", "\"[{\\\"txid\\\" : \\\"mytxid\\\",\\\"vout\\\":0}]\" \"[{\\\"myaddress\\\":0.01}]\"") +
            "Sign the transaction, and get back the hex\n"
            + HelpExampleCli("signrawtransactionwithvault", "\"myhex\"") +
            "\nSend the transaction (signed hex)\n"
            + HelpExampleCli("sendrawtransaction", "\"signedhex\"") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("sendrawtransaction", "\"signedhex\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            const CAmount max_burn_amount = request.params[1].isNull() ? 0 : AmountFromValue(request.params[1]);

            CMutableTransaction mtx;
            if (!DecodeHexTx(mtx, request.params[0].get_str())) {
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed. Make sure the tx has at least one input.");
            }

            for (const auto& out : mtx.vout) {
                if((out.scriptPubKey.IsUnspendable() || !out.scriptPubKey.HasValidOps()) && out.nValue > max_burn_amount) {
                    throw JSONRPCTransactionError(TransactionError::MAX_BURN_EXCEEDED);
                }
            }

            CTransactionRef tx(MakeTransactionRef(std::move(mtx)));

            std::string err_string;
            AssertLockNotHeld(cs_main);
            NodeContext& node = EnsureAnyNodeContext(request.context);
            const TransactionError err = BroadcastTransaction(node, tx, err_string, /*relay=*/true, /*wait_callback=*/true);
            if (TransactionError::OK != err) {
                throw JSONRPCTransactionError(err, err_string);
            }

            return tx->GetHash().GetHex();
        },
    };
}

static RPCHelpMan testrelaypoolaccept()
{
    return RPCHelpMan{"testrelaypoolaccept",
        "\nReturns result of relay pool acceptance tests indicating if raw transaction(s) (serialized, hex-encoded) would be accepted by the relay pool.\n"
        "\nIf multiple transactions are passed in, parents must come before children and package policies apply: the transactions cannot conflict with any relay pool transactions or each other.\n"
        "\nIf one transaction fails, other transactions may not be fully validated (the 'allowed' key will be blank).\n"
        "\nThe maximum number of transactions allowed is " + ToString(MAX_PACKAGE_COUNT) + ".\n"
        "\nThis checks if transactions violate the consensus or policy rules.\n"
        "\nSee sendrawtransaction call.\n",
        {
            {"rawtxs", RPCArg::Type::ARR, RPCArg::Optional::NO, "An array of hex strings of raw transactions.",
                {
                    {"rawtx", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, ""},
                },
            },
        },
        RPCResult{
            RPCResult::Type::ARR, "", "The result of the relay pool acceptance test for each raw transaction in the input array.\n"
                                      "Returns results for each transaction in the same order they were passed in.\n"
                                      "Transactions that cannot be fully validated due to failures in other transactions will not contain an 'allowed' result.\n",
            {
                {RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::STR_HEX, "txid", "The transaction hash in hex"},
                    {RPCResult::Type::STR_HEX, "wtxid", "The transaction witness hash in hex"},
                    {RPCResult::Type::STR, "package-error", /*optional=*/true, "Package validation error, if any (only possible if rawtxs had more than 1 transaction)."},
                    {RPCResult::Type::BOOL, "allowed", /*optional=*/true, "Whether this tx would be accepted to the relay pool. "
                                                       "If not present, the tx was not fully validated due to a failure in another tx in the list."},
                    {RPCResult::Type::NUM, "vsize", /*optional=*/true, "Virtual transaction size as defined in BIP 141. This is different from actual serialized size for witness transactions as witness data is discounted (only present when 'allowed' is true)"},
                    {RPCResult::Type::STR, "reject-reason", /*optional=*/true, "Rejection reason (only present when 'allowed' is false)"},
                    {RPCResult::Type::STR, "reject-details", /*optional=*/true, "Rejection details (only present when 'allowed' is false and rejection details exist)"},
                }},
            }
        },
        RPCExamples{
            "\nCreate a transaction\n"
            + HelpExampleCli("createrawtransaction", "\"[{\\\"txid\\\" : \\\"mytxid\\\",\\\"vout\\\":0}]\" \"[{\\\"myaddress\\\":0.01}]\"") +
            "Sign the transaction, and get back the hex\n"
            + HelpExampleCli("signrawtransactionwithvault", "\"myhex\"") +
            "\nTest acceptance of the transaction (signed hex)\n"
            + HelpExampleCli("testrelaypoolaccept", R"('["signedhex"]')") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("testrelaypoolaccept", "[\"signedhex\"]")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            const UniValue raw_transactions = request.params[0].get_array();
            if (raw_transactions.size() < 1 || raw_transactions.size() > MAX_PACKAGE_COUNT) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "Array must contain between 1 and " + ToString(MAX_PACKAGE_COUNT) + " transactions.");
            }

            std::vector<CTransactionRef> txns;
            txns.reserve(raw_transactions.size());
            for (const auto& rawtx : raw_transactions.getValues()) {
                CMutableTransaction mtx;
                if (!DecodeHexTx(mtx, rawtx.get_str())) {
                    throw JSONRPCError(RPC_DESERIALIZATION_ERROR,
                                       "TX decode failed: " + rawtx.get_str() + " Make sure the tx has at least one input.");
                }
                txns.emplace_back(MakeTransactionRef(std::move(mtx)));
            }

            NodeContext& node = EnsureAnyNodeContext(request.context);
            CTxRelayPool& relaypool = EnsureRelayPool(node);
            ChainstateManager& chainman = EnsureChainman(node);
            Chainstate& chainstate = chainman.ActiveChainstate();
            const PackageRelayPoolAcceptResult package_result = [&] {
                LOCK(::cs_main);
                if (txns.size() > 1) return ProcessNewPackage(chainstate, relaypool, txns, /*test_accept=*/true);
                return PackageRelayPoolAcceptResult(txns[0]->GetWitnessHash(),
                                                  chainman.ProcessTransaction(txns[0], /*test_accept=*/true));
            }();

            UniValue rpc_result(UniValue::VARR);
            for (const auto& tx : txns) {
                UniValue result_inner(UniValue::VOBJ);
                result_inner.pushKV("txid", tx->GetHash().GetHex());
                result_inner.pushKV("wtxid", tx->GetWitnessHash().GetHex());
                if (package_result.m_state.GetResult() == PackageValidationResult::PCKG_POLICY) {
                    result_inner.pushKV("package-error", package_result.m_state.ToString());
                }
                auto it = package_result.m_tx_results.find(tx->GetWitnessHash());
                if (it == package_result.m_tx_results.end()) {
                    // Validation unfinished. Just return the txid and wtxid.
                    rpc_result.push_back(std::move(result_inner));
                    continue;
                }
                const auto& tx_result = it->second;
                // Package testrelaypoolaccept doesn't allow transactions to already be in the relaypool.
                CHECK_NONFATAL(tx_result.m_result_type != RelayPoolAcceptResult::ResultType::RELAYPOOL_ENTRY);
                if (tx_result.m_result_type == RelayPoolAcceptResult::ResultType::VALID) {
                    const int64_t virtual_size = tx_result.m_vsize.value();
                    result_inner.pushKV("allowed", true);
                    result_inner.pushKV("vsize", virtual_size);
                } else {
                    result_inner.pushKV("allowed", false);
                    const TxValidationState state = tx_result.m_state;
                    if (state.GetResult() == TxValidationResult::TX_MISSING_INPUTS) {
                        result_inner.pushKV("reject-reason", "missing-inputs");
                    } else {
                        result_inner.pushKV("reject-reason", state.GetRejectReason());
                        result_inner.pushKV("reject-details", state.ToString());
                    }
                }
                rpc_result.push_back(std::move(result_inner));
            }
            return rpc_result;
        },
    };
}

static std::vector<RPCResult> RelayPoolEntryDescription()
{
    return {
        RPCResult{RPCResult::Type::NUM, "vsize", "virtual transaction size as defined in BIP 141. This is different from actual serialized size for witness transactions as witness data is discounted."},
        RPCResult{RPCResult::Type::NUM, "weight", "transaction weight as defined in BIP 141."},
        RPCResult{RPCResult::Type::STR, "txwork_surplus", "Quicksilver: surplus per-tx PoW work (actual - floor) as hex arith_uint256, the inclusion/replacement ranking key (#6)"},
        RPCResult{RPCResult::Type::NUM, "txwork_rate", "Quicksilver: surplus per-tx PoW work per vsize byte (surplus/vsize); higher = included/preferred first (#6)"},
        RPCResult{RPCResult::Type::NUM_TIME, "time", "local time transaction entered pool in seconds since 1 Jan 1970 GMT"},
        RPCResult{RPCResult::Type::NUM, "height", "block height when transaction entered pool"},
        RPCResult{RPCResult::Type::NUM, "descendantcount", "number of in-relay-pool descendant transactions (including this one)"},
        RPCResult{RPCResult::Type::NUM, "descendantsize", "virtual transaction size of in-relay-pool descendants (including this one)"},
        RPCResult{RPCResult::Type::NUM, "ancestorcount", "number of in-relay-pool ancestor transactions (including this one)"},
        RPCResult{RPCResult::Type::NUM, "ancestorsize", "virtual transaction size of in-relay-pool ancestors (including this one)"},
        RPCResult{RPCResult::Type::STR_HEX, "wtxid", "hash of serialized transaction, including witness data"},
        RPCResult{RPCResult::Type::ARR, "depends", "unconfirmed transactions used as inputs for this transaction",
            {RPCResult{RPCResult::Type::STR_HEX, "transactionid", "parent transaction id"}}},
        RPCResult{RPCResult::Type::ARR, "spentby", "unconfirmed transactions spending outputs from this transaction",
            {RPCResult{RPCResult::Type::STR_HEX, "transactionid", "child transaction id"}}},
        RPCResult{RPCResult::Type::BOOL, "unbroadcast", "Whether this transaction is currently unbroadcast (initial broadcast not yet acknowledged by any peers)"},
    };
}

static void entryToJSON(const CTxRelayPool& pool, UniValue& info, const CTxRelayPoolEntry& e) EXCLUSIVE_LOCKS_REQUIRED(pool.cs)
{
    AssertLockHeld(pool.cs);

    info.pushKV("vsize", (int)e.GetTxSize());
    info.pushKV("weight", (int)e.GetTxWeight());
    // #6: surplus per-tx PoW work (actual - floor) and its rate (surplus/vsize) — the
    // 5c-2 inclusion/replacement ranking key. The floor is chainstate-derived and already
    // baked into the stored entry, so these are read directly (no anchor lookup here).
    info.pushKV("txwork_surplus", e.GetTxWorkSurplusValue().ToString());
    info.pushKV("txwork_rate", e.GetTxWorkRate());
    info.pushKV("time", count_seconds(e.GetTime()));
    info.pushKV("height", (int)e.GetHeight());
    info.pushKV("descendantcount", e.GetCountWithDescendants());
    info.pushKV("descendantsize", e.GetSizeWithDescendants());
    info.pushKV("ancestorcount", e.GetCountWithAncestors());
    info.pushKV("ancestorsize", e.GetSizeWithAncestors());
    info.pushKV("wtxid", e.GetTx().GetWitnessHash().ToString());

    const CTransaction& tx = e.GetTx();
    std::set<std::string> setDepends;
    for (const CTxIn& txin : tx.vin)
    {
        if (pool.exists(GenTxid::Txid(txin.prevout.hash)))
            setDepends.insert(txin.prevout.hash.ToString());
    }

    UniValue depends(UniValue::VARR);
    for (const std::string& dep : setDepends)
    {
        depends.push_back(dep);
    }

    info.pushKV("depends", std::move(depends));

    UniValue spent(UniValue::VARR);
    for (const CTxRelayPoolEntry& child : e.GetRelayPoolChildrenConst()) {
        spent.push_back(child.GetTx().GetHash().ToString());
    }

    info.pushKV("spentby", std::move(spent));

    info.pushKV("unbroadcast", pool.IsUnbroadcastTx(tx.GetHash()));
}

UniValue RelayPoolToJSON(const CTxRelayPool& pool, bool verbose, bool include_relaypool_sequence)
{
    if (verbose) {
        if (include_relaypool_sequence) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Verbose results cannot contain relay pool sequence values.");
        }
        LOCK(pool.cs);
        UniValue o(UniValue::VOBJ);
        for (const CTxRelayPoolEntry& e : pool.entryAll()) {
            UniValue info(UniValue::VOBJ);
            entryToJSON(pool, info, e);
            // RelayPool has unique entries so there is no advantage in using
            // UniValue::pushKV, which checks if the key already exists in O(N).
            // UniValue::pushKVEnd is used instead which currently is O(1).
            o.pushKVEnd(e.GetTx().GetHash().ToString(), std::move(info));
        }
        return o;
    } else {
        UniValue a(UniValue::VARR);
        uint64_t relaypool_sequence;
        {
            LOCK(pool.cs);
            for (const CTxRelayPoolEntry& e : pool.entryAll()) {
                a.push_back(e.GetTx().GetHash().ToString());
            }
            relaypool_sequence = pool.GetSequence();
        }
        if (!include_relaypool_sequence) {
            return a;
        } else {
            UniValue o(UniValue::VOBJ);
            o.pushKV("txids", std::move(a));
            o.pushKV("relaypool_sequence", relaypool_sequence);
            return o;
        }
    }
}

static RPCHelpMan getrawrelaypool()
{
    return RPCHelpMan{"getrawrelaypool",
        "\nReturns all transaction ids in the relay pool as a json array of string transaction ids.\n"
        "\nHint: use getrelaypoolentry to fetch a specific transaction from the relay pool.\n",
        {
            {"verbose", RPCArg::Type::BOOL, RPCArg::Default{false}, "True for a json object, false for array of transaction ids"},
            {"relaypool_sequence", RPCArg::Type::BOOL, RPCArg::Default{false}, "If verbose=false, returns a json object with transaction list and relay pool sequence number attached."},
        },
        {
            RPCResult{"for verbose = false",
                RPCResult::Type::ARR, "", "",
                {
                    {RPCResult::Type::STR_HEX, "", "The transaction id"},
                }},
            RPCResult{"for verbose = true",
                RPCResult::Type::OBJ_DYN, "", "",
                {
                    {RPCResult::Type::OBJ, "transactionid", "", RelayPoolEntryDescription()},
                }},
            RPCResult{"for verbose = false and relaypool_sequence = true",
                RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::ARR, "txids", "",
                    {
                        {RPCResult::Type::STR_HEX, "", "The transaction id"},
                    }},
                    {RPCResult::Type::NUM, "relaypool_sequence", "The relay pool sequence value."},
                }},
        },
        RPCExamples{
            HelpExampleCli("getrawrelaypool", "true")
            + HelpExampleRpc("getrawrelaypool", "true")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    bool fVerbose = false;
    if (!request.params[0].isNull())
        fVerbose = request.params[0].get_bool();

    bool include_relaypool_sequence = false;
    if (!request.params[1].isNull()) {
        include_relaypool_sequence = request.params[1].get_bool();
    }

    return RelayPoolToJSON(EnsureAnyRelayPool(request.context), fVerbose, include_relaypool_sequence);
},
    };
}

static RPCHelpMan getrelaypoolancestors()
{
    return RPCHelpMan{"getrelaypoolancestors",
        "\nIf txid is in the relay pool, returns all in-relay-pool ancestors.\n",
        {
            {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id (must be in the relay pool)"},
            {"verbose", RPCArg::Type::BOOL, RPCArg::Default{false}, "True for a json object, false for array of transaction ids"},
        },
        {
            RPCResult{"for verbose = false",
                RPCResult::Type::ARR, "", "",
                {{RPCResult::Type::STR_HEX, "", "The transaction id of an in-relay-pool ancestor transaction"}}},
            RPCResult{"for verbose = true",
                RPCResult::Type::OBJ_DYN, "", "",
                {
                    {RPCResult::Type::OBJ, "transactionid", "", RelayPoolEntryDescription()},
                }},
        },
        RPCExamples{
            HelpExampleCli("getrelaypoolancestors", "\"mytxid\"")
            + HelpExampleRpc("getrelaypoolancestors", "\"mytxid\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    bool fVerbose = false;
    if (!request.params[1].isNull())
        fVerbose = request.params[1].get_bool();

    uint256 hash = ParseHashV(request.params[0], "parameter 1");

    const CTxRelayPool& relaypool = EnsureAnyRelayPool(request.context);
    LOCK(relaypool.cs);

    const auto entry{relaypool.GetEntry(Txid::FromUint256(hash))};
    if (entry == nullptr) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction not in the relay pool");
    }

    auto ancestors{relaypool.AssumeCalculateRelayPoolAncestors(self.m_name, *entry, CTxRelayPool::Limits::NoLimits(), /*fSearchForParents=*/false)};

    if (!fVerbose) {
        UniValue o(UniValue::VARR);
        for (CTxRelayPool::txiter ancestorIt : ancestors) {
            o.push_back(ancestorIt->GetTx().GetHash().ToString());
        }
        return o;
    } else {
        UniValue o(UniValue::VOBJ);
        for (CTxRelayPool::txiter ancestorIt : ancestors) {
            const CTxRelayPoolEntry &e = *ancestorIt;
            const uint256& _hash = e.GetTx().GetHash();
            UniValue info(UniValue::VOBJ);
            entryToJSON(relaypool, info, e);
            o.pushKV(_hash.ToString(), std::move(info));
        }
        return o;
    }
},
    };
}

static RPCHelpMan getrelaypooldescendants()
{
    return RPCHelpMan{"getrelaypooldescendants",
        "\nIf txid is in the relay pool, returns all in-relay-pool descendants.\n",
        {
            {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id (must be in the relay pool)"},
            {"verbose", RPCArg::Type::BOOL, RPCArg::Default{false}, "True for a json object, false for array of transaction ids"},
        },
        {
            RPCResult{"for verbose = false",
                RPCResult::Type::ARR, "", "",
                {{RPCResult::Type::STR_HEX, "", "The transaction id of an in-relay-pool descendant transaction"}}},
            RPCResult{"for verbose = true",
                RPCResult::Type::OBJ_DYN, "", "",
                {
                    {RPCResult::Type::OBJ, "transactionid", "", RelayPoolEntryDescription()},
                }},
        },
        RPCExamples{
            HelpExampleCli("getrelaypooldescendants", "\"mytxid\"")
            + HelpExampleRpc("getrelaypooldescendants", "\"mytxid\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    bool fVerbose = false;
    if (!request.params[1].isNull())
        fVerbose = request.params[1].get_bool();

    uint256 hash = ParseHashV(request.params[0], "parameter 1");

    const CTxRelayPool& relaypool = EnsureAnyRelayPool(request.context);
    LOCK(relaypool.cs);

    const auto it{relaypool.GetIter(hash)};
    if (!it) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction not in the relay pool");
    }

    CTxRelayPool::setEntries setDescendants;
    relaypool.CalculateDescendants(*it, setDescendants);
    // CTxRelayPool::CalculateDescendants will include the given tx
    setDescendants.erase(*it);

    if (!fVerbose) {
        UniValue o(UniValue::VARR);
        for (CTxRelayPool::txiter descendantIt : setDescendants) {
            o.push_back(descendantIt->GetTx().GetHash().ToString());
        }

        return o;
    } else {
        UniValue o(UniValue::VOBJ);
        for (CTxRelayPool::txiter descendantIt : setDescendants) {
            const CTxRelayPoolEntry &e = *descendantIt;
            const uint256& _hash = e.GetTx().GetHash();
            UniValue info(UniValue::VOBJ);
            entryToJSON(relaypool, info, e);
            o.pushKV(_hash.ToString(), std::move(info));
        }
        return o;
    }
},
    };
}

static RPCHelpMan getrelaypoolentry()
{
    return RPCHelpMan{"getrelaypoolentry",
        "\nReturns relay pool data for given transaction\n",
        {
            {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id (must be in the relay pool)"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "", RelayPoolEntryDescription()},
        RPCExamples{
            HelpExampleCli("getrelaypoolentry", "\"mytxid\"")
            + HelpExampleRpc("getrelaypoolentry", "\"mytxid\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    uint256 hash = ParseHashV(request.params[0], "parameter 1");

    const CTxRelayPool& relaypool = EnsureAnyRelayPool(request.context);
    LOCK(relaypool.cs);

    const auto entry{relaypool.GetEntry(Txid::FromUint256(hash))};
    if (entry == nullptr) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction not in the relay pool");
    }

    UniValue info(UniValue::VOBJ);
    entryToJSON(relaypool, info, *entry);
    return info;
},
    };
}

static RPCHelpMan gettxspendingprevout()
{
    return RPCHelpMan{"gettxspendingprevout",
        "Scans the relay pool to find transactions spending any of the given outputs",
        {
            {"outputs", RPCArg::Type::ARR, RPCArg::Optional::NO, "The transaction outputs that we want to check, and within each, the txid (string) vout (numeric).",
                {
                    {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                        {
                            {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                            {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output number"},
                        },
                    },
                },
            },
        },
        RPCResult{
            RPCResult::Type::ARR, "", "",
            {
                {RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::STR_HEX, "txid", "the transaction id of the checked output"},
                    {RPCResult::Type::NUM, "vout", "the vout value of the checked output"},
                    {RPCResult::Type::STR_HEX, "spendingtxid", /*optional=*/true, "the transaction id of the relay pool transaction spending this output (omitted if unspent)"},
                }},
            }
        },
        RPCExamples{
            HelpExampleCli("gettxspendingprevout", "\"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":3}]\"")
            + HelpExampleRpc("gettxspendingprevout", "\"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":3}]\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            const UniValue& output_params = request.params[0].get_array();
            if (output_params.empty()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, outputs are missing");
            }

            std::vector<COutPoint> prevouts;
            prevouts.reserve(output_params.size());

            for (unsigned int idx = 0; idx < output_params.size(); idx++) {
                const UniValue& o = output_params[idx].get_obj();

                RPCTypeCheckObj(o,
                                {
                                    {"txid", UniValueType(UniValue::VSTR)},
                                    {"vout", UniValueType(UniValue::VNUM)},
                                }, /*fAllowNull=*/false, /*fStrict=*/true);

                const Txid txid = Txid::FromUint256(ParseHashO(o, "txid"));
                const int nOutput{o.find_value("vout").getInt<int>()};
                if (nOutput < 0) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, vout cannot be negative");
                }

                prevouts.emplace_back(txid, nOutput);
            }

            const CTxRelayPool& relaypool = EnsureAnyRelayPool(request.context);
            LOCK(relaypool.cs);

            UniValue result{UniValue::VARR};

            for (const COutPoint& prevout : prevouts) {
                UniValue o(UniValue::VOBJ);
                o.pushKV("txid", prevout.hash.ToString());
                o.pushKV("vout", (uint64_t)prevout.n);

                const CTransaction* spendingTx = relaypool.GetConflictTx(prevout);
                if (spendingTx != nullptr) {
                    o.pushKV("spendingtxid", spendingTx->GetHash().ToString());
                }

                result.push_back(std::move(o));
            }

            return result;
        },
    };
}

UniValue RelayPoolInfoToJSON(const CTxRelayPool& pool)
{
    // Make sure this call is atomic in the pool.
    LOCK(pool.cs);
    UniValue ret(UniValue::VOBJ);
    ret.pushKV("loaded", pool.GetLoadTried());
    ret.pushKV("size", (int64_t)pool.size());
    ret.pushKV("bytes", (int64_t)pool.GetTotalTxSize());
    ret.pushKV("usage", (int64_t)pool.DynamicMemoryUsage());
    ret.pushKV("maxrelaypool", pool.m_opts.max_size_bytes);
    ret.pushKV("unbroadcastcount", uint64_t{pool.GetUnbroadcastTxs().size()});
    return ret;
}

static RPCHelpMan getrelaypoolinfo()
{
    return RPCHelpMan{"getrelaypoolinfo",
        "Returns details on the active state of the relay pool.",
        {},
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::BOOL, "loaded", "True if the initial load attempt of the persisted relay pool finished"},
                {RPCResult::Type::NUM, "size", "Current tx count"},
                {RPCResult::Type::NUM, "bytes", "Sum of all virtual transaction sizes as defined in BIP 141. Differs from actual serialized size because witness data is discounted"},
                {RPCResult::Type::NUM, "usage", "Total memory usage for the relay pool"},
                {RPCResult::Type::NUM, "maxrelaypool", "Maximum memory usage for the relay pool"},
                {RPCResult::Type::NUM, "unbroadcastcount", "Current number of transactions that haven't passed initial broadcast yet"},
            }},
        RPCExamples{
            HelpExampleCli("getrelaypoolinfo", "")
            + HelpExampleRpc("getrelaypoolinfo", "")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    return RelayPoolInfoToJSON(EnsureAnyRelayPool(request.context));
},
    };
}

static RPCHelpMan importrelaypool()
{
    return RPCHelpMan{
        "importrelaypool",
        "Import a relaypool.dat file and attempt to add its contents to the relay pool.\n"
        "Warning: Importing untrusted files is dangerous, especially if metadata from the file is taken over.",
        {
            {"filepath", RPCArg::Type::STR, RPCArg::Optional::NO, "The relay pool file"},
            {"options",
             RPCArg::Type::OBJ_NAMED_PARAMS,
             RPCArg::Optional::OMITTED,
             "",
             {
                 {"use_current_time", RPCArg::Type::BOOL, RPCArg::Default{true},
                  "Whether to use the current system time or use the entry time metadata from the relay pool file.\n"
                  "Warning: Importing untrusted metadata may lead to unexpected issues and undesirable behavior."},
                 {"apply_unbroadcast_set", RPCArg::Type::BOOL, RPCArg::Default{false},
                  "Whether to apply the unbroadcast set metadata from the relay pool file.\n"
                  "Warning: Importing untrusted metadata may lead to unexpected issues and undesirable behavior."},
             },
             RPCArgOptions{.oneline_description = "options"}},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", std::vector<RPCResult>{}},
        RPCExamples{HelpExampleCli("importrelaypool", "/path/to/relaypool.dat") + HelpExampleRpc("importrelaypool", "/path/to/relaypool.dat")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            const NodeContext& node{EnsureAnyNodeContext(request.context)};

            CTxRelayPool& relaypool{EnsureRelayPool(node)};
            ChainstateManager& chainman = EnsureChainman(node);
            Chainstate& chainstate = chainman.ActiveChainstate();

            if (chainman.IsInitialBlockDownload()) {
                throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "Can only import the relay pool after the block download and sync is done.");
            }

            const fs::path load_path{fs::u8path(request.params[0].get_str())};
            const UniValue& use_current_time{request.params[1]["use_current_time"]};
            const UniValue& apply_unbroadcast{request.params[1]["apply_unbroadcast_set"]};
            node::ImportRelayPoolOptions opts{
                .use_current_time = use_current_time.isNull() ? true : use_current_time.get_bool(),
                .apply_unbroadcast_set = apply_unbroadcast.isNull() ? false : apply_unbroadcast.get_bool(),
            };

            if (!node::LoadRelayPool(relaypool, load_path, chainstate, std::move(opts))) {
                throw JSONRPCError(RPC_MISC_ERROR, "Unable to import relay pool file, see debug.log for details.");
            }

            UniValue ret{UniValue::VOBJ};
            return ret;
        },
    };
}

static RPCHelpMan saverelaypool()
{
    return RPCHelpMan{"saverelaypool",
        "\nDumps the relay pool to disk. It will fail until the previous dump is fully loaded.\n",
        {},
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "filename", "the directory and file where the relay pool was saved"},
            }},
        RPCExamples{
            HelpExampleCli("saverelaypool", "")
            + HelpExampleRpc("saverelaypool", "")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const ArgsManager& args{EnsureAnyArgsman(request.context)};
    const CTxRelayPool& relaypool = EnsureAnyRelayPool(request.context);

    if (!relaypool.GetLoadTried()) {
        throw JSONRPCError(RPC_MISC_ERROR, "The relay pool was not loaded yet");
    }

    const fs::path& dump_path = RelayPoolPath(args);

    if (!DumpRelayPool(relaypool, dump_path)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Unable to dump relay pool to disk");
    }

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("filename", dump_path.utf8string());

    return ret;
},
    };
}

static std::vector<RPCResult> OrphanDescription()
{
    return {
        RPCResult{RPCResult::Type::STR_HEX, "txid", "The transaction hash in hex"},
        RPCResult{RPCResult::Type::STR_HEX, "wtxid", "The transaction witness hash in hex"},
        RPCResult{RPCResult::Type::NUM, "bytes", "The serialized transaction size in bytes"},
        RPCResult{RPCResult::Type::NUM, "vsize", "The virtual transaction size as defined in BIP 141. This is different from actual serialized size for witness transactions as witness data is discounted."},
        RPCResult{RPCResult::Type::NUM, "weight", "The transaction weight as defined in BIP 141."},
        RPCResult{RPCResult::Type::NUM_TIME, "entry", "The entry time into the orphanage expressed in " + UNIX_EPOCH_TIME},
        RPCResult{RPCResult::Type::NUM_TIME, "expiration", "The orphan expiration time expressed in " + UNIX_EPOCH_TIME},
        RPCResult{RPCResult::Type::ARR, "from", "",
        {
            RPCResult{RPCResult::Type::NUM, "peer_id", "Peer ID"},
        }},
    };
}

static UniValue OrphanToJSON(const TxOrphanage::OrphanTxBase& orphan)
{
    UniValue o(UniValue::VOBJ);
    o.pushKV("txid", orphan.tx->GetHash().ToString());
    o.pushKV("wtxid", orphan.tx->GetWitnessHash().ToString());
    o.pushKV("bytes", orphan.tx->GetTotalSize());
    o.pushKV("vsize", GetVirtualTransactionSize(*orphan.tx));
    o.pushKV("weight", GetTransactionWeight(*orphan.tx));
    o.pushKV("entry", int64_t{TicksSinceEpoch<std::chrono::seconds>(orphan.nTimeExpire - ORPHAN_TX_EXPIRE_TIME)});
    o.pushKV("expiration", int64_t{TicksSinceEpoch<std::chrono::seconds>(orphan.nTimeExpire)});
    UniValue from(UniValue::VARR);
    for (const auto fromPeer: orphan.announcers) {
        from.push_back(fromPeer);
    }
    o.pushKV("from", from);
    return o;
}

static RPCHelpMan getorphantxs()
{
    return RPCHelpMan{"getorphantxs",
        "\nShows transactions in the tx orphanage.\n"
        "\nEXPERIMENTAL warning: this call may be changed in future releases.\n",
        {
            {"verbosity", RPCArg::Type::NUM, RPCArg::Default{0}, "0 for an array of txids (may contain duplicates), 1 for an array of objects with tx details, and 2 for details from (1) and tx hex"},
        },
        {
            RPCResult{"for verbosity = 0",
                RPCResult::Type::ARR, "", "",
                {
                    {RPCResult::Type::STR_HEX, "txid", "The transaction hash in hex"},
                }},
            RPCResult{"for verbosity = 1",
                RPCResult::Type::ARR, "", "",
                {
                    {RPCResult::Type::OBJ, "", "", OrphanDescription()},
                }},
            RPCResult{"for verbosity = 2",
                RPCResult::Type::ARR, "", "",
                {
                    {RPCResult::Type::OBJ, "", "",
                        Cat<std::vector<RPCResult>>(
                            OrphanDescription(),
                            {{RPCResult::Type::STR_HEX, "hex", "The serialized, hex-encoded transaction data"}}
                        )
                    },
                }},
        },
        RPCExamples{
            HelpExampleCli("getorphantxs", "2")
            + HelpExampleRpc("getorphantxs", "2")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            const NodeContext& node = EnsureAnyNodeContext(request.context);
            PeerManager& peerman = EnsurePeerman(node);
            std::vector<TxOrphanage::OrphanTxBase> orphanage = peerman.GetOrphanTransactions();

            int verbosity{ParseVerbosity(request.params[0], /*default_verbosity=*/0)};

            UniValue ret(UniValue::VARR);

            if (verbosity == 0) {
                for (auto const& orphan : orphanage) {
                    ret.push_back(orphan.tx->GetHash().ToString());
                }
            } else if (verbosity == 1) {
                for (auto const& orphan : orphanage) {
                    ret.push_back(OrphanToJSON(orphan));
                }
            } else if (verbosity == 2) {
                for (auto const& orphan : orphanage) {
                    UniValue o{OrphanToJSON(orphan)};
                    o.pushKV("hex", EncodeHexTx(*orphan.tx));
                    ret.push_back(o);
                }
            } else {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid verbosity value " + ToString(verbosity));
            }

            return ret;
        },
    };
}

static RPCHelpMan submitpackage()
{
    return RPCHelpMan{"submitpackage",
        "Submit a package of raw transactions (serialized, hex-encoded) to local node.\n"
        "The package will be validated according to consensus and relay pool policy rules. If any transaction passes, it will be accepted to the relay pool.\n"
        "This RPC is experimental and the interface may be unstable. Refer to doc/policy/packages.md for documentation on package policies.\n"
        "Warning: successful submission does not mean the transactions will propagate throughout the network.\n"
        ,
        {
            {"package", RPCArg::Type::ARR, RPCArg::Optional::NO, "An array of raw transactions.\n"
                "The package must solely consist of a child transaction and all of its unconfirmed parents, if any. None of the parents may depend on each other.\n"
                "The package must be topologically sorted, with the child being the last element in the array.",
                {
                    {"rawtx", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, ""},
                },
            },
            {"maxburnamount", RPCArg::Type::AMOUNT, RPCArg::Default{FormatMoney(DEFAULT_MAX_BURN_AMOUNT)},
             "Reject transactions with provably unspendable outputs (e.g. 'datacarrier' outputs that use the OP_RETURN opcode) greater than the specified value, expressed in " + CURRENCY_UNIT + ".\n"
             "If burning funds through unspendable outputs is desired, increase this value.\n"
             "This check is based on heuristics and does not guarantee spendability of outputs.\n"
            },
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "package_msg", "The transaction package result message. \"success\" indicates all transactions were accepted into or are already in the relay pool."},
                {RPCResult::Type::OBJ_DYN, "tx-results", "transaction results keyed by wtxid",
                {
                    {RPCResult::Type::OBJ, "wtxid", "transaction wtxid", {
                        {RPCResult::Type::STR_HEX, "txid", "The transaction hash in hex"},
                        {RPCResult::Type::STR_HEX, "other-wtxid", /*optional=*/true, "The wtxid of a different transaction with the same txid but different witness found in the relay pool. This means the submitted transaction was ignored."},
                        {RPCResult::Type::NUM, "vsize", /*optional=*/true, "Sigops-adjusted virtual transaction size."},
                        {RPCResult::Type::STR, "error", /*optional=*/true, "The transaction error string, if it was rejected by the relay pool"},
                    }}
                }},
                {RPCResult::Type::ARR, "replaced-transactions", /*optional=*/true, "List of txids of replaced transactions",
                {
                    {RPCResult::Type::STR_HEX, "", "The transaction id"},
                }},
            },
        },
        RPCExamples{
            HelpExampleRpc("submitpackage", R"(["raw-parent-tx-1", "raw-parent-tx-2", "raw-child-tx"])") +
            HelpExampleCli("submitpackage", R"('["raw-tx-without-unconfirmed-parents"]')")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            const UniValue raw_transactions = request.params[0].get_array();
            if (raw_transactions.empty() || raw_transactions.size() > MAX_PACKAGE_COUNT) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "Array must contain between 1 and " + ToString(MAX_PACKAGE_COUNT) + " transactions.");
            }

            // Burn sanity check is run with no context
            const CAmount max_burn_amount = request.params[1].isNull() ? 0 : AmountFromValue(request.params[1]);

            std::vector<CTransactionRef> txns;
            txns.reserve(raw_transactions.size());
            for (const auto& rawtx : raw_transactions.getValues()) {
                CMutableTransaction mtx;
                if (!DecodeHexTx(mtx, rawtx.get_str())) {
                    throw JSONRPCError(RPC_DESERIALIZATION_ERROR,
                                       "TX decode failed: " + rawtx.get_str() + " Make sure the tx has at least one input.");
                }

                for (const auto& out : mtx.vout) {
                    if((out.scriptPubKey.IsUnspendable() || !out.scriptPubKey.HasValidOps()) && out.nValue > max_burn_amount) {
                        throw JSONRPCTransactionError(TransactionError::MAX_BURN_EXCEEDED);
                    }
                }

                txns.emplace_back(MakeTransactionRef(std::move(mtx)));
            }
            CHECK_NONFATAL(!txns.empty());
            if (txns.size() > 1 && !IsChildWithParentsTree(txns)) {
                throw JSONRPCTransactionError(TransactionError::INVALID_PACKAGE, "package topology disallowed. not child-with-parents or parents depend on each other.");
            }

            NodeContext& node = EnsureAnyNodeContext(request.context);
            CTxRelayPool& relaypool = EnsureRelayPool(node);
            Chainstate& chainstate = EnsureChainman(node).ActiveChainstate();
            const auto package_result = WITH_LOCK(::cs_main, return ProcessNewPackage(chainstate, relaypool, txns, /*test_accept=*/ false));

            std::string package_msg = "success";

            // First catch package-wide errors, continue if we can
            switch(package_result.m_state.GetResult()) {
                case PackageValidationResult::PCKG_RESULT_UNSET:
                {
                    // Belt-and-suspenders check; everything should be successful here
                    CHECK_NONFATAL(package_result.m_tx_results.size() == txns.size());
                    for (const auto& tx : txns) {
                        CHECK_NONFATAL(relaypool.exists(GenTxid::Txid(tx->GetHash())));
                    }
                    break;
                }
                case PackageValidationResult::PCKG_RELAYPOOL_ERROR:
                {
                    // This only happens with internal bug; user should stop and report
                    throw JSONRPCTransactionError(TransactionError::RELAYPOOL_ERROR,
                        package_result.m_state.GetRejectReason());
                }
                case PackageValidationResult::PCKG_POLICY:
                case PackageValidationResult::PCKG_TX:
                {
                    // Package-wide error we want to return, but we also want to return individual responses
                    package_msg = package_result.m_state.ToString();
                    CHECK_NONFATAL(package_result.m_tx_results.size() == txns.size() ||
                            package_result.m_tx_results.empty());
                    break;
                }
            }

            size_t num_broadcast{0};
            for (const auto& tx : txns) {
                // We don't want to re-submit the txn for validation in BroadcastTransaction
                if (!relaypool.exists(GenTxid::Txid(tx->GetHash()))) {
                    continue;
                }

                // We do not expect an error here; we are only broadcasting things already/still in relaypool
                std::string err_string;
                const auto err = BroadcastTransaction(node, tx, err_string, /*relay=*/true, /*wait_callback=*/true);
                if (err != TransactionError::OK) {
                    throw JSONRPCTransactionError(err,
                        strprintf("transaction broadcast failed: %s (%d transactions were broadcast successfully)",
                            err_string, num_broadcast));
                }
                num_broadcast++;
            }

            UniValue rpc_result{UniValue::VOBJ};
            rpc_result.pushKV("package_msg", package_msg);
            UniValue tx_result_map{UniValue::VOBJ};
            std::set<uint256> replaced_txids;
            for (const auto& tx : txns) {
                UniValue result_inner{UniValue::VOBJ};
                result_inner.pushKV("txid", tx->GetHash().GetHex());
                auto it = package_result.m_tx_results.find(tx->GetWitnessHash());
                if (it == package_result.m_tx_results.end()) {
                    // No results, report error and continue
                    result_inner.pushKV("error", "unevaluated");
                    continue;
                }
                switch(it->second.m_result_type) {
                case RelayPoolAcceptResult::ResultType::DIFFERENT_WITNESS:
                    result_inner.pushKV("other-wtxid", it->second.m_other_wtxid.value().GetHex());
                    break;
                case RelayPoolAcceptResult::ResultType::INVALID:
                    result_inner.pushKV("error", it->second.m_state.ToString());
                    break;
                case RelayPoolAcceptResult::ResultType::VALID:
                case RelayPoolAcceptResult::ResultType::RELAYPOOL_ENTRY:
                    result_inner.pushKV("vsize", int64_t{it->second.m_vsize.value()});
                    for (const auto& ptx : it->second.m_replaced_transactions) {
                        replaced_txids.insert(ptx->GetHash());
                    }
                    break;
                }
                tx_result_map.pushKV(tx->GetWitnessHash().GetHex(), std::move(result_inner));
            }
            rpc_result.pushKV("tx-results", std::move(tx_result_map));
            UniValue replaced_list(UniValue::VARR);
            for (const uint256& hash : replaced_txids) replaced_list.push_back(hash.ToString());
            rpc_result.pushKV("replaced-transactions", std::move(replaced_list));
            return rpc_result;
        },
    };
}

void RegisterRelayPoolRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"rawtransactions", &sendrawtransaction},
        {"rawtransactions", &testrelaypoolaccept},
        {"blockchain", &getrelaypoolancestors},
        {"blockchain", &getrelaypooldescendants},
        {"blockchain", &getrelaypoolentry},
        {"blockchain", &gettxspendingprevout},
        {"blockchain", &getrelaypoolinfo},
        {"blockchain", &getrawrelaypool},
        {"blockchain", &importrelaypool},
        {"blockchain", &saverelaypool},
        {"hidden", &getorphantxs},
        {"rawtransactions", &submitpackage},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
