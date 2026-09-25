// Copyright (c) 2021-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <init.h>
#include <interfaces/chain.h>
#include <interfaces/init.h>
#include <interfaces/mining.h>
#include <interfaces/node.h>
#include <interfaces/vault.h>
#include <node/context.h>
#include <util/check.h>

#include <memory>

namespace init {
namespace {
class QuicksilverInit : public interfaces::Init
{
public:
    QuicksilverInit()
    {
        InitContext(m_node);
        m_node.init = this;
    }
    std::unique_ptr<interfaces::Node> makeNode() override { return interfaces::MakeNode(m_node); }
    std::unique_ptr<interfaces::Chain> makeChain() override { return interfaces::MakeChain(m_node); }
    std::unique_ptr<interfaces::Mining> makeMining() override { return interfaces::MakeMining(m_node); }
    std::unique_ptr<interfaces::VaultLoader> makeVaultLoader(interfaces::Chain& chain) override
    {
        return MakeVaultLoader(chain, *Assert(m_node.args));
    }
    node::NodeContext m_node;
};
} // namespace
} // namespace init

namespace interfaces {
std::unique_ptr<Init> MakeGuiInit()
{
    return std::make_unique<init::QuicksilverInit>();
}
} // namespace interfaces
