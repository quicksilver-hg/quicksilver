// Copyright (c) 2016-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_VAULT_TEST_VAULT_TEST_FIXTURE_H
#define QUICKSILVER_VAULT_TEST_VAULT_TEST_FIXTURE_H

#include <test/util/setup_common.h>

#include <interfaces/chain.h>
#include <interfaces/vault.h>
#include <node/context.h>
#include <util/chaintype.h>
#include <util/check.h>
#include <vault/vault.h>

#include <memory>

namespace vault {
/** Testing setup and teardown for vault.
 */
struct VaultTestingSetup : public TestingSetup {
    explicit VaultTestingSetup(const ChainType chainType = ChainType::MAIN);
    ~VaultTestingSetup();

    std::unique_ptr<interfaces::VaultLoader> m_vault_loader;
    CVault m_vault;
    std::unique_ptr<interfaces::Handler> m_chain_notifications_handler;
};
} // namespace vault

#endif // QUICKSILVER_VAULT_TEST_VAULT_TEST_FIXTURE_H
