// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <quicksilver-build-config.h> // IWYU pragma: keep

#include <chain.h>
#include <chainparams.h>
#include <chainparamsbase.h>
#include <common/args.h>
#include <common/system.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <core_io.h>
#include <deploymentinfo.h>
#include <deploymentstatus.h>
#include <interfaces/mining.h>
#include <key_io.h>
#include <net.h>
#include <node/block_solve.h>
#include <node/context.h>
#include <node/miner.h>
#include <node/mining_service.h>
#include <node/warnings.h>
#include <policy/policy.h>
#include <policy/settings.h>
#include <arith_uint256.h>
#include <crypto/cuckatoo/cuckatoo.h>
#include <pow.h>
#include <rpc/blockchain.h>
#include <rpc/mining.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <script/descriptor.h>
#include <script/script.h>
#include <script/signingprovider.h>
#include <txrelaypool.h>
#include <univalue.h>
#include <util/signalinterrupt.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/time.h>
#include <util/translation.h>
#include <validation.h>
#include <validationinterface.h>

#include <algorithm>
#include <memory>
#include <stdint.h>
#include <vector>

using interfaces::BlockTemplate;
using interfaces::Mining;
using node::BlockAssembler;
using node::GetMinimumTime;
using node::NodeContext;
using node::RegenerateCommitments;
using node::UpdateTime;
using util::ToString;

/**
 * Return estimated network work per second based on the last 'lookup' blocks,
 * or from the last difficulty change if 'lookup' is -1.
 * If 'height' is -1, compute the estimate from current chain tip.
 * If 'height' is a valid block height, compute the estimate at the time when a given block was found.
 */
static UniValue GetNetworkWorkPS(int lookup, int height, const CChain& active_chain) {
    if (lookup < -1 || lookup == 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid nblocks. Must be a positive number or -1.");
    }

    if (height < -1 || height > active_chain.Height()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Block does not exist at specified height");
    }

    const CBlockIndex* pb = active_chain.Tip();

    if (height >= 0) {
        pb = active_chain[height];
    }

    if (pb == nullptr || !pb->nHeight)
        return 0;

    // If lookup is -1, then use blocks since last difficulty change.
    if (lookup == -1)
        lookup = pb->nHeight % Params().GetConsensus().DifficultyAdjustmentInterval() + 1;

    // If lookup is larger than chain, then set it to chain length.
    if (lookup > pb->nHeight)
        lookup = pb->nHeight;

    const CBlockIndex* pb0 = pb;
    int64_t minTime = pb0->GetBlockTime();
    int64_t maxTime = minTime;
    for (int i = 0; i < lookup; i++) {
        pb0 = pb0->pprev;
        int64_t time = pb0->GetBlockTime();
        minTime = std::min(time, minTime);
        maxTime = std::max(time, maxTime);
    }

    // In case there's a situation where minTime == maxTime, we don't want a divide by zero exception.
    if (minTime == maxTime)
        return 0;

    arith_uint256 workDiff = pb->nChainWork - pb0->nChainWork;
    int64_t timeDiff = maxTime - minTime;

    return workDiff.getdouble() / timeDiff;
}

static RPCHelpMan getnetworkworkps()
{
    return RPCHelpMan{"getnetworkworkps",
                "\nReturns the estimated network work per second based on the last n blocks.\n"
                "Pass in [blocks] to override # of blocks, -1 specifies since last difficulty change.\n"
                "Pass in [height] to estimate the network speed at the time when a certain block was found.\n",
                {
                    {"nblocks", RPCArg::Type::NUM, RPCArg::Default{120}, "The number of previous blocks to calculate estimate from, or -1 for blocks since last difficulty change."},
                    {"height", RPCArg::Type::NUM, RPCArg::Default{-1}, "To estimate at the time of the given height."},
                },
                RPCResult{
                    RPCResult::Type::NUM, "", "Work per second estimated"},
                RPCExamples{
                    HelpExampleCli("getnetworkworkps", "")
            + HelpExampleRpc("getnetworkworkps", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    return GetNetworkWorkPS(self.Arg<int>("nblocks"), self.Arg<int>("height"), chainman.ActiveChain());
},
    };
}

static bool GenerateBlock(ChainstateManager& chainman, CBlock&& block, uint64_t& max_tries, std::shared_ptr<const CBlock>& block_out, bool process_new_block)
{
    block_out.reset();

    if (!node::SolveBlockPoW(chainman, block, max_tries)) {
        return false;
    }

    block_out = std::make_shared<const CBlock>(std::move(block));

    if (!process_new_block) return true;

    if (!chainman.ProcessNewBlock(block_out, /*force_processing=*/true, /*min_pow_checked=*/true, nullptr)) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ProcessNewBlock, block not accepted");
    }

    return true;
}

static UniValue generateBlocks(ChainstateManager& chainman, Mining& miner, const CScript& coinbase_output_script, int nGenerate, uint64_t nMaxTries)
{
    UniValue blockHashes(UniValue::VARR);
    while (nGenerate > 0 && !chainman.m_interrupt) {
        std::unique_ptr<BlockTemplate> block_template(miner.createNewBlock({ .coinbase_output_script = coinbase_output_script }));
        CHECK_NONFATAL(block_template);

        std::shared_ptr<const CBlock> block_out;
        if (!GenerateBlock(chainman, block_template->getBlock(), nMaxTries, block_out, /*process_new_block=*/true)) {
            break;
        }

        if (block_out) {
            --nGenerate;
            blockHashes.push_back(block_out->GetHash().GetHex());
        }
    }
    return blockHashes;
}

static bool getScriptFromDescriptor(const std::string& descriptor, CScript& script, std::string& error)
{
    FlatSigningProvider key_provider;
    const auto descs = Parse(descriptor, key_provider, error, /* require_checksum = */ false);
    if (descs.empty()) return false;
    if (descs.size() > 1) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Multipath descriptor not accepted");
    }
    const auto& desc = descs.at(0);
    if (desc->IsRange()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Ranged descriptor not accepted. Maybe pass through deriveaddresses first?");
    }

    FlatSigningProvider provider;
    std::vector<CScript> scripts;
    if (!desc->Expand(0, key_provider, scripts, provider)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Cannot derive script without private keys");
    }

    // A combo() descriptor expands to several scripts for one key: P2PK and
    // P2PKH always, plus P2WPKH when the key is compressed. Choose by what a
    // script is rather than by how many came back, so that changing the set
    // cannot silently repoint mining at a different output type.
    CHECK_NONFATAL(!scripts.empty());

    if (scripts.size() == 1) {
        script = scripts.at(0);
        return true;
    }

    // Prefer the witness script; an uncompressed key has none, so pay to its
    // P2PKH. Never mine to the bare P2PK, which no address can express.
    const auto witness{std::find_if(scripts.begin(), scripts.end(), [](const CScript& s) {
        int version;
        std::vector<unsigned char> program;
        return s.IsWitnessProgram(version, program);
    })};
    if (witness != scripts.end()) {
        script = *witness;
        return true;
    }
    script = scripts.at(1);

    return true;
}

static RPCHelpMan generatetodescriptor()
{
    return RPCHelpMan{
        "generatetodescriptor",
        "Mine to a specified descriptor and return the block hashes.",
        {
            {"num_blocks", RPCArg::Type::NUM, RPCArg::Optional::NO, "How many blocks are generated."},
            {"descriptor", RPCArg::Type::STR, RPCArg::Optional::NO, "The descriptor to send the newly generated coins to."},
            {"maxtries", RPCArg::Type::NUM, RPCArg::Default{DEFAULT_MAX_TRIES}, "How many iterations to try."},
        },
        RPCResult{
            RPCResult::Type::ARR, "", "hashes of blocks generated",
            {
                {RPCResult::Type::STR_HEX, "", "blockhash"},
            }
        },
        RPCExamples{
            "\nGenerate 11 blocks to mydesc\n" + HelpExampleCli("generatetodescriptor", "11 \"mydesc\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const auto num_blocks{self.Arg<int>("num_blocks")};
    const auto max_tries{self.Arg<uint64_t>("maxtries")};

    CScript coinbase_output_script;
    std::string error;
    if (!getScriptFromDescriptor(self.Arg<std::string>("descriptor"), coinbase_output_script, error)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, error);
    }

    NodeContext& node = EnsureAnyNodeContext(request.context);
    Mining& miner = EnsureMining(node);
    ChainstateManager& chainman = EnsureChainman(node);

    return generateBlocks(chainman, miner, coinbase_output_script, num_blocks, max_tries);
},
    };
}

static RPCHelpMan generatetoaddress()
{
    return RPCHelpMan{"generatetoaddress",
        "Mine to a specified address and return the block hashes.",
         {
             {"nblocks", RPCArg::Type::NUM, RPCArg::Optional::NO, "How many blocks are generated."},
             {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The address to send the newly generated coins to."},
             {"maxtries", RPCArg::Type::NUM, RPCArg::Default{DEFAULT_MAX_TRIES}, "How many iterations to try."},
         },
         RPCResult{
             RPCResult::Type::ARR, "", "hashes of blocks generated",
             {
                 {RPCResult::Type::STR_HEX, "", "blockhash"},
             }},
         RPCExamples{
            "\nGenerate 11 blocks to myaddress\n"
            + HelpExampleCli("generatetoaddress", "11 \"myaddress\"")
            + "If you are using the " CLIENT_NAME " vault, you can get a new address to send the newly generated coins to with:\n"
            + HelpExampleCli("getnewaddress", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const int num_blocks{request.params[0].getInt<int>()};
    const uint64_t max_tries{request.params[2].isNull() ? DEFAULT_MAX_TRIES : request.params[2].getInt<int>()};

    CTxDestination destination = DecodeDestination(request.params[1].get_str());
    if (!IsValidDestination(destination)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Error: Invalid address");
    }

    NodeContext& node = EnsureAnyNodeContext(request.context);
    Mining& miner = EnsureMining(node);
    ChainstateManager& chainman = EnsureChainman(node);

    CScript coinbase_output_script = GetScriptForDestination(destination);

    return generateBlocks(chainman, miner, coinbase_output_script, num_blocks, max_tries);
},
    };
}

static RPCHelpMan generateblock()
{
    return RPCHelpMan{"generateblock",
        "Mine a set of ordered transactions to a specified address or descriptor and return the block hash.",
        {
            {"output", RPCArg::Type::STR, RPCArg::Optional::NO, "The address or descriptor to send the newly generated coins to."},
            {"transactions", RPCArg::Type::ARR, RPCArg::Optional::NO, "An array of hex strings which are either txids or raw transactions.\n"
                "Txids must reference transactions currently in the relay pool.\n"
                "All transactions must be valid and in valid order, otherwise the block will be rejected.",
                {
                    {"rawtx/txid", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, ""},
                },
            },
            {"submit", RPCArg::Type::BOOL, RPCArg::Default{true}, "Whether to submit the block before the RPC call returns or to return it as hex."},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR_HEX, "hash", "hash of generated block"},
                {RPCResult::Type::STR_HEX, "hex", /*optional=*/true, "hex of generated block, only present when submit=false"},
            }
        },
        RPCExamples{
            "\nGenerate a block to myaddress, with txs rawtx and relaypool_txid\n"
            + HelpExampleCli("generateblock", R"("myaddress" '["rawtx", "relaypool_txid"]')")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const auto address_or_descriptor = request.params[0].get_str();
    CScript coinbase_output_script;
    std::string error;

    if (!getScriptFromDescriptor(address_or_descriptor, coinbase_output_script, error)) {
        const auto destination = DecodeDestination(address_or_descriptor);
        if (!IsValidDestination(destination)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Error: Invalid address or descriptor");
        }

        coinbase_output_script = GetScriptForDestination(destination);
    }

    NodeContext& node = EnsureAnyNodeContext(request.context);
    Mining& miner = EnsureMining(node);
    const CTxRelayPool& relaypool = EnsureRelayPool(node);

    std::vector<CTransactionRef> txs;
    const auto raw_txs_or_txids = request.params[1].get_array();
    for (size_t i = 0; i < raw_txs_or_txids.size(); i++) {
        const auto& str{raw_txs_or_txids[i].get_str()};

        CMutableTransaction mtx;
        if (auto hash{uint256::FromHex(str)}) {
            const auto tx{relaypool.get(*hash)};
            if (!tx) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Transaction %s not in the relay pool.", str));
            }

            txs.emplace_back(tx);

        } else if (DecodeHexTx(mtx, str)) {
            txs.push_back(MakeTransactionRef(std::move(mtx)));

        } else {
            throw JSONRPCError(RPC_DESERIALIZATION_ERROR, strprintf("Transaction decode failed for %s. Make sure the tx has at least one input.", str));
        }
    }

    const bool process_new_block{request.params[2].isNull() ? true : request.params[2].get_bool()};
    CBlock block;

    ChainstateManager& chainman = EnsureChainman(node);
    {
        LOCK(chainman.GetMutex());
        {
            std::unique_ptr<BlockTemplate> block_template{miner.createNewBlock({.use_relaypool = false, .coinbase_output_script = coinbase_output_script})};
            CHECK_NONFATAL(block_template);

            block = block_template->getBlock();
        }

        CHECK_NONFATAL(block.vtx.size() == 1);

        // Add transactions
        block.vtx.insert(block.vtx.end(), txs.begin(), txs.end());
        RegenerateCommitments(block, chainman);

        BlockValidationState state;
        if (!TestBlockValidity(state, chainman.GetParams(), chainman.ActiveChainstate(), block, chainman.m_blockman.LookupBlockIndex(block.hashPrevBlock), /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/false)) {
            throw JSONRPCError(RPC_VERIFY_ERROR, strprintf("TestBlockValidity failed: %s", state.ToString()));
        }
    }

    std::shared_ptr<const CBlock> block_out;
    uint64_t max_tries{DEFAULT_MAX_TRIES};

    if (!GenerateBlock(chainman, std::move(block), max_tries, block_out, process_new_block) || !block_out) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to make block.");
    }

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("hash", block_out->GetHash().GetHex());
    if (!process_new_block) {
        DataStream block_ser;
        block_ser << TX_WITH_WITNESS(*block_out);
        obj.pushKV("hex", HexStr(block_ser));
    }
    return obj;
},
    };
}

static node::MiningService& EnsureMiningService(const NodeContext& node)
{
    if (!node.mining_service) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Mining service not available");
    }
    return *node.mining_service;
}

static RPCHelpMan getmininginfo()
{
    return RPCHelpMan{"getmininginfo",
                "\nReturns a json object containing mining-related information.",
                {},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::NUM, "blocks", "The current block"},
                        {RPCResult::Type::NUM, "currentblockweight", /*optional=*/true, "The block weight (including reserved weight for block header, txs count and coinbase tx) of the last assembled block (only present if a block was ever assembled)"},
                        {RPCResult::Type::NUM, "currentblocktx", /*optional=*/true, "The number of block transactions (excluding coinbase) of the last assembled block (only present if a block was ever assembled). This is the last template assembled by any caller, including your own getblocktemplate call, and is not necessarily the block this node is mining -- for that, see getminingstatus template_transactions"},
                        {RPCResult::Type::STR_HEX, "bits", "The current nBits, compact representation of the block difficulty target"},
                        {RPCResult::Type::NUM, "difficulty", "The current difficulty"},
                        {RPCResult::Type::STR_HEX, "target", "The current target"},
                        {RPCResult::Type::NUM, "networkworkps", "The estimated network work per second"},
                        {RPCResult::Type::NUM, "pooledtx", "The size of the relay pool"},
                        {RPCResult::Type::STR, "chain", "current network name (" LIST_CHAIN_NAMES ")"},
                        {RPCResult::Type::OBJ, "next", "The next block",
                        {
                            {RPCResult::Type::NUM, "height", "The next height"},
                            {RPCResult::Type::STR_HEX, "bits", "The next target nBits"},
                            {RPCResult::Type::NUM, "difficulty", "The next difficulty"},
                            {RPCResult::Type::STR_HEX, "target", "The next target"}
                        }},
                        RPCResult{RPCResult::Type::ARR, "warnings", "any network and blockchain warnings",
                            {
                                {RPCResult::Type::STR, "", "warning"},
                            }
                        },
                    }},
                RPCExamples{
                    HelpExampleCli("getmininginfo", "")
            + HelpExampleRpc("getmininginfo", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    NodeContext& node = EnsureAnyNodeContext(request.context);
    const CTxRelayPool& relaypool = EnsureRelayPool(node);
    ChainstateManager& chainman = EnsureChainman(node);
    LOCK(cs_main);
    const CChain& active_chain = chainman.ActiveChain();
    CBlockIndex& tip{*CHECK_NONFATAL(active_chain.Tip())};

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("blocks",           active_chain.Height());
    if (BlockAssembler::m_last_block_weight) obj.pushKV("currentblockweight", *BlockAssembler::m_last_block_weight);
    if (BlockAssembler::m_last_block_num_txs) obj.pushKV("currentblocktx", *BlockAssembler::m_last_block_num_txs);
    obj.pushKV("bits", strprintf("%08x", tip.nBits));
    obj.pushKV("difficulty", GetDifficulty(tip, chainman.GetConsensus().powLimit));
    obj.pushKV("target", GetTarget(tip, chainman.GetConsensus().powLimit).GetHex());
    obj.pushKV("networkworkps",    getnetworkworkps().HandleRequest(request));
    obj.pushKV("pooledtx",         (uint64_t)relaypool.size());
    obj.pushKV("chain", chainman.GetParams().GetChainTypeString());

    UniValue next(UniValue::VOBJ);
    CBlockIndex next_index;
    NextEmptyBlockIndex(tip, chainman.GetConsensus(), next_index);

    next.pushKV("height", next_index.nHeight);
    next.pushKV("bits", strprintf("%08x", next_index.nBits));
    next.pushKV("difficulty", GetDifficulty(next_index, chainman.GetConsensus().powLimit));
    next.pushKV("target", GetTarget(next_index, chainman.GetConsensus().powLimit).GetHex());
    obj.pushKV("next", next);

    obj.pushKV("warnings", node::GetWarningsForRpc(*CHECK_NONFATAL(node.warnings)));
    return obj;
},
    };
}

static RPCHelpMan startmining()
{
    return RPCHelpMan{"startmining",
        "Start the opt-in background block-mining role. Off by default.\n",
        {
            {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED,
             "Payout address for mined coins. If omitted, uses -mineaddress."},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::BOOL, "active", "Whether the mining role is now active"},
            {RPCResult::Type::STR, "address", "The payout address in use"},
        }},
        RPCExamples{HelpExampleCli("startmining", "\"" + EXAMPLE_ADDRESS[0] + "\"")
            + HelpExampleRpc("startmining", "\"" + EXAMPLE_ADDRESS[0] + "\"")},
    [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    NodeContext& node = EnsureAnyNodeContext(request.context);
    node::MiningService& svc = EnsureMiningService(node);

    std::string addr_str;
    if (!request.params[0].isNull()) {
        addr_str = request.params[0].get_str();
    } else if (node.args) {
        addr_str = node.args->GetArg("-mineaddress", "");
    }
    if (addr_str.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "No payout address: pass an address or set -mineaddress");
    }
    if (auto res = node::StartMining(svc, addr_str); !res) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, util::ErrorString(res).original);
    }

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("active", svc.IsActive());
    obj.pushKV("address", svc.GetStatus().address);
    return obj;
},
    };
}

static RPCHelpMan stopmining()
{
    return RPCHelpMan{"stopmining",
        "Stop the background block-mining role.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::BOOL, "active", "Whether the mining role is active (false after stop)"},
        }},
        RPCExamples{HelpExampleCli("stopmining", "") + HelpExampleRpc("stopmining", "")},
    [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    NodeContext& node = EnsureAnyNodeContext(request.context);
    node::MiningService& svc = EnsureMiningService(node);
    svc.Stop();
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("active", svc.IsActive());
    return obj;
},
    };
}

static RPCHelpMan getminingstatus()
{
    return RPCHelpMan{"getminingstatus",
        "Returns the state of the background mining role and this session's mint telemetry.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::BOOL, "active", "Whether the mining role is active"},
            {RPCResult::Type::STR, "address", "Payout address (empty if never started)"},
            {RPCResult::Type::NUM, "blocks_found", "Blocks mined this session"},
            {RPCResult::Type::STR_AMOUNT, "coins_minted_session", "Coins minted this session (" + CURRENCY_UNIT + ")"},
            {RPCResult::Type::NUM, "attempts_per_second", /*optional=*/true, "Cuckatoo graphs solved per second over the trailing " + util::ToString(node::AttemptRateWindow::WINDOW_SECONDS) + " seconds. This is the rate now, not a session average, so it falls away when a solver stops and recovers when one comes back. Omitted entirely while no attempt has been observed this session -- that state is unknown, not zero, and a caller must not substitute one"},
            {RPCResult::Type::NUM, "graphs_attempted", "Cuckatoo graphs attempted this session. The session total the rate deliberately is not: it is what separates a solver that has never produced anything from one that has gone quiet. Read it and solver_ok together"},
            {RPCResult::Type::BOOL, "solver_ok", "Whether the last solver attempt completed without a fault"},
            {RPCResult::Type::STR, "last_solver_error", "What the solver failed with (empty when solver_ok)"},
            {RPCResult::Type::NUM_TIME, "last_block_time", "UNIX epoch time of the last mined block (0 if none)"},
            {RPCResult::Type::NUM, "elapsed_seconds", "Seconds since the mining role started (0 if inactive)"},
            {RPCResult::Type::NUM, "congestion_multiplier", "Per-tx PoW congestion multiplier (1.0 = hardware floor)"},
            {RPCResult::Type::NUM, "base_tx_work", "Chain-dependent base for transaction PoW at the current tip, in Cuckatoo cycles; a transaction's requirement is this scaled by its serialized bytes and net UTXO creation"},
            {RPCResult::Type::NUM, "template_height", "Height of the block this node is grinding right now (0 if none)"},
            {RPCResult::Type::NUM, "template_transactions", "Transactions in the block being ground, excluding the coinbase. The miner rate-limits relay-pool checks and rebuilds when transfers arrive. Unlike getmininginfo's currentblocktx, this is not overwritten by other callers assembling templates"},
        }},
        RPCExamples{HelpExampleCli("getminingstatus", "") + HelpExampleRpc("getminingstatus", "")},
    [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    NodeContext& node = EnsureAnyNodeContext(request.context);
    node::MiningService& svc = EnsureMiningService(node);
    ChainstateManager& chainman = EnsureChainman(node);
    const interfaces::MiningStatus st = node::BuildMiningStatus(svc, chainman);

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("active", st.active);
    obj.pushKV("address", st.address);
    obj.pushKV("blocks_found", st.blocks_found);
    obj.pushKV("coins_minted_session", ValueFromAmount(st.coins_minted_session));
    // Deliberately absent rather than 0 when unknown: a zero rate is a real
    // reading (an armed miner whose solver has stopped) and must not be produced
    // by a miner that simply has not finished its first graph yet.
    if (st.attempts_per_second) obj.pushKV("attempts_per_second", *st.attempts_per_second);
    obj.pushKV("graphs_attempted", st.graphs_attempted);
    obj.pushKV("solver_ok", st.solver_ok);
    obj.pushKV("last_solver_error", st.last_solver_error);
    obj.pushKV("last_block_time", st.last_block_time);
    obj.pushKV("elapsed_seconds", st.elapsed_seconds);
    obj.pushKV("congestion_multiplier", st.congestion_multiplier);
    obj.pushKV("base_tx_work", st.base_tx_work);
    obj.pushKV("template_height", st.template_height);
    obj.pushKV("template_transactions", st.template_transactions);
    return obj;
},
    };
}


// NOTE: Assumes a conclusive result; if result is inconclusive, it must be handled by caller
static UniValue BIP22ValidationResult(const BlockValidationState& state)
{
    if (state.IsValid())
        return UniValue::VNULL;

    if (state.IsError())
        throw JSONRPCError(RPC_VERIFY_ERROR, state.ToString());
    if (state.IsInvalid())
    {
        std::string strRejectReason = state.GetRejectReason();
        if (strRejectReason.empty())
            return "rejected";
        return strRejectReason;
    }
    // Should be impossible
    return "valid?";
}

static std::string gbt_vb_name(const Consensus::DeploymentPos pos) {
    const struct VBDeploymentInfo& vbinfo = VersionBitsDeploymentInfo[pos];
    std::string s = vbinfo.name;
    if (!vbinfo.gbt_force) {
        s.insert(s.begin(), '!');
    }
    return s;
}

UniValue BlockPowDescriptor(const Consensus::Params& params, const arith_uint256& hashTarget)
{
    UniValue pow(UniValue::VOBJ);
    pow.pushKV("algorithm", "cuckatoo");
    pow.pushKV("edgebits", (int)params.nEdgeBits);
    pow.pushKV("proofsize", (int)cuckatoo::PROOFSIZE);
    pow.pushKV("proofhash", "blake2b");
    pow.pushKV("proofhashtarget", hashTarget.GetHex());
    pow.pushKV("prepowsize", (int)CBlockHeader::PREPOW_SIZE);
    pow.pushKV("cycleencoding",
               strprintf("uint32le x42 ascending, appended to the %u-byte header",
                         (unsigned)CBlockHeader::PREPOW_SIZE));
    // Quicksilver (#5c-1 Phase 2): the recurrence a miner needs to recompute nCongestion
    // for itself. The template's "congestion" field is only correct for the template's own
    // transaction set; "transactions" is in the mutable list, so a miner that changes the
    // body MUST recompute from these and the parent multiplier.
    UniValue congestion(UniValue::VOBJ);
    congestion.pushKV("one", (int64_t)CONGESTION_ONE);
    congestion.pushKV("targetpermille", params.nCongestionTargetPermille);
    congestion.pushKV("stepdenom", params.nCongestionStepDenom);
    congestion.pushKV("maxmultiplier", params.nCongestionMaxMultiplier);
    pow.pushKV("congestionparams", std::move(congestion));
    if (params.fBlockPowNoCycle) pow.pushKV("trivialcycle", true);
    return pow;
}

static RPCHelpMan getblocktemplate()
{
    return RPCHelpMan{"getblocktemplate",
        "\nIf the request parameters include a 'mode' key, that is used to explicitly select between the default 'template' request or a 'proposal'.\n"
        "It returns data needed to construct a block to work on.\n"
        "\nThis method is for external mining software on a populated network. On the live network it\n"
        "refuses while the node has no peers or is still syncing. To mine while bootstrapping a chain,\n"
        "use startmining, which builds templates in-process and is not gated on peer count.\n",
        {
            {"template_request", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Format of the template",
            {
                {"mode", RPCArg::Type::STR, /* treat as named arg */ RPCArg::Optional::OMITTED, "This must be set to \"template\", \"proposal\" (see BIP 23), or omitted"},
                {"rules", RPCArg::Type::ARR, RPCArg::Optional::NO, "A list of strings",
                {
                    {"segwit", RPCArg::Type::STR, RPCArg::Optional::NO, "(literal) indicates client side segwit support"},
                    {"str", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "other client side supported softfork deployment"},
                }},
                {"longpollid", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "delay processing request until the result would vary significantly from the \"longpollid\" of a prior template"},
                {"data", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "proposed block data to check, encoded in hexadecimal; valid only for mode=\"proposal\""},
            },
            },
        },
        {
            RPCResult{"If the proposal was accepted with mode=='proposal'", RPCResult::Type::NONE, "", ""},
            RPCResult{"If the proposal was not accepted with mode=='proposal'", RPCResult::Type::STR, "", "According to BIP22"},
            RPCResult{"Otherwise", RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::NUM, "version", "The preferred block version"},
                {RPCResult::Type::ARR, "rules", "specific block rules that are to be enforced",
                {
                    {RPCResult::Type::STR, "", "name of a rule the client must understand to some extent; see BIP 9 for format"},
                }},
                {RPCResult::Type::OBJ_DYN, "vbavailable", "set of pending, supported versionbit (BIP 9) softfork deployments",
                {
                    {RPCResult::Type::NUM, "rulename", "identifies the bit number as indicating acceptance and readiness for the named softfork rule"},
                }},
                {RPCResult::Type::ARR, "capabilities", "",
                {
                    {RPCResult::Type::STR, "value", "A supported feature, for example 'proposal'"},
                }},
                {RPCResult::Type::STR, "previousblockhash", "The hash of current highest block"},
                {RPCResult::Type::NUM, "congestion", "Quicksilver header field nCongestion for this template: the EIP-1559 congestion multiplier, fixed point against pow.congestionparams.one. Valid ONLY for this template's transaction set — 'transactions' is mutable, so a miner that changes the body must recompute it (see pow.congestionparams) before grinding, since it sits inside the pre-pow"},
                {RPCResult::Type::ARR, "transactions", "contents of non-coinbase transactions that should be included in the next block",
                {
                    {RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "data", "transaction data encoded in hexadecimal (byte-for-byte)"},
                        {RPCResult::Type::STR_HEX, "txid", "transaction hash excluding witness data, shown in byte-reversed hex"},
                        {RPCResult::Type::STR_HEX, "hash", "transaction hash including witness data, shown in byte-reversed hex"},
                        {RPCResult::Type::ARR, "depends", "array of numbers",
                        {
                            {RPCResult::Type::NUM, "", "transactions before this one (by 1-based index in 'transactions' list) that must be present in the final block if this one is"},
                        }},
                        {RPCResult::Type::NUM, "mintvalue", "Quicksilver per-tx mint claim authorized by this transaction's proof, in cinnabar"},
                        {RPCResult::Type::STR, "txwork", "Quicksilver actual per-tx proof work as a decimal arith_uint256 string"},
                        {RPCResult::Type::STR, "txwork_surplus", "Quicksilver per-tx proof work above the anchor-derived floor as a decimal arith_uint256 string"},
                        {RPCResult::Type::NUM, "txwork_rate", "Quicksilver surplus per-tx proof work per virtual byte"},
                        {RPCResult::Type::NUM, "sigops", "total SigOps cost, as counted for purposes of block limits; if key is not present, sigop cost is unknown and clients MUST NOT assume it is zero"},
                        {RPCResult::Type::NUM, "weight", "total transaction weight, as counted for purposes of block limits"},
                    }},
                }},
                {RPCResult::Type::NUM, "coinbasevalue", "maximum allowable input to coinbase transaction, including the generation award and per-tx mints (in cinnabar); equals subsidy + mintvalue"},
                {RPCResult::Type::NUM, "subsidy", "the base block subsidy portion of coinbasevalue (in cinnabar)"},
                {RPCResult::Type::NUM, "mintvalue", "the Frame-B per-tx mint portion of coinbasevalue (sum of nTxPowMint over the anchor-valid included txs, in cinnabar)"},
                {RPCResult::Type::STR, "longpollid", "an id to include with a request to longpoll on an update to this template"},
                {RPCResult::Type::STR, "target", "The hash target"},
                {RPCResult::Type::OBJ, "pow", "block proof-of-work descriptor (Cuckatoo) for external miners",
                {
                    {RPCResult::Type::STR, "algorithm", "the block PoW algorithm ('cuckatoo')"},
                    {RPCResult::Type::NUM, "edgebits", "the Cuckatoo graph size (log2 of edges); the block PoW solves a graph this large"},
                    {RPCResult::Type::NUM, "proofsize", "the cycle length (42)"},
                    {RPCResult::Type::STR, "proofhash", "the hash applied to the cycle to derive the proof hash ('blake2b')"},
                    {RPCResult::Type::STR, "proofhashtarget", "CuckatooProofHash(nCycle) must be <= this (equals 'target')"},
                    {RPCResult::Type::NUM, "prepowsize", "bytes of the header prefix (through nNonce, including nCongestion) that seed the siphash keys"},
                    {RPCResult::Type::STR, "cycleencoding", "how the 42-cycle is encoded into the header"},
                    {RPCResult::Type::OBJ, "congestionparams", "inputs for recomputing nCongestion when the template's transaction set is modified",
                    {
                        {RPCResult::Type::NUM, "one", "fixed-point scale; this value means multiplier 1.0"},
                        {RPCResult::Type::NUM, "targetpermille", "target block fullness T, in permille of the weight limit"},
                        {RPCResult::Type::NUM, "stepdenom", "max per-block step is 1/stepdenom"},
                        {RPCResult::Type::NUM, "maxmultiplier", "ceiling on the multiplier, in units of 'one'"},
                    }},
                    {RPCResult::Type::BOOL, "trivialcycle", /*optional=*/true, "present and true only on sandbox (fBlockPowNoCycle): no real 42-cycle required, only the proof-hash target"},
                }},
                {RPCResult::Type::NUM_TIME, "mintime", "The minimum timestamp appropriate for the next block time, expressed in " + UNIX_EPOCH_TIME + "."},
                {RPCResult::Type::ARR, "mutable", "list of ways the block template may be changed",
                {
                    {RPCResult::Type::STR, "value", "A way the block template may be changed, e.g. 'time', 'transactions', 'prevblock'"},
                }},
                {RPCResult::Type::NUM, "sigoplimit", "limit of sigops in blocks"},
                {RPCResult::Type::NUM, "sizelimit", "limit of block size"},
                {RPCResult::Type::NUM, "weightlimit", "limit of block weight"},
                {RPCResult::Type::NUM_TIME, "curtime", "current timestamp in " + UNIX_EPOCH_TIME + "."},
                {RPCResult::Type::STR, "bits", "compressed target of next block"},
                {RPCResult::Type::NUM, "height", "The height of the next block"},
                {RPCResult::Type::STR_HEX, "default_witness_commitment", /*optional=*/true, "a valid witness commitment for the unmodified block template"},
            }},
        },
        RPCExamples{
                    HelpExampleCli("getblocktemplate", "'{\"rules\": [\"segwit\"]}'")
            + HelpExampleRpc("getblocktemplate", "{\"rules\": [\"segwit\"]}")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    NodeContext& node = EnsureAnyNodeContext(request.context);
    ChainstateManager& chainman = EnsureChainman(node);
    Mining& miner = EnsureMining(node);
    LOCK(cs_main);
    uint256 tip{CHECK_NONFATAL(miner.getTip()).value().hash};

    std::string strMode = "template";
    UniValue lpval = NullUniValue;
    std::set<std::string> setClientRules;
    if (!request.params[0].isNull())
    {
        const UniValue& oparam = request.params[0].get_obj();
        const UniValue& modeval = oparam.find_value("mode");
        if (modeval.isStr())
            strMode = modeval.get_str();
        else if (modeval.isNull())
        {
            /* Do nothing */
        }
        else
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid mode");
        lpval = oparam.find_value("longpollid");

        if (strMode == "proposal")
        {
            const UniValue& dataval = oparam.find_value("data");
            if (!dataval.isStr())
                throw JSONRPCError(RPC_TYPE_ERROR, "Missing data String key for proposal");

            CBlock block;
            if (!DecodeHexBlk(block, dataval.get_str()))
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block decode failed");

            uint256 hash = block.GetHash();
            const CBlockIndex* pindex = chainman.m_blockman.LookupBlockIndex(hash);
            if (pindex) {
                if (pindex->IsValid(BLOCK_VALID_SCRIPTS))
                    return "duplicate";
                if (pindex->nStatus & BLOCK_FAILED_MASK)
                    return "duplicate-invalid";
                return "duplicate-inconclusive";
            }

            // TestBlockValidity only supports blocks built on the current Tip
            if (block.hashPrevBlock != tip) {
                return "inconclusive-not-best-prevblk";
            }
            BlockValidationState state;
            TestBlockValidity(state, chainman.GetParams(), chainman.ActiveChainstate(), block, chainman.m_blockman.LookupBlockIndex(block.hashPrevBlock), /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/true);
            return BIP22ValidationResult(state);
        }

        const UniValue& aClientRules = oparam.find_value("rules");
        if (aClientRules.isArray()) {
            for (unsigned int i = 0; i < aClientRules.size(); ++i) {
                const UniValue& v = aClientRules[i];
                setClientRules.insert(v.get_str());
            }
        }
    }

    if (strMode != "template")
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid mode");

    if (!miner.isTestChain()) {
        const CConnman& connman = EnsureConnman(node);
        if (connman.GetNodeCount(ConnectionDirection::Both) == 0) {
            // The first node on a new chain has no peer and never will until a
            // second one arrives, so this is the bootstrap condition rather than
            // a fault. The gate stays -- it is what stops a syncing node mining a
            // stale fork -- and the message names the path that does work.
            throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED,
                               CLIENT_NAME " has no peers, so it will not build a block template. "
                               "getblocktemplate is for external mining software on a populated network; "
                               "to mine while bootstrapping, use startmining instead.");
        }

        if (miner.isInitialBlockDownload()) {
            throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, CLIENT_NAME " is in initial sync and waiting for blocks...");
        }
    }

    static unsigned int nTransactionsUpdatedLast;
    const CTxRelayPool& relaypool = EnsureRelayPool(node);

    if (!lpval.isNull())
    {
        // Wait to respond until either the best block changes, OR a minute has passed and there are more transactions
        uint256 hashWatchedChain;
        unsigned int nTransactionsUpdatedLastLP;

        if (lpval.isStr())
        {
            // Format: <hashBestChain><nTransactionsUpdatedLast>
            const std::string& lpstr = lpval.get_str();

            hashWatchedChain = ParseHashV(lpstr.substr(0, 64), "longpollid");
            nTransactionsUpdatedLastLP = LocaleIndependentAtoi<int64_t>(lpstr.substr(64));
        }
        else
        {
            // NOTE: Spec does not specify behaviour for non-string longpollid, but this makes testing easier
            hashWatchedChain = tip;
            nTransactionsUpdatedLastLP = nTransactionsUpdatedLast;
        }

        // Release lock while waiting
        LEAVE_CRITICAL_SECTION(cs_main);
        {
            MillisecondsDouble checktxtime{std::chrono::minutes(1)};
            while (tip == hashWatchedChain && IsRPCRunning()) {
                tip = miner.waitTipChanged(hashWatchedChain, checktxtime).hash;
                // Timeout: Check transactions for update
                // without holding the relaypool lock to avoid deadlocks
                if (relaypool.GetTransactionsUpdated() != nTransactionsUpdatedLastLP)
                    break;
                checktxtime = std::chrono::seconds(10);
            }
        }
        ENTER_CRITICAL_SECTION(cs_main);

        tip = CHECK_NONFATAL(miner.getTip()).value().hash;

        if (!IsRPCRunning())
            throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "Shutting down");
        // TODO: Maybe recheck connections/IBD and (if something wrong) send an expires-immediately template to stop miners?
    }

    const Consensus::Params& consensusParams = chainman.GetParams().GetConsensus();

    // GBT must be called with 'segwit' set in the rules
    if (setClientRules.count("segwit") != 1) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "getblocktemplate must be called with the segwit rule set (call with {\"rules\": [\"segwit\"]})");
    }

    // Update block
    static CBlockIndex* pindexPrev;
    static int64_t time_start;
    static std::unique_ptr<BlockTemplate> block_template;
    if (!pindexPrev || pindexPrev->GetBlockHash() != tip ||
        (relaypool.GetTransactionsUpdated() != nTransactionsUpdatedLast && GetTime() - time_start > 5))
    {
        // Clear pindexPrev so future calls make a new block, despite any failures from here on
        pindexPrev = nullptr;

        // Store the pindexBest used before createNewBlock, to avoid races
        nTransactionsUpdatedLast = relaypool.GetTransactionsUpdated();
        CBlockIndex* pindexPrevNew = chainman.m_blockman.LookupBlockIndex(tip);
        time_start = GetTime();

        // Create new block
        block_template = miner.createNewBlock();
        CHECK_NONFATAL(block_template);


        // Need to update only after we know createNewBlock succeeded
        pindexPrev = pindexPrevNew;
    }
    CHECK_NONFATAL(pindexPrev);
    CBlock block{block_template->getBlock()};

    // Update nTime
    UpdateTime(&block, consensusParams, pindexPrev);
    block.nNonce = 0;

    UniValue aCaps(UniValue::VARR); aCaps.push_back("proposal");

    UniValue transactions(UniValue::VARR);
    std::map<uint256, int64_t> setTxIndex;
    std::vector<CAmount> tx_sigops{block_template->getTxSigops()};

    int i = 0;
    for (const auto& it : block.vtx) {
        const CTransaction& tx = *it;
        uint256 txHash = tx.GetHash();
        setTxIndex[txHash] = i++;

        if (tx.IsCoinBase())
            continue;

        UniValue entry(UniValue::VOBJ);

        entry.pushKV("data", EncodeHexTx(tx));
        entry.pushKV("txid", txHash.GetHex());
        entry.pushKV("hash", tx.GetWitnessHash().GetHex());

        UniValue deps(UniValue::VARR);
        for (const CTxIn &in : tx.vin)
        {
            if (setTxIndex.count(in.prevout.hash))
                deps.push_back(setTxIndex[in.prevout.hash]);
        }
        entry.pushKV("depends", std::move(deps));

        int index_in_template = i - 1;
        int64_t nTxSigOps{tx_sigops.at(index_in_template)};
        entry.pushKV("sigops", nTxSigOps);
        entry.pushKV("weight", GetTransactionWeight(tx));
        const CBlockIndex* tx_anchor{CheckTxAnchor(tx, pindexPrev, consensusParams)};
        const bool mints{tx_anchor != nullptr &&
            CheckTxProofOfWork(tx, GetTxPowTarget(consensusParams, tx_anchor, tx), tx_anchor->GetBlockHash(), consensusParams)};
        const arith_uint256 txwork{GetTxActualWork(tx)};
        const arith_uint256 txwork_surplus{GetTxWorkSurplus(tx, tx_anchor, consensusParams)};
        const int64_t virtual_size{GetVirtualTransactionSize(tx, tx_sigops.at(index_in_template), ::nBytesPerSigOp)};
        entry.pushKV("mintvalue", mints ? static_cast<int64_t>(consensusParams.nTxPowMint) : int64_t{0});
        entry.pushKV("txwork", txwork.ToString());
        entry.pushKV("txwork_surplus", txwork_surplus.ToString());
        entry.pushKV("txwork_rate", virtual_size > 0 ? txwork_surplus.getdouble() / virtual_size : 0.0);

        transactions.push_back(std::move(entry));
    }

    arith_uint256 hashTarget = arith_uint256().SetCompact(block.nBits);

    UniValue aMutable(UniValue::VARR);
    aMutable.push_back("time");
    aMutable.push_back("transactions");
    aMutable.push_back("prevblock");

    UniValue result(UniValue::VOBJ);
    result.pushKV("capabilities", std::move(aCaps));

    UniValue aRules(UniValue::VARR);
    aRules.push_back("csv");
    aRules.push_back("!segwit");

    UniValue vbavailable(UniValue::VOBJ);
    for (int j = 0; j < (int)Consensus::MAX_VERSION_BITS_DEPLOYMENTS; ++j) {
        Consensus::DeploymentPos pos = Consensus::DeploymentPos(j);
        ThresholdState state = chainman.m_versionbitscache.State(pindexPrev, consensusParams, pos);
        switch (state) {
            case ThresholdState::DEFINED:
            case ThresholdState::FAILED:
                // Not exposed to GBT at all
                break;
            case ThresholdState::LOCKED_IN:
                // Ensure bit is set in block version
                block.nVersion |= chainman.m_versionbitscache.Mask(consensusParams, pos);
                [[fallthrough]];
            case ThresholdState::STARTED:
            {
                const struct VBDeploymentInfo& vbinfo = VersionBitsDeploymentInfo[pos];
                vbavailable.pushKV(gbt_vb_name(pos), consensusParams.vDeployments[pos].bit);
                if (setClientRules.find(vbinfo.name) == setClientRules.end()) {
                    if (!vbinfo.gbt_force) {
                        // If the client doesn't support this, don't indicate it in the [default] version
                        block.nVersion &= ~chainman.m_versionbitscache.Mask(consensusParams, pos);
                    }
                }
                break;
            }
            case ThresholdState::ACTIVE:
            {
                // Add to rules only
                const struct VBDeploymentInfo& vbinfo = VersionBitsDeploymentInfo[pos];
                aRules.push_back(gbt_vb_name(pos));
                if (setClientRules.find(vbinfo.name) == setClientRules.end()) {
                    // Not supported by the client; make sure it's safe to proceed
                    if (!vbinfo.gbt_force) {
                        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Support for '%s' rule requires explicit client support", vbinfo.name));
                    }
                }
                break;
            }
        }
    }
    result.pushKV("version", block.nVersion);
    result.pushKV("rules", std::move(aRules));
    result.pushKV("vbavailable", std::move(vbavailable));

    result.pushKV("previousblockhash", block.hashPrevBlock.GetHex());
    result.pushKV("congestion", (int64_t)block.nCongestion);
    result.pushKV("transactions", std::move(transactions));
    result.pushKV("coinbasevalue", (int64_t)block.vtx[0]->vout[0].nValue);
    result.pushKV("longpollid", tip.GetHex() + ToString(nTransactionsUpdatedLast));
    result.pushKV("target", hashTarget.GetHex());
    result.pushKV("pow", BlockPowDescriptor(consensusParams, hashTarget));
    const int nextHeight = pindexPrev->nHeight + 1;
    const CAmount subsidy = GetBlockSubsidy(nextHeight, consensusParams);
    const CAmount cbvalue = block.vtx[0]->vout[0].nValue;
    result.pushKV("subsidy", (int64_t)subsidy);
    result.pushKV("mintvalue", (int64_t)(cbvalue - subsidy));
    result.pushKV("mintime", GetMinimumTime(pindexPrev, consensusParams.DifficultyAdjustmentInterval()));
    result.pushKV("mutable", std::move(aMutable));
    result.pushKV("sigoplimit", (int64_t)MAX_BLOCK_SIGOPS_COST);
    result.pushKV("sizelimit", (int64_t)MAX_BLOCK_SERIALIZED_SIZE);
    result.pushKV("weightlimit", (int64_t)MAX_BLOCK_WEIGHT);
    result.pushKV("curtime", block.GetBlockTime());
    result.pushKV("bits", strprintf("%08x", block.nBits));
    result.pushKV("height", (int64_t)(pindexPrev->nHeight+1));

    if (!block_template->getCoinbaseCommitment().empty()) {
        result.pushKV("default_witness_commitment", HexStr(block_template->getCoinbaseCommitment()));
    }

    return result;
},
    };
}

class submitblock_StateCatcher final : public CValidationInterface
{
public:
    uint256 hash;
    bool found{false};
    BlockValidationState state;

    explicit submitblock_StateCatcher(const uint256 &hashIn) : hash(hashIn), state() {}

protected:
    void BlockChecked(const CBlock& block, const BlockValidationState& stateIn) override {
        if (block.GetHash() != hash)
            return;
        found = true;
        state = stateIn;
    }
};

static RPCHelpMan submitblock()
{
    return RPCHelpMan{"submitblock",
        "\nAttempts to submit new block to network.\n",
        {
            {"hexdata", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "the hex-encoded block data to submit"},
        },
        {
            RPCResult{"If the block was accepted", RPCResult::Type::NONE, "", ""},
            RPCResult{"Otherwise", RPCResult::Type::STR, "", "According to BIP22"},
        },
        RPCExamples{
                    HelpExampleCli("submitblock", "\"mydata\"")
            + HelpExampleRpc("submitblock", "\"mydata\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CBlock> blockptr = std::make_shared<CBlock>();
    CBlock& block = *blockptr;
    if (!DecodeHexBlk(block, request.params[0].get_str())) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block decode failed");
    }

    ChainstateManager& chainman = EnsureAnyChainman(request.context);

    // Quicksilver (#5c-1 Phase 2): the submitted block is validated exactly as it
    // arrived. A node that filled in a missing coinbase witness nonce here would
    // change the block's weight, and weight feeds nCongestion -- so a miner who
    // ground a correct multiplier over their own bytes would be rejected as
    // 'bad-congestion', pointing at the one field they got right. Supplying the
    // 32-byte reserved value is the miner's job; omitting it now earns
    // 'bad-witness-nonce-size', which names the actual mistake.

    bool new_block;
    auto sc = std::make_shared<submitblock_StateCatcher>(block.GetHash());
    CHECK_NONFATAL(chainman.m_options.signals)->RegisterSharedValidationInterface(sc);
    bool accepted = chainman.ProcessNewBlock(blockptr, /*force_processing=*/true, /*min_pow_checked=*/true, /*new_block=*/&new_block);
    CHECK_NONFATAL(chainman.m_options.signals)->UnregisterSharedValidationInterface(sc);
    if (!new_block && accepted) {
        return "duplicate";
    }
    if (!sc->found) {
        return "inconclusive";
    }
    return BIP22ValidationResult(sc->state);
},
    };
}

static RPCHelpMan submitheader()
{
    return RPCHelpMan{"submitheader",
                "\nDecode the given hexdata as a header and submit it as a candidate chain tip if valid."
                "\nThrows when the header is invalid.\n",
                {
                    {"hexdata", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "the hex-encoded block header data"},
                },
                RPCResult{
                    RPCResult::Type::NONE, "", "None"},
                RPCExamples{
                    HelpExampleCli("submitheader", "\"aabbcc\"") +
                    HelpExampleRpc("submitheader", "\"aabbcc\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    CBlockHeader h;
    if (!DecodeHexBlockHeader(h, request.params[0].get_str())) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block header decode failed");
    }
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    {
        LOCK(cs_main);
        if (!chainman.m_blockman.LookupBlockIndex(h.hashPrevBlock)) {
            throw JSONRPCError(RPC_VERIFY_ERROR, "Must submit previous header (" + h.hashPrevBlock.GetHex() + ") first");
        }
    }

    BlockValidationState state;
    chainman.ProcessNewBlockHeaders({{h}}, /*min_pow_checked=*/true, state);
    if (state.IsValid()) return UniValue::VNULL;
    if (state.IsError()) {
        throw JSONRPCError(RPC_VERIFY_ERROR, state.ToString());
    }
    throw JSONRPCError(RPC_VERIFY_ERROR, state.GetRejectReason());
},
    };
}

void RegisterMiningRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"mining", &getnetworkworkps},
        {"mining", &getmininginfo},
        {"mining", &getblocktemplate},
        {"mining", &submitblock},
        {"mining", &submitheader},
        {"mining", &startmining},
        {"mining", &stopmining},
        {"mining", &getminingstatus},

        {"hidden", &generatetoaddress},
        {"hidden", &generatetodescriptor},
        {"hidden", &generateblock},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
