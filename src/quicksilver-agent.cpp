// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <quicksilver-build-config.h> // IWYU pragma: keep

#include <addresstype.h>
#include <agent/agentclient.h>
#include <agent/messageio.h>
#include <agent/peertransport.h>
#include <agent/txmessages.h>
#include <agent/allotmentpolicy.h>
#include <agent/allotmentspend.h>
#include <agent/interrupt.h>
#include <agent/allotmentstore.h>
#include <chainparams.h>
#include <chainparamsbase.h>
#include <clientversion.h>
#include <common/args.h>
#include <common/system.h>
#include <compat/compat.h>
#include <consensus/amount.h>
#include <core_io.h>
#include <hash.h>
#include <key_io.h>
#include <netbase.h>
#include <netmessagemaker.h>
#include <node/protocol_version.h>
#include <primitives/block.h>
#include <protocol.h>
#include <span.h>
#include <streams.h>
#include <tinyformat.h>
#include <util/exception.h>
#include <util/fs.h>
#include <util/result.h>
#include <util/readwritefile.h>
#include <util/moneystr.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/threadinterrupt.h>
#include <util/time.h>
#include <util/translation.h>
#include <univalue.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <ios>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

static constexpr int CONTINUE_EXECUTION{-1};
static constexpr size_t MAX_AGENT_PEER_STORE_FILE_SIZE{100'000};
static constexpr size_t MAX_AGENT_DISCOVERED_PEERS{1000};
static constexpr int64_t DEFAULT_AGENT_PEER_TIMEOUT_MS{10'000};
static constexpr int MAX_AGENT_PEER_HANDSHAKE_MESSAGES{64};
static constexpr int MAX_AGENT_PEER_DISCOVERY_MESSAGES{64};
static constexpr int MAX_AGENT_PEER_HEADER_SYNC_MESSAGES{256};
static constexpr bool DEFAULT_AGENT_PROXY_RANDOMIZE{true};

const TranslateFn G_TRANSLATION_FUN{nullptr};

static void SetupAgentArgs(ArgsManager& argsman)
{
    SetupHelpOptions(argsman);
    SetupChainParamsBaseOptions(argsman);

    argsman.AddArg("-version", "Print version and exit", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-datadir=<dir>", "Specify data directory", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::OPTIONS);
    argsman.AddArg("-headerstore=<file>", "Specify the agent header store path. Relative paths are resolved under the network data directory. (default: agent/headers.dat)", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::OPTIONS);
    argsman.AddArg("-receiptstore=<file>", "Specify the agent payment receipt store path. Relative paths are resolved under the network data directory. (default: agent/payment-receipts.dat)", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::OPTIONS);
    argsman.AddArg("-paymentreceiptdir=<dir>", "Directory scanned by scanreceipts for agent payment receipt JSON files. Relative paths are resolved under the network data directory. (default: agent/payment-receipts.d)", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::OPTIONS);
    argsman.AddArg("-peerstore=<file>", "Specify the agent relay peer store path. Relative paths are resolved under the network data directory. (default: agent/relay-peers.json)", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::OPTIONS);
    argsman.AddArg("-message=<hex>", "Hex-encoded P2P message payload. Use '-' to read from standard input.", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::OPTIONS);
    argsman.AddArg("-peeraddresses=<json>", "JSON array from quicksilver-cli getnodeaddresses. Use '-' to read from standard input.", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::OPTIONS);
    argsman.AddArg("-peer=<host[:port]>", "Relay peer address used by addpeer, removepeer, or sendtxpeer. Hostnames are resolved and the active chain default port is used when no port is specified.", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::OPTIONS);
    argsman.AddArg("-peertimeout=<ms>", "Socket connect, send, receive, and handshake timeout in milliseconds for configured-peer commands. (default: 10000)", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::OPTIONS);
    argsman.AddArg("-proxy=<ip:port|path>", "Connect through a SOCKS5 proxy. This is also the default proxy for fixed onion seeds. May be a local file path prefixed with 'unix:'.", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_ELISION, OptionsCategory::OPTIONS);
    argsman.AddArg("-onion=<ip:port|path>", "Use a separate SOCKS5 proxy for Tor onion peers, or -onion=0 to disable onion connections (default: -proxy). May be a local file path prefixed with 'unix:'.", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-proxyrandomize", "Randomize credentials for every proxy connection to isolate Tor streams (default: 1).", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-scantxoutset=<json>", "JSON result from quicksilver-cli scantxoutset start. Use '-' to read from standard input.", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::OPTIONS);
    argsman.AddArg("-fundingaddress=<address>", "Agent funding address for importrecovery.", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::OPTIONS);
    argsman.AddArg("-policyrequest=<json>", "Agent Allotment Gateway policy request JSON. Use '-' to read from standard input.", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::OPTIONS);
    argsman.AddArg("-policybundle=<json>", "Agent Allotment Gateway policy, key, and funding-output bundle JSON. Use '-' to read from standard input.", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::OPTIONS);
    argsman.AddArg("-paymentreceipt=<json>", "Agent payment receipt JSON with a post-handoff funding output. May be specified multiple times; use '-' to read one receipt from standard input.", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::OPTIONS);
    argsman.AddArg("-spendamount=<cinnabar>", "Spend amount in cinnabar for checkpolicy/checkbundle/signbundle.", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::OPTIONS);
    argsman.AddArg("-spenttoday=<cinnabar>", "Amount already spent today in cinnabar for checkpolicy/checkbundle/signbundle. If omitted, checkbundle and signbundle sum today's SPENT receipt activity; checkpolicy defaults to 0.", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::OPTIONS);
    argsman.AddArg("-prevtxid=<hex>", "Optional funding transaction id for signbundle. If omitted, signbundle selects from funding_outputs in the bundle.", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::OPTIONS);
    argsman.AddArg("-prevout=<n>", "Optional funding transaction output index for signbundle. If omitted, signbundle selects from funding_outputs in the bundle.", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::OPTIONS);
    argsman.AddArg("-prevamount=<cinnabar>", "Optional funding transaction output amount in cinnabar for signbundle. If omitted, signbundle selects from funding_outputs in the bundle.", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::OPTIONS);
    argsman.AddArg("-destination=<address>", "Spend destination address for signbundle.", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::OPTIONS);
    argsman.AddArg("-cuckatoosolver=<path>", "Path to an external GPU Cuckatoo solver binary for proving agent spends on non-sandbox networks.", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::OPTIONS);
    argsman.AddArg("-allowcputxpow", "Permit this computer's processor to produce the per-transaction proof when no GPU solver is configured (default: false). Proving one spend on a processor takes many minutes and uses every core, and an agent spend can start at any time, including while the computer is in use. A GPU solver is strongly preferred.", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-cuckatoosolvertimeout=<sec>", "No-progress watchdog window for the external GPU solver, in seconds.", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::OPTIONS);
    argsman.AddArg("-prove=<0|1>", "Create per-tx proof-of-work for signbundle. Proving reads the anchor's congestion multiplier from the header store, so -prove=1 works at any synced header, not only at genesis. Set -prove=0 for offline tests only; the transaction will not relay. (default: 1)", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::OPTIONS);

    argsman.AddCommand("status", "Print agent client header state");
    argsman.AddCommand("initheaders", "Create or refresh the local agent header store");
    argsman.AddCommand("requestheaders", "Print a getheaders payload for the current agent header tip");
    argsman.AddCommand("processheaders", "Process a hex-encoded headers payload and save accepted headers");
    argsman.AddCommand("processinv", "Process a hex-encoded transaction inv payload and print any getdata payload");
    argsman.AddCommand("processtx", "Process a hex-encoded tx payload");
    argsman.AddCommand("processaddr", "Import peers from a hex-encoded addr payload into the local agent peer store");
    argsman.AddCommand("processaddrv2", "Import peers from a hex-encoded addrv2 payload into the local agent peer store");
    argsman.AddCommand("importnodeaddresses", "Import peers from quicksilver-cli getnodeaddresses JSON into the local agent peer store");
    argsman.AddCommand("announcetx", "Print an inv payload for a hex-encoded tx payload");
    argsman.AddCommand("sendtx", "Print a tx payload for a hex-encoded tx payload");
    argsman.AddCommand("addpeer", "Add a configured relay peer to the local agent peer store");
    argsman.AddCommand("removepeer", "Remove a configured relay peer from the local agent peer store");
    argsman.AddCommand("listpeers", "List configured relay peers from the local agent peer store");
    argsman.AddCommand("discoverpeers", "Ask configured relay peers for address gossip and import discovered peers");
    argsman.AddCommand("syncheaderspeer", "Connect to configured relay peers, request headers, process returned headers, and save the local header store");
    argsman.AddCommand("sendtxpeer", "Connect to configured relay peers, complete a minimal handshake, and send a hex-encoded tx payload");
    argsman.AddCommand("checkpolicy", "Decode an agent allotment policy request and check a spend against its daily guardrail");
    argsman.AddCommand("checkbundle", "Decode an agent allotment policy bundle, merge payment receipts, verify its funding key, and check a spend against its daily guardrail");
    argsman.AddCommand("signbundle", "Create and sign an agent allotment spend from a desktop policy bundle and optional payment receipts");
    argsman.AddCommand("importreceipt", "Import one or more agent payment receipts into the local receipt store");
    argsman.AddCommand("importrecovery", "Import recovered funding outputs from a scantxoutset result into the local receipt store");
    argsman.AddCommand("scanreceipts", "Scan a directory of agent payment receipt JSON files and import newly discovered receipts");
    argsman.AddCommand("listreceipts", "List locally stored agent payment receipts");
    argsman.AddCommand("listreceiptactivity", "List local agent payment receipt activity");
}

static int AppInitAgent(ArgsManager& args, int argc, char* argv[])
{
    SetupAgentArgs(args);

    std::string error;
    if (!args.ParseParameters(argc, argv, error)) {
        tfm::format(std::cerr, "Error parsing command line arguments: %s\n", error);
        return EXIT_FAILURE;
    }

    const bool missing_args{argc < 2};
    if (missing_args || HelpRequested(args) || args.GetBoolArg("-version", false)) {
        std::string usage = "quicksilver-agent version " + FormatFullVersion() + "\n";

        if (args.GetBoolArg("-version", false)) {
            usage += FormatParagraph(LicenseInfo());
        } else {
            usage += "\n"
                     "The quicksilver-agent tool is the local command-line consumer of the agent client core.\n\n"
                     "It manages the thin header store used by software agents, checks desktop-issued agent allotment policy requests and key bundles, scans a local payment receipt inbox, imports scantxoutset recovery output, can sign a bundle spend from recorded funding outputs, and can relay a signed tx payload to configured peers. It can import local-node addrman exports, sync headers, and discover peers from explicit, stored, or fixed-seed peers, but does not perform durable coin refresh. Fixed onion seeds require a reachable SOCKS5 proxy configured with -proxy or -onion.\n"
                     "\n"
                     "Usage: quicksilver-agent [options] <command>\n"
                     "or:    quicksilver-agent [options] status\n"
                     "or:    quicksilver-agent [options] initheaders\n"
                     "or:    quicksilver-agent [options] requestheaders\n"
                     "or:    quicksilver-agent [options] -message=<hex|-> processheaders\n"
                     "or:    quicksilver-agent [options] -message=<hex|-> processinv\n"
                     "or:    quicksilver-agent [options] -message=<hex|-> processtx\n"
                     "or:    quicksilver-agent [options] -message=<hex|-> processaddr\n"
                     "or:    quicksilver-agent [options] -message=<hex|-> processaddrv2\n"
                     "or:    quicksilver-agent [options] -peeraddresses=<json|-> importnodeaddresses\n"
                     "or:    quicksilver-agent [options] -message=<hex|-> announcetx\n"
                     "or:    quicksilver-agent [options] -message=<hex|-> sendtx\n"
                     "or:    quicksilver-agent [options] -peer=<host[:port]> addpeer\n"
                     "or:    quicksilver-agent [options] -peer=<host[:port]> removepeer\n"
                     "or:    quicksilver-agent [options] listpeers\n"
                     "or:    quicksilver-agent [options] [-peer=<host[:port]>] discoverpeers\n"
                     "or:    quicksilver-agent [options] [-peer=<host[:port]>] syncheaderspeer\n"
                     "or:    quicksilver-agent [options] [-peer=<host[:port]>] -message=<hex|-> sendtxpeer\n"
                     "or:    quicksilver-agent [options] -policyrequest=<json|-> -spendamount=<cinnabar> [-spenttoday=<cinnabar>] checkpolicy\n"
                     "or:    quicksilver-agent [options] -policybundle=<json|-> [-paymentreceipt=<json|->...] -spendamount=<cinnabar> [-spenttoday=<cinnabar>] checkbundle\n"
                     "or:    quicksilver-agent [options] -policybundle=<json|-> [-paymentreceipt=<json|->...] -destination=<address> -spendamount=<cinnabar> [-spenttoday=<cinnabar>] signbundle\n"
                     "or:    quicksilver-agent [options] -policybundle=<json|-> -prevtxid=<hex> -prevout=<n> -prevamount=<cinnabar> -destination=<address> -spendamount=<cinnabar> [-spenttoday=<cinnabar>] signbundle\n"
                     "or:    quicksilver-agent [options] -paymentreceipt=<json|->... importreceipt\n"
                     "or:    quicksilver-agent [options] -fundingaddress=<address> -scantxoutset=<json|-> importrecovery\n"
                     "or:    quicksilver-agent [options] [-paymentreceiptdir=<dir>] scanreceipts\n"
                     "or:    quicksilver-agent [options] listreceipts\n"
                     "or:    quicksilver-agent [options] listreceiptactivity\n"
                     "\n";
            usage += "\n" + args.GetHelpMessage();
        }

        tfm::format(std::cout, "%s", usage);
        if (missing_args) {
            tfm::format(std::cerr, "Error: too few parameters\n");
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    if (!CheckDataDirOption(args)) {
        tfm::format(std::cerr, "Error: Specified data directory \"%s\" does not exist.\n", args.GetArg("-datadir", ""));
        return EXIT_FAILURE;
    }

    try {
        SelectParams(args.GetChainType());
    } catch (const std::exception& e) {
        tfm::format(std::cerr, "Error: %s\n", e.what());
        return EXIT_FAILURE;
    }

    const bool randomize_proxy_credentials{args.GetBoolArg("-proxyrandomize", DEFAULT_AGENT_PROXY_RANDOMIZE)};
    const auto parse_proxy = [&](const std::string& value, const char* option) -> std::optional<Proxy> {
        Proxy proxy;
        if (IsUnixSocketPath(value)) {
            proxy = Proxy{value, randomize_proxy_credentials};
        } else {
            const std::optional<CService> endpoint{Lookup(value, 9050, /*fAllowLookup=*/true)};
            if (!endpoint.has_value()) {
                tfm::format(std::cerr, "Error: invalid %s address or hostname: %s\n", option, value);
                return std::nullopt;
            }
            proxy = Proxy{*endpoint, randomize_proxy_credentials};
        }
        if (!proxy.IsValid()) {
            tfm::format(std::cerr, "Error: invalid %s address or hostname: %s\n", option, value);
            return std::nullopt;
        }
        return proxy;
    };

    std::optional<Proxy> default_proxy;
    const std::string proxy_arg{args.GetArg("-proxy", "")};
    if (!proxy_arg.empty() && proxy_arg != "0") {
        default_proxy = parse_proxy(proxy_arg, "-proxy");
        if (!default_proxy.has_value()) return EXIT_FAILURE;
        SetProxy(NET_IPV4, *default_proxy);
        SetProxy(NET_IPV6, *default_proxy);
        SetProxy(NET_CJDNS, *default_proxy);
    }

    std::optional<Proxy> onion_proxy{default_proxy};
    const std::string onion_arg{args.GetArg("-onion", "")};
    if (!onion_arg.empty()) {
        if (onion_arg == "0") {
            onion_proxy.reset();
        } else {
            onion_proxy = parse_proxy(onion_arg, "-onion");
            if (!onion_proxy.has_value()) return EXIT_FAILURE;
        }
    }
    if (onion_proxy.has_value()) {
        SetProxy(NET_ONION, *onion_proxy);
    }

    return CONTINUE_EXECUTION;
}

static fs::path HeaderStorePath(const ArgsManager& args)
{
    const fs::path configured_path{args.GetPathArg("-headerstore", fs::path{"agent"} / "headers.dat")};
    return fsbridge::AbsPathJoin(args.GetDataDirNet(), configured_path);
}

static fs::path ReceiptStorePath(const ArgsManager& args)
{
    const fs::path configured_path{args.GetPathArg("-receiptstore", fs::path{"agent"} / "payment-receipts.dat")};
    return fsbridge::AbsPathJoin(args.GetDataDirNet(), configured_path);
}

static fs::path PaymentReceiptDirectory(const ArgsManager& args)
{
    const fs::path configured_path{args.GetPathArg("-paymentreceiptdir", fs::path{"agent"} / "payment-receipts.d")};
    return fsbridge::AbsPathJoin(args.GetDataDirNet(), configured_path);
}

static fs::path RelayPeerStorePath(const ArgsManager& args)
{
    const fs::path configured_path{args.GetPathArg("-peerstore", fs::path{"agent"} / "relay-peers.json")};
    return fsbridge::AbsPathJoin(args.GetDataDirNet(), configured_path);
}

static void SetEnvVarOverwrite(const char* name, const std::string& value)
{
#ifdef WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

static bool GpuSolverConfigured()
{
    const char* solver{std::getenv("CUCKATOO_GPU_SOLVER")};
    return solver != nullptr && solver[0] != '\0';
}

static void ConfigureCuckatooSolverEnvironment(const ArgsManager& args)
{
    if (args.IsArgSet("-cuckatoosolver")) {
        SetEnvVarOverwrite("CUCKATOO_GPU_SOLVER", args.GetArg("-cuckatoosolver", ""));
    }
    if (args.IsArgSet("-cuckatoosolvertimeout")) {
        SetEnvVarOverwrite("CUCKATOO_GPU_TIMEOUT", args.GetArg("-cuckatoosolvertimeout", ""));
    }
}

static agent::AgentClient MakeAgentClient(const ArgsManager& args, const fs::path& header_store_path)
{
    const CChainParams& params = Params();
    agent::AgentClientOptions options;
    options.header_store_path = header_store_path;
    return agent::AgentClient{params.GetConsensus(), params.GenesisBlock(), std::move(options)};
}

static int PrintStatus(const ArgsManager& args)
{
    const fs::path header_store_path{HeaderStorePath(args)};
    const agent::AgentClient client{MakeAgentClient(args, header_store_path)};
    const agent::AgentClientLoadResult& load_result{client.LastLoadResult()};

    tfm::format(std::cout, "network=%s\n", Params().GetChainTypeString());
    tfm::format(std::cout, "header_store=%s\n", fs::PathToString(header_store_path));
    tfm::format(std::cout, "load_status=%s\n", agent::HeaderStoreResultString(load_result.status));
    if (!load_result.ok()) {
        tfm::format(std::cout, "invalid_header=%s\n", agent::HeaderAcceptCodeString(load_result.invalid_header.code));
    }
    tfm::format(std::cout, "header_height=%d\n", client.HeaderHeight());
    tfm::format(std::cout, "header_tip=%s\n", client.HeaderTip().GetBlockHash().ToString());
    tfm::format(std::cout, "peer_count=%s\n", util::ToString(client.PeerCount()));
    return load_result.ok() ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int InitHeaders(const ArgsManager& args)
{
    const fs::path header_store_path{HeaderStorePath(args)};
    const agent::AgentClient client{MakeAgentClient(args, header_store_path)};
    const agent::AgentClientLoadResult& load_result{client.LastLoadResult()};
    if (!load_result.ok()) {
        tfm::format(std::cerr, "Error: refusing to overwrite invalid header store %s: %s\n",
                    fs::PathToString(header_store_path),
                    agent::HeaderStoreResultString(load_result.status));
        return EXIT_FAILURE;
    }

    if (header_store_path.has_parent_path()) {
        fs::create_directories(header_store_path.parent_path());
    }

    const agent::HeaderStoreResult save_result{client.SaveHeaders()};
    if (save_result != agent::HeaderStoreResult::OK) {
        tfm::format(std::cerr, "Error: could not write header store %s: %s\n",
                    fs::PathToString(header_store_path),
                    agent::HeaderStoreResultString(save_result));
        return EXIT_FAILURE;
    }

    tfm::format(std::cout, "header_store=%s\n", fs::PathToString(header_store_path));
    tfm::format(std::cout, "header_height=%d\n", client.HeaderHeight());
    tfm::format(std::cout, "header_tip=%s\n", client.HeaderTip().GetBlockHash().ToString());
    return EXIT_SUCCESS;
}

static void PrintOutboundMessages(std::vector<CSerializedNetMsg> messages)
{
    tfm::format(std::cout, "outbound_count=%s\n", util::ToString(messages.size()));
    for (size_t i{0}; i < messages.size(); ++i) {
        tfm::format(std::cout, "outbound_%s_type=%s\n", util::ToString(i), messages[i].m_type);
        tfm::format(std::cout, "outbound_%s_payload=%s\n", util::ToString(i), agent::AgentMessagePayloadHex(messages[i]));
    }
}

static void PrintTransactionRelayPayloads(const CTransaction& transaction)
{
    const CSerializedNetMsg tx_message{agent::MakeTxMessage(transaction)};
    const CInv inventory{MSG_WTX, transaction.GetWitnessHash()};
    const CSerializedNetMsg inv_message{agent::MakeTxInvMessage(std::span{&inventory, 1})};
    tfm::format(std::cout, "tx_payload=%s\n", agent::AgentMessagePayloadHex(tx_message));
    tfm::format(std::cout, "inv_payload=%s\n", agent::AgentMessagePayloadHex(inv_message));
}

static std::string AgentPaymentReceiptJson(const std::string& funding_address,
                                           const agent::AllotmentFundingOutputArtifact& funding_output,
                                           int64_t received_time)
{
    return strprintf(
        R"({"type":"quicksilver.agent_payment_receipt","version":1,"chain":"%s","genesis_hash":"%s","funding_address":"%s","txid":"%s","vout":%s,"amount_cinnabar":"%s","received_time":"%s"})",
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash().ToString(),
        funding_address,
        funding_output.txid,
        util::ToString(funding_output.vout),
        util::ToString(funding_output.amount),
        util::ToString(received_time));
}

static agent::AllotmentPaymentReceiptArtifact MakeAgentPaymentReceipt(const std::string& funding_address,
                                                                        const agent::AllotmentFundingOutputArtifact& funding_output,
                                                                        int64_t received_time)
{
    return agent::AllotmentPaymentReceiptArtifact{
        .chain = Params().GetChainTypeString(),
        .genesis_hash = Params().GenesisBlock().GetHash().ToString(),
        .funding_address = funding_address,
        .funding_output = funding_output,
        .received_time = received_time,
        .payment_id = {},
        .label = {},
        .memo = {},
        .payer = {},
    };
}

static int RequestHeaders(const ArgsManager& args)
{
    const fs::path header_store_path{HeaderStorePath(args)};
    agent::AgentClient client{MakeAgentClient(args, header_store_path)};
    const agent::AgentClientLoadResult& load_result{client.LastLoadResult()};
    if (!load_result.ok()) {
        tfm::format(std::cerr, "Error: could not load header store %s: %s\n",
                    fs::PathToString(header_store_path),
                    agent::HeaderStoreResultString(load_result.status));
        return EXIT_FAILURE;
    }

    const auto action{client.StartHeaders()};
    tfm::format(std::cout, "action=%s\n", agent::AgentPeerActionCodeString(action.code));
    tfm::format(std::cout, "header_height=%d\n", client.HeaderHeight());
    tfm::format(std::cout, "header_tip=%s\n", client.HeaderTip().GetBlockHash().ToString());
    PrintOutboundMessages(client.DrainOutboundMessages());
    return action.ok() ? EXIT_SUCCESS : EXIT_FAILURE;
}

static std::optional<std::string> ReadPayloadHex(const ArgsManager& args)
{
    const std::optional<std::string> value{args.GetArg("-message")};
    if (!value.has_value()) {
        return std::nullopt;
    }
    if (*value != "-") {
        return util::TrimString(*value);
    }

    const std::string input{std::istreambuf_iterator<char>{std::cin}, std::istreambuf_iterator<char>{}};
    return util::TrimString(input);
}

static std::optional<std::string> ReadNodeAddressesArg(const ArgsManager& args)
{
    const std::optional<std::string> value{args.GetArg("-peeraddresses")};
    if (!value.has_value()) {
        return std::nullopt;
    }
    if (*value != "-") {
        return util::TrimString(*value);
    }

    const std::string input{std::istreambuf_iterator<char>{std::cin}, std::istreambuf_iterator<char>{}};
    return util::TrimString(input);
}

static std::optional<std::string> ReadPolicyRequestArg(const ArgsManager& args)
{
    const std::optional<std::string> value{args.GetArg("-policyrequest")};
    if (!value.has_value()) {
        return std::nullopt;
    }
    if (*value != "-") {
        return util::TrimString(*value);
    }

    const std::string input{std::istreambuf_iterator<char>{std::cin}, std::istreambuf_iterator<char>{}};
    return util::TrimString(input);
}

static std::optional<std::string> ReadPolicyBundleArg(const ArgsManager& args)
{
    const std::optional<std::string> value{args.GetArg("-policybundle")};
    if (!value.has_value()) {
        return std::nullopt;
    }
    if (*value != "-") {
        return util::TrimString(*value);
    }

    const std::string input{std::istreambuf_iterator<char>{std::cin}, std::istreambuf_iterator<char>{}};
    return util::TrimString(input);
}

static std::vector<std::string> ReadPaymentReceiptArgs(const ArgsManager& args)
{
    std::vector<std::string> receipts;
    for (const std::string& value : args.GetArgs("-paymentreceipt")) {
        if (value != "-") {
            receipts.push_back(util::TrimString(value));
            continue;
        }
        const std::string input{std::istreambuf_iterator<char>{std::cin}, std::istreambuf_iterator<char>{}};
        receipts.push_back(util::TrimString(input));
    }
    return receipts;
}

static std::optional<std::string> ReadScanTxOutsetArg(const ArgsManager& args)
{
    const std::optional<std::string> value{args.GetArg("-scantxoutset")};
    if (!value.has_value()) {
        return std::nullopt;
    }
    if (*value != "-") {
        return util::TrimString(*value);
    }

    const std::string input{std::istreambuf_iterator<char>{std::cin}, std::istreambuf_iterator<char>{}};
    return util::TrimString(input);
}

static std::optional<CAmount> ParseCinnabarArg(const ArgsManager& args,
                                               const char* arg_name,
                                               CAmount default_value,
                                               bool required)
{
    const std::optional<std::string> value{args.GetArg(arg_name)};
    if (!value.has_value()) {
        if (required) {
            tfm::format(std::cerr, "Error: %s is required\n", arg_name);
            return std::nullopt;
        }
        return default_value;
    }

    const auto amount{ToIntegral<CAmount>(util::TrimString(*value))};
    if (!amount || *amount < 0 || !MoneyRange(*amount)) {
        tfm::format(std::cerr, "Error: %s must be a valid nonnegative cinnabar amount\n", arg_name);
        return std::nullopt;
    }
    return *amount;
}

static std::optional<uint32_t> ParseUInt32Arg(const ArgsManager& args,
                                              const char* arg_name,
                                              uint32_t default_value,
                                              bool required)
{
    const std::optional<std::string> value{args.GetArg(arg_name)};
    if (!value.has_value()) {
        if (required) {
            tfm::format(std::cerr, "Error: %s is required\n", arg_name);
            return std::nullopt;
        }
        return default_value;
    }

    const auto parsed{ToIntegral<uint32_t>(util::TrimString(*value))};
    if (!parsed.has_value()) {
        tfm::format(std::cerr, "Error: %s must be a valid uint32 value\n", arg_name);
        return std::nullopt;
    }
    return *parsed;
}

static std::optional<CTxDestination> ParseDestinationArg(const ArgsManager& args, const char* arg_name)
{
    const std::optional<std::string> value{args.GetArg(arg_name)};
    if (!value.has_value() || util::TrimString(*value).empty()) {
        tfm::format(std::cerr, "Error: %s is required\n", arg_name);
        return std::nullopt;
    }

    const CTxDestination destination{DecodeDestination(util::TrimString(*value))};
    if (!IsValidDestination(destination)) {
        tfm::format(std::cerr, "Error: %s must be a valid address for this chain\n", arg_name);
        return std::nullopt;
    }
    return destination;
}

static std::optional<std::string> ParseFundingAddressArg(const ArgsManager& args, const char* arg_name)
{
    const std::optional<std::string> value{args.GetArg(arg_name)};
    if (!value.has_value() || util::TrimString(*value).empty()) {
        tfm::format(std::cerr, "Error: %s is required\n", arg_name);
        return std::nullopt;
    }

    const std::string address{util::TrimString(*value)};
    const CTxDestination destination{DecodeDestination(address)};
    if (!IsValidDestination(destination)) {
        tfm::format(std::cerr, "Error: %s must be a valid address for this chain\n", arg_name);
        return std::nullopt;
    }
    return address;
}

static std::optional<Txid> ParseTxidArg(const ArgsManager& args, const char* arg_name)
{
    const std::optional<std::string> value{args.GetArg(arg_name)};
    if (!value.has_value() || util::TrimString(*value).empty()) {
        tfm::format(std::cerr, "Error: %s is required\n", arg_name);
        return std::nullopt;
    }

    auto txid{Txid::FromHex(util::TrimString(*value))};
    if (!txid.has_value()) {
        tfm::format(std::cerr, "Error: %s must be a transaction id hex string\n", arg_name);
        return std::nullopt;
    }
    return *txid;
}

static bool FundingSecretMatchesAddress(const std::string& funding_secret, const std::string& funding_address, std::string& error)
{
    const CKey key{DecodeSecret(funding_secret)};
    if (!key.IsValid()) {
        error = "funding_secret_wif is not a valid private key for this chain";
        return false;
    }

    const CTxDestination dest{DecodeDestination(funding_address)};
    if (!IsValidDestination(dest)) {
        error = "funding_address is not valid for this chain";
        return false;
    }

    const CKeyID key_id{key.GetPubKey().GetID()};
    if (const auto* pkhash{std::get_if<PKHash>(&dest)}) {
        return ToKeyID(*pkhash) == key_id;
    }
    if (const auto* witness_hash{std::get_if<WitnessV0KeyHash>(&dest)}) {
        return ToKeyID(*witness_hash) == key_id;
    }

    error = "funding_address does not map to an importable single-key agent address";
    return false;
}

static util::Result<agent::AllotmentReceiptStoreData> LoadStoredPaymentReceiptStore(const ArgsManager& args)
{
    return agent::LoadAllotmentReceiptStore(ReceiptStorePath(args), Params().GetChainTypeString(), Params().GenesisBlock().GetHash());
}

static util::Result<std::vector<agent::AllotmentPaymentReceiptArtifact>> LoadStoredPaymentReceipts(const ArgsManager& args)
{
    auto data{LoadStoredPaymentReceiptStore(args)};
    if (!data) return util::Error{util::ErrorString(data)};
    return data->receipts;
}

static util::Result<void> SaveStoredPaymentReceiptStore(const ArgsManager& args,
                                                        const agent::AllotmentReceiptStoreData& data)
{
    const fs::path receipt_store_path{ReceiptStorePath(args)};
    if (receipt_store_path.has_parent_path()) {
        fs::create_directories(receipt_store_path.parent_path());
    }
    const agent::AllotmentStoreResult saved{agent::SaveAllotmentReceiptStore(data, receipt_store_path, Params().GenesisBlock().GetHash())};
    if (saved != agent::AllotmentStoreResult::OK) {
        return util::Error{Untranslated(strprintf("Could not write receipt store %s: %s", fs::PathToString(receipt_store_path), agent::AllotmentStoreResultString(saved)))};
    }
    return {};
}

static util::Result<size_t> AddReceiptsToStore(agent::AllotmentReceiptStoreData& store,
                                               const std::vector<agent::AllotmentPaymentReceiptArtifact>& receipts,
                                               std::vector<agent::AllotmentPaymentReceiptArtifact>* added_receipts = nullptr)
{
    return agent::AddAllotmentPaymentReceipts(store, receipts, added_receipts);
}

static util::Result<std::string> ReadRecoveryStringField(const UniValue& object, const std::string& key)
{
    const UniValue& field{object.find_value(key)};
    if (!field.isStr()) {
        return util::Error{Untranslated(strprintf("scantxoutset unspent field '%s' must be a string.", key))};
    }
    const std::string value{util::TrimString(field.get_str())};
    if (value.empty()) {
        return util::Error{Untranslated(strprintf("scantxoutset unspent field '%s' must not be empty.", key))};
    }
    return value;
}

static util::Result<uint32_t> ReadRecoveryVoutField(const UniValue& object)
{
    const UniValue& field{object.find_value("vout")};
    if (!field.isNum()) {
        return util::Error{Untranslated("scantxoutset unspent field 'vout' must be a number.")};
    }
    try {
        return field.getInt<uint32_t>();
    } catch (const std::exception&) {
        return util::Error{Untranslated("scantxoutset unspent field 'vout' must be a valid uint32 value.")};
    }
}

static util::Result<CAmount> ReadRecoveryAmountField(const UniValue& object)
{
    const UniValue& field{object.find_value("amount")};
    if (!field.isNum() && !field.isStr()) {
        return util::Error{Untranslated("scantxoutset unspent field 'amount' must be a number or string.")};
    }

    const auto amount{ParseMoney(field.getValStr())};
    if (!amount || *amount <= 0 || !MoneyRange(*amount)) {
        return util::Error{Untranslated("scantxoutset unspent field 'amount' must be a valid positive quicksilver amount.")};
    }
    return *amount;
}

static util::Result<std::vector<agent::AllotmentPaymentReceiptArtifact>> DecodeScanTxOutsetRecoveryReceipts(const std::string& scan_json,
                                                                                                              const std::string& funding_address)
{
    UniValue result{UniValue::VOBJ};
    if (!result.read(scan_json) || !result.isObject()) {
        return util::Error{Untranslated("scantxoutset result must be a JSON object.")};
    }

    const UniValue& success{result.find_value("success")};
    if (success.isBool() && !success.get_bool()) {
        return util::Error{Untranslated("scantxoutset result did not complete successfully.")};
    }

    const UniValue& unspents{result.find_value("unspents")};
    if (!unspents.isArray()) {
        return util::Error{Untranslated("scantxoutset result must contain an unspents array.")};
    }

    const int64_t import_time{GetTime()};
    std::vector<agent::AllotmentPaymentReceiptArtifact> receipts;
    receipts.reserve(unspents.size());
    for (const UniValue& unspent : unspents.get_array().getValues()) {
        if (!unspent.isObject()) {
            return util::Error{Untranslated("scantxoutset unspent entry must be a JSON object.")};
        }

        auto txid{ReadRecoveryStringField(unspent, "txid")};
        if (!txid) return util::Error{util::ErrorString(txid)};
        if (!Txid::FromHex(*txid).has_value()) {
            return util::Error{Untranslated("scantxoutset unspent field 'txid' must be a transaction id hex string.")};
        }
        auto vout{ReadRecoveryVoutField(unspent)};
        if (!vout) return util::Error{util::ErrorString(vout)};
        auto amount{ReadRecoveryAmountField(unspent)};
        if (!amount) return util::Error{util::ErrorString(amount)};

        std::string memo{"scantxoutset recovery"};
        const UniValue& height{unspent.find_value("height")};
        if (height.isNum()) {
            memo += strprintf("; height=%s", height.getValStr());
        }
        const UniValue& blockhash{unspent.find_value("blockhash")};
        if (blockhash.isStr() && !blockhash.get_str().empty()) {
            memo += strprintf("; blockhash=%s", blockhash.get_str());
        }

        receipts.push_back(agent::AllotmentPaymentReceiptArtifact{
            .chain = Params().GetChainTypeString(),
            .genesis_hash = Params().GenesisBlock().GetHash().ToString(),
            .funding_address = funding_address,
            .funding_output = agent::AllotmentFundingOutputArtifact{
                .txid = *txid,
                .vout = *vout,
                .amount = *amount,
            },
            .received_time = import_time,
            .payment_id = strprintf("scantxoutset:%s:%s", *txid, util::ToString(*vout)),
            .label = "Recovered output",
            .memo = memo,
            .payer = "scantxoutset",
        });
    }
    return receipts;
}



static agent::AllotmentReceiptActivity MakeReceiptActivity(agent::AllotmentReceiptActivityType type,
                                                             const agent::AllotmentPaymentReceiptArtifact& receipt,
                                                             int64_t event_time,
                                                             std::string related_txid = {})
{
    return agent::AllotmentReceiptActivity{
        .type = type,
        .funding_address = receipt.funding_address,
        .funding_output = receipt.funding_output,
        .event_time = event_time,
        .related_txid = std::move(related_txid),
        .payment_id = receipt.payment_id,
        .label = receipt.label,
        .memo = receipt.memo,
        .payer = receipt.payer,
    };
}

static void PrintReceiptMetadata(const std::string& prefix, const agent::AllotmentPaymentReceiptArtifact& receipt)
{
    if (!receipt.payment_id.empty()) tfm::format(std::cout, "%s_payment_id=%s\n", prefix, receipt.payment_id);
    if (!receipt.label.empty()) tfm::format(std::cout, "%s_label=%s\n", prefix, receipt.label);
    if (!receipt.memo.empty()) tfm::format(std::cout, "%s_memo=%s\n", prefix, receipt.memo);
    if (!receipt.payer.empty()) tfm::format(std::cout, "%s_payer=%s\n", prefix, receipt.payer);
}

static void PrintReceiptMetadata(const std::string& prefix, const agent::AllotmentReceiptActivity& activity)
{
    if (!activity.payment_id.empty()) tfm::format(std::cout, "%s_payment_id=%s\n", prefix, activity.payment_id);
    if (!activity.label.empty()) tfm::format(std::cout, "%s_label=%s\n", prefix, activity.label);
    if (!activity.memo.empty()) tfm::format(std::cout, "%s_memo=%s\n", prefix, activity.memo);
    if (!activity.payer.empty()) tfm::format(std::cout, "%s_payer=%s\n", prefix, activity.payer);
}

static std::vector<agent::AllotmentFundingOutputArtifact> FundingOutputsFromSpendInputs(const std::vector<agent::AllotmentSpendInput>& inputs)
{
    std::vector<agent::AllotmentFundingOutputArtifact> outputs;
    outputs.reserve(inputs.size());
    for (const agent::AllotmentSpendInput& input : inputs) {
        outputs.push_back(agent::AllotmentFundingOutputArtifact{
            .txid = input.prevout.hash.ToString(),
            .vout = input.prevout.n,
            .amount = input.amount,
        });
    }
    return outputs;
}

static std::optional<CAmount> ResolveSpentToday(const ArgsManager& args,
                                                const agent::AllotmentReceiptStoreData* store,
                                                std::string_view funding_address)
{
    if (args.IsArgSet("-spenttoday")) {
        return ParseCinnabarArg(args, "-spenttoday", 0, /*required=*/false);
    }
    if (store == nullptr) return CAmount{0};
    auto spent_today{agent::SpentTodayFromActivities(store->activities, funding_address, GetTime())};
    if (!spent_today) {
        tfm::format(std::cerr, "Error: %s\n", util::ErrorString(spent_today).original);
        return std::nullopt;
    }
    return *spent_today;
}

static util::Result<std::vector<agent::AllotmentPaymentReceiptArtifact>> DecodePaymentReceiptArgs(const ArgsManager& args,
                                                                                                    std::optional<std::string> expected_funding_address = std::nullopt)
{
    const std::vector<std::string> receipts{ReadPaymentReceiptArgs(args)};
    std::vector<agent::AllotmentPaymentReceiptArtifact> decoded_receipts;
    decoded_receipts.reserve(receipts.size());

    for (const std::string& receipt_json : receipts) {
        if (receipt_json.empty()) {
            return util::Error{Untranslated("Agent allotment payment receipt must not be empty.")};
        }
        auto receipt{agent::DecodeAllotmentPaymentReceipt(
            receipt_json,
            Params().GetChainTypeString(),
            Params().GenesisBlock().GetHash().ToString(),
            expected_funding_address.value_or(std::string{}))};
        if (!receipt) return util::Error{util::ErrorString(receipt)};
        decoded_receipts.push_back(*receipt);
    }
    return decoded_receipts;
}

static util::Result<agent::AllotmentReceiptStoreData> ApplyStoredReceiptsToBundle(agent::AllotmentPolicyBundleArtifact& bundle,
                                                                                   const ArgsManager& args)
{
    auto store{LoadStoredPaymentReceiptStore(args)};
    if (!store) return util::Error{util::ErrorString(store)};
    auto arg_receipts{DecodePaymentReceiptArgs(args, bundle.funding_address)};
    if (!arg_receipts) return util::Error{util::ErrorString(arg_receipts)};

    std::vector<agent::AllotmentPaymentReceiptArtifact> receipts{store->receipts};
    receipts.insert(receipts.end(), arg_receipts->begin(), arg_receipts->end());
    auto applied{agent::ApplyPaymentReceiptsToBundle(bundle, receipts, store->activities)};
    if (!applied) return util::Error{util::ErrorString(applied)};
    return store;
}

static int CheckPolicyArtifact(const agent::AllotmentPolicyArtifact& artifact, CAmount spend_amount, CAmount spent_today)
{
    const agent::AllotmentPolicyState state{
        .funding_available = artifact.funding_available,
        .spent_today = spent_today,
    };
    const agent::AllotmentPolicyCheck check{agent::CheckAllotmentSpend(artifact.policy, state, spend_amount)};

    tfm::format(std::cout, "policy_id=%s\n", artifact.id);
    tfm::format(std::cout, "policy_label=%s\n", artifact.label);
    tfm::format(std::cout, "policy_status=%s\n", artifact.policy_status);
    tfm::format(std::cout, "funding_address=%s\n", artifact.funding_address);
    tfm::format(std::cout, "funding_limit_cinnabar=%s\n", util::ToString(artifact.policy.funding_limit));
    tfm::format(std::cout, "funding_available_cinnabar=%s\n", util::ToString(artifact.funding_available));
    tfm::format(std::cout, "daily_limit_cinnabar=%s\n", util::ToString(artifact.policy.daily_limit));
    tfm::format(std::cout, "spent_today_cinnabar=%s\n", util::ToString(spent_today));
    tfm::format(std::cout, "spend_amount_cinnabar=%s\n", util::ToString(spend_amount));
    tfm::format(std::cout, "policy_result=%s\n", agent::AllotmentPolicyResultCodeString(check.code));
    tfm::format(std::cout, "daily_remaining_cinnabar=%s\n", util::ToString(check.daily_remaining));
    tfm::format(std::cout, "allowed=%s\n", check.allowed() ? "true" : "false");
    return check.allowed() ? EXIT_SUCCESS : EXIT_FAILURE;
}

static bool LoadResultOk(const fs::path& header_store_path, const agent::AgentClientLoadResult& load_result);
static void PrintTransaction(const CTransaction& transaction);

static int CheckPolicy(const ArgsManager& args)
{
    const std::optional<std::string> policy_request{ReadPolicyRequestArg(args)};
    if (!policy_request.has_value() || policy_request->empty()) {
        tfm::format(std::cerr, "Error: checkpolicy requires -policyrequest=<json> or -policyrequest=-\n");
        return EXIT_FAILURE;
    }

    const std::optional<CAmount> spend_amount{ParseCinnabarArg(args, "-spendamount", 0, /*required=*/true)};
    if (!spend_amount.has_value()) return EXIT_FAILURE;
    const std::optional<CAmount> spent_today{ResolveSpentToday(args, /*store=*/nullptr, {})};
    if (!spent_today.has_value()) return EXIT_FAILURE;

    auto artifact{agent::DecodeAllotmentPolicyRequest(
        *policy_request,
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash().ToString())};
    if (!artifact) {
        tfm::format(std::cerr, "Error: %s\n", util::ErrorString(artifact).original);
        return EXIT_FAILURE;
    }

    return CheckPolicyArtifact(*artifact, *spend_amount, *spent_today);
}

static int CheckBundle(const ArgsManager& args)
{
    const std::optional<std::string> policy_bundle{ReadPolicyBundleArg(args)};
    if (!policy_bundle.has_value() || policy_bundle->empty()) {
        tfm::format(std::cerr, "Error: checkbundle requires -policybundle=<json> or -policybundle=-\n");
        return EXIT_FAILURE;
    }

    const std::optional<CAmount> spend_amount{ParseCinnabarArg(args, "-spendamount", 0, /*required=*/true)};
    if (!spend_amount.has_value()) return EXIT_FAILURE;

    auto bundle{agent::DecodeAllotmentPolicyBundle(
        *policy_bundle,
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash().ToString())};
    if (!bundle) {
        tfm::format(std::cerr, "Error: %s\n", util::ErrorString(bundle).original);
        return EXIT_FAILURE;
    }
    auto store{ApplyStoredReceiptsToBundle(*bundle, args)};
    if (!store) {
        tfm::format(std::cerr, "Error: %s\n", util::ErrorString(store).original);
        return EXIT_FAILURE;
    }
    const std::optional<CAmount> spent_today{ResolveSpentToday(args, &*store, bundle->funding_address)};
    if (!spent_today.has_value()) return EXIT_FAILURE;

    std::string key_error;
    if (!FundingSecretMatchesAddress(bundle->funding_secret, bundle->funding_address, key_error)) {
        tfm::format(std::cerr, "Error: %s\n", key_error);
        return EXIT_FAILURE;
    }

    tfm::format(std::cout, "bundle_type=quicksilver.agent_allotment_key_bundle\n");
    tfm::format(std::cout, "funding_secret_valid=true\n");
    tfm::format(std::cout, "funding_secret_matches_address=true\n");
    CAmount funding_output_total{0};
    for (const agent::AllotmentFundingOutputArtifact& output : bundle->funding_outputs) {
        if (!MoneyRange(funding_output_total + output.amount)) {
            tfm::format(std::cerr, "Error: bundle funding output total is out of range\n");
            return EXIT_FAILURE;
        }
        funding_output_total += output.amount;
    }
    tfm::format(std::cout, "funding_output_count=%s\n", util::ToString(bundle->funding_outputs.size()));
    tfm::format(std::cout, "funding_output_total_cinnabar=%s\n", util::ToString(funding_output_total));
    for (size_t i{0}; i < bundle->funding_outputs.size(); ++i) {
        const agent::AllotmentFundingOutputArtifact& output{bundle->funding_outputs[i]};
        tfm::format(std::cout, "funding_output_%s=%s:%s\n", util::ToString(i), output.txid, util::ToString(output.vout));
        tfm::format(std::cout, "funding_output_%s_amount_cinnabar=%s\n", util::ToString(i), util::ToString(output.amount));
    }
    return CheckPolicyArtifact(bundle->policy_request, *spend_amount, *spent_today);
}

static int ImportPaymentReceipts(const ArgsManager& args)
{
    auto decoded_receipts{DecodePaymentReceiptArgs(args)};
    if (!decoded_receipts) {
        tfm::format(std::cerr, "Error: %s\n", util::ErrorString(decoded_receipts).original);
        return EXIT_FAILURE;
    }
    if (decoded_receipts->empty()) {
        tfm::format(std::cerr, "Error: importreceipt requires -paymentreceipt=<json> or -paymentreceipt=-\n");
        return EXIT_FAILURE;
    }

    auto store{LoadStoredPaymentReceiptStore(args)};
    if (!store) {
        tfm::format(std::cerr, "Error: %s\n", util::ErrorString(store).original);
        return EXIT_FAILURE;
    }
    std::vector<agent::AllotmentPaymentReceiptArtifact> added_receipts;
    auto added{AddReceiptsToStore(*store, *decoded_receipts, &added_receipts)};
    if (!added) {
        tfm::format(std::cerr, "Error: %s\n", util::ErrorString(added).original);
        return EXIT_FAILURE;
    }
    for (const agent::AllotmentPaymentReceiptArtifact& receipt : added_receipts) {
        store->activities.push_back(MakeReceiptActivity(agent::AllotmentReceiptActivityType::IMPORTED, receipt, receipt.received_time));
    }
    auto saved{SaveStoredPaymentReceiptStore(args, *store)};
    if (!saved) {
        tfm::format(std::cerr, "Error: %s\n", util::ErrorString(saved).original);
        return EXIT_FAILURE;
    }

    tfm::format(std::cout, "receipt_store=%s\n", fs::PathToString(ReceiptStorePath(args)));
    tfm::format(std::cout, "imported_receipts=%s\n", util::ToString(*added));
    tfm::format(std::cout, "stored_receipt_count=%s\n", util::ToString(store->receipts.size()));
    tfm::format(std::cout, "receipt_activity_count=%s\n", util::ToString(store->activities.size()));
    return EXIT_SUCCESS;
}

static int ImportRecoveredPaymentReceipts(const ArgsManager& args)
{
    const std::optional<std::string> scan_json{ReadScanTxOutsetArg(args)};
    if (!scan_json.has_value() || scan_json->empty()) {
        tfm::format(std::cerr, "Error: importrecovery requires -scantxoutset=<json> or -scantxoutset=-\n");
        return EXIT_FAILURE;
    }
    const std::optional<std::string> funding_address{ParseFundingAddressArg(args, "-fundingaddress")};
    if (!funding_address.has_value()) {
        return EXIT_FAILURE;
    }

    auto decoded_receipts{DecodeScanTxOutsetRecoveryReceipts(*scan_json, *funding_address)};
    if (!decoded_receipts) {
        tfm::format(std::cerr, "Error: %s\n", util::ErrorString(decoded_receipts).original);
        return EXIT_FAILURE;
    }

    auto store{LoadStoredPaymentReceiptStore(args)};
    if (!store) {
        tfm::format(std::cerr, "Error: %s\n", util::ErrorString(store).original);
        return EXIT_FAILURE;
    }

    std::vector<agent::AllotmentPaymentReceiptArtifact> added_receipts;
    auto added{AddReceiptsToStore(*store, *decoded_receipts, &added_receipts)};
    if (!added) {
        tfm::format(std::cerr, "Error: %s\n", util::ErrorString(added).original);
        return EXIT_FAILURE;
    }
    for (const agent::AllotmentPaymentReceiptArtifact& receipt : added_receipts) {
        store->activities.push_back(MakeReceiptActivity(agent::AllotmentReceiptActivityType::IMPORTED, receipt, receipt.received_time));
    }

    auto saved{SaveStoredPaymentReceiptStore(args, *store)};
    if (!saved) {
        tfm::format(std::cerr, "Error: %s\n", util::ErrorString(saved).original);
        return EXIT_FAILURE;
    }

    tfm::format(std::cout, "funding_address=%s\n", *funding_address);
    tfm::format(std::cout, "decoded_recovery_outputs=%s\n", util::ToString(decoded_receipts->size()));
    tfm::format(std::cout, "imported_receipts=%s\n", util::ToString(*added));
    tfm::format(std::cout, "stored_receipt_count=%s\n", util::ToString(store->receipts.size()));
    tfm::format(std::cout, "receipt_activity_count=%s\n", util::ToString(store->activities.size()));
    tfm::format(std::cout, "receipt_store=%s\n", fs::PathToString(ReceiptStorePath(args)));
    return EXIT_SUCCESS;
}

static int ScanPaymentReceiptDirectory(const ArgsManager& args)
{
    const fs::path receipt_directory{PaymentReceiptDirectory(args)};
    auto scan{agent::ScanAllotmentPaymentReceiptDirectory(
        receipt_directory,
        ReceiptStorePath(args),
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash())};
    if (!scan) {
        tfm::format(std::cerr, "Error: %s\n", util::ErrorString(scan).original);
        return EXIT_FAILURE;
    }

    tfm::format(std::cout, "payment_receipt_dir=%s\n", fs::PathToString(receipt_directory));
    tfm::format(std::cout, "scanned_receipt_files=%s\n", util::ToString(scan->scanned_files));
    tfm::format(std::cout, "decoded_receipts=%s\n", util::ToString(scan->scanned_files));
    tfm::format(std::cout, "imported_receipts=%s\n", util::ToString(scan->imported_receipts));
    tfm::format(std::cout, "stored_receipt_count=%s\n", util::ToString(scan->store.receipts.size()));
    tfm::format(std::cout, "receipt_activity_count=%s\n", util::ToString(scan->store.activities.size()));
    tfm::format(std::cout, "receipt_store=%s\n", fs::PathToString(ReceiptStorePath(args)));
    return EXIT_SUCCESS;
}

static int ListPaymentReceipts(const ArgsManager& args)
{
    auto stored_receipts{LoadStoredPaymentReceipts(args)};
    if (!stored_receipts) {
        tfm::format(std::cerr, "Error: %s\n", util::ErrorString(stored_receipts).original);
        return EXIT_FAILURE;
    }

    tfm::format(std::cout, "receipt_store=%s\n", fs::PathToString(ReceiptStorePath(args)));
    tfm::format(std::cout, "stored_receipt_count=%s\n", util::ToString(stored_receipts->size()));
    for (size_t i{0}; i < stored_receipts->size(); ++i) {
        const agent::AllotmentPaymentReceiptArtifact& receipt{(*stored_receipts)[i]};
        tfm::format(std::cout, "receipt_%s_funding_address=%s\n", util::ToString(i), receipt.funding_address);
        tfm::format(std::cout, "receipt_%s_output=%s:%s\n", util::ToString(i), receipt.funding_output.txid, util::ToString(receipt.funding_output.vout));
        tfm::format(std::cout, "receipt_%s_amount_cinnabar=%s\n", util::ToString(i), util::ToString(receipt.funding_output.amount));
        tfm::format(std::cout, "receipt_%s_received_time=%s\n", util::ToString(i), util::ToString(receipt.received_time));
        PrintReceiptMetadata(strprintf("receipt_%s", util::ToString(i)), receipt);
    }
    return EXIT_SUCCESS;
}

static int ListPaymentReceiptActivity(const ArgsManager& args)
{
    auto store{LoadStoredPaymentReceiptStore(args)};
    if (!store) {
        tfm::format(std::cerr, "Error: %s\n", util::ErrorString(store).original);
        return EXIT_FAILURE;
    }

    tfm::format(std::cout, "receipt_store=%s\n", fs::PathToString(ReceiptStorePath(args)));
    tfm::format(std::cout, "receipt_activity_count=%s\n", util::ToString(store->activities.size()));
    for (size_t i{0}; i < store->activities.size(); ++i) {
        const agent::AllotmentReceiptActivity& activity{store->activities[i]};
        tfm::format(std::cout, "activity_%s_type=%s\n", util::ToString(i), agent::AllotmentReceiptActivityTypeString(activity.type));
        tfm::format(std::cout, "activity_%s_funding_address=%s\n", util::ToString(i), activity.funding_address);
        tfm::format(std::cout, "activity_%s_output=%s:%s\n", util::ToString(i), activity.funding_output.txid, util::ToString(activity.funding_output.vout));
        tfm::format(std::cout, "activity_%s_amount_cinnabar=%s\n", util::ToString(i), util::ToString(activity.funding_output.amount));
        tfm::format(std::cout, "activity_%s_event_time=%s\n", util::ToString(i), util::ToString(activity.event_time));
        if (!activity.related_txid.empty()) {
            tfm::format(std::cout, "activity_%s_related_txid=%s\n", util::ToString(i), activity.related_txid);
        }
        PrintReceiptMetadata(strprintf("activity_%s", util::ToString(i)), activity);
    }
    return EXIT_SUCCESS;
}

static int SignBundle(const ArgsManager& args)
{
    const std::optional<std::string> policy_bundle{ReadPolicyBundleArg(args)};
    if (!policy_bundle.has_value() || policy_bundle->empty()) {
        tfm::format(std::cerr, "Error: signbundle requires -policybundle=<json> or -policybundle=-\n");
        return EXIT_FAILURE;
    }

    const std::optional<CTxDestination> destination{ParseDestinationArg(args, "-destination")};
    if (!destination.has_value()) return EXIT_FAILURE;
    const std::optional<CAmount> spend_amount{ParseCinnabarArg(args, "-spendamount", 0, /*required=*/true)};
    if (!spend_amount.has_value()) return EXIT_FAILURE;

    const fs::path header_store_path{HeaderStorePath(args)};
    agent::AgentClient client{MakeAgentClient(args, header_store_path)};
    const agent::AgentClientLoadResult& load_result{client.LastLoadResult()};
    if (!LoadResultOk(header_store_path, load_result)) {
        return EXIT_FAILURE;
    }

    auto context{agent::ImportAllotmentBundle(
        *policy_bundle,
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash().ToString())};
    if (!context) {
        tfm::format(std::cerr, "Error: %s\n", util::ErrorString(context).original);
        return EXIT_FAILURE;
    }
    auto applied_store{ApplyStoredReceiptsToBundle(context->bundle, args)};
    if (!applied_store) {
        tfm::format(std::cerr, "Error: %s\n", util::ErrorString(applied_store).original);
        return EXIT_FAILURE;
    }
    const std::optional<CAmount> spent_today{ResolveSpentToday(args, &*applied_store, context->bundle.funding_address)};
    if (!spent_today.has_value()) return EXIT_FAILURE;

    const bool prove{args.GetBoolArg("-prove", true)};
    const bool allow_cpu_txpow{args.GetBoolArg("-allowcputxpow", false)};
    // Only when the processor is about to be used. Sandbox never needs the opt-in,
    // a configured GPU solver is tried first, and -prove=0 does not grind at all.
    // Printed before the call: the message is useless once the minutes are gone.
    if (allow_cpu_txpow && prove && Params().GetConsensus().nTxEdgeBits == 28 && !GpuSolverConfigured()) {
        tfm::format(std::cerr, "Notice: no GPU solver is configured, so this spend will be proved on the processor. This takes many minutes and will use every core.\n");
    }

    // Ctrl-C during a grind used to kill the agent outright and leave qsgpusolve
    // running with the card pinned: the solver sits in its own process group, so
    // the terminal's SIGINT never reached it. Catching it here lets the prove
    // loop stop between attempts and the solver bridge kill its own child.
    const agent::InterruptHandler interrupt;
    const cuckatoo::SolverCancelCallback cancel{interrupt.Cancel()};

    const bool has_prev_txid{args.IsArgSet("-prevtxid")};
    const bool has_prevout{args.IsArgSet("-prevout")};
    const bool has_prevamount{args.IsArgSet("-prevamount")};
    auto signed_spend{[&]() -> util::Result<agent::AllotmentSignedSpend> {
        if (!(has_prev_txid || has_prevout || has_prevamount)) {
            return agent::CreateSignedAllotmentSpendFromBundleOutputs(
                *context,
                agent::AllotmentBundleSpendRequest{
                    .destination = *destination,
                    .spend_amount = *spend_amount,
                    .spent_today = *spent_today,
                    .prove = prove,
                    .anchor = &client.HeaderTip(),
                    .cancel = cancel,
                    .allow_cpu_txpow = allow_cpu_txpow,
                },
                Params().GetConsensus());
        }

        if (!has_prev_txid || !has_prevout || !has_prevamount) {
            return util::Error{Untranslated("-prevtxid, -prevout, and -prevamount must be provided together")};
        }
        const std::optional<Txid> prev_txid{ParseTxidArg(args, "-prevtxid")};
        if (!prev_txid.has_value()) return util::Error{Untranslated("Invalid -prevtxid")};
        const std::optional<uint32_t> prevout_index{ParseUInt32Arg(args, "-prevout", 0, /*required=*/true)};
        if (!prevout_index.has_value()) return util::Error{Untranslated("Invalid -prevout")};
        const std::optional<CAmount> prevout_amount{ParseCinnabarArg(args, "-prevamount", 0, /*required=*/true)};
        if (!prevout_amount.has_value()) return util::Error{Untranslated("Invalid -prevamount")};

        return agent::CreateSignedAllotmentSpend(
            *context,
            agent::AllotmentSpendRequest{
                .prevout = COutPoint{*prev_txid, *prevout_index},
                .prevout_value = *prevout_amount,
                .destination = *destination,
                .spend_amount = *spend_amount,
                .spent_today = *spent_today,
                .prove = prove,
                .anchor = &client.HeaderTip(),
                .cancel = cancel,
                .allow_cpu_txpow = allow_cpu_txpow,
            },
            Params().GetConsensus());
    }()};
    if (!signed_spend) {
        tfm::format(std::cerr, "Error: %s\n", util::ErrorString(signed_spend).original);
        return EXIT_FAILURE;
    }

    std::optional<agent::AllotmentPaymentReceiptArtifact> change_receipt;
    size_t stored_spent_receipts_removed{0};
    size_t stored_change_receipts_added{0};
    bool receipt_store_saved{false};
    auto store{LoadStoredPaymentReceiptStore(args)};
    if (!store) {
        tfm::format(std::cerr, "Error: %s\n", util::ErrorString(store).original);
        return EXIT_FAILURE;
    }
    const int64_t activity_time{GetTime()};
    const std::string spend_txid{signed_spend->transaction.GetHash().ToUint256().ToString()};
    std::vector<agent::AllotmentPaymentReceiptArtifact> spent_receipts;
    const std::vector<agent::AllotmentFundingOutputArtifact> spent_outputs{FundingOutputsFromSpendInputs(signed_spend->inputs)};
    stored_spent_receipts_removed = agent::RemoveSpentReceipts(store->receipts, spent_outputs, &spent_receipts);
    agent::AppendSpentActivities(*store, context->bundle.funding_address, spent_outputs, spent_receipts, activity_time, spend_txid);
    if (signed_spend->change_amount > 0) {
        change_receipt = MakeAgentPaymentReceipt(
            context->bundle.funding_address,
            agent::AllotmentFundingOutputArtifact{
                .txid = spend_txid,
                .vout = 1,
                .amount = signed_spend->change_amount,
            },
            activity_time);
        std::vector<agent::AllotmentPaymentReceiptArtifact> change_receipts_added;
        auto added{AddReceiptsToStore(*store, {*change_receipt}, &change_receipts_added)};
        if (!added) {
            tfm::format(std::cerr, "Error: %s\n", util::ErrorString(added).original);
            return EXIT_FAILURE;
        }
        stored_change_receipts_added = *added;
        for (const agent::AllotmentPaymentReceiptArtifact& receipt : change_receipts_added) {
            store->activities.push_back(MakeReceiptActivity(agent::AllotmentReceiptActivityType::CHANGE, receipt, activity_time, spend_txid));
        }
    }
    if (!spent_outputs.empty() || stored_change_receipts_added > 0) {
        auto saved{SaveStoredPaymentReceiptStore(args, *store)};
        if (!saved) {
            tfm::format(std::cerr, "Error: %s\n", util::ErrorString(saved).original);
            return EXIT_FAILURE;
        }
        receipt_store_saved = true;
    }

    tfm::format(std::cout, "policy_id=%s\n", context->bundle.policy_request.id);
    tfm::format(std::cout, "funding_address=%s\n", context->bundle.funding_address);
    tfm::format(std::cout, "selected_input_count=%s\n", util::ToString(signed_spend->inputs.size()));
    for (size_t i{0}; i < signed_spend->inputs.size(); ++i) {
        const agent::AllotmentSpendInput& input{signed_spend->inputs[i]};
        tfm::format(std::cout, "selected_input_%s=%s:%s\n", util::ToString(i), input.prevout.hash.ToString(), util::ToString(input.prevout.n));
        tfm::format(std::cout, "selected_input_%s_amount_cinnabar=%s\n", util::ToString(i), util::ToString(input.amount));
    }
    tfm::format(std::cout, "input_amount_cinnabar=%s\n", util::ToString(signed_spend->input_amount));
    tfm::format(std::cout, "spend_amount_cinnabar=%s\n", util::ToString(*spend_amount));
    tfm::format(std::cout, "change_amount_cinnabar=%s\n", util::ToString(signed_spend->change_amount));
    tfm::format(std::cout, "policy_result=%s\n", agent::AllotmentPolicyResultCodeString(signed_spend->policy_check.code));
    tfm::format(std::cout, "proved=%s\n", prove ? "true" : "false");
    tfm::format(std::cout, "anchor_height=%s\n", util::ToString(signed_spend->transaction.nAnchorHeight));
    tfm::format(std::cout, "anchor_hash=%s\n", client.HeaderTip().GetBlockHash().ToString());
    PrintTransaction(signed_spend->transaction);
    tfm::format(std::cout, "hex=%s\n", EncodeHexTx(signed_spend->transaction));
    PrintTransactionRelayPayloads(signed_spend->transaction);
    if (change_receipt.has_value()) {
        tfm::format(std::cout,
                    "change_paymentreceipt=%s\n",
                    AgentPaymentReceiptJson(
                        change_receipt->funding_address,
                        change_receipt->funding_output,
                        change_receipt->received_time));
    }
    tfm::format(std::cout, "receipt_store_saved=%s\n", receipt_store_saved ? "true" : "false");
    tfm::format(std::cout, "receipt_store_spent_removed=%s\n", util::ToString(stored_spent_receipts_removed));
    tfm::format(std::cout, "receipt_store_change_added=%s\n", util::ToString(stored_change_receipts_added));
    tfm::format(std::cout, "receipt_activity_count=%s\n", util::ToString(store->activities.size()));
    tfm::format(std::cout, "receipt_store=%s\n", fs::PathToString(ReceiptStorePath(args)));
    return EXIT_SUCCESS;
}

static std::optional<agent::AgentMessageDecodeResult> DecodePayloadArg(const ArgsManager& args,
                                                                       const char* command,
                                                                       const char* message_type)
{
    const std::optional<std::string> payload_hex{ReadPayloadHex(args)};
    if (!payload_hex.has_value()) {
        tfm::format(std::cerr, "Error: %s requires -message=<hex> or -message=-\n", command);
        return std::nullopt;
    }

    agent::AgentMessageDecodeResult decoded{agent::DecodeAgentMessage(message_type, *payload_hex)};
    if (!decoded.ok()) {
        tfm::format(std::cerr, "Error: could not decode %s payload: %s\n",
                    message_type,
                    agent::AgentMessageIoResultCodeString(decoded.code));
        return std::nullopt;
    }
    return decoded;
}

static std::optional<std::chrono::milliseconds> ParsePeerTimeoutArg(const ArgsManager& args)
{
    const std::optional<std::string> value{args.GetArg("-peertimeout")};
    if (!value.has_value()) {
        return std::chrono::milliseconds{DEFAULT_AGENT_PEER_TIMEOUT_MS};
    }

    const auto parsed{ToIntegral<int64_t>(util::TrimString(*value))};
    if (!parsed.has_value() || *parsed <= 0) {
        tfm::format(std::cerr, "Error: -peertimeout must be a positive millisecond value\n");
        return std::nullopt;
    }
    return std::chrono::milliseconds{*parsed};
}

static std::optional<CService> ParsePeerValue(const std::string& peer_value, const char* command)
{
    const std::string trimmed_peer{util::TrimString(peer_value)};
    if (trimmed_peer.empty()) {
        tfm::format(std::cerr, "Error: %s requires -peer=<host[:port]>\n", command);
        return std::nullopt;
    }

    const std::optional<CService> peer{Lookup(trimmed_peer, Params().GetDefaultPort(), /*fAllowLookup=*/true)};
    if (!peer.has_value() || !peer->IsValid()) {
        tfm::format(std::cerr, "Error: could not resolve peer %s\n", trimmed_peer);
        return std::nullopt;
    }
    return *peer;
}

static std::optional<CService> ParsePeerArg(const ArgsManager& args, const char* command)
{
    const std::optional<std::string> peer_arg{args.GetArg("-peer")};
    if (!peer_arg.has_value()) {
        tfm::format(std::cerr, "Error: %s requires -peer=<host[:port]>\n", command);
        return std::nullopt;
    }
    return ParsePeerValue(*peer_arg, command);
}

static bool PushUniquePeer(std::vector<CService>& peers, const CService& peer)
{
    const std::string canonical{peer.ToStringAddrPort()};
    const auto duplicate{std::find_if(peers.begin(), peers.end(), [&](const CService& existing) {
        return existing.ToStringAddrPort() == canonical;
    })};
    if (duplicate != peers.end()) {
        return false;
    }
    peers.push_back(peer);
    return true;
}

static void SortRelayPeers(std::vector<CService>& peers)
{
    std::sort(peers.begin(), peers.end(), [](const CService& left, const CService& right) {
        return left.ToStringAddrPort() < right.ToStringAddrPort();
    });
}

static util::Result<std::vector<CService>> LoadRelayPeers(const ArgsManager& args)
{
    const fs::path peer_store_path{RelayPeerStorePath(args)};
    if (!fs::exists(peer_store_path)) {
        return std::vector<CService>{};
    }
    if (!fs::is_regular_file(peer_store_path)) {
        return util::Error{Untranslated(strprintf("Relay peer store %s is not a regular file.", fs::PathToString(peer_store_path)))};
    }

    const auto [ok, contents]{ReadBinaryFile(peer_store_path, MAX_AGENT_PEER_STORE_FILE_SIZE)};
    if (!ok) {
        return util::Error{Untranslated(strprintf("Could not read relay peer store %s.", fs::PathToString(peer_store_path)))};
    }
    if (util::TrimString(contents).empty()) {
        return std::vector<CService>{};
    }

    UniValue root{UniValue::VOBJ};
    if (!root.read(contents) || !root.isObject()) {
        return util::Error{Untranslated(strprintf("Relay peer store %s must be a JSON object.", fs::PathToString(peer_store_path)))};
    }

    const UniValue& chain{root.find_value("chain")};
    if (chain.isStr() && chain.get_str() != Params().GetChainTypeString()) {
        return util::Error{Untranslated(strprintf("Relay peer store %s is for chain %s, not %s.",
                                                  fs::PathToString(peer_store_path),
                                                  chain.get_str(),
                                                  Params().GetChainTypeString()))};
    }
    const UniValue& genesis_hash{root.find_value("genesis_hash")};
    if (genesis_hash.isStr() && genesis_hash.get_str() != Params().GenesisBlock().GetHash().ToString()) {
        return util::Error{Untranslated(strprintf("Relay peer store %s is for a different genesis hash.", fs::PathToString(peer_store_path)))};
    }

    const UniValue& stored_peers{root.find_value("peers")};
    if (!stored_peers.isArray()) {
        return util::Error{Untranslated(strprintf("Relay peer store %s must contain a peers array.", fs::PathToString(peer_store_path)))};
    }

    std::vector<CService> peers;
    for (const UniValue& value : stored_peers.getValues()) {
        if (!value.isStr()) {
            return util::Error{Untranslated(strprintf("Relay peer store %s contains a non-string peer.", fs::PathToString(peer_store_path)))};
        }
        const std::optional<CService> peer{ParsePeerValue(value.get_str(), "relay peer store")};
        if (!peer.has_value()) {
            return util::Error{Untranslated(strprintf("Relay peer store %s contains an invalid peer.", fs::PathToString(peer_store_path)))};
        }
        PushUniquePeer(peers, *peer);
    }
    SortRelayPeers(peers);
    return peers;
}

static util::Result<void> SaveRelayPeers(const ArgsManager& args, const std::vector<CService>& peers)
{
    const fs::path peer_store_path{RelayPeerStorePath(args)};
    const fs::path parent_path{peer_store_path.parent_path()};
    if (!parent_path.empty()) {
        try {
            fs::create_directories(parent_path);
        } catch (const std::exception& e) {
            return util::Error{Untranslated(strprintf("Could not create relay peer store directory %s: %s",
                                                      fs::PathToString(parent_path),
                                                      e.what()))};
        }
    }

    UniValue peer_values{UniValue::VARR};
    for (const CService& peer : peers) {
        peer_values.push_back(peer.ToStringAddrPort());
    }

    UniValue root{UniValue::VOBJ};
    root.pushKV("type", "quicksilver.agent_relay_peers");
    root.pushKV("version", 1);
    root.pushKV("chain", Params().GetChainTypeString());
    root.pushKV("genesis_hash", Params().GenesisBlock().GetHash().ToString());
    root.pushKV("peers", std::move(peer_values));

    if (!WriteBinaryFile(peer_store_path, root.write(2) + "\n")) {
        return util::Error{Untranslated(strprintf("Could not write relay peer store %s.", fs::PathToString(peer_store_path)))};
    }
    return {};
}

static void PrintRelayPeers(const ArgsManager& args, const std::vector<CService>& peers)
{
    tfm::format(std::cout, "peer_store=%s\n", fs::PathToString(RelayPeerStorePath(args)));
    tfm::format(std::cout, "stored_peer_count=%s\n", util::ToString(peers.size()));
    for (size_t i{0}; i < peers.size(); ++i) {
        tfm::format(std::cout, "peer_%s=%s\n", util::ToString(i), peers[i].ToStringAddrPort());
    }
}

static int AddRelayPeer(const ArgsManager& args)
{
    const std::optional<CService> peer{ParsePeerArg(args, "addpeer")};
    if (!peer.has_value()) return EXIT_FAILURE;

    auto peers{LoadRelayPeers(args)};
    if (!peers) {
        tfm::format(std::cerr, "Error: %s\n", util::ErrorString(peers).original);
        return EXIT_FAILURE;
    }

    const bool added{PushUniquePeer(*peers, *peer)};
    SortRelayPeers(*peers);
    if (added) {
        auto saved{SaveRelayPeers(args, *peers)};
        if (!saved) {
            tfm::format(std::cerr, "Error: %s\n", util::ErrorString(saved).original);
            return EXIT_FAILURE;
        }
    }

    tfm::format(std::cout, "peer=%s\n", peer->ToStringAddrPort());
    tfm::format(std::cout, "peer_added=%s\n", added ? "true" : "false");
    PrintRelayPeers(args, *peers);
    return EXIT_SUCCESS;
}

static int RemoveRelayPeer(const ArgsManager& args)
{
    const std::optional<CService> peer{ParsePeerArg(args, "removepeer")};
    if (!peer.has_value()) return EXIT_FAILURE;

    auto peers{LoadRelayPeers(args)};
    if (!peers) {
        tfm::format(std::cerr, "Error: %s\n", util::ErrorString(peers).original);
        return EXIT_FAILURE;
    }

    const std::string canonical{peer->ToStringAddrPort()};
    const size_t before{peers->size()};
    peers->erase(std::remove_if(peers->begin(), peers->end(), [&](const CService& existing) {
        return existing.ToStringAddrPort() == canonical;
    }), peers->end());
    const bool removed{peers->size() != before};
    if (removed) {
        auto saved{SaveRelayPeers(args, *peers)};
        if (!saved) {
            tfm::format(std::cerr, "Error: %s\n", util::ErrorString(saved).original);
            return EXIT_FAILURE;
        }
    }

    tfm::format(std::cout, "peer=%s\n", canonical);
    tfm::format(std::cout, "peer_removed=%s\n", removed ? "true" : "false");
    PrintRelayPeers(args, *peers);
    return EXIT_SUCCESS;
}

static int ListRelayPeers(const ArgsManager& args)
{
    auto peers{LoadRelayPeers(args)};
    if (!peers) {
        tfm::format(std::cerr, "Error: %s\n", util::ErrorString(peers).original);
        return EXIT_FAILURE;
    }
    PrintRelayPeers(args, *peers);
    return EXIT_SUCCESS;
}

static util::Result<std::vector<CService>> DecodeAddressPeers(const CSerializedNetMsg& message)
{
    if (message.m_type != NetMsgType::ADDR && message.m_type != NetMsgType::ADDRV2) {
        return util::Error{Untranslated(strprintf("Expected addr or addrv2 payload, got %s.", message.m_type))};
    }

    std::vector<CAddress> addresses;
    try {
        DataStream stream{MakeByteSpan(message.data)};
        if (message.m_type == NetMsgType::ADDRV2) {
            stream >> CAddress::V2_NETWORK(addresses);
        } else {
            stream >> CAddress::V1_NETWORK(addresses);
        }
        if (!stream.empty()) {
            return util::Error{Untranslated(strprintf("%s payload contains trailing data.", message.m_type))};
        }
    } catch (const std::ios_base::failure& e) {
        return util::Error{Untranslated(strprintf("Could not decode %s payload: %s", message.m_type, e.what()))};
    }

    if (addresses.size() > MAX_AGENT_DISCOVERED_PEERS) {
        return util::Error{Untranslated(strprintf("%s payload announced too many peers: %s", message.m_type, util::ToString(addresses.size())))};
    }

    std::vector<CService> peers;
    for (const CAddress& address : addresses) {
        const CService service{address};
        if (!service.IsValid() || service.GetPort() == 0 || !service.IsRelayable()) {
            continue;
        }
        PushUniquePeer(peers, service);
    }
    SortRelayPeers(peers);
    return peers;
}

static util::Result<std::vector<CService>> DecodeNodeAddressPeers(const std::string& node_addresses_json)
{
    UniValue addresses{UniValue::VARR};
    if (!addresses.read(node_addresses_json) || !addresses.isArray()) {
        return util::Error{Untranslated("Node address import must be a JSON array from getnodeaddresses.")};
    }
    if (addresses.size() > MAX_AGENT_DISCOVERED_PEERS) {
        return util::Error{Untranslated(strprintf("Node address import contained too many peers: %s", util::ToString(addresses.size())))};
    }

    std::vector<CService> peers;
    for (const UniValue& value : addresses.getValues()) {
        if (!value.isObject()) {
            return util::Error{Untranslated("Node address import contains a non-object entry.")};
        }

        const UniValue& address_value{value.find_value("address")};
        const UniValue& port_value{value.find_value("port")};
        if (!address_value.isStr() || !port_value.isNum()) {
            return util::Error{Untranslated("Node address import entries must contain address and port fields.")};
        }

        const auto port{ToIntegral<uint16_t>(port_value.getValStr())};
        if (!port.has_value() || *port == 0) {
            continue;
        }

        const std::optional<CNetAddr> net_addr{LookupHost(address_value.get_str(), /*fAllowLookup=*/false)};
        if (!net_addr.has_value()) {
            continue;
        }

        const CService peer{MaybeFlipIPv6toCJDNS(CService{*net_addr, *port})};
        if (!peer.IsValid() || !peer.IsRelayable()) {
            continue;
        }
        PushUniquePeer(peers, peer);
    }
    SortRelayPeers(peers);
    return peers;
}

static int ImportDecodedRelayPeers(const ArgsManager& args, const std::vector<CService>& decoded_peers, const char* decoded_count_label)
{
    auto stored_peers{LoadRelayPeers(args)};
    if (!stored_peers) {
        tfm::format(std::cerr, "Error: %s\n", util::ErrorString(stored_peers).original);
        return EXIT_FAILURE;
    }

    size_t imported{0};
    for (const CService& peer : decoded_peers) {
        if (PushUniquePeer(*stored_peers, peer)) {
            ++imported;
        }
    }
    SortRelayPeers(*stored_peers);
    if (imported > 0) {
        auto saved{SaveRelayPeers(args, *stored_peers)};
        if (!saved) {
            tfm::format(std::cerr, "Error: %s\n", util::ErrorString(saved).original);
            return EXIT_FAILURE;
        }
    }

    tfm::format(std::cout, "%s=%s\n", decoded_count_label, util::ToString(decoded_peers.size()));
    tfm::format(std::cout, "imported_peer_count=%s\n", util::ToString(imported));
    PrintRelayPeers(args, *stored_peers);
    return EXIT_SUCCESS;
}

static int ImportAddressPeers(const ArgsManager& args, const char* command, const char* message_type)
{
    const std::optional<agent::AgentMessageDecodeResult> decoded{DecodePayloadArg(args, command, message_type)};
    if (!decoded.has_value()) return EXIT_FAILURE;

    auto decoded_peers{DecodeAddressPeers(decoded->message)};
    if (!decoded_peers) {
        tfm::format(std::cerr, "Error: %s\n", util::ErrorString(decoded_peers).original);
        return EXIT_FAILURE;
    }

    return ImportDecodedRelayPeers(args, *decoded_peers, "decoded_peer_count");
}

static int ImportNodeAddressPeers(const ArgsManager& args)
{
    const std::optional<std::string> node_addresses_json{ReadNodeAddressesArg(args)};
    if (!node_addresses_json.has_value()) {
        tfm::format(std::cerr, "Error: importnodeaddresses requires -peeraddresses=<json|->\n");
        return EXIT_FAILURE;
    }

    auto decoded_peers{DecodeNodeAddressPeers(*node_addresses_json)};
    if (!decoded_peers) {
        tfm::format(std::cerr, "Error: %s\n", util::ErrorString(decoded_peers).original);
        return EXIT_FAILURE;
    }

    return ImportDecodedRelayPeers(args, *decoded_peers, "node_address_peer_count");
}

static std::vector<unsigned char> FrameWireMessage(const CSerializedNetMsg& message)
{
    const uint256 payload_hash{Hash(message.data)};
    CMessageHeader header{Params().MessageStart(), message.m_type.c_str(), static_cast<unsigned int>(message.data.size())};
    std::memcpy(header.pchChecksum, payload_hash.begin(), CMessageHeader::CHECKSUM_SIZE);

    std::vector<unsigned char> bytes;
    bytes.reserve(CMessageHeader::HEADER_SIZE + message.data.size());
    VectorWriter{bytes, 0, header};
    bytes.insert(bytes.end(), message.data.begin(), message.data.end());
    return bytes;
}

static util::Result<size_t> SendWireMessage(const Sock& sock,
                                            const CSerializedNetMsg& message,
                                            std::chrono::milliseconds timeout)
{
    const std::vector<unsigned char> bytes{FrameWireMessage(message)};
    CThreadInterrupt interrupt;
    try {
        sock.SendComplete(bytes, timeout, interrupt);
    } catch (const std::exception& e) {
        return util::Error{Untranslated(strprintf("Could not send %s message: %s", message.m_type, e.what()))};
    }
    return bytes.size();
}

static util::Result<std::vector<unsigned char>> ReceiveBytes(const Sock& sock,
                                                             size_t byte_count,
                                                             std::chrono::milliseconds timeout)
{
    std::vector<unsigned char> bytes(byte_count);
    size_t received{0};
    const auto deadline{GetTime<std::chrono::milliseconds>() + timeout};
    while (received < byte_count) {
        const auto now{GetTime<std::chrono::milliseconds>()};
        if (now >= deadline) {
            return util::Error{Untranslated(strprintf("Timed out waiting for %s bytes from peer", util::ToString(byte_count - received)))};
        }

        Sock::Event occurred{};
        const auto wait_time{std::min(deadline - now, std::chrono::milliseconds{250})};
        if (!sock.Wait(wait_time, Sock::RECV, &occurred)) {
            return util::Error{Untranslated(strprintf("Peer socket wait failed: %s", NetworkErrorString(WSAGetLastError())))};
        }
        if (occurred == 0) {
            continue;
        }
        if (occurred & Sock::ERR) {
            return util::Error{Untranslated("Peer socket reported an error")};
        }

        const ssize_t read_count{sock.Recv(bytes.data() + received, byte_count - received, MSG_DONTWAIT)};
        if (read_count > 0) {
            received += static_cast<size_t>(read_count);
            continue;
        }
        if (read_count == 0) {
            return util::Error{Untranslated("Peer closed connection")};
        }
        const int error{WSAGetLastError()};
        if (error != WSAEAGAIN && error != WSAEINTR && error != WSAEWOULDBLOCK && error != WSAEINPROGRESS) {
            return util::Error{Untranslated(strprintf("Could not receive from peer: %s", NetworkErrorString(error)))};
        }
    }
    return bytes;
}

static util::Result<CSerializedNetMsg> ReceiveWireMessage(const Sock& sock, std::chrono::milliseconds timeout)
{
    auto header_bytes{ReceiveBytes(sock, CMessageHeader::HEADER_SIZE, timeout)};
    if (!header_bytes) return util::Error{util::ErrorString(header_bytes)};

    CMessageHeader header;
    try {
        DataStream stream{Span<const unsigned char>{header_bytes->data(), header_bytes->size()}};
        stream >> header;
    } catch (const std::exception& e) {
        return util::Error{Untranslated(strprintf("Could not decode peer message header: %s", e.what()))};
    }

    if (header.pchMessageStart != Params().MessageStart()) {
        return util::Error{Untranslated("Peer sent a message for a different network")};
    }
    if (!header.IsMessageTypeValid()) {
        return util::Error{Untranslated("Peer sent an invalid message type")};
    }
    if (header.nMessageSize > MAX_PROTOCOL_MESSAGE_LENGTH) {
        return util::Error{Untranslated(strprintf("Peer message %s is too large: %s bytes", header.GetMessageType(), util::ToString(header.nMessageSize)))};
    }

    auto payload{ReceiveBytes(sock, header.nMessageSize, timeout)};
    if (!payload) return util::Error{util::ErrorString(payload)};

    const uint256 payload_hash{Hash(*payload)};
    if (std::memcmp(payload_hash.begin(), header.pchChecksum, CMessageHeader::CHECKSUM_SIZE) != 0) {
        return util::Error{Untranslated(strprintf("Peer message %s has an invalid checksum", header.GetMessageType()))};
    }

    CSerializedNetMsg message;
    message.m_type = header.GetMessageType();
    message.data = std::move(*payload);
    return message;
}

static CSerializedNetMsg MakeAgentVersionMessage(const CService& peer)
{
    const int64_t now{count_seconds(GetTime<std::chrono::seconds>())};
    const uint64_t nonce{static_cast<uint64_t>(count_microseconds(GetTime<std::chrono::microseconds>()))};
    return NetMsg::Make(NetMsgType::VERSION,
                        PROTOCOL_VERSION,
                        uint64_t{NODE_NONE},
                        now,
                        uint64_t{NODE_NONE},
                        CNetAddr::V1(peer),
                        uint64_t{NODE_NONE},
                        CNetAddr::V1(CService{}),
                        nonce,
                        FormatSubVersion(CLIENT_NAME, CLIENT_VERSION, {"agent"}),
                        int32_t{0},
                        true);
}

static util::Result<void> SendHandshakeFeatureMessages(const Sock& sock, std::chrono::milliseconds timeout)
{
    auto sent_wtxidrelay{SendWireMessage(sock, NetMsg::Make(NetMsgType::WTXIDRELAY), timeout)};
    if (!sent_wtxidrelay) return util::Error{util::ErrorString(sent_wtxidrelay)};
    auto sent_sendaddrv2{SendWireMessage(sock, NetMsg::Make(NetMsgType::SENDADDRV2), timeout)};
    if (!sent_sendaddrv2) return util::Error{util::ErrorString(sent_sendaddrv2)};
    auto sent_verack{SendWireMessage(sock, NetMsg::Make(NetMsgType::VERACK), timeout)};
    if (!sent_verack) return util::Error{util::ErrorString(sent_verack)};
    return {};
}

struct PeerTransactionRelayResult {
    CService peer;
    bool connected{false};
    bool peer_version_received{false};
    bool peer_verack_received{false};
    bool local_verack_sent{false};
    bool sent_tx{false};
    size_t wire_tx_bytes{0};
    std::string error;
};

struct PeerHandshakeResult {
    bool peer_version_received{false};
    bool peer_verack_received{false};
    bool local_verack_sent{false};
    std::string error;
};

static PeerHandshakeResult CompletePeerHandshake(const Sock& sock, const CService& peer, std::chrono::milliseconds timeout)
{
    PeerHandshakeResult result;

    auto sent_version{SendWireMessage(sock, MakeAgentVersionMessage(peer), timeout)};
    if (!sent_version) {
        result.error = util::ErrorString(sent_version).original;
        return result;
    }

    for (int message_count{0}; message_count < MAX_AGENT_PEER_HANDSHAKE_MESSAGES && !(result.peer_version_received && result.peer_verack_received); ++message_count) {
        auto peer_message{ReceiveWireMessage(sock, timeout)};
        if (!peer_message) {
            result.error = util::ErrorString(peer_message).original;
            return result;
        }
        if (peer_message->m_type == NetMsgType::VERSION) {
            if (result.peer_version_received) {
                result.error = "peer sent duplicate version message";
                return result;
            }
            result.peer_version_received = true;
            auto sent_features{SendHandshakeFeatureMessages(sock, timeout)};
            if (!sent_features) {
                result.error = util::ErrorString(sent_features).original;
                return result;
            }
            result.local_verack_sent = true;
            continue;
        }
        if (peer_message->m_type == NetMsgType::VERACK) {
            result.peer_verack_received = true;
            continue;
        }
        if (peer_message->m_type == NetMsgType::PING && peer_message->data.size() == sizeof(uint64_t)) {
            CSerializedNetMsg pong;
            pong.m_type = NetMsgType::PONG;
            pong.data = peer_message->data;
            auto sent_pong{SendWireMessage(sock, pong, timeout)};
            if (!sent_pong) {
                result.error = util::ErrorString(sent_pong).original;
                return result;
            }
        }
    }
    if (!result.peer_version_received || !result.peer_verack_received || !result.local_verack_sent) {
        result.error = "peer handshake did not complete";
    }
    return result;
}

static PeerTransactionRelayResult SendTransactionToOnePeer(const CService& peer,
                                                           const CSerializedNetMsg& tx_payload,
                                                           std::chrono::milliseconds timeout)
{
    PeerTransactionRelayResult result;
    result.peer = peer;

    nConnectTimeout = static_cast<int>(std::min<int64_t>(timeout.count(), std::numeric_limits<int>::max()));
    std::unique_ptr<Sock> sock{agent::ConnectToPeer(peer)};
    if (!sock) {
        result.error = strprintf("could not connect to peer %s", peer.ToStringAddrPort());
        return result;
    }
    result.connected = true;

    const PeerHandshakeResult handshake{CompletePeerHandshake(*sock, peer, timeout)};
    result.peer_version_received = handshake.peer_version_received;
    result.peer_verack_received = handshake.peer_verack_received;
    result.local_verack_sent = handshake.local_verack_sent;
    if (!handshake.error.empty()) {
        result.error = handshake.error;
        return result;
    }

    auto sent_tx{SendWireMessage(*sock, tx_payload, timeout)};
    if (!sent_tx) {
        result.error = util::ErrorString(sent_tx).original;
        return result;
    }
    result.sent_tx = true;
    result.wire_tx_bytes = *sent_tx;
    return result;
}

static void PrintPeerTransactionRelayResult(const PeerTransactionRelayResult& result, std::optional<size_t> index = std::nullopt)
{
    const std::string prefix{index.has_value() ? strprintf("peer_%s_", util::ToString(*index)) : ""};
    tfm::format(std::cout, "%speer=%s\n", prefix, result.peer.ToStringAddrPort());
    tfm::format(std::cout, "%sconnected=%s\n", prefix, result.connected ? "true" : "false");
    tfm::format(std::cout, "%speer_version_received=%s\n", prefix, result.peer_version_received ? "true" : "false");
    tfm::format(std::cout, "%speer_verack_received=%s\n", prefix, result.peer_verack_received ? "true" : "false");
    tfm::format(std::cout, "%slocal_verack_sent=%s\n", prefix, result.local_verack_sent ? "true" : "false");
    if (index.has_value()) {
        tfm::format(std::cout, "%ssent_tx=%s\n", prefix, result.sent_tx ? "true" : "false");
    }
    if (result.sent_tx) {
        tfm::format(std::cout, "%swire_tx_bytes=%s\n", prefix, util::ToString(result.wire_tx_bytes));
    } else if (!result.error.empty()) {
        tfm::format(std::cout, "%serror=%s\n", prefix, result.error);
    }
}

static std::optional<std::vector<CService>> ConfiguredRelayPeers(const ArgsManager& args, const char* command)
{
    std::vector<CService> peers;
    const std::optional<std::string> peer_arg{args.GetArg("-peer")};
    if (peer_arg.has_value()) {
        const std::optional<CService> peer{ParsePeerValue(*peer_arg, command)};
        if (!peer.has_value()) return std::nullopt;
        peers.push_back(*peer);
        return peers;
    }

    auto stored_peers{LoadRelayPeers(args)};
    if (!stored_peers) {
        tfm::format(std::cerr, "Error: %s\n", util::ErrorString(stored_peers).original);
        return std::nullopt;
    }
    if (!stored_peers->empty()) {
        return *stored_peers;
    }

    std::vector<CService> fixed_seed_peers;
    try {
        fixed_seed_peers = agent::FixedSeedPeers();
    } catch (const std::exception& e) {
        tfm::format(std::cerr, "Error: could not decode fixed seed peers: %s\n", e.what());
        return std::nullopt;
    }
    if (fixed_seed_peers.empty()) {
        tfm::format(std::cerr, "Error: %s requires -peer=<host[:port]>, at least one stored relay peer, or a fixed seed on the active network.\n", command);
        return std::nullopt;
    }
    return fixed_seed_peers;
}

struct PeerDiscoveryResult {
    CService peer;
    bool connected{false};
    bool peer_version_received{false};
    bool peer_verack_received{false};
    bool local_verack_sent{false};
    bool getaddr_sent{false};
    std::vector<CService> discovered_peers;
    std::string error;
};

static PeerDiscoveryResult DiscoverPeersFromOnePeer(const CService& peer, std::chrono::milliseconds timeout)
{
    PeerDiscoveryResult result;
    result.peer = peer;

    nConnectTimeout = static_cast<int>(std::min<int64_t>(timeout.count(), std::numeric_limits<int>::max()));
    std::unique_ptr<Sock> sock{agent::ConnectToPeer(peer)};
    if (!sock) {
        result.error = strprintf("could not connect to peer %s", peer.ToStringAddrPort());
        return result;
    }
    result.connected = true;

    const PeerHandshakeResult handshake{CompletePeerHandshake(*sock, peer, timeout)};
    result.peer_version_received = handshake.peer_version_received;
    result.peer_verack_received = handshake.peer_verack_received;
    result.local_verack_sent = handshake.local_verack_sent;
    if (!handshake.error.empty()) {
        result.error = handshake.error;
        return result;
    }

    auto sent_getaddr{SendWireMessage(*sock, NetMsg::Make(NetMsgType::GETADDR), timeout)};
    if (!sent_getaddr) {
        result.error = util::ErrorString(sent_getaddr).original;
        return result;
    }
    result.getaddr_sent = true;

    for (int message_count{0}; message_count < MAX_AGENT_PEER_DISCOVERY_MESSAGES; ++message_count) {
        auto peer_message{ReceiveWireMessage(*sock, timeout)};
        if (!peer_message) {
            result.error = util::ErrorString(peer_message).original;
            return result;
        }
        if (peer_message->m_type == NetMsgType::ADDR || peer_message->m_type == NetMsgType::ADDRV2) {
            auto peers{DecodeAddressPeers(*peer_message)};
            if (!peers) {
                result.error = util::ErrorString(peers).original;
                return result;
            }
            result.discovered_peers = std::move(*peers);
            return result;
        }
        if (peer_message->m_type == NetMsgType::PING && peer_message->data.size() == sizeof(uint64_t)) {
            CSerializedNetMsg pong;
            pong.m_type = NetMsgType::PONG;
            pong.data = peer_message->data;
            auto sent_pong{SendWireMessage(*sock, pong, timeout)};
            if (!sent_pong) {
                result.error = util::ErrorString(sent_pong).original;
                return result;
            }
        }
    }

    result.error = "peer did not send addr or addrv2 after getaddr";
    return result;
}

static void PrintPeerDiscoveryResult(const PeerDiscoveryResult& result, size_t index)
{
    const std::string prefix{strprintf("bootstrap_peer_%s_", util::ToString(index))};
    tfm::format(std::cout, "%speer=%s\n", prefix, result.peer.ToStringAddrPort());
    tfm::format(std::cout, "%sconnected=%s\n", prefix, result.connected ? "true" : "false");
    tfm::format(std::cout, "%speer_version_received=%s\n", prefix, result.peer_version_received ? "true" : "false");
    tfm::format(std::cout, "%speer_verack_received=%s\n", prefix, result.peer_verack_received ? "true" : "false");
    tfm::format(std::cout, "%slocal_verack_sent=%s\n", prefix, result.local_verack_sent ? "true" : "false");
    tfm::format(std::cout, "%sgetaddr_sent=%s\n", prefix, result.getaddr_sent ? "true" : "false");
    tfm::format(std::cout, "%sdiscovered_peer_count=%s\n", prefix, util::ToString(result.discovered_peers.size()));
    if (!result.error.empty()) {
        tfm::format(std::cout, "%serror=%s\n", prefix, result.error);
    }
}

static int DiscoverRelayPeers(const ArgsManager& args)
{
    const std::optional<std::vector<CService>> bootstrap_peers{ConfiguredRelayPeers(args, "discoverpeers")};
    if (!bootstrap_peers.has_value()) return EXIT_FAILURE;
    const std::optional<std::chrono::milliseconds> timeout{ParsePeerTimeoutArg(args)};
    if (!timeout.has_value()) return EXIT_FAILURE;

    auto stored_peers{LoadRelayPeers(args)};
    if (!stored_peers) {
        tfm::format(std::cerr, "Error: %s\n", util::ErrorString(stored_peers).original);
        return EXIT_FAILURE;
    }

    tfm::format(std::cout, "bootstrap_peer_count=%s\n", util::ToString(bootstrap_peers->size()));
    size_t discovered_peer_count{0};
    size_t imported_peer_count{0};
    for (size_t i{0}; i < bootstrap_peers->size(); ++i) {
        const PeerDiscoveryResult discovery{DiscoverPeersFromOnePeer((*bootstrap_peers)[i], *timeout)};
        PrintPeerDiscoveryResult(discovery, i);
        for (const CService& peer : discovery.discovered_peers) {
            ++discovered_peer_count;
            if (PushUniquePeer(*stored_peers, peer)) {
                ++imported_peer_count;
            }
        }
    }
    SortRelayPeers(*stored_peers);
    if (imported_peer_count > 0) {
        auto saved{SaveRelayPeers(args, *stored_peers)};
        if (!saved) {
            tfm::format(std::cerr, "Error: %s\n", util::ErrorString(saved).original);
            return EXIT_FAILURE;
        }
    }

    tfm::format(std::cout, "discovered_peer_count=%s\n", util::ToString(discovered_peer_count));
    tfm::format(std::cout, "imported_peer_count=%s\n", util::ToString(imported_peer_count));
    PrintRelayPeers(args, *stored_peers);
    return discovered_peer_count > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

struct PeerHeaderSyncResult {
    CService peer;
    bool connected{false};
    bool peer_version_received{false};
    bool peer_verack_received{false};
    bool local_verack_sent{false};
    bool getheaders_sent{false};
    size_t outbound_message_count{0};
    size_t header_messages{0};
    size_t decoded_headers{0};
    size_t accepted_headers{0};
    size_t duplicate_headers{0};
    bool peer_synced{false};
    bool header_store_saved{false};
    agent::HeaderStoreResult save_status{agent::HeaderStoreResult::FILE_NOT_FOUND};
    int header_height{0};
    uint256 header_tip;
    std::string error;
};

static void AccumulateHeaderSyncResult(PeerHeaderSyncResult& result, const agent::AgentPeerAction& action)
{
    if (!action.header_action.has_value()) return;
    const agent::HeaderSyncDriverResult& driver_result{action.header_action->driver_result};
    if (driver_result.header_message.has_value()) {
        ++result.header_messages;
        const agent::HeaderMessageProcessResult& header_message{*driver_result.header_message};
        result.decoded_headers += header_message.decode.decoded_count;
        if (header_message.sync.has_value()) {
            result.accepted_headers += header_message.sync->accepted_count;
            result.duplicate_headers += header_message.sync->duplicate_count;
            result.peer_synced = result.peer_synced || header_message.sync->peer_synced;
        }
    }
    result.peer_synced = result.peer_synced ||
                         driver_result.code == agent::HeaderSyncDriverResultCode::PEER_SYNCED;
}

static util::Result<size_t> SendAgentClientOutboundMessages(const Sock& sock,
                                                            agent::AgentClient& client,
                                                            std::chrono::milliseconds timeout,
                                                            bool* sent_getheaders = nullptr)
{
    size_t sent_count{0};
    for (CSerializedNetMsg& outbound : client.DrainOutboundMessages()) {
        auto sent{SendWireMessage(sock, outbound, timeout)};
        if (!sent) return util::Error{util::ErrorString(sent)};
        if (sent_getheaders && outbound.m_type == NetMsgType::GETHEADERS) {
            *sent_getheaders = true;
        }
        ++sent_count;
    }
    return sent_count;
}

static PeerHeaderSyncResult SyncHeadersFromOnePeer(const ArgsManager& args,
                                                   const CService& peer,
                                                   std::chrono::milliseconds timeout)
{
    PeerHeaderSyncResult result;
    result.peer = peer;

    const fs::path header_store_path{HeaderStorePath(args)};
    agent::AgentClient client{MakeAgentClient(args, header_store_path)};
    const agent::AgentClientLoadResult& load_result{client.LastLoadResult()};
    if (!load_result.ok()) {
        result.error = strprintf("could not load header store %s: %s",
                                 fs::PathToString(header_store_path),
                                 agent::HeaderStoreResultString(load_result.status));
        return result;
    }

    nConnectTimeout = static_cast<int>(std::min<int64_t>(timeout.count(), std::numeric_limits<int>::max()));
    std::unique_ptr<Sock> sock{agent::ConnectToPeer(peer)};
    if (!sock) {
        result.error = strprintf("could not connect to peer %s", peer.ToStringAddrPort());
        return result;
    }
    result.connected = true;

    const PeerHandshakeResult handshake{CompletePeerHandshake(*sock, peer, timeout)};
    result.peer_version_received = handshake.peer_version_received;
    result.peer_verack_received = handshake.peer_verack_received;
    result.local_verack_sent = handshake.local_verack_sent;
    if (!handshake.error.empty()) {
        result.error = handshake.error;
        return result;
    }

    const agent::AgentPeerAction start{client.StartHeaders()};
    AccumulateHeaderSyncResult(result, start);
    if (!start.ok()) {
        result.error = strprintf("could not start header sync: %s", agent::AgentPeerActionCodeString(start.code));
        return result;
    }

    bool sent_getheaders{false};
    auto sent_start{SendAgentClientOutboundMessages(*sock, client, timeout, &sent_getheaders)};
    if (!sent_start) {
        result.error = util::ErrorString(sent_start).original;
        return result;
    }
    result.getheaders_sent = sent_getheaders;
    result.outbound_message_count = *sent_start;
    if (!result.getheaders_sent) {
        result.error = "agent client did not queue a getheaders message";
        return result;
    }

    for (int message_count{0}; message_count < MAX_AGENT_PEER_HEADER_SYNC_MESSAGES && !result.peer_synced; ++message_count) {
        auto peer_message{ReceiveWireMessage(*sock, timeout)};
        if (!peer_message) {
            result.error = util::ErrorString(peer_message).original;
            break;
        }
        if (peer_message->m_type == NetMsgType::PING && peer_message->data.size() == sizeof(uint64_t)) {
            CSerializedNetMsg pong;
            pong.m_type = NetMsgType::PONG;
            pong.data = peer_message->data;
            auto sent_pong{SendWireMessage(*sock, pong, timeout)};
            if (!sent_pong) {
                result.error = util::ErrorString(sent_pong).original;
                break;
            }
            continue;
        }
        const agent::AgentPeerAction processed{client.ProcessMessage(*peer_message)};
        AccumulateHeaderSyncResult(result, processed);
        if (!processed.ok()) {
            result.error = strprintf("could not process peer message %s: %s",
                                     peer_message->m_type,
                                     agent::AgentPeerActionCodeString(processed.code));
            break;
        }
        auto sent_more{SendAgentClientOutboundMessages(*sock, client, timeout)};
        if (!sent_more) {
            result.error = util::ErrorString(sent_more).original;
            break;
        }
    }

    result.header_height = client.HeaderHeight();
    result.header_tip = client.HeaderTip().GetBlockHash();
    if (result.accepted_headers > 0 || result.peer_synced) {
        if (header_store_path.has_parent_path()) {
            fs::create_directories(header_store_path.parent_path());
        }
        result.save_status = client.SaveHeaders();
        result.header_store_saved = result.save_status == agent::HeaderStoreResult::OK;
        if (!result.header_store_saved && result.error.empty()) {
            result.error = strprintf("could not write header store %s: %s",
                                     fs::PathToString(header_store_path),
                                     agent::HeaderStoreResultString(result.save_status));
        }
    }
    return result;
}

static void PrintPeerHeaderSyncResult(const PeerHeaderSyncResult& result, size_t index)
{
    const std::string prefix{strprintf("header_peer_%s_", util::ToString(index))};
    tfm::format(std::cout, "%speer=%s\n", prefix, result.peer.ToStringAddrPort());
    tfm::format(std::cout, "%sconnected=%s\n", prefix, result.connected ? "true" : "false");
    tfm::format(std::cout, "%speer_version_received=%s\n", prefix, result.peer_version_received ? "true" : "false");
    tfm::format(std::cout, "%speer_verack_received=%s\n", prefix, result.peer_verack_received ? "true" : "false");
    tfm::format(std::cout, "%slocal_verack_sent=%s\n", prefix, result.local_verack_sent ? "true" : "false");
    tfm::format(std::cout, "%sgetheaders_sent=%s\n", prefix, result.getheaders_sent ? "true" : "false");
    if (result.getheaders_sent) {
        tfm::format(std::cout, "%soutbound_message_count=%s\n", prefix, util::ToString(result.outbound_message_count));
    }
    tfm::format(std::cout, "%sheader_messages=%s\n", prefix, util::ToString(result.header_messages));
    tfm::format(std::cout, "%sdecoded_headers=%s\n", prefix, util::ToString(result.decoded_headers));
    tfm::format(std::cout, "%saccepted_headers=%s\n", prefix, util::ToString(result.accepted_headers));
    tfm::format(std::cout, "%sduplicate_headers=%s\n", prefix, util::ToString(result.duplicate_headers));
    tfm::format(std::cout, "%speer_synced=%s\n", prefix, result.peer_synced ? "true" : "false");
    tfm::format(std::cout, "%sheader_store_saved=%s\n", prefix, result.header_store_saved ? "true" : "false");
    if (result.header_store_saved || result.save_status != agent::HeaderStoreResult::FILE_NOT_FOUND) {
        tfm::format(std::cout, "%ssave_status=%s\n", prefix, agent::HeaderStoreResultString(result.save_status));
    }
    tfm::format(std::cout, "%sheader_height=%d\n", prefix, result.header_height);
    tfm::format(std::cout, "%sheader_tip=%s\n", prefix, result.header_tip.ToString());
    if (!result.error.empty()) {
        tfm::format(std::cout, "%serror=%s\n", prefix, result.error);
    }
}

static int SyncHeadersFromPeers(const ArgsManager& args)
{
    const std::optional<std::vector<CService>> peers{ConfiguredRelayPeers(args, "syncheaderspeer")};
    if (!peers.has_value()) return EXIT_FAILURE;
    const std::optional<std::chrono::milliseconds> timeout{ParsePeerTimeoutArg(args)};
    if (!timeout.has_value()) return EXIT_FAILURE;

    tfm::format(std::cout, "configured_peer_count=%s\n", util::ToString(peers->size()));
    bool synced{false};
    size_t accepted_header_count{0};
    for (size_t i{0}; i < peers->size(); ++i) {
        const PeerHeaderSyncResult sync{SyncHeadersFromOnePeer(args, (*peers)[i], *timeout)};
        PrintPeerHeaderSyncResult(sync, i);
        accepted_header_count += sync.accepted_headers;
        if (sync.peer_synced && sync.header_store_saved) {
            synced = true;
            break;
        }
    }

    tfm::format(std::cout, "accepted_header_count=%s\n", util::ToString(accepted_header_count));
    tfm::format(std::cout, "headers_synced=%s\n", synced ? "true" : "false");
    return synced ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int SendTransactionToPeer(const ArgsManager& args)
{
    std::optional<agent::AgentMessageDecodeResult> decoded{DecodePayloadArg(args, "sendtxpeer", NetMsgType::TX)};
    if (!decoded.has_value()) {
        return EXIT_FAILURE;
    }

    agent::TxMessageDecodeResult tx_message{agent::DecodeTxMessage(decoded->message)};
    if (!tx_message.ok() || !tx_message.transaction) {
        tfm::format(std::cerr, "Error: could not decode tx payload: %s\n", agent::TxMessageResultCodeString(tx_message.code));
        return EXIT_FAILURE;
    }

    const std::optional<std::vector<CService>> peers{ConfiguredRelayPeers(args, "sendtxpeer")};
    if (!peers.has_value()) return EXIT_FAILURE;
    const std::optional<std::chrono::milliseconds> timeout{ParsePeerTimeoutArg(args)};
    if (!timeout.has_value()) return EXIT_FAILURE;

    tfm::format(std::cout, "tx_payload_bytes=%s\n", util::ToString(decoded->message.data.size()));
    tfm::format(std::cout, "configured_peer_count=%s\n", util::ToString(peers->size()));
    size_t sent_peer_count{0};
    for (size_t i{0}; i < peers->size(); ++i) {
        const PeerTransactionRelayResult relay_result{SendTransactionToOnePeer((*peers)[i], decoded->message, *timeout)};
        PrintPeerTransactionRelayResult(relay_result, peers->size() == 1 ? std::optional<size_t>{} : std::optional<size_t>{i});
        if (relay_result.sent_tx) {
            ++sent_peer_count;
        }
    }
    tfm::format(std::cout, "sent_peer_count=%s\n", util::ToString(sent_peer_count));
    tfm::format(std::cout, "sent_tx=%s\n", sent_peer_count > 0 ? "true" : "false");
    PrintTransaction(*tx_message.transaction);
    return sent_peer_count > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

static bool LoadResultOk(const fs::path& header_store_path, const agent::AgentClientLoadResult& load_result)
{
    if (load_result.ok()) {
        return true;
    }

    tfm::format(std::cerr, "Error: could not load header store %s: %s\n",
                fs::PathToString(header_store_path),
                agent::HeaderStoreResultString(load_result.status));
    return false;
}

static void PrintTransaction(const CTransaction& transaction)
{
    tfm::format(std::cout, "txid=%s\n", transaction.GetHash().ToUint256().ToString());
    tfm::format(std::cout, "wtxid=%s\n", transaction.GetWitnessHash().ToUint256().ToString());
    tfm::format(std::cout, "anchor_height=%s\n", util::ToString(transaction.nAnchorHeight));
}

static void PrintTxPeerAction(const agent::TxPeerAction& tx_action)
{
    const agent::TxPeerResult& result{tx_action.result};
    tfm::format(std::cout, "tx_result=%s\n", agent::TxPeerResultCodeString(result.code));
    if (result.inventory_message.has_value()) {
        const agent::TxInventoryMessageDecodeResult& inventory_message{*result.inventory_message};
        tfm::format(std::cout, "inventory_decode_result=%s\n", agent::TxMessageResultCodeString(inventory_message.code));
        tfm::format(std::cout, "announced_inventory=%s\n", util::ToString(inventory_message.announced_count));
        tfm::format(std::cout, "decoded_inventory=%s\n", util::ToString(inventory_message.decoded_count));
    }
    if (result.tx_message.has_value()) {
        const agent::TxMessageDecodeResult& tx_message{*result.tx_message};
        tfm::format(std::cout, "tx_decode_result=%s\n", agent::TxMessageResultCodeString(tx_message.code));
        if (tx_message.transaction) {
            PrintTransaction(*tx_message.transaction);
        }
    }
    tfm::format(std::cout, "requested_inventory=%s\n", util::ToString(result.requested_inventory.size()));
    tfm::format(std::cout, "served_inventory=%s\n", util::ToString(result.served_inventory.size()));
    tfm::format(std::cout, "matched_requests=%s\n", util::ToString(result.matched_requests.size()));
    tfm::format(std::cout, "duplicate_inventory=%s\n", util::ToString(result.duplicate_count));
    tfm::format(std::cout, "missing_inventory=%s\n", util::ToString(result.missing_count));
}

static void PrintTransactionAction(const agent::AgentPeerAction& action, const agent::AgentClient& client)
{
    tfm::format(std::cout, "action=%s\n", agent::AgentPeerActionCodeString(action.code));
    if (action.tx_action.has_value()) {
        PrintTxPeerAction(*action.tx_action);
    }
    tfm::format(std::cout, "pending_tx_requests=%s\n", util::ToString(client.PendingTxRequestCount()));
    tfm::format(std::cout, "known_tx_inventory=%s\n", util::ToString(client.KnownTxInventoryCount()));
}

static int ProcessHeaders(const ArgsManager& args)
{
    std::optional<agent::AgentMessageDecodeResult> decoded{DecodePayloadArg(args, "processheaders", NetMsgType::HEADERS)};
    if (!decoded.has_value()) {
        return EXIT_FAILURE;
    }

    const fs::path header_store_path{HeaderStorePath(args)};
    agent::AgentClient client{MakeAgentClient(args, header_store_path)};
    const agent::AgentClientLoadResult& load_result{client.LastLoadResult()};
    if (!LoadResultOk(header_store_path, load_result)) {
        return EXIT_FAILURE;
    }

    const auto request{client.StartHeaders()};
    if (!request.ok()) {
        tfm::format(std::cerr, "Error: could not start header request: %s\n", agent::AgentPeerActionCodeString(request.code));
        return EXIT_FAILURE;
    }
    client.DrainOutboundMessages();

    const auto processed{client.ProcessMessage(decoded->message)};
    tfm::format(std::cout, "action=%s\n", agent::AgentPeerActionCodeString(processed.code));
    if (processed.header_action.has_value()) {
        tfm::format(std::cout, "driver_result=%s\n", agent::HeaderSyncDriverResultCodeString(processed.header_action->driver_result.code));
        if (processed.header_action->driver_result.header_message.has_value()) {
            const agent::HeaderMessageProcessResult& header_message{*processed.header_action->driver_result.header_message};
            tfm::format(std::cout, "decode_result=%s\n", agent::HeaderMessageResultCodeString(header_message.decode.code));
            tfm::format(std::cout, "decoded_headers=%s\n", util::ToString(header_message.decode.decoded_count));
            if (header_message.sync.has_value()) {
                tfm::format(std::cout, "sync_result=%s\n", agent::HeaderSyncResultCodeString(header_message.sync->code));
                tfm::format(std::cout, "accepted_headers=%s\n", util::ToString(header_message.sync->accepted_count));
                tfm::format(std::cout, "duplicate_headers=%s\n", util::ToString(header_message.sync->duplicate_count));
                tfm::format(std::cout, "peer_synced=%s\n", header_message.sync->peer_synced ? "true" : "false");
            }
        }
    }
    tfm::format(std::cout, "header_height=%d\n", client.HeaderHeight());
    tfm::format(std::cout, "header_tip=%s\n", client.HeaderTip().GetBlockHash().ToString());

    if (!processed.ok()) {
        PrintOutboundMessages(client.DrainOutboundMessages());
        return EXIT_FAILURE;
    }

    const agent::HeaderStoreResult save_result{client.SaveHeaders()};
    if (save_result != agent::HeaderStoreResult::OK) {
        tfm::format(std::cerr, "Error: could not write header store %s: %s\n",
                    fs::PathToString(header_store_path),
                    agent::HeaderStoreResultString(save_result));
        return EXIT_FAILURE;
    }
    tfm::format(std::cout, "save_status=%s\n", agent::HeaderStoreResultString(save_result));
    PrintOutboundMessages(client.DrainOutboundMessages());
    return EXIT_SUCCESS;
}

static int ProcessTransactionMessage(const ArgsManager& args, const char* command, const char* message_type)
{
    std::optional<agent::AgentMessageDecodeResult> decoded{DecodePayloadArg(args, command, message_type)};
    if (!decoded.has_value()) {
        return EXIT_FAILURE;
    }

    const fs::path header_store_path{HeaderStorePath(args)};
    agent::AgentClient client{MakeAgentClient(args, header_store_path)};
    const agent::AgentClientLoadResult& load_result{client.LastLoadResult()};
    if (!LoadResultOk(header_store_path, load_result)) {
        return EXIT_FAILURE;
    }

    const auto processed{client.ProcessMessage(decoded->message)};
    PrintTransactionAction(processed, client);
    PrintOutboundMessages(client.DrainOutboundMessages());
    return processed.ok() ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int RelayTransaction(const ArgsManager& args, bool announce)
{
    const char* command{announce ? "announcetx" : "sendtx"};
    std::optional<agent::AgentMessageDecodeResult> decoded{DecodePayloadArg(args, command, NetMsgType::TX)};
    if (!decoded.has_value()) {
        return EXIT_FAILURE;
    }

    agent::TxMessageDecodeResult tx_message{agent::DecodeTxMessage(decoded->message)};
    if (!tx_message.ok() || !tx_message.transaction) {
        tfm::format(std::cerr, "Error: could not decode tx payload: %s\n", agent::TxMessageResultCodeString(tx_message.code));
        return EXIT_FAILURE;
    }

    const fs::path header_store_path{HeaderStorePath(args)};
    agent::AgentClient client{MakeAgentClient(args, header_store_path)};
    const agent::AgentClientLoadResult& load_result{client.LastLoadResult()};
    if (!LoadResultOk(header_store_path, load_result)) {
        return EXIT_FAILURE;
    }

    const auto action{announce ? client.AnnounceTransaction(*tx_message.transaction) : client.SendTransaction(*tx_message.transaction)};
    PrintTransactionAction(action, client);
    tfm::format(std::cout, "tx_decode_result=%s\n", agent::TxMessageResultCodeString(tx_message.code));
    PrintTransaction(*tx_message.transaction);
    PrintOutboundMessages(client.DrainOutboundMessages());
    return action.ok() ? EXIT_SUCCESS : EXIT_FAILURE;
}

MAIN_FUNCTION
{
    ArgsManager& args = gArgs;
#ifdef WIN32
    common::WinCmdLineArgs winArgs;
    std::tie(argc, argv) = winArgs.get();
#endif

    SetupEnvironment();

    try {
        const int ret{AppInitAgent(args, argc, argv)};
        if (ret != CONTINUE_EXECUTION) return ret;
    } catch (const std::exception& e) {
        PrintExceptionContinue(&e, "AppInitAgent()");
        return EXIT_FAILURE;
    } catch (...) {
        PrintExceptionContinue(nullptr, "AppInitAgent()");
        return EXIT_FAILURE;
    }

    const auto command{args.GetCommand()};
    if (!command) {
        tfm::format(std::cerr, "Error: must specify a command\n");
        return EXIT_FAILURE;
    }
    if (!command->args.empty()) {
        tfm::format(std::cerr, "Error: command does not accept positional arguments\n");
        return EXIT_FAILURE;
    }

    ECC_Context ecc_context{};
    ConfigureCuckatooSolverEnvironment(args);

    try {
        if (command->command == "status") return PrintStatus(args);
        if (command->command == "initheaders") return InitHeaders(args);
        if (command->command == "requestheaders") return RequestHeaders(args);
        if (command->command == "processheaders") return ProcessHeaders(args);
        if (command->command == "processinv") return ProcessTransactionMessage(args, "processinv", NetMsgType::INV);
        if (command->command == "processtx") return ProcessTransactionMessage(args, "processtx", NetMsgType::TX);
        if (command->command == "processaddr") return ImportAddressPeers(args, "processaddr", NetMsgType::ADDR);
        if (command->command == "processaddrv2") return ImportAddressPeers(args, "processaddrv2", NetMsgType::ADDRV2);
        if (command->command == "importnodeaddresses") return ImportNodeAddressPeers(args);
        if (command->command == "announcetx") return RelayTransaction(args, true);
        if (command->command == "sendtx") return RelayTransaction(args, false);
        if (command->command == "addpeer") return AddRelayPeer(args);
        if (command->command == "removepeer") return RemoveRelayPeer(args);
        if (command->command == "listpeers") return ListRelayPeers(args);
        if (command->command == "discoverpeers") return DiscoverRelayPeers(args);
        if (command->command == "syncheaderspeer") return SyncHeadersFromPeers(args);
        if (command->command == "sendtxpeer") return SendTransactionToPeer(args);
        if (command->command == "checkpolicy") return CheckPolicy(args);
        if (command->command == "checkbundle") return CheckBundle(args);
        if (command->command == "signbundle") return SignBundle(args);
        if (command->command == "importreceipt") return ImportPaymentReceipts(args);
        if (command->command == "importrecovery") return ImportRecoveredPaymentReceipts(args);
        if (command->command == "scanreceipts") return ScanPaymentReceiptDirectory(args);
        if (command->command == "listreceipts") return ListPaymentReceipts(args);
        if (command->command == "listreceiptactivity") return ListPaymentReceiptActivity(args);
        assert(false);
    } catch (const std::exception& e) {
        tfm::format(std::cerr, "Error: %s\n", e.what());
        return EXIT_FAILURE;
    } catch (...) {
        tfm::format(std::cerr, "Error: unknown error\n");
        return EXIT_FAILURE;
    }
}
