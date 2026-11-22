// Copyright (c) 2020-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <key.h>
#include <key_io.h>
#include <test/util/setup_common.h>
#include <script/solver.h>
#include <vault/scriptpubkeyman.h>
#include <vault/vault.h>
#include <vault/vaultdb.h>
#include <vault/test/util.h>

#include <boost/test/unit_test.hpp>

namespace vault {
BOOST_FIXTURE_TEST_SUITE(scriptpubkeyman_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(DescriptorScriptPubKeyManTests)
{
    std::unique_ptr<interfaces::Chain>& chain = m_node.chain;

    CVault keystore(chain.get(), "", CreateMockableVaultDatabase());
    auto key_scriptpath = GenerateRandomKey();

    // Verify that a SigningProvider for a pubkey is only returned if its corresponding private key is available
    auto key_internal = GenerateRandomKey();
    std::string desc_str = "tr(" + EncodeSecret(key_internal) + ",pk(" + HexStr(key_scriptpath.GetPubKey()) + "))";
    auto spk_man1 = dynamic_cast<DescriptorScriptPubKeyMan*>(CreateDescriptor(keystore, desc_str, true));
    // REQUIRE: the next line dereferences it (S0207-B-02).
    BOOST_REQUIRE(spk_man1 != nullptr);
    auto signprov_keypath_spendable = spk_man1->GetSigningProvider(key_internal.GetPubKey());
    BOOST_REQUIRE(signprov_keypath_spendable != nullptr);
    BOOST_CHECK(signprov_keypath_spendable->HaveKey(key_internal.GetPubKey().GetID()));
    // The script-path key is in the descriptor as a public key only, so it has
    // no signing provider; nor does a key the descriptor never mentions
    // (S0207-B-03).
    BOOST_CHECK(spk_man1->GetSigningProvider(key_scriptpath.GetPubKey()) == nullptr);
    BOOST_CHECK(spk_man1->GetSigningProvider(GenerateRandomKey().GetPubKey()) == nullptr);

    desc_str = "tr(" + HexStr(XOnlyPubKey::NUMS_H) + ",pk(" + HexStr(key_scriptpath.GetPubKey()) + "))";
    auto spk_man2 = dynamic_cast<DescriptorScriptPubKeyMan*>(CreateDescriptor(keystore, desc_str, true));
    BOOST_REQUIRE(spk_man2 != nullptr);
    auto signprov_keypath_nums_h = spk_man2->GetSigningProvider(XOnlyPubKey::NUMS_H.GetEvenCorrespondingCPubKey());
    BOOST_CHECK(signprov_keypath_nums_h == nullptr);
}

BOOST_AUTO_TEST_CASE(encrypt_checks_crypted_descriptor_key_write)
{
    CVault vault(m_node.chain.get(), "", CreateMockableVaultDatabase());
    const CKey key{GenerateRandomKey()};
    auto* manager{dynamic_cast<DescriptorScriptPubKeyMan*>(CreateDescriptor(vault, "tr(" + EncodeSecret(key) + ")", true))};
    BOOST_REQUIRE(manager != nullptr);
    GetMockableDatabase(vault).m_fail_descriptor_key_erase = true;
    CKeyingMaterial master_key(VAULT_CRYPTO_KEY_SIZE, 1);
    VaultBatch batch{vault.GetDatabase()};
    BOOST_CHECK(!manager->Encrypt(master_key, &batch));
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace vault
