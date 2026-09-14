// Copyright (c) 2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <vault/test/util.h>
#include <vault/transaction.h>
#include <vault/vault.h>
#include <vault/vaultdb.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <streams.h>
#include <test/util/logging.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <string>
#include <vector>

namespace vault {

BOOST_AUTO_TEST_SUITE(vaultload_tests)

class DummyDescriptor final : public Descriptor {
private:
    std::string desc;
public:
    explicit DummyDescriptor(const std::string& descriptor) : desc(descriptor) {};
    ~DummyDescriptor() = default;

    std::string ToString(bool compat_format) const override { return desc; }
    std::optional<OutputType> GetOutputType() const override { return OutputType::UNKNOWN; }

    bool IsRange() const override { return false; }
    bool IsSolvable() const override { return false; }
    bool IsSingleType() const override { return true; }
    bool ToPrivateString(const SigningProvider& provider, std::string& out) const override { return false; }
    bool ToNormalizedString(const SigningProvider& provider, std::string& out, const DescriptorCache* cache = nullptr) const override { return false; }
    bool Expand(int pos, const SigningProvider& provider, std::vector<CScript>& output_scripts, FlatSigningProvider& out, DescriptorCache* write_cache = nullptr) const override { return false; };
    bool ExpandFromCache(int pos, const DescriptorCache& read_cache, std::vector<CScript>& output_scripts, FlatSigningProvider& out) const override { return false; }
    void ExpandPrivate(int pos, const SigningProvider& provider, FlatSigningProvider& out) const override {}
    std::optional<int64_t> ScriptSize() const override { return {}; }
    std::optional<int64_t> MaxSatisfactionWeight(bool) const override { return {}; }
    std::optional<int64_t> MaxSatisfactionElems() const override { return {}; }
    void GetPubKeys(std::set<CPubKey>& pubkeys, std::set<CExtPubKey>& ext_pubs) const override {}
};

BOOST_FIXTURE_TEST_CASE(vault_load_descriptors, TestingSetup)
{
    std::unique_ptr<VaultDatabase> database = CreateMockableVaultDatabase();
    {
        // Write unknown active descriptor
        VaultBatch batch(*database, false);
        std::string unknown_desc = "trx(squb6UShJgvLuWbXj3LDnxhVTYLQkhu1KEK3EoKQBcF5rB3YZLVTqhVKEJXtYBfS5gpN1nvLJuEc8RoiCVeER3uLK9jSq9cbWgvWBULetj1FNAg/86'/1'/0'/0/*)#nndh6usm";
        VaultDescriptor vault_descriptor(std::make_shared<DummyDescriptor>(unknown_desc), 0, 0, 0, 0);
        BOOST_CHECK(batch.WriteDescriptor(uint256(), vault_descriptor));
        BOOST_CHECK(batch.WriteActiveScriptPubKeyMan(static_cast<uint8_t>(OutputType::UNKNOWN), uint256(), false));
    }

    {
        // Now try to load the vault and verify the error.
        const std::shared_ptr<CVault> vault(new CVault(m_node.chain.get(), "", std::move(database)));
        BOOST_CHECK_EQUAL(vault->LoadVault(), DBErrors::UNKNOWN_DESCRIPTOR);
    }

    // Test 2
    // Now write a valid descriptor with an invalid ID.
    // As the software produces another ID for the descriptor, the loading process must be aborted.
    database = CreateMockableVaultDatabase();

    // Verify the error
    bool found = false;
    DebugLogHelper logHelper("The descriptor ID calculated by the vault differs from the one in DB", [&](const std::string* s) {
        found = true;
        return false;
    });

    {
        // Write valid descriptor with invalid ID
        VaultBatch batch(*database, false);
        std::string desc = "wpkh([d34db33f/84h/9556h/0h]qpub3wUBVhnZq5yiYohGtmWwsZnWWEZnLfwy2fAECN8WBL5KXFRokRHyqiBt9TQug6UZpSPqYNBp2MvqLn7nHMzxDw49NcpUkudZ5uMiSigN9MT/0/*)#yvgzdg90";
        VaultDescriptor vault_descriptor(std::make_shared<DummyDescriptor>(desc), 0, 0, 0, 0);
        BOOST_CHECK(batch.WriteDescriptor(uint256::ONE, vault_descriptor));
    }

    {
        // Now try to load the vault and verify the error.
        const std::shared_ptr<CVault> vault(new CVault(m_node.chain.get(), "", std::move(database)));
        BOOST_CHECK_EQUAL(vault->LoadVault(), DBErrors::CORRUPT);
        BOOST_CHECK(found); // The error must be logged
    }
}

BOOST_FIXTURE_TEST_CASE(vault_load_rejects_an_unknown_address_type, TestingSetup)
{
    constexpr uint8_t UNKNOWN_ADDRESS_TYPE{255};
    BOOST_REQUIRE(!OutputTypeFromStored(UNKNOWN_ADDRESS_TYPE));

    MockableData invalid_records;
    {
        CVault vault(m_node.chain.get(), "", CreateMockableVaultDatabase());
        uint256 spkm_id;
        {
            LOCK(vault.cs_vault);
            vault.SetVaultFlag(VAULT_FLAG_DESCRIPTORS);
            vault.SetupDescriptorScriptPubKeyMans();
            ScriptPubKeyMan* spkm{vault.GetScriptPubKeyMan(OutputType::BECH32, /*internal=*/false)};
            BOOST_REQUIRE(spkm);
            spkm_id = spkm->GetID();
        }
        BOOST_CHECK(VaultBatch(vault.GetDatabase()).WriteActiveScriptPubKeyMan(UNKNOWN_ADDRESS_TYPE, spkm_id, /*internal=*/false));
        invalid_records = GetMockableDatabase(vault).m_records;
    }

    std::unique_ptr<VaultDatabase> database = CreateMockableVaultDatabase(invalid_records);

    bool logged = false;
    DebugLogHelper log_helper("Unknown stored address type 255", [&](const std::string* s) {
        logged = true;
        return false;
    });

    const std::shared_ptr<CVault> vault(new CVault(m_node.chain.get(), "", std::move(database)));
    BOOST_CHECK_EQUAL(vault->LoadVault(), DBErrors::CORRUPT);
    BOOST_CHECK(logged);
}

BOOST_FIXTURE_TEST_CASE(vault_load_rejects_a_transaction_without_an_order_position, TestingSetup)
{
    std::unique_ptr<VaultDatabase> database = CreateMockableVaultDatabase();
    {
        CVaultTx tx{MakeTransactionRef(CMutableTransaction{}), TxStateInactive{}};
        BOOST_REQUIRE_EQUAL(tx.nOrderPos, -1);
        BOOST_CHECK(VaultBatch(*database).WriteTx(tx));
    }

    bool logged = false;
    DebugLogHelper log_helper("Transaction record is missing its vault order position", [&](const std::string* s) {
        logged = true;
        return false;
    });

    CVault vault(m_node.chain.get(), "", std::move(database));
    BOOST_CHECK_EQUAL(vault.LoadVault(), DBErrors::CORRUPT);
    BOOST_CHECK(logged);
}

static SerializeData SerializedKey(const std::string& key)
{
    DataStream ss;
    ss << key;
    return SerializeData(ss.begin(), ss.end());
}

BOOST_FIXTURE_TEST_CASE(write_bestblock_stores_the_real_locator, TestingSetup)
{
    MockableDatabase db;
    CBlockLocator locator{std::vector<uint256>{uint256{1}, uint256{2}}};
    BOOST_CHECK(VaultBatch(db).WriteBestBlock(locator));

    CBlockLocator loaded;
    BOOST_CHECK(VaultBatch(db).ReadBestBlock(loaded));
    BOOST_CHECK(loaded.vHave == locator.vHave);

    const SerializeData bestblock_key{SerializedKey("bestblock")};
    const SerializeData nomerkle_key{SerializedKey("bestblock_nomerkle")};
    BOOST_CHECK(db.m_records.count(bestblock_key));
    BOOST_CHECK(!db.m_records.count(nomerkle_key));

    DataStream expected;
    expected << locator;
    const SerializeData expected_value(expected.begin(), expected.end());
    BOOST_CHECK(db.m_records.at(bestblock_key) == expected_value);
}

BOOST_FIXTURE_TEST_CASE(read_bestblock_ignores_leftover_nomerkle, TestingSetup)
{
    MockableDatabase db;
    CBlockLocator leftover{std::vector<uint256>{uint256{9}}};
    BOOST_CHECK(db.MakeBatch()->Write(std::string{"bestblock_nomerkle"}, leftover));

    CBlockLocator loaded;
    BOOST_CHECK(!VaultBatch(db).ReadBestBlock(loaded));
    BOOST_CHECK(loaded.IsNull());
}

BOOST_FIXTURE_TEST_CASE(load_rejects_minversion_newer_than_latest, TestingSetup)
{
    std::unique_ptr<VaultDatabase> database = CreateMockableVaultDatabase();
    {
        VaultBatch batch(*database, false);
        BOOST_CHECK(batch.WriteMinVersion(VAULT_FILE_VERSION + 1));
    }

    CVault vault(m_node.chain.get(), "", std::move(database));
    BOOST_CHECK_EQUAL(vault.LoadVault(), DBErrors::TOO_NEW);
}

BOOST_FIXTURE_TEST_CASE(load_rejects_pre_quicksilver_master_key, TestingSetup)
{
    // Five-field mkey value written before the dummy derivation slots were
    // dropped. The third unsigned is always 0 (the unimplemented-scrypt
    // method word). Without the nDeriveIterations==0 check, CMasterKey
    // reads that 0 into nDeriveIterations and LoadEncryptionKey ignores
    // the leftover iteration count + empty compact-size, so the vault
    // opens and later reports a bad passphrase.
    std::vector<unsigned char> crypted_key(32, 0xab);
    std::vector<unsigned char> salt(8, 0xcd);
    const unsigned int unused_method{0};
    const unsigned int rounds{25000};
    const std::vector<unsigned char> unused_params{};

    DataStream ssKey;
    ssKey << unsigned(1);
    DataStream ssValue;
    ssValue << crypted_key << salt << unused_method << rounds << unused_params;

    CVault vault(m_node.chain.get(), "", CreateMockableVaultDatabase());
    std::string err;
    BOOST_CHECK(!LoadEncryptionKey(&vault, ssKey, ssValue, err));
    BOOST_CHECK(err.find("Pre-Quicksilver master key record; this vault cannot be opened") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace vault
