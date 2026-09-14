// Copyright (c) 2021-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <vault/test/util.h>

#include <chain.h>
#include <key.h>
#include <key_io.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <validationinterface.h>
#include <vault/context.h>
#include <vault/vault.h>
#include <vault/vaultdb.h>

#include <memory>

namespace vault {
std::unique_ptr<CVault> CreateSyncedVault(interfaces::Chain& chain, CChain& cchain, const CKey& key)
{
    auto vault = std::make_unique<CVault>(&chain, "", CreateMockableVaultDatabase());
    {
        LOCK2(vault->cs_vault, ::cs_main);
        vault->SetLastBlockProcessed(cchain.Height(), cchain.Tip()->GetBlockHash());
    }
    {
        LOCK(vault->cs_vault);
        vault->SetVaultFlag(VAULT_FLAG_DESCRIPTORS);
        vault->SetupDescriptorScriptPubKeyMans();

        FlatSigningProvider provider;
        std::string error;
        auto descs = Parse("combo(" + EncodeSecret(key) + ")", provider, error, /* require_checksum=*/ false);
        assert(descs.size() == 1);
        auto& desc = descs.at(0);
        VaultDescriptor w_desc(std::move(desc), 0, 0, 1, 1);
        if (!vault->AddVaultDescriptor(w_desc, provider, "", false)) assert(false);
    }
    VaultRescanReserver reserver(*vault);
    reserver.reserve();
    CVault::ScanResult result = vault->ScanForVaultTransactions(cchain.Genesis()->GetBlockHash(), /*start_height=*/0, /*max_height=*/{}, reserver, /*fUpdate=*/false, /*save_progress=*/false);
    assert(result.status == CVault::ScanResult::SUCCESS);
    assert(result.last_scanned_block == cchain.Tip()->GetBlockHash());
    assert(*result.last_scanned_height == cchain.Height());
    assert(result.last_failed_block.IsNull());
    return vault;
}

std::shared_ptr<CVault> TestLoadVault(std::unique_ptr<VaultDatabase> database, VaultContext& context, uint64_t create_flags)
{
    bilingual_str error;
    std::vector<bilingual_str> warnings;
    auto vault = CVault::Create(context, "", std::move(database), create_flags, error, warnings);
    NotifyVaultLoaded(context, vault);
    if (context.chain) {
        vault->postInitProcess();
    }
    return vault;
}

std::shared_ptr<CVault> TestLoadVault(VaultContext& context)
{
    DatabaseOptions options;
    options.create_flags = VAULT_FLAG_DESCRIPTORS;
    DatabaseStatus status;
    bilingual_str error;
    std::vector<bilingual_str> warnings;
    auto database = MakeVaultDatabase("", options, status, error);
    return TestLoadVault(std::move(database), context, options.create_flags);
}

void TestUnloadVault(std::shared_ptr<CVault>&& vault)
{
    // Calls SyncWithValidationInterfaceQueue
    vault->chain().waitForNotificationsIfTipChanged({});
    vault->m_chain_notifications_handler.reset();
    WaitForDeleteVault(std::move(vault));
}

std::unique_ptr<VaultDatabase> DuplicateMockDatabase(VaultDatabase& database)
{
    return std::make_unique<MockableDatabase>(dynamic_cast<MockableDatabase&>(database).m_records);
}

std::string getnewaddress(CVault& w)
{
    constexpr auto output_type = OutputType::BECH32;
    return EncodeDestination(getNewDestination(w, output_type));
}

CTxDestination getNewDestination(CVault& w, OutputType output_type)
{
    return *Assert(w.GetNewDestination(output_type, ""));
}

MockableCursor::MockableCursor(const MockableData& records, bool pass, Span<const std::byte> prefix)
{
    m_pass = pass;
    std::tie(m_cursor, m_cursor_end) = records.equal_range(BytePrefix{prefix});
}

DatabaseCursor::Status MockableCursor::Next(DataStream& key, DataStream& value)
{
    if (!m_pass) {
        return Status::FAIL;
    }
    if (m_cursor == m_cursor_end) {
        return Status::DONE;
    }
    key.clear();
    value.clear();
    const auto& [key_data, value_data] = *m_cursor;
    key.write(key_data);
    value.write(value_data);
    m_cursor++;
    return Status::MORE;
}

bool MockableBatch::ReadKey(DataStream&& key, DataStream& value)
{
    if (!m_pass) {
        return false;
    }
    SerializeData key_data{key.begin(), key.end()};
    const auto& it = m_records.find(key_data);
    if (it == m_records.end()) {
        return false;
    }
    value.clear();
    value.write(it->second);
    return true;
}

bool MockableBatch::WriteKey(DataStream&& key, DataStream&& value, bool overwrite)
{
    if (!m_pass) {
        return false;
    }
    SerializeData key_data{key.begin(), key.end()};
    SerializeData value_data{value.begin(), value.end()};
    auto [it, inserted] = m_records.emplace(key_data, value_data);
    if (!inserted && overwrite) { // Overwrite if requested
        it->second = value_data;
        inserted = true;
    }
    return inserted;
}

bool MockableBatch::EraseKey(DataStream&& key)
{
    if (!m_pass) {
        return false;
    }
    SerializeData key_data{key.begin(), key.end()};
    m_records.erase(key_data);
    return true;
}

bool MockableBatch::HasKey(DataStream&& key)
{
    if (!m_pass) {
        return false;
    }
    SerializeData key_data{key.begin(), key.end()};
    return m_records.count(key_data) > 0;
}

bool MockableBatch::ErasePrefix(Span<const std::byte> prefix)
{
    if (!m_pass) {
        return false;
    }
    auto it = m_records.begin();
    while (it != m_records.end()) {
        auto& key = it->first;
        if (key.size() < prefix.size() || std::search(key.begin(), key.end(), prefix.begin(), prefix.end()) != key.begin()) {
            it++;
            continue;
        }
        it = m_records.erase(it);
    }
    return true;
}

std::unique_ptr<VaultDatabase> CreateMockableVaultDatabase(MockableData records)
{
    return std::make_unique<MockableDatabase>(records);
}

MockableDatabase& GetMockableDatabase(CVault& vault)
{
    return dynamic_cast<MockableDatabase&>(vault.GetDatabase());
}

vault::ScriptPubKeyMan* CreateDescriptor(CVault& keystore, const std::string& desc_str, const bool success)
{
    keystore.SetVaultFlag(VAULT_FLAG_DESCRIPTORS);

    FlatSigningProvider keys;
    std::string error;
    auto parsed_descs = Parse(desc_str, keys, error, false);
    Assert(success == (!parsed_descs.empty()));
    if (!success) return nullptr;
    auto& desc = parsed_descs.at(0);

    const int64_t range_start = 0, range_end = 1, next_index = 0, timestamp = 1;

    VaultDescriptor w_desc(std::move(desc), timestamp, range_start, range_end, next_index);

    LOCK(keystore.cs_vault);

    return Assert(keystore.AddVaultDescriptor(w_desc, keys,/*label=*/"", /*internal=*/false));
};
} // namespace vault
