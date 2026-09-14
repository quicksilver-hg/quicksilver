// Copyright (c) 2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/relaypool_persist.h>

#include <clientversion.h>
#include <consensus/amount.h>
#include <logging.h>
#include <primitives/transaction.h>
#include <random.h>
#include <serialize.h>
#include <streams.h>
#include <sync.h>
#include <txrelaypool.h>
#include <uint256.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/signalinterrupt.h>
#include <util/time.h>
#include <validation.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

using fsbridge::FopenFn;

namespace node {

static const uint64_t RELAYPOOL_DUMP_VERSION{4};

bool LoadRelayPool(CTxRelayPool& pool, const fs::path& load_path, Chainstate& active_chainstate, ImportRelayPoolOptions&& opts)
{
    if (load_path.empty()) return false;

    AutoFile file{opts.mockable_fopen_function(load_path, "rb")};
    if (file.IsNull()) {
        LogInfo(HgLog::RELAYPOOL, "loaded reason=file-open-failed\n");
        return false;
    }

    int64_t count = 0;
    int64_t expired = 0;
    int64_t failed = 0;
    int64_t already_there = 0;
    int64_t unbroadcast = 0;
    const auto now{NodeClock::now()};

    try {
        uint64_t version;
        file >> version;
        std::vector<std::byte> xor_key;
        if (version != RELAYPOOL_DUMP_VERSION) {
            return false;
        }
        file >> xor_key;
        file.SetXor(xor_key);
        uint64_t total_txns_to_load;
        file >> total_txns_to_load;
        uint64_t txns_tried = 0;
        LogInfo(HgLog::RELAYPOOL, "loading txs=%u\n", total_txns_to_load);
        int next_tenth_to_report = 0;
        while (txns_tried < total_txns_to_load) {
            const int percentage_done(100.0 * txns_tried / total_txns_to_load);
            if (next_tenth_to_report < percentage_done / 10) {
                LogInfo(HgLog::RELAYPOOL, "loading percent=%d tried=%u remaining=%u\n",
                        percentage_done, txns_tried, total_txns_to_load - txns_tried);
                next_tenth_to_report = percentage_done / 10;
            }
            ++txns_tried;

            CTransactionRef tx;
            int64_t nTime;
            file >> TX_WITH_WITNESS(tx);
            file >> nTime;

            if (opts.use_current_time) {
                nTime = TicksSinceEpoch<std::chrono::seconds>(now);
            }

            if (nTime > TicksSinceEpoch<std::chrono::seconds>(now - pool.m_opts.expiry)) {
                LOCK(cs_main);
                const auto& accepted = AcceptToMemoryPool(active_chainstate, tx, nTime, /*bypass_limits=*/false, /*test_accept=*/false);
                if (accepted.m_result_type == RelayPoolAcceptResult::ResultType::VALID) {
                    ++count;
                } else {
                    // relaypool may contain the transaction already, e.g. from
                    // vault(s) having loaded it while we were processing
                    // relaypool transactions; consider these as valid, instead of
                    // failed, but mark them as 'already there'
                    if (pool.exists(GenTxid::Txid(tx->GetHash()))) {
                        ++already_there;
                    } else {
                        ++failed;
                    }
                }
            } else {
                ++expired;
            }
            if (active_chainstate.m_chainman.m_interrupt)
                return false;
        }

        std::set<uint256> unbroadcast_txids;
        file >> unbroadcast_txids;
        if (opts.apply_unbroadcast_set) {
            unbroadcast = unbroadcast_txids.size();
            for (const auto& txid : unbroadcast_txids) {
                // Ensure transactions were accepted to relaypool then add to
                // unbroadcast set.
                if (pool.get(txid) != nullptr) pool.AddUnbroadcastTx(txid);
            }
        }
    } catch (const std::exception& e) {
        LogInfo(HgLog::RELAYPOOL, "loaded reason=deserialize-failed detail=%s\n", e.what());
        return false;
    }

    LogInfo(HgLog::RELAYPOOL, "loaded ok=%i failed=%i expired=%i present=%i pending_broadcast=%i\n",
            count, failed, expired, already_there, unbroadcast);
    return true;
}

bool DumpRelayPool(const CTxRelayPool& pool, const fs::path& dump_path, FopenFn mockable_fopen_function, bool skip_file_commit)
{
    auto start = SteadyClock::now();

    std::vector<TxRelayPoolInfo> vinfo;
    std::set<uint256> unbroadcast_txids;

    static Mutex dump_mutex;
    LOCK(dump_mutex);

    {
        LOCK(pool.cs);
        vinfo = pool.infoAll();
        unbroadcast_txids = pool.GetUnbroadcastTxs();
    }

    auto mid = SteadyClock::now();

    AutoFile file{mockable_fopen_function(dump_path + ".new", "wb")};
    if (file.IsNull()) {
        return false;
    }

    try {
        const uint64_t version{RELAYPOOL_DUMP_VERSION};
        file << version;

        std::vector<std::byte> xor_key(8);
        FastRandomContext{}.fillrand(xor_key);
        file << xor_key;
        file.SetXor(xor_key);

        uint64_t relaypool_transactions_to_write(vinfo.size());
        file << relaypool_transactions_to_write;
        LogInfo(HgLog::RELAYPOOL, "saving txs=%u\n", relaypool_transactions_to_write);
        for (const auto& i : vinfo) {
            file << TX_WITH_WITNESS(*(i.tx));
            file << int64_t{count_seconds(i.m_time)};
        }

        LogInfo(HgLog::RELAYPOOL, "saving unbroadcast_txs=%d\n", unbroadcast_txids.size());
        file << unbroadcast_txids;

        if (!skip_file_commit && !file.Commit())
            throw std::runtime_error("Commit failed");
        file.fclose();
        if (!RenameOver(dump_path + ".new", dump_path)) {
            throw std::runtime_error("Rename failed");
        }
        auto last = SteadyClock::now();

        LogInfo(HgLog::RELAYPOOL, "saved copy_ms=%.0f dump_ms=%.0f bytes=%d\n",
                  Ticks<MillisecondsDouble>(mid - start),
                  Ticks<MillisecondsDouble>(last - mid),
                  fs::file_size(dump_path));
    } catch (const std::exception& e) {
        LogInfo(HgLog::RELAYPOOL, "saved reason=dump-failed detail=%s\n", e.what());
        return false;
    }
    return true;
}

} // namespace node
