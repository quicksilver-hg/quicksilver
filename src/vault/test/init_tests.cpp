// Copyright (c) 2018-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <common/args.h>
#include <noui.h>
#include <test/util/logging.h>
#include <test/util/setup_common.h>
#include <vault/test/init_test_fixture.h>

namespace vault {
BOOST_FIXTURE_TEST_SUITE(init_tests, InitVaultDirTestingSetup)

BOOST_AUTO_TEST_CASE(vaultinit_verify_vaultdir_default)
{
    SetVaultDir(m_vaultdir_path_cases["default"]);
    bool result = m_vault_loader->verify();
    BOOST_CHECK(result == true);
    fs::path vaultdir = m_args.GetPathArg("-vaultdir");
    fs::path expected_path = fs::canonical(m_vaultdir_path_cases["default"]);
    BOOST_CHECK_EQUAL(vaultdir, expected_path);
}

BOOST_AUTO_TEST_CASE(vaultinit_verify_vaultdir_custom)
{
    SetVaultDir(m_vaultdir_path_cases["custom"]);
    bool result = m_vault_loader->verify();
    BOOST_CHECK(result == true);
    fs::path vaultdir = m_args.GetPathArg("-vaultdir");
    fs::path expected_path = fs::canonical(m_vaultdir_path_cases["custom"]);
    BOOST_CHECK_EQUAL(vaultdir, expected_path);
}

BOOST_AUTO_TEST_CASE(vaultinit_verify_vaultdir_does_not_exist)
{
    SetVaultDir(m_vaultdir_path_cases["nonexistent"]);
    {
        ASSERT_DEBUG_LOG("does not exist");
        bool result = m_vault_loader->verify();
        BOOST_CHECK(result == false);
    }
}

BOOST_AUTO_TEST_CASE(vaultinit_verify_vaultdir_is_not_directory)
{
    SetVaultDir(m_vaultdir_path_cases["file"]);
    {
        ASSERT_DEBUG_LOG("is not a directory");
        bool result = m_vault_loader->verify();
        BOOST_CHECK(result == false);
    }
}

BOOST_AUTO_TEST_CASE(vaultinit_verify_vaultdir_is_not_relative)
{
    SetVaultDir(m_vaultdir_path_cases["relative"]);
    {
        ASSERT_DEBUG_LOG("is a relative path");
        bool result = m_vault_loader->verify();
        BOOST_CHECK(result == false);
    }
}

BOOST_AUTO_TEST_CASE(vaultinit_verify_vaultdir_no_trailing)
{
    SetVaultDir(m_vaultdir_path_cases["trailing"]);
    bool result = m_vault_loader->verify();
    BOOST_CHECK(result == true);
    fs::path vaultdir = m_args.GetPathArg("-vaultdir");
    fs::path expected_path = fs::canonical(m_vaultdir_path_cases["default"]);
    BOOST_CHECK_EQUAL(vaultdir, expected_path);
}

BOOST_AUTO_TEST_CASE(vaultinit_verify_vaultdir_no_trailing2)
{
    SetVaultDir(m_vaultdir_path_cases["trailing2"]);
    bool result = m_vault_loader->verify();
    BOOST_CHECK(result == true);
    fs::path vaultdir = m_args.GetPathArg("-vaultdir");
    fs::path expected_path = fs::canonical(m_vaultdir_path_cases["default"]);
    BOOST_CHECK_EQUAL(vaultdir, expected_path);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace vault
