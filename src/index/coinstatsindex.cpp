// Copyright (c) 2020-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <coins.h>
#include <common/args.h>
#include <crypto/muhash.h>
#include <index/coinstatsindex.h>
#include <kernel/coinstats.h>
#include <logging.h>
#include <node/blockstorage.h>
#include <pow.h>
#include <serialize.h>
#include <txdb.h>
#include <undo.h>
#include <validation.h>

using kernel::ApplyCoinHash;
using kernel::CCoinsStats;
using kernel::GetBogoSize;
using kernel::RemoveCoinHash;

static constexpr uint8_t DB_BLOCK_HASH{'s'};
static constexpr uint8_t DB_BLOCK_HEIGHT{'t'};
static constexpr uint8_t DB_MUHASH{'M'};
static constexpr uint8_t DB_VERSION{'V'};
static constexpr uint8_t COINSTATSINDEX_DB_VERSION{2};

namespace {

struct DBVal {
    uint256 muhash;
    uint64_t transaction_output_count;
    uint64_t bogo_size;
    CAmount total_amount;
    CAmount total_subsidy;
    CAmount total_mint_allowance;
    CAmount total_unspendable_amount;
    CAmount total_prevout_spent_amount;
    CAmount total_new_outputs_ex_coinbase_amount;
    CAmount total_coinbase_amount;
    CAmount total_unspendables_genesis_block;
    CAmount total_unspendables_scripts;
    CAmount total_unspendables_unclaimed_rewards;

    SERIALIZE_METHODS(DBVal, obj)
    {
        READWRITE(obj.muhash);
        READWRITE(obj.transaction_output_count);
        READWRITE(obj.bogo_size);
        READWRITE(obj.total_amount);
        READWRITE(obj.total_subsidy);
        READWRITE(obj.total_mint_allowance);
        READWRITE(obj.total_unspendable_amount);
        READWRITE(obj.total_prevout_spent_amount);
        READWRITE(obj.total_new_outputs_ex_coinbase_amount);
        READWRITE(obj.total_coinbase_amount);
        READWRITE(obj.total_unspendables_genesis_block);
        READWRITE(obj.total_unspendables_scripts);
        READWRITE(obj.total_unspendables_unclaimed_rewards);
    }
};

struct DBHeightKey {
    int height;

    explicit DBHeightKey(int height_in) : height(height_in) {}

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        ser_writedata8(s, DB_BLOCK_HEIGHT);
        ser_writedata32be(s, height);
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        const uint8_t prefix{ser_readdata8(s)};
        if (prefix != DB_BLOCK_HEIGHT) {
            throw std::ios_base::failure("Invalid format for coinstatsindex DB height key");
        }
        height = ser_readdata32be(s);
    }
};

struct DBHashKey {
    uint256 block_hash;

    explicit DBHashKey(const uint256& hash_in) : block_hash(hash_in) {}

    SERIALIZE_METHODS(DBHashKey, obj)
    {
        uint8_t prefix{DB_BLOCK_HASH};
        READWRITE(prefix);
        if (prefix != DB_BLOCK_HASH) {
            throw std::ios_base::failure("Invalid format for coinstatsindex DB hash key");
        }

        READWRITE(obj.block_hash);
    }
};

}; // namespace

std::unique_ptr<CoinStatsIndex> g_coin_stats_index;

CoinStatsIndex::CoinStatsIndex(std::unique_ptr<interfaces::Chain> chain, size_t n_cache_size, bool f_memory, bool f_wipe)
    : BaseIndex(std::move(chain), "coinstatsindex")
{
    fs::path path{gArgs.GetDataDirNet() / "indexes" / "coinstats"};
    fs::create_directories(path);

    m_db = std::make_unique<CoinStatsIndex::DB>(path / "db", n_cache_size, f_memory, f_wipe);
}

bool CoinStatsIndex::CustomAppend(const interfaces::BlockInfo& block)
{
    CBlockUndo block_undo;
    const Consensus::Params& consensus{Params().GetConsensus()};
    const CAmount block_subsidy{GetBlockSubsidy(block.height, consensus)};

    const CBlockIndex* pindex{WITH_LOCK(cs_main, return m_chainstate->m_blockman.LookupBlockIndex(block.hash))};
    if (!pindex || !block.data) {
        LogError(HgLog::SCAN, "%s: Cannot read indexed block %s\n", __func__, block.hash.ToString());
        return false;
    }
    const CAmount block_mint_allowance{GetBlockMintAllowance(*block.data, consensus, pindex->pprev)};
    if (block_mint_allowance == MINT_ALLOWANCE_INVALID) {
        LogError(HgLog::SCAN, "%s: Mint allowance for indexed block %s is out of range\n",
                 __func__, block.hash.ToString());
        return false;
    }
    m_total_subsidy += block_subsidy;
    m_total_mint_allowance += block_mint_allowance;

    // Ignore genesis block
    if (block.height > 0) {
        // Undo data is stored separately from the block and is required to
        // subtract the outputs spent by this block.
        if (!m_chainstate->m_blockman.ReadBlockUndo(block_undo, *pindex)) {
            return false;
        }

        std::pair<uint256, DBVal> read_out;
        if (!m_db->Read(DBHeightKey(block.height - 1), read_out)) {
            return false;
        }

        uint256 expected_block_hash{*Assert(block.prev_hash)};
        if (read_out.first != expected_block_hash) {
            LogInfo(HgLog::SCAN, "WARNING: previous block header belongs to unexpected block %s; expected %s\n",
                      read_out.first.ToString(), expected_block_hash.ToString());

            if (!m_db->Read(DBHashKey(expected_block_hash), read_out)) {
                LogError(HgLog::SCAN, "%s: previous block header not found; expected %s\n",
                             __func__, expected_block_hash.ToString());
                return false;
            }
        }

        // Add the new utxos created from the block
        for (size_t i = 0; i < block.data->vtx.size(); ++i) {
            const auto& tx{block.data->vtx.at(i)};

            for (uint32_t j = 0; j < tx->vout.size(); ++j) {
                const CTxOut& out{tx->vout[j]};
                Coin coin{out, block.height, tx->IsCoinBase()};
                COutPoint outpoint{tx->GetHash(), j};

                // Skip unspendable coins
                if (coin.out.scriptPubKey.IsUnspendable()) {
                    m_total_unspendable_amount += coin.out.nValue;
                    m_total_unspendables_scripts += coin.out.nValue;
                    continue;
                }

                ApplyCoinHash(m_muhash, outpoint, coin);

                if (tx->IsCoinBase()) {
                    m_total_coinbase_amount += coin.out.nValue;
                } else {
                    m_total_new_outputs_ex_coinbase_amount += coin.out.nValue;
                }

                ++m_transaction_output_count;
                m_total_amount += coin.out.nValue;
                m_bogo_size += GetBogoSize(coin.out.scriptPubKey);
            }

            // The coinbase tx has no undo data since no former output is spent
            if (!tx->IsCoinBase()) {
                const auto& tx_undo{block_undo.vtxundo.at(i - 1)};

                for (size_t j = 0; j < tx_undo.vprevout.size(); ++j) {
                    Coin coin{tx_undo.vprevout[j]};
                    COutPoint outpoint{tx->vin[j].prevout.hash, tx->vin[j].prevout.n};

                    RemoveCoinHash(m_muhash, outpoint, coin);

                    m_total_prevout_spent_amount += coin.out.nValue;

                    --m_transaction_output_count;
                    m_total_amount -= coin.out.nValue;
                    m_bogo_size -= GetBogoSize(coin.out.scriptPubKey);
                }
            }
        }
    } else {
        // genesis block
        m_total_unspendable_amount += block_subsidy;
        m_total_unspendables_genesis_block += block_subsidy;
    }

    // If spent prevouts + block subsidy + Frame-B mint allowance are still a
    // higher amount than new outputs + coinbase + current unspendable amount,
    // the miner did not claim the full block reward. Unclaimed block rewards
    // are also unspendable.
    const CAmount unclaimed_rewards{
        (m_total_prevout_spent_amount + m_total_subsidy + m_total_mint_allowance) -
        (m_total_new_outputs_ex_coinbase_amount + m_total_coinbase_amount + m_total_unspendable_amount)};
    if (unclaimed_rewards < 0) {
        LogError(HgLog::SCAN, "%s: Indexed block %s produced negative unclaimed rewards\n",
                 __func__, block.hash.ToString());
        return false;
    }
    m_total_unspendable_amount += unclaimed_rewards;
    m_total_unspendables_unclaimed_rewards += unclaimed_rewards;

    std::pair<uint256, DBVal> value;
    value.first = block.hash;
    value.second.transaction_output_count = m_transaction_output_count;
    value.second.bogo_size = m_bogo_size;
    value.second.total_amount = m_total_amount;
    value.second.total_subsidy = m_total_subsidy;
    value.second.total_mint_allowance = m_total_mint_allowance;
    value.second.total_unspendable_amount = m_total_unspendable_amount;
    value.second.total_prevout_spent_amount = m_total_prevout_spent_amount;
    value.second.total_new_outputs_ex_coinbase_amount = m_total_new_outputs_ex_coinbase_amount;
    value.second.total_coinbase_amount = m_total_coinbase_amount;
    value.second.total_unspendables_genesis_block = m_total_unspendables_genesis_block;
    value.second.total_unspendables_scripts = m_total_unspendables_scripts;
    value.second.total_unspendables_unclaimed_rewards = m_total_unspendables_unclaimed_rewards;

    uint256 out;
    m_muhash.Finalize(out);
    value.second.muhash = out;

    // Intentionally do not update DB_MUHASH here so it stays in sync with
    // DB_BEST_BLOCK, and the index is not corrupted if there is an unclean shutdown.
    return m_db->Write(DBHeightKey(block.height), value);
}

[[nodiscard]] static bool CopyHeightIndexToHashIndex(CDBIterator& db_it, CDBBatch& batch,
                                       const std::string& index_name,
                                       int start_height, int stop_height)
{
    DBHeightKey key{start_height};
    db_it.Seek(key);

    for (int height = start_height; height <= stop_height; ++height) {
        if (!db_it.GetKey(key) || key.height != height) {
            LogError(HgLog::SCAN, "%s: unexpected key in %s: expected (%c, %d)\n",
                         __func__, index_name, DB_BLOCK_HEIGHT, height);
            return false;
        }

        std::pair<uint256, DBVal> value;
        if (!db_it.GetValue(value)) {
            LogError(HgLog::SCAN, "%s: unable to read value in %s at key (%c, %d)\n",
                         __func__, index_name, DB_BLOCK_HEIGHT, height);
            return false;
        }

        batch.Write(DBHashKey(value.first), std::move(value.second));

        db_it.Next();
    }
    return true;
}

bool CoinStatsIndex::CustomRewind(const interfaces::BlockRef& current_tip, const interfaces::BlockRef& new_tip)
{
    CDBBatch batch(*m_db);
    std::unique_ptr<CDBIterator> db_it(m_db->NewIterator());

    // During a reorg, we need to copy all hash digests for blocks that are
    // getting disconnected from the height index to the hash index so we can
    // still find them when the height index entries are overwritten.
    if (!CopyHeightIndexToHashIndex(*db_it, batch, m_name, new_tip.height, current_tip.height)) {
        return false;
    }

    if (!m_db->WriteBatch(batch)) return false;

    {
        LOCK(cs_main);
        const CBlockIndex* iter_tip{m_chainstate->m_blockman.LookupBlockIndex(current_tip.hash)};
        const CBlockIndex* new_tip_index{m_chainstate->m_blockman.LookupBlockIndex(new_tip.hash)};

        do {
            CBlock block;

            if (!m_chainstate->m_blockman.ReadBlock(block, *iter_tip)) {
                LogError(HgLog::SCAN, "%s: Failed to read block %s from disk\n",
                             __func__, iter_tip->GetBlockHash().ToString());
                return false;
            }

            if (!ReverseBlock(block, iter_tip)) {
                return false; // failure cause logged internally
            }

            iter_tip = iter_tip->GetAncestor(iter_tip->nHeight - 1);
        } while (new_tip_index != iter_tip);
    }

    return true;
}

static bool LookUpOne(const CDBWrapper& db, const interfaces::BlockRef& block, DBVal& result)
{
    // First check if the result is stored under the height index and the value
    // there matches the block hash. This should be the case if the block is on
    // the active chain.
    std::pair<uint256, DBVal> read_out;
    if (!db.Read(DBHeightKey(block.height), read_out)) {
        return false;
    }
    if (read_out.first == block.hash) {
        result = std::move(read_out.second);
        return true;
    }

    // If value at the height index corresponds to an different block, the
    // result will be stored in the hash index.
    return db.Read(DBHashKey(block.hash), result);
}

std::optional<CCoinsStats> CoinStatsIndex::LookUpStats(const CBlockIndex& block_index) const
{
    CCoinsStats stats{block_index.nHeight, block_index.GetBlockHash()};
    stats.index_used = true;

    DBVal entry;
    if (!LookUpOne(*m_db, {block_index.GetBlockHash(), block_index.nHeight}, entry)) {
        return std::nullopt;
    }

    stats.hash = entry.muhash;
    stats.nTransactionOutputs = entry.transaction_output_count;
    stats.nBogoSize = entry.bogo_size;
    stats.total_amount = entry.total_amount;
    stats.total_subsidy = entry.total_subsidy;
    stats.total_unspendable_amount = entry.total_unspendable_amount;
    stats.total_prevout_spent_amount = entry.total_prevout_spent_amount;
    stats.total_new_outputs_ex_coinbase_amount = entry.total_new_outputs_ex_coinbase_amount;
    stats.total_coinbase_amount = entry.total_coinbase_amount;
    stats.total_unspendables_genesis_block = entry.total_unspendables_genesis_block;
    stats.total_unspendables_scripts = entry.total_unspendables_scripts;
    stats.total_unspendables_unclaimed_rewards = entry.total_unspendables_unclaimed_rewards;

    return stats;
}

bool CoinStatsIndex::CustomInit(const std::optional<interfaces::BlockRef>& block)
{
    uint8_t db_version{0};
    if (m_db->Read(DB_VERSION, db_version)) {
        if (db_version != COINSTATSINDEX_DB_VERSION) {
            LogError(HgLog::SCAN, "%s: Unsupported %s database version %u; restart with -reindex to rebuild it\n",
                     __func__, GetName(), db_version);
            return false;
        }
    } else if (m_db->Exists(DB_VERSION)) {
        LogError(HgLog::SCAN, "%s: Cannot read %s database version; index may be corrupted\n",
                 __func__, GetName());
        return false;
    } else if (block) {
        // Indexes created before Frame-B mint allowances were accounted for
        // contain semantically invalid cumulative reward statistics.
        LogError(HgLog::SCAN, "%s: %s database predates mint-allowance accounting; restart with -reindex to rebuild it\n",
                 __func__, GetName());
        return false;
    }

    if (!m_db->Read(DB_MUHASH, m_muhash)) {
        // Check that the cause of the read failure is that the key does not
        // exist. Any other errors indicate database corruption or a disk
        // failure, and starting the index would cause further corruption.
        if (m_db->Exists(DB_MUHASH)) {
            LogError(HgLog::SCAN, "%s: Cannot read current %s state; index may be corrupted\n",
                         __func__, GetName());
            return false;
        }
    }

    if (block) {
        DBVal entry;
        if (!LookUpOne(*m_db, *block, entry)) {
            LogError(HgLog::SCAN, "%s: Cannot read current %s state; index may be corrupted\n",
                         __func__, GetName());
            return false;
        }

        uint256 out;
        m_muhash.Finalize(out);
        if (entry.muhash != out) {
            LogError(HgLog::SCAN, "%s: Cannot read current %s state; index may be corrupted\n",
                         __func__, GetName());
            return false;
        }

        m_transaction_output_count = entry.transaction_output_count;
        m_bogo_size = entry.bogo_size;
        m_total_amount = entry.total_amount;
        m_total_subsidy = entry.total_subsidy;
        m_total_mint_allowance = entry.total_mint_allowance;
        m_total_unspendable_amount = entry.total_unspendable_amount;
        m_total_prevout_spent_amount = entry.total_prevout_spent_amount;
        m_total_new_outputs_ex_coinbase_amount = entry.total_new_outputs_ex_coinbase_amount;
        m_total_coinbase_amount = entry.total_coinbase_amount;
        m_total_unspendables_genesis_block = entry.total_unspendables_genesis_block;
        m_total_unspendables_scripts = entry.total_unspendables_scripts;
        m_total_unspendables_unclaimed_rewards = entry.total_unspendables_unclaimed_rewards;
    }

    return true;
}

bool CoinStatsIndex::CustomCommit(CDBBatch& batch)
{
    // DB_MUHASH should always be committed in a batch together with DB_BEST_BLOCK
    // to prevent an inconsistent state of the DB.
    batch.Write(DB_MUHASH, m_muhash);
    batch.Write(DB_VERSION, COINSTATSINDEX_DB_VERSION);
    return true;
}

// Reverse a single block as part of a reorg
bool CoinStatsIndex::ReverseBlock(const CBlock& block, const CBlockIndex* pindex)
{
    CBlockUndo block_undo;
    std::pair<uint256, DBVal> read_out;

    const Consensus::Params& consensus{Params().GetConsensus()};
    const CAmount block_subsidy{GetBlockSubsidy(pindex->nHeight, consensus)};
    const CAmount block_mint_allowance{GetBlockMintAllowance(block, consensus, pindex->pprev)};
    if (block_mint_allowance == MINT_ALLOWANCE_INVALID) {
        LogError(HgLog::SCAN, "%s: Mint allowance for indexed block %s is out of range\n",
                 __func__, pindex->GetBlockHash().ToString());
        return false;
    }
    m_total_subsidy -= block_subsidy;
    m_total_mint_allowance -= block_mint_allowance;

    // Ignore genesis block
    if (pindex->nHeight > 0) {
        if (!m_chainstate->m_blockman.ReadBlockUndo(block_undo, *pindex)) {
            return false;
        }

        if (!m_db->Read(DBHeightKey(pindex->nHeight - 1), read_out)) {
            return false;
        }

        uint256 expected_block_hash{pindex->pprev->GetBlockHash()};
        if (read_out.first != expected_block_hash) {
            LogInfo(HgLog::SCAN, "WARNING: previous block header belongs to unexpected block %s; expected %s\n",
                      read_out.first.ToString(), expected_block_hash.ToString());

            if (!m_db->Read(DBHashKey(expected_block_hash), read_out)) {
                LogError(HgLog::SCAN, "%s: previous block header not found; expected %s\n",
                             __func__, expected_block_hash.ToString());
                return false;
            }
        }
    }

    // Remove the new UTXOs that were created from the block
    for (size_t i = 0; i < block.vtx.size(); ++i) {
        const auto& tx{block.vtx.at(i)};

        for (uint32_t j = 0; j < tx->vout.size(); ++j) {
            const CTxOut& out{tx->vout[j]};
            COutPoint outpoint{tx->GetHash(), j};
            Coin coin{out, pindex->nHeight, tx->IsCoinBase()};

            // Skip unspendable coins
            if (coin.out.scriptPubKey.IsUnspendable()) {
                m_total_unspendable_amount -= coin.out.nValue;
                m_total_unspendables_scripts -= coin.out.nValue;
                continue;
            }

            RemoveCoinHash(m_muhash, outpoint, coin);

            if (tx->IsCoinBase()) {
                m_total_coinbase_amount -= coin.out.nValue;
            } else {
                m_total_new_outputs_ex_coinbase_amount -= coin.out.nValue;
            }

            --m_transaction_output_count;
            m_total_amount -= coin.out.nValue;
            m_bogo_size -= GetBogoSize(coin.out.scriptPubKey);
        }

        // The coinbase tx has no undo data since no former output is spent
        if (!tx->IsCoinBase()) {
            const auto& tx_undo{block_undo.vtxundo.at(i - 1)};

            for (size_t j = 0; j < tx_undo.vprevout.size(); ++j) {
                Coin coin{tx_undo.vprevout[j]};
                COutPoint outpoint{tx->vin[j].prevout.hash, tx->vin[j].prevout.n};

                ApplyCoinHash(m_muhash, outpoint, coin);

                m_total_prevout_spent_amount -= coin.out.nValue;

                m_transaction_output_count++;
                m_total_amount += coin.out.nValue;
                m_bogo_size += GetBogoSize(coin.out.scriptPubKey);
            }
        }
    }

    const CAmount unclaimed_rewards{
        (m_total_new_outputs_ex_coinbase_amount + m_total_coinbase_amount + m_total_unspendable_amount) -
        (m_total_prevout_spent_amount + m_total_subsidy + m_total_mint_allowance)};
    if (unclaimed_rewards < 0) {
        LogError(HgLog::SCAN, "%s: Reversing indexed block %s produced negative unclaimed rewards\n",
                 __func__, pindex->GetBlockHash().ToString());
        return false;
    }
    m_total_unspendable_amount -= unclaimed_rewards;
    m_total_unspendables_unclaimed_rewards -= unclaimed_rewards;

    // Check that the rolled back internal values are consistent with the DB read out
    uint256 out;
    m_muhash.Finalize(out);
    Assert(read_out.second.muhash == out);

    Assert(m_transaction_output_count == read_out.second.transaction_output_count);
    Assert(m_total_amount == read_out.second.total_amount);
    Assert(m_bogo_size == read_out.second.bogo_size);
    Assert(m_total_subsidy == read_out.second.total_subsidy);
    Assert(m_total_mint_allowance == read_out.second.total_mint_allowance);
    Assert(m_total_unspendable_amount == read_out.second.total_unspendable_amount);
    Assert(m_total_prevout_spent_amount == read_out.second.total_prevout_spent_amount);
    Assert(m_total_new_outputs_ex_coinbase_amount == read_out.second.total_new_outputs_ex_coinbase_amount);
    Assert(m_total_coinbase_amount == read_out.second.total_coinbase_amount);
    Assert(m_total_unspendables_genesis_block == read_out.second.total_unspendables_genesis_block);
    Assert(m_total_unspendables_scripts == read_out.second.total_unspendables_scripts);
    Assert(m_total_unspendables_unclaimed_rewards == read_out.second.total_unspendables_unclaimed_rewards);

    return true;
}
