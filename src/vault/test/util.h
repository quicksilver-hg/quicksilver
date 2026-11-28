// Copyright (c) 2021-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_VAULT_TEST_UTIL_H
#define QUICKSILVER_VAULT_TEST_UTIL_H

#include <addresstype.h>
#include <vault/db.h>
#include <vault/scriptpubkeyman.h>

#include <memory>
#include <set>
#include <string>

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
       DatabaseFormat::SQLITE,
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

/** How many separate commits a test's database work cost. A write made while no
 *  transaction is active commits on its own (as the SQLite batch does); an explicit
 *  transaction commits once however many writes it holds. */
struct MockableCommitCounters
{
    size_t implicit{0}; //!< writes made outside any transaction
    size_t explicit_txn{0}; //!< successful TxnCommit calls
    size_t Total() const { return implicit + explicit_txn; }
};

class MockableBatch : public DatabaseBatch
{
private:
    MockableData& m_records;
    bool m_pass;
    bool m_txn_active{false};
    MockableCommitCounters* m_counters{nullptr};
    bool* m_allotment_read_error{nullptr};
    bool* m_fail_txn_begin{nullptr};
    bool* m_fail_master_key_write{nullptr};
    bool* m_fail_descriptor_key_erase{nullptr};
    const std::set<std::string>* m_fail_write_types{nullptr};
    bool* m_fail_txn_commit{nullptr};

    bool ReadKey(DataStream&& key, DataStream& value) override;
    ReadStatus ReadKeyStatus(DataStream&& key, DataStream& value) override;
    bool WriteKey(DataStream&& key, DataStream&& value, bool overwrite=true) override;
    bool EraseKey(DataStream&& key) override;
    bool HasKey(DataStream&& key) override;
    bool ErasePrefix(Span<const std::byte> prefix) override;

public:
    explicit MockableBatch(MockableData& records, bool pass, MockableCommitCounters* counters = nullptr, bool* allotment_read_error = nullptr, bool* fail_txn_begin = nullptr, bool* fail_master_key_write = nullptr, bool* fail_descriptor_key_erase = nullptr, const std::set<std::string>* fail_write_types = nullptr, bool* fail_txn_commit = nullptr) : m_records(records), m_pass(pass), m_counters(counters), m_allotment_read_error(allotment_read_error), m_fail_txn_begin(fail_txn_begin), m_fail_master_key_write(fail_master_key_write), m_fail_descriptor_key_erase(fail_descriptor_key_erase), m_fail_write_types(fail_write_types), m_fail_txn_commit(fail_txn_commit) {}
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
        if (!m_pass || m_txn_active || (m_fail_txn_begin && *m_fail_txn_begin)) return false;
        m_txn_active = true;
        return true;
    }
    bool TxnCommit() override
    {
        if (!m_pass || !m_txn_active) return false;
        m_txn_active = false;
        // A failed commit ends the transaction, as a real one does.
        if (m_fail_txn_commit && *m_fail_txn_commit) return false;
        if (m_counters) ++m_counters->explicit_txn;
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
    bool m_allotment_read_error{false};
    bool m_fail_txn_begin{false};
    bool m_fail_master_key_write{false};
    bool m_fail_descriptor_key_erase{false};
    //! Writes of these record types fail, for fault injection on one path.
    std::set<std::string> m_fail_write_types;
    bool m_fail_rewrite{false};
    //! TxnCommit fails (the writes made inside it are not undone by the mock).
    bool m_fail_txn_commit{false};
    MockableCommitCounters m_commits;

    MockableDatabase(MockableData records = {}) : VaultDatabase(), m_records(records) {}
    ~MockableDatabase() = default;

    void Open() override {}

    bool Rewrite() override { return m_pass && !m_fail_rewrite; }
    bool Backup(const std::string& strDest) const override { return m_pass && m_backup_pass; }
    void Flush() override {}
    void Close() override {}

    std::string Filename() override { return "mockable"; }
    std::string Format() override { return "mock"; }
    std::unique_ptr<DatabaseBatch> MakeBatch(bool flush_on_close = true) override { return std::make_unique<MockableBatch>(m_records, m_pass, &m_commits, &m_allotment_read_error, &m_fail_txn_begin, &m_fail_master_key_write, &m_fail_descriptor_key_erase, &m_fail_write_types, &m_fail_txn_commit); }
};

std::unique_ptr<VaultDatabase> CreateMockableVaultDatabase(MockableData records = {});
MockableDatabase& GetMockableDatabase(CVault& vault);

ScriptPubKeyMan* CreateDescriptor(CVault& keystore, const std::string& desc_str, const bool success);
} // namespace vault

#endif // QUICKSILVER_VAULT_TEST_UTIL_H
