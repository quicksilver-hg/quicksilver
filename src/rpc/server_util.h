// Copyright (c) 2021-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_RPC_SERVER_UTIL_H
#define QUICKSILVER_RPC_SERVER_UTIL_H

#include <any>

#include <consensus/params.h>

class AddrMan;
class ArgsManager;
class CBlockIndex;
class CConnman;
class CTxRelayPool;
class ChainstateManager;
class PeerManager;
class BanMan;
namespace node {
struct NodeContext;
} // namespace node
namespace interfaces {
class Mining;
} // namespace interfaces

node::NodeContext& EnsureAnyNodeContext(const std::any& context);
CTxRelayPool& EnsureRelayPool(const node::NodeContext& node);
CTxRelayPool& EnsureAnyRelayPool(const std::any& context);
BanMan& EnsureBanman(const node::NodeContext& node);
BanMan& EnsureAnyBanman(const std::any& context);
ArgsManager& EnsureArgsman(const node::NodeContext& node);
ArgsManager& EnsureAnyArgsman(const std::any& context);
ChainstateManager& EnsureChainman(const node::NodeContext& node);
ChainstateManager& EnsureAnyChainman(const std::any& context);
CConnman& EnsureConnman(const node::NodeContext& node);
interfaces::Mining& EnsureMining(const node::NodeContext& node);
PeerManager& EnsurePeerman(const node::NodeContext& node);
AddrMan& EnsureAddrman(const node::NodeContext& node);
AddrMan& EnsureAnyAddrman(const std::any& context);

/** Return an empty block index on top of the tip, with height, time and nBits set */
void NextEmptyBlockIndex(CBlockIndex& tip, const Consensus::Params& consensusParams, CBlockIndex& next_index);

#endif // QUICKSILVER_RPC_SERVER_UTIL_H
