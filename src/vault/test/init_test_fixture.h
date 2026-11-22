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
#include <util/fs.h>

#include <map>
#include <memory>
#include <string>

namespace vault {
struct InitVaultDirTestingSetup: public BasicTestingSetup {
    //! Restores the working directory on destruction. A member, so it runs
    //! even when the constructor throws after changing directory (S0206-B-06).
    struct ScopedCurrentPath {
        fs::path saved{fs::current_path()};
        ~ScopedCurrentPath() { fs::current_path(saved); }
    };

    explicit InitVaultDirTestingSetup(const ChainType chain_type = ChainType::MAIN);
    ~InitVaultDirTestingSetup();
    void SetVaultDir(const fs::path& vaultdir_path);

    ScopedCurrentPath m_restore_cwd;
    fs::path m_datadir;
    std::map<std::string, fs::path> m_vaultdir_path_cases;
    std::unique_ptr<interfaces::VaultLoader> m_vault_loader;
};

} // namespace vault

#endif // QUICKSILVER_VAULT_TEST_INIT_TEST_FIXTURE_H
