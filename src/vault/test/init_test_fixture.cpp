// Copyright (c) 2018-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/args.h>
#include <univalue.h>
#include <util/chaintype.h>
#include <util/check.h>
#include <util/fs.h>

#include <fstream>
#include <string>

#include <vault/test/init_test_fixture.h>

namespace vault {
InitVaultDirTestingSetup::InitVaultDirTestingSetup(const ChainType chainType) : BasicTestingSetup(chainType)
{
    m_vault_loader = MakeVaultLoader(*m_node.chain, m_args);

    const auto sep = fs::path::preferred_separator;

    m_datadir = m_args.GetDataDirNet();
    m_cwd = fs::current_path();

    m_vaultdir_path_cases["default"] = m_datadir / "vaults";
    m_vaultdir_path_cases["custom"] = m_datadir / "my_vaults";
    m_vaultdir_path_cases["nonexistent"] = m_datadir / "path_does_not_exist";
    m_vaultdir_path_cases["file"] = m_datadir / "not_a_directory.dat";
    m_vaultdir_path_cases["trailing"] = (m_datadir / "vaults") + sep;
    m_vaultdir_path_cases["trailing2"] = (m_datadir / "vaults") + sep + sep;

    fs::current_path(m_datadir);
    m_vaultdir_path_cases["relative"] = "vaults";

    fs::create_directories(m_vaultdir_path_cases["default"]);
    fs::create_directories(m_vaultdir_path_cases["custom"]);
    fs::create_directories(m_vaultdir_path_cases["relative"]);
    std::ofstream f{m_vaultdir_path_cases["file"]};
    f.close();
}

InitVaultDirTestingSetup::~InitVaultDirTestingSetup()
{
    fs::current_path(m_cwd);
}

void InitVaultDirTestingSetup::SetVaultDir(const fs::path& vaultdir_path)
{
    m_args.ForceSetArg("-vaultdir", fs::PathToString(vaultdir_path));
}
} // namespace vault
