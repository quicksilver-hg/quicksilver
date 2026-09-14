// Copyright (c) 2021-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_VAULT_TEST_UTIL_H
#define QUICKSILVER_VAULT_TEST_UTIL_H

#include <quicksilver-build-config.h> // IWYU pragma: keep

#include <addresstype.h>
#include <vault/db.h>
#include <vault/scriptpubkeyman.h>

#include <memory>

class ArgsManager;
class CChain;
class CKey;
enum class OutputType;
namespace interfaces {
class Chain;
} // namespace interfaces

namespace vault {
class CVault;
class VaultDatabase;
struct VaultContext;

static const DatabaseFormat DATABASE_FORMATS[] = {
#ifdef USE_SQLITE
       DatabaseFormat::SQLITE,
#endif
};

//! Sandbox P2WSH over an all-zero 32-byte witness program: valid to encode, and
//! provably unspendable. Was upstream's "bcrt1qqqq...3xueyj", which no Quicksilver
//! network can decode -- every user of it hit an IsValidDestination assert.
const std::string ADDRESS_SANDBOX_UNSPENDABLE = "shg1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq55gtdc";

std::unique_ptr<CVault> CreateSyncedVault(interfaces::Chain& chain, CChain& cchain, const CKey& key);

std::shared_ptr<CVault> TestLoadVault(VaultContext& context);
std::shared_ptr<CVault> TestLoadVault(std::unique_ptr<VaultDatabase> database, VaultContext& context, uint64_t create_flags);
void TestUnloadVault(std::shared_ptr<CVault>&& vault);

// Creates a copy of the provided database
std::unique_ptr<VaultDatabase> DuplicateMockDatabase(VaultDatabase& database);

/** Returns a new encoded destination from the vault (hardcoded to BECH32) */
std::string getnewaddress(CVault& w);
/** Returns a new destination, of an specific type, from the vault */
CTxDestination getNewDestination(CVault& w, OutputType output_type);

using MockableData = std::map<SerializeData, SerializeData, std::less<>>;

class MockableCursor: public DatabaseCursor
{
public:
    MockableData::const_iterator m_cursor;
    MockableData::const_iterator m_cursor_end;
    bool m_pass;

    explicit MockableCursor(const MockableData& records, bool pass) : m_cursor(records.begin()), m_cursor_end(records.end()), m_pass(pass) {}
    MockableCursor(const MockableData& records, bool pass, Span<const std::byte> prefix);
    ~MockableCursor() = default;

    Status Next(DataStream& key, DataStream& value) override;
};

class MockableBatch : public DatabaseBatch
{
private:
    MockableData& m_records;
    bool m_pass;
    bool m_txn_active{false};

    bool ReadKey(DataStream&& key, DataStream& value) override;
    bool WriteKey(DataStream&& key, DataStream&& value, bool overwrite=true) override;
    bool EraseKey(DataStream&& key) override;
    bool HasKey(DataStream&& key) override;
    bool ErasePrefix(Span<const std::byte> prefix) override;

public:
    explicit MockableBatch(MockableData& records, bool pass) : m_records(records), m_pass(pass) {}
    ~MockableBatch() = default;

    void Flush() override {}
    void Close() override {}

    std::unique_ptr<DatabaseCursor> GetNewCursor() override
    {
        return std::make_unique<MockableCursor>(m_records, m_pass);
    }
    std::unique_ptr<DatabaseCursor> GetNewPrefixCursor(Span<const std::byte> prefix) override {
        return std::make_unique<MockableCursor>(m_records, m_pass, prefix);
    }
    bool TxnBegin() override
    {
        if (!m_pass || m_txn_active) return false;
        m_txn_active = true;
        return true;
    }
    bool TxnCommit() override
    {
        if (!m_pass || !m_txn_active) return false;
        m_txn_active = false;
        return true;
    }
    bool TxnAbort() override
    {
        if (!m_pass || !m_txn_active) return false;
        m_txn_active = false;
        return true;
    }
    bool HasActiveTxn() override { return m_txn_active; }
};

/** A VaultDatabase whose contents and return values can be modified as needed for testing
 **/
class MockableDatabase : public VaultDatabase
{
public:
    MockableData m_records;
    bool m_pass{true};
    bool m_backup_pass{true};

    MockableDatabase(MockableData records = {}) : VaultDatabase(), m_records(records) {}
    ~MockableDatabase() = default;

    void Open() override {}

    bool Rewrite() override { return m_pass; }
    bool Backup(const std::string& strDest) const override { return m_pass && m_backup_pass; }
    void Flush() override {}
    void Close() override {}
    bool PeriodicFlush() override { return m_pass; }
    void IncrementUpdateCounter() override {}
    void ReloadDbEnv() override {}

    std::string Filename() override { return "mockable"; }
    std::string Format() override { return "mock"; }
    std::unique_ptr<DatabaseBatch> MakeBatch(bool flush_on_close = true) override { return std::make_unique<MockableBatch>(m_records, m_pass); }
};

std::unique_ptr<VaultDatabase> CreateMockableVaultDatabase(MockableData records = {});
MockableDatabase& GetMockableDatabase(CVault& vault);

ScriptPubKeyMan* CreateDescriptor(CVault& keystore, const std::string& desc_str, const bool success);
} // namespace vault

#endif // QUICKSILVER_VAULT_TEST_UTIL_H
