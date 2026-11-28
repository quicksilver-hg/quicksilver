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

// S0191-A-05: a write failure part way through Encrypt must leave the manager
// as it was, so a retry (after the caller aborts its DB transaction) succeeds.
// Mutation: fill m_map_crypted_keys inside the loop again; the retry then
// fails at the "already encrypted" check.
BOOST_AUTO_TEST_CASE(encrypt_failure_leaves_manager_retryable)
{
    CVault vault(m_node.chain.get(), "", CreateMockableVaultDatabase());
    const CKey key{GenerateRandomKey()};
    auto* manager{dynamic_cast<DescriptorScriptPubKeyMan*>(CreateDescriptor(vault, "tr(" + EncodeSecret(key) + ")", true))};
    BOOST_REQUIRE(manager != nullptr);
    CKeyingMaterial master_key(VAULT_CRYPTO_KEY_SIZE, 1);

    GetMockableDatabase(vault).m_fail_write_types = {"vaultdescriptorckey"};
    {
        VaultBatch batch{vault.GetDatabase()};
        BOOST_CHECK(!manager->Encrypt(master_key, &batch));
    }
    // Still unencrypted: the private key is available in the clear.
    BOOST_CHECK(manager->GetSigningProvider(key.GetPubKey()) != nullptr);

    GetMockableDatabase(vault).m_fail_write_types.clear();
    VaultBatch batch{vault.GetDatabase()};
    BOOST_CHECK(manager->Encrypt(master_key, &batch));
}

// S0201-A-01: a failed write leaves the in-memory lock set as it was.
// Mutation: insert into setLockedCoins before the write again.
BOOST_AUTO_TEST_CASE(lock_coin_failed_write_leaves_coin_unlocked)
{
    CVault vault(m_node.chain.get(), "", CreateMockableVaultDatabase());
    const COutPoint outpoint{Txid::FromUint256(uint256::ONE), 0};
    LOCK(vault.cs_vault);
    GetMockableDatabase(vault).m_fail_write_types = {"lockedutxo"};
    {
        VaultBatch batch{vault.GetDatabase()};
        BOOST_CHECK(!vault.LockCoin(outpoint, &batch));
    }
    BOOST_CHECK(!vault.IsLockedCoin(outpoint));
    GetMockableDatabase(vault).m_fail_write_types.clear();
    VaultBatch batch{vault.GetDatabase()};
    BOOST_CHECK(vault.LockCoin(outpoint, &batch));
    BOOST_CHECK(vault.IsLockedCoin(outpoint));
}

// S0204-A-04: a vault flag changes in memory only after its write succeeded.
// Mutation: assign m_vault_flags before the write in SetVaultFlagWithDB or UnsetVaultFlagWithDB.
BOOST_AUTO_TEST_CASE(vault_flag_change_with_a_failed_write_leaves_the_flag_as_it_was)
{
    CVault vault(m_node.chain.get(), "", CreateMockableVaultDatabase());
    GetMockableDatabase(vault).m_fail_write_types = {"flags"};
    BOOST_CHECK_THROW(vault.SetVaultFlag(VAULT_FLAG_AVOID_REUSE), std::runtime_error);
    BOOST_CHECK(!vault.IsVaultFlagSet(VAULT_FLAG_AVOID_REUSE));

    GetMockableDatabase(vault).m_fail_write_types.clear();
    vault.SetVaultFlag(VAULT_FLAG_AVOID_REUSE);
    BOOST_CHECK(vault.IsVaultFlagSet(VAULT_FLAG_AVOID_REUSE));

    GetMockableDatabase(vault).m_fail_write_types = {"flags"};
    BOOST_CHECK_THROW(vault.UnsetVaultFlag(VAULT_FLAG_AVOID_REUSE), std::runtime_error);
    BOOST_CHECK(vault.IsVaultFlagSet(VAULT_FLAG_AVOID_REUSE));
}

namespace {
//! Exposes the descriptor so a test can drop its cache while the pubkey map stays filled.
class CachelessDescriptorSpkm : public DescriptorScriptPubKeyMan
{
public:
    using DescriptorScriptPubKeyMan::DescriptorScriptPubKeyMan;
    void DropCache()
    {
        LOCK(cs_desc_man);
        m_vault_descriptor.cache = DescriptorCache{};
    }
};
} // namespace

// S0191-A-02: a pubkey that is in m_map_pubkeys but whose signing provider cannot
// be built (the cache no longer expands its index) gets nullptr, not a null dereference.
// Mutation: drop the "!out ||" test in GetSigningProvider(const CPubKey&).
BOOST_AUTO_TEST_CASE(signing_provider_for_a_mapped_pubkey_that_cannot_be_built_is_null)
{
    CExtKey master;
    const std::vector<unsigned char> seed(32, 0x42);
    master.SetSeed(MakeByteSpan(seed));
    const std::string desc_str{"wpkh(" + EncodeExtKey(master) + "/0/*)"};

    // A manager over that descriptor, with its private key and the pubkey map filled by TopUp.
    // The pubkey of index 0 is derived independently of the manager under test.
    CPubKey pubkey;
    const auto make_manager = [&](CVault& vault) {
        FlatSigningProvider keys;
        std::string error;
        auto parsed{Parse(desc_str, keys, error, /*require_checksum=*/false)};
        BOOST_REQUIRE(!parsed.empty());
        FlatSigningProvider expanded;
        std::vector<CScript> scripts;
        BOOST_REQUIRE(parsed.at(0)->Expand(0, keys, scripts, expanded));
        BOOST_REQUIRE_EQUAL(expanded.pubkeys.size(), 1U);
        pubkey = expanded.pubkeys.begin()->second;
        VaultDescriptor vault_desc(std::move(parsed.at(0)), /*creation_time=*/1, /*range_start=*/0, /*range_end=*/1, /*next_index=*/0);
        auto manager{std::make_unique<CachelessDescriptorSpkm>(vault, vault_desc, /*keypool_size=*/1)};
        for (const auto& [id, key] : keys.keys) manager->AddDescriptorKey(key, key.GetPubKey());
        BOOST_REQUIRE(manager->TopUp());
        return manager;
    };

    // Control: the pubkey is mapped and its provider builds, with the private key.
    {
        CVault vault(m_node.chain.get(), "", CreateMockableVaultDatabase());
        vault.SetVaultFlag(VAULT_FLAG_DESCRIPTORS);
        auto manager{make_manager(vault)};
        auto provider{manager->GetSigningProvider(pubkey)};
        BOOST_REQUIRE(provider != nullptr);
        BOOST_CHECK(provider->HaveKey(pubkey.GetID()));
    }
    // Same manager state but the cache cannot expand index 0 (and no provider was
    // cached yet): nullptr, not a dereference of the failed build.
    {
        // A separate vault: the key record of the first one cannot be written twice.
        CVault vault(m_node.chain.get(), "", CreateMockableVaultDatabase());
        vault.SetVaultFlag(VAULT_FLAG_DESCRIPTORS);
        auto manager{make_manager(vault)};
        manager->DropCache();
        BOOST_CHECK(manager->GetSigningProvider(pubkey) == nullptr);
    }
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace vault
