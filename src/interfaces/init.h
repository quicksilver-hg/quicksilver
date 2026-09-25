// Copyright (c) 2021-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_INTERFACES_INIT_H
#define QUICKSILVER_INTERFACES_INIT_H

#include <interfaces/chain.h>
#include <interfaces/mining.h>
#include <interfaces/node.h>
#include <interfaces/vault.h>

#include <memory>

namespace node {
struct NodeContext;
} // namespace node

namespace interfaces {
//! Initial interface created when a process is first started, and used to give
//! and get access to other interfaces (Node, Chain, Vault, etc).
//!
//! There is a different Init interface implementation for each executable
//! (quicksilver-daemon, quicksilver) and each implementation can implement the make
//! methods for interfaces it supports. The default make methods all return null.
class Init
{
public:
    virtual ~Init() = default;
    virtual std::unique_ptr<Node> makeNode() { return nullptr; }
    virtual std::unique_ptr<Chain> makeChain() { return nullptr; }
    virtual std::unique_ptr<Mining> makeMining() { return nullptr; }
    virtual std::unique_ptr<VaultLoader> makeVaultLoader(Chain& chain) { return nullptr; }
};

//! Return implementation of Init interface for the daemon.
std::unique_ptr<Init> MakeNodeInit(node::NodeContext& node);

//! Return implementation of Init interface for the gui.
std::unique_ptr<Init> MakeGuiInit();
} // namespace interfaces

#endif // QUICKSILVER_INTERFACES_INIT_H
