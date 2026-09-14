// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparamsbase.h>

#include <common/args.h>
#include <tinyformat.h>
#include <util/chaintype.h>

#include <assert.h>

void SetupChainParamsBaseOptions(ArgsManager& argsman)
{
    argsman.AddArg("-chain=<chain>", "Use the chain <chain> (default: main). Allowed values: " LIST_CHAIN_NAMES, ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-sandbox", "Use the Quicksilver Sandbox, a private local chain in which blocks can be solved instantly. "
                 "This is intended for tests, tools, demonstrations, and app development. Equivalent to -chain=sandbox.", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-testactivationheight=name@height.", "Set the activation height of 'name' (segwit, bip34, dersig, cltv, csv). (sandbox-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-txpownocycle", "Skip real per-transaction Cuckatoo cycle solving and verification on sandbox, while still checking anchor recency and proof-hash target. For functional tests only.", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-publictest", "Use the Quicksilver Public Test Network. Equivalent to -chain=publictest.", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-vbparams=deployment:start:end[:min_activation_height]", "Use given start/end times and min_activation_height for specified version bits deployment (sandbox-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
}

static std::unique_ptr<CBaseChainParams> globalChainBaseParams;

const CBaseChainParams& BaseParams()
{
    assert(globalChainBaseParams);
    return *globalChainBaseParams;
}

/**
 * Quicksilver RPC defaults. Main, publictest, and sandbox use Quicksilver-owned
 * ports.
 */
std::unique_ptr<CBaseChainParams> CreateBaseChainParams(const ChainType chain)
{
    switch (chain) {
    case ChainType::MAIN:
        return std::make_unique<CBaseChainParams>("", 9554);
    case ChainType::PUBLIC_TEST:
        return std::make_unique<CBaseChainParams>("publictest", 19558);
    case ChainType::SANDBOX:
        return std::make_unique<CBaseChainParams>("sandbox", 19557);
    }
    assert(false);
}

void SelectBaseParams(const ChainType chain)
{
    globalChainBaseParams = CreateBaseChainParams(chain);
    gArgs.SelectConfigNetwork(ChainTypeToString(chain));
}
