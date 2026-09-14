// Copyright (c) 2016-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <vault/test/util.h>
#include <vault/test/vault_test_fixture.h>

#include <scheduler.h>
#include <util/chaintype.h>

namespace vault {
VaultTestingSetup::VaultTestingSetup(const ChainType chainType)
    : TestingSetup(chainType),
      m_vault_loader{interfaces::MakeVaultLoader(*m_node.chain, *Assert(m_node.args))},
      m_vault(m_node.chain.get(), "", CreateMockableVaultDatabase())
{
    m_vault.LoadVault();
    m_chain_notifications_handler = m_node.chain->handleNotifications({ &m_vault, [](CVault*) {} });
    m_vault_loader->registerRpcs();
}

VaultTestingSetup::~VaultTestingSetup()
{
    if (m_node.scheduler) m_node.scheduler->stop();
}
} // namespace vault
