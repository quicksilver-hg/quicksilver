// Copyright (c) 2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <vault/context.h>
#include <vault/db.h>
#include <vault/test/util.h>
#include <vault/transaction.h>
#include <vault/vault.h>
#include <vault/vaultdb.h>
#include <vault/vaultutil.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <streams.h>
#include <test/util/logging.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <utility>
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
        // Write unknown active descriptor. The descriptors flag is set too:
        // without it the load would stop on an assertion before reaching
        // what this case checks.
        VaultBatch batch(*database, false);
        BOOST_CHECK(batch.WriteVaultFlags(VAULT_FLAG_DESCRIPTORS));
        std::string unknown_desc = "trx(squb6UShJgvLuWbXj3LDnxhVTYLQkhu1KEK3EoKQBcF5rB3YZLVTqhVKEJXtYBfS5gpN1nvLJuEc8RoiCVeER3uLK9jSq9cbWgvWBULetj1FNAg/86'/1'/0'/0/*)#nndh6usm";
        VaultDescriptor vault_descriptor(std::make_shared<DummyDescriptor>(unknown_desc), 0, 0, 0, 0);
        BOOST_CHECK(batch.WriteDescriptor(uint256(), vault_descriptor));
        BOOST_CHECK(batch.WriteActiveScriptPubKeyMan(static_cast<uint8_t>(OutputType::UNKNOWN), uint256(), false));
    }

    {
        // Now try to load the vault and verify the error. The return code alone
        // would stay UNKNOWN_DESCRIPTOR without the early return too (std::max
        // over CORRUPT), so also show the load stopped at the descriptor and
        // never read the active-manager record written above (S0210-B-01).
        bool descriptor_logged{false};
        DebugLogHelper descriptor_log("Unrecognized descriptor found", [&](const std::string* s) {
            if (s) descriptor_logged = true;
            return false;
        });
        bool active_record_read{false};
        DebugLogHelper active_log("Unknown stored address type", [&](const std::string* s) {
            if (s) active_record_read = true;
            return false;
        });
        const std::shared_ptr<CVault> vault(new CVault(m_node.chain.get(), "", std::move(database)));
        BOOST_CHECK_EQUAL(vault->LoadVault(), DBErrors::UNKNOWN_DESCRIPTOR);
        BOOST_CHECK(descriptor_logged);
        BOOST_CHECK(!active_record_read);
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

// S0197-A-04: a descriptor cache record whose xpub is not BIP32_EXTKEY_SIZE bytes
// must be rejected, never decoded (CExtPubKey::Decode reads 78 bytes unchecked).
BOOST_FIXTURE_TEST_CASE(vault_load_rejects_a_short_xpub_cache_record, TestingSetup)
{
    MockableData base_records;
    uint256 spkm_id;
    {
        CVault vault(m_node.chain.get(), "", CreateMockableVaultDatabase());
        LOCK(vault.cs_vault);
        vault.SetVaultFlag(VAULT_FLAG_DESCRIPTORS);
        vault.SetupDescriptorScriptPubKeyMans();
        ScriptPubKeyMan* spkm{vault.GetScriptPubKeyMan(OutputType::BECH32, /*internal=*/false)};
        BOOST_REQUIRE(spkm);
        spkm_id = spkm->GetID();
        base_records = GetMockableDatabase(vault).m_records;
    }

    const auto with_record = [&](const std::string& type, const auto& key_tail, const std::vector<unsigned char>& xpub_bytes, bool claim_full_size) {
        MockableData records{base_records};
        DataStream key;
        key << type << spkm_id << key_tail;
        DataStream value;
        if (claim_full_size) {
            // Compact size says 78 but only xpub_bytes.size() bytes follow.
            WriteCompactSize(value, BIP32_EXTKEY_SIZE);
            value.write(MakeByteSpan(xpub_bytes));
        } else {
            value << xpub_bytes;
        }
        records[SerializeData(key.begin(), key.end())] = SerializeData(value.begin(), value.end());
        return records;
    };

    const std::vector<unsigned char> short_xpub(10, 0x01);
    const std::vector<unsigned char> full_xpub(BIP32_EXTKEY_SIZE, 0x00);
    const auto load = [&](MockableData records) {
        CVault vault(m_node.chain.get(), "", CreateMockableVaultDatabase(std::move(records)));
        return vault.LoadVault();
    };

    // Control: the unmodified records load, so a rejection below comes from the injected record.
    BOOST_REQUIRE(load(base_records) != DBErrors::CORRUPT);

    // A record of the wrong size (valid encoding) for each of the three cache record kinds.
    BOOST_CHECK_EQUAL(load(with_record("vaultdescriptorcache", uint32_t{0}, short_xpub, false)), DBErrors::CORRUPT);
    BOOST_CHECK_EQUAL(load(with_record("vaultdescriptorcache", std::make_pair(uint32_t{0}, uint32_t{1}), short_xpub, false)), DBErrors::CORRUPT);
    BOOST_CHECK_EQUAL(load(with_record("vaultdescriptorlhcache", uint32_t{0}, short_xpub, false)), DBErrors::CORRUPT);
    // A record that claims the full size and is cut short.
    BOOST_CHECK_EQUAL(load(with_record("vaultdescriptorcache", uint32_t{0}, short_xpub, true)), DBErrors::CORRUPT);
    BOOST_CHECK_EQUAL(load(with_record("vaultdescriptorlhcache", uint32_t{0}, short_xpub, true)), DBErrors::CORRUPT);
    // A record one byte too long is not accepted either.
    std::vector<unsigned char> long_xpub{full_xpub};
    long_xpub.push_back(0);
    BOOST_CHECK_EQUAL(load(with_record("vaultdescriptorcache", uint32_t{0}, long_xpub, false)), DBErrors::CORRUPT);
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

// F-679 removed the external signer, but VAULT_FLAG_EXTERNAL_SIGNER stays known:
// a vault that carries it is recognised and refused with the existing error,
// never opened as an ordinary vault that has lost its signing device.
BOOST_FIXTURE_TEST_CASE(load_refuses_external_signer_vault, TestingSetup)
{
    std::unique_ptr<VaultDatabase> database = CreateMockableVaultDatabase();
    {
        VaultBatch batch(*database, false);
        BOOST_CHECK(batch.WriteVaultFlags(VAULT_FLAG_DESCRIPTORS | VAULT_FLAG_DISABLE_PRIVATE_KEYS | VAULT_FLAG_EXTERNAL_SIGNER));
    }

    VaultContext context;
    context.args = &m_args;
    context.chain = m_node.chain.get();
    bilingual_str error;
    std::vector<bilingual_str> warnings;
    const auto vault = CVault::Create(context, "signer-vault", std::move(database), /*vault_creation_flags=*/0, error, warnings);
    BOOST_CHECK(!vault);
    // The message names the database file; the mock database is "mockable".
    BOOST_CHECK_EQUAL(error.original, "Error loading mockable: External signer vault being loaded without external signer support compiled");
}

// Nothing in the tree can create a signer vault any more, so a request carrying
// the flag is a bug: it fails before any file is made, instead of writing a
// vault the loader above would then refuse.
BOOST_FIXTURE_TEST_CASE(create_refuses_external_signer_flag, TestingSetup)
{
    VaultContext context;
    context.args = &m_args;
    context.chain = m_node.chain.get();
    DatabaseOptions options;
    options.create_flags = VAULT_FLAG_DISABLE_PRIVATE_KEYS | VAULT_FLAG_EXTERNAL_SIGNER;
    DatabaseStatus status{DatabaseStatus::SUCCESS};
    bilingual_str error;
    std::vector<bilingual_str> warnings;
    const auto vault = CreateVault(context, "signer-create", /*load_on_start=*/std::nullopt, options, status, error, warnings);
    BOOST_CHECK(!vault);
    BOOST_CHECK(status == DatabaseStatus::FAILED_CREATE);
    BOOST_CHECK_MESSAGE(error.original.find("Internal bug detected") != std::string::npos, error.original);
    BOOST_CHECK(!fs::exists(GetVaultDir() / fs::u8path("signer-create")));
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
    {
        DataStream key_copy{ssKey};
        DataStream value_copy{ssValue};
        BOOST_CHECK(!LoadEncryptionKey(&vault, key_copy, value_copy, err));
    }
    BOOST_CHECK(err.find("Pre-Quicksilver master key record; this vault cannot be opened") != std::string::npos);
    BOOST_CHECK(WITH_LOCK(vault.cs_vault, return vault.mapMasterKeys.empty()));

    // Positive control (S0210-B-02): the current three-field record loads and
    // fills the key map, so the refusal above is about the old layout, not
    // every record.
    CMasterKey current;
    current.vchCryptedKey = crypted_key;
    current.vchSalt = salt;
    current.nDeriveIterations = rounds;
    {
        DataStream key{};
        key << unsigned(1);
        DataStream value{};
        value << current;
        std::string ok_err;
        BOOST_CHECK(LoadEncryptionKey(&vault, key, value, ok_err));
        BOOST_CHECK_EQUAL(ok_err, "");
        LOCK(vault.cs_vault);
        BOOST_REQUIRE_EQUAL(vault.mapMasterKeys.count(1), 1U);
        BOOST_CHECK(vault.mapMasterKeys.at(1).vchSalt == salt);
        BOOST_CHECK_EQUAL(vault.mapMasterKeys.at(1).nDeriveIterations, rounds);
    }

    // And through LoadVault: the old record maps to CORRUPT, the current one
    // does not.
    const auto stored = [](const DataStream& value) {
        DataStream key{};
        key << std::make_pair(DBKeys::MASTER_KEY, unsigned(1));
        MockableData records;
        records.emplace(SerializeData(key.begin(), key.end()), SerializeData(value.begin(), value.end()));
        return records;
    };
    {
        CVault old_layout(m_node.chain.get(), "", CreateMockableVaultDatabase(stored(ssValue)));
        BOOST_CHECK_EQUAL(old_layout.LoadVault(), DBErrors::CORRUPT);
    }
    {
        DataStream value{};
        value << current;
        CVault current_layout(m_node.chain.get(), "", CreateMockableVaultDatabase(stored(value)));
        BOOST_CHECK(current_layout.LoadVault() != DBErrors::CORRUPT);
        BOOST_CHECK_EQUAL(WITH_LOCK(current_layout.cs_vault, return current_layout.mapMasterKeys.count(1)), 1U);
    }
}

BOOST_FIXTURE_TEST_CASE(transaction_record_in_the_base_layout_keeps_its_keys, TestingSetup)
{
    // F-698 adds a "revived" mapValue key and F-678 keeps vOrderForm, which holds a transfer's
    // Message. Neither changes the record: a transaction written in the layout of b1a45281
    // (tx, block hash, index, mapValue, vOrderForm, time received, from me) still loads, with
    // every mapValue key and the Message intact. The bytes are assembled field by field here,
    // not with CVaultTx::Serialize, so a change to the serializer cannot hide behind itself.
    CMutableTransaction mtx;
    mtx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 0});
    mtx.vout.emplace_back(COIN, CScript{} << OP_TRUE);
    const CTransactionRef tx{MakeTransactionRef(mtx)};
    const std::map<std::string, std::string> map_value{
        {"comment", "rent"},
        {"n", "7"},
        {"revived", "4"},
        {"timesmart", "1700000000"},
        {"to", "landlord"},
    };
    const std::vector<std::pair<std::string, std::string>> order_form{{"Message", "for November"}};

    DataStream key{};
    key << std::make_pair(DBKeys::TX, tx->GetHash());
    DataStream value{};
    value << TX_WITH_WITNESS(*tx) << uint256::ZERO << int{0} << map_value << order_form << static_cast<unsigned int>(1700000001) << true;
    MockableData records;
    records.emplace(SerializeData(key.begin(), key.end()), SerializeData(value.begin(), value.end()));

    CVault vault(m_node.chain.get(), "", CreateMockableVaultDatabase(records));
    BOOST_CHECK(vault.LoadVault() != DBErrors::CORRUPT);
    LOCK(vault.cs_vault);
    BOOST_REQUIRE_EQUAL(vault.mapVault.count(tx->GetHash()), 1U);
    const CVaultTx& wtx{vault.mapVault.at(tx->GetHash())};
    BOOST_CHECK(wtx.isInactive() && !wtx.isAbandoned());
    BOOST_CHECK_EQUAL(wtx.nOrderPos, 7);
    BOOST_CHECK_EQUAL(wtx.nTimeSmart, 1700000000U);
    BOOST_CHECK_EQUAL(wtx.nTimeReceived, 1700000001U);
    BOOST_CHECK(wtx.fFromMe);
    const std::map<std::string, std::string> kept{{"comment", "rent"}, {"revived", "4"}, {"to", "landlord"}};
    BOOST_CHECK(wtx.mapValue == kept);
    BOOST_CHECK(wtx.vOrderForm == order_form);
    BOOST_CHECK(wtx.GetRevivalSequence() == std::optional<int64_t>{4});
    // The next disconnect order continues past the stored one.
    BOOST_CHECK_EQUAL(vault.m_next_revival_sequence, 5);

    // Written back, the record is the same bytes as the base layout.
    DataStream rewritten{};
    rewritten << wtx;
    BOOST_CHECK(std::ranges::equal(rewritten, value));
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace vault
