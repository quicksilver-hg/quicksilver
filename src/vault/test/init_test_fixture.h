// Copyright (c) 2018-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_VAULT_TEST_INIT_TEST_FIXTURE_H
#define QUICKSILVER_VAULT_TEST_INIT_TEST_FIXTURE_H

#include <interfaces/chain.h>
#include <interfaces/vault.h>
#include <node/context.h>
#include <test/util/setup_common.h>
#include <util/chaintype.h>


namespace vault {
struct InitVaultDirTestingSetup: public BasicTestingSetup {
    explicit InitVaultDirTestingSetup(const ChainType chain_type = ChainType::MAIN);
    ~InitVaultDirTestingSetup();
    void SetVaultDir(const fs::path& vaultdir_path);

    fs::path m_datadir;
    fs::path m_cwd;
    std::map<std::string, fs::path> m_vaultdir_path_cases;
    std::unique_ptr<interfaces::VaultLoader> m_vault_loader;
};

#endif // QUICKSILVER_VAULT_TEST_INIT_TEST_FIXTURE_H
} // namespace vault
