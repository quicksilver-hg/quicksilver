// Copyright (c) 2019-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
#include <node/blockstorage.h>
#include <script/solver.h>
#include <sync.h>
#include <test/util/coins.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <undo.h>
#include <util/fs.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(validation_flush_tests, TestingSetup)

namespace {
class ScopedFlushFailureHook
{
public:
    explicit ScopedFlushFailureHook(FlatFileSeq::FlushFailureHook hook)
    {
        FlatFileSeq::SetFlushFailureHookForTesting(std::move(hook));
    }
    ~ScopedFlushFailureHook() { FlatFileSeq::SetFlushFailureHookForTesting({}); }
    void Reset() { FlatFileSeq::SetFlushFailureHookForTesting({}); }
};

class BlockFilePathRestorer
{
public:
    explicit BlockFilePathRestorer(fs::path path) : m_path{std::move(path)}, m_saved_path{SavedPath(m_path)}
    {
        BOOST_REQUIRE(fs::is_regular_file(m_path));
        fs::rename(m_path, m_saved_path);
        BOOST_REQUIRE(fs::create_directory(m_path));
    }

    ~BlockFilePathRestorer()
    {
        std::error_code error;
        if (fs::exists(m_saved_path)) {
            fs::remove_all(m_path, error);
            error.clear();
            fs::rename(m_saved_path, m_path, error);
        }
    }

    void Restore()
    {
        BOOST_REQUIRE(fs::remove(m_path));
        fs::rename(m_saved_path, m_path);
    }

private:
    static fs::path SavedPath(fs::path path)
    {
        path += ".saved";
        return path;
    }

    const fs::path m_path;
    const fs::path m_saved_path;
};
} // namespace

BOOST_FIXTURE_TEST_CASE(block_file_flush_failure_stops_persistence, TestChain100Setup)
{
    auto& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    const fs::path block_path{chainman.m_blockman.GetBlockPosFilename(FlatFilePos{0, 0})};
    fs::path saved_block_path{block_path};
    saved_block_path += ".saved";

    BOOST_REQUIRE(fs::is_regular_file(block_path));
    fs::rename(block_path, saved_block_path);
    struct BlockFileRestorer {
        const fs::path& block_path;
        const fs::path& saved_block_path;
        ~BlockFileRestorer()
        {
            std::error_code error;
            fs::remove_all(block_path, error);
            error.clear();
            fs::rename(saved_block_path, block_path, error);
        }
    } restore_block_file{block_path, saved_block_path};

    // A directory at the current block-file path makes FlatFileSeq::Open fail
    // deterministically, including when the test runs as root.
    BOOST_REQUIRE(fs::create_directory(block_path));

    size_t cache_size;
    {
        LOCK(::cs_main);
        AddTestCoin(m_rng, chainstate.CoinsTip());
        cache_size = chainstate.CoinsTip().GetCacheSize();
    }

    BlockValidationState state;
    BOOST_CHECK(!chainstate.FlushStateToDisk(state, FlushStateMode::ALWAYS));
    BOOST_CHECK(state.IsError());
    {
        LOCK(::cs_main);
        BOOST_CHECK_EQUAL(chainstate.CoinsTip().GetCacheSize(), cache_size);
    }
}

BOOST_FIXTURE_TEST_CASE(block_file_rollover_flush_failure_stops_index_write, TestChain100Setup)
{
    auto& chainman{*Assert(m_node.chainman)};
    auto& blockman{chainman.m_blockman};
    Chainstate& chainstate{chainman.ActiveChainstate()};

    BlockValidationState initial_state;
    BOOST_REQUIRE(chainstate.FlushStateToDisk(initial_state, FlushStateMode::ALWAYS));
    int persisted_last_file{-1};
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return blockman.m_block_tree_db->ReadLastBlockFile(persisted_last_file)));
    BOOST_REQUIRE_EQUAL(persisted_last_file, 0);

    const CScript coinbase_script{GetScriptForRawPubKey(coinbaseKey.GetPubKey())};
    const CBlock block{CreateBlock({}, coinbase_script, chainstate)};
    const unsigned int serialized_size{static_cast<unsigned int>(GetSerializeSize(TX_WITH_WITNESS(block)))};
    const unsigned int record_size{static_cast<unsigned int>(serialized_size + node::BLOCK_SERIALIZATION_HEADER_SIZE)};
    WITH_LOCK(::cs_main, blockman.GetBlockFileInfo(0)->nSize = node::MAX_BLOCKFILE_SIZE - record_size);

    BlockFilePathRestorer restore_block_file{blockman.GetBlockPosFilename(FlatFilePos{0, 0})};
    BOOST_REQUIRE(chainman.ProcessNewBlock(std::make_shared<const CBlock>(block), true, true, nullptr));
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return blockman.LookupBlockIndex(block.GetHash())->nFile), 1);

    BlockValidationState failed_state;
    BOOST_CHECK(!chainstate.FlushStateToDisk(failed_state, FlushStateMode::ALWAYS));
    BOOST_CHECK(failed_state.IsError());
    int last_file_after_failure{-1};
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return blockman.m_block_tree_db->ReadLastBlockFile(last_file_after_failure)));
    BOOST_CHECK_EQUAL(last_file_after_failure, persisted_last_file);

    restore_block_file.Restore();
    BlockValidationState retry_state;
    BOOST_CHECK(chainstate.FlushStateToDisk(retry_state, FlushStateMode::ALWAYS));
    int last_file_after_retry{-1};
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return blockman.m_block_tree_db->ReadLastBlockFile(last_file_after_retry)));
    BOOST_CHECK_EQUAL(last_file_after_retry, 1);
}

BOOST_FIXTURE_TEST_CASE(older_undo_file_flush_failure_stops_index_write, TestChain100Setup)
{
    auto& chainman{*Assert(m_node.chainman)};
    auto& blockman{chainman.m_blockman};
    Chainstate& chainstate{chainman.ActiveChainstate()};

    BlockValidationState initial_state;
    BOOST_REQUIRE(chainstate.FlushStateToDisk(initial_state, FlushStateMode::ALWAYS));

    CBlockIndex* old_tip{WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip())};
    const CScript coinbase_script{GetScriptForRawPubKey(coinbaseKey.GetPubKey())};
    const CBlock rollover_block{CreateBlock({}, coinbase_script, chainstate)};
    const unsigned int record_size{static_cast<unsigned int>(GetSerializeSize(TX_WITH_WITNESS(rollover_block)) + node::BLOCK_SERIALIZATION_HEADER_SIZE)};
    WITH_LOCK(::cs_main, blockman.GetBlockFileInfo(0)->nSize = node::MAX_BLOCKFILE_SIZE - record_size);
    BOOST_REQUIRE(chainman.ProcessNewBlock(std::make_shared<const CBlock>(rollover_block), true, true, nullptr));

    BlockValidationState baseline_state;
    BOOST_REQUIRE(chainstate.FlushStateToDisk(baseline_state, FlushStateMode::ALWAYS));
    CBlockFileInfo persisted_file_info;
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return blockman.m_block_tree_db->ReadBlockFileInfo(0, persisted_file_info)));

    const fs::path undo_path{blockman.GetBlockPosFilename(FlatFilePos{0, 0}).parent_path() / "rev00000.dat"};
    int hook_calls{0};
    ScopedFlushFailureHook flush_failure{[&](const fs::path& path, const FlatFilePos& pos, bool finalize) {
        if (path == undo_path && pos.nFile == 0 && finalize) {
            ++hook_calls;
            return true;
        }
        return false;
    }};

    BlockValidationState undo_state;
    {
        LOCK(::cs_main);
        old_tip->nUndoPos = 0;
        old_tip->nStatus &= ~BLOCK_HAVE_UNDO;
        BOOST_REQUIRE(blockman.WriteBlockUndo(CBlockUndo{}, undo_state, *old_tip));
    }
    BOOST_REQUIRE_EQUAL(hook_calls, 1);

    BlockValidationState failed_state;
    BOOST_CHECK(!chainstate.FlushStateToDisk(failed_state, FlushStateMode::ALWAYS));
    BOOST_CHECK(failed_state.IsError());
    CBlockFileInfo file_info_after_failure;
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return blockman.m_block_tree_db->ReadBlockFileInfo(0, file_info_after_failure)));
    BOOST_CHECK_EQUAL(file_info_after_failure.nUndoSize, persisted_file_info.nUndoSize);

    flush_failure.Reset();
    BlockValidationState retry_state;
    BOOST_CHECK(chainstate.FlushStateToDisk(retry_state, FlushStateMode::ALWAYS));
    CBlockFileInfo file_info_after_retry;
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return blockman.m_block_tree_db->ReadBlockFileInfo(0, file_info_after_retry)));
    BOOST_CHECK_GT(file_info_after_retry.nUndoSize, persisted_file_info.nUndoSize);
}

//! Test utilities for detecting when we need to flush the coins cache based
//! on estimated memory usage.
//!
//! @sa Chainstate::GetCoinsCacheSizeState()
//!
BOOST_AUTO_TEST_CASE(getcoinscachesizestate)
{
    Chainstate& chainstate{m_node.chainman->ActiveChainstate()};

    constexpr bool is_64_bit = sizeof(void*) == 8;

    LOCK(::cs_main);
    auto& view = chainstate.CoinsTip();

    // The number of bytes consumed by coin's heap data, i.e. CScript
    // (prevector<28, unsigned char>) when assigned 56 bytes of data per above.
    //
    // See also: Coin::DynamicMemoryUsage().
    constexpr unsigned int COIN_SIZE = is_64_bit ? 80 : 64;

    auto print_view_mem_usage = [](CCoinsViewCache& view) {
        BOOST_TEST_MESSAGE("CCoinsViewCache memory usage: " << view.DynamicMemoryUsage());
    };

    // PoolResource defaults to 256 KiB that will be allocated, so we'll take that and make it a bit larger.
    constexpr size_t MAX_COINS_CACHE_BYTES = 262144 + 512;

    // Without any coins in the cache, we shouldn't need to flush.
    BOOST_TEST(
        chainstate.GetCoinsCacheSizeState(MAX_COINS_CACHE_BYTES, /*max_relaypool_size_bytes=*/ 0) != CoinsCacheSizeState::CRITICAL);

    // If the initial memory allocations of cacheCoins don't match these common
    // cases, we can't really continue to make assertions about memory usage.
    // End the test early.
    if (view.DynamicMemoryUsage() != 32 && view.DynamicMemoryUsage() != 16) {
        // Add a bunch of coins to see that we at least flip over to CRITICAL.

        for (int i{0}; i < 1000; ++i) {
            const COutPoint res = AddTestCoin(m_rng, view);
            BOOST_CHECK_EQUAL(view.AccessCoin(res).DynamicMemoryUsage(), COIN_SIZE);
        }

        BOOST_CHECK_EQUAL(
            chainstate.GetCoinsCacheSizeState(MAX_COINS_CACHE_BYTES, /*max_relaypool_size_bytes=*/0),
            CoinsCacheSizeState::CRITICAL);

        BOOST_TEST_MESSAGE("Exiting cache flush tests early due to unsupported arch");
        return;
    }

    print_view_mem_usage(view);
    BOOST_CHECK_EQUAL(view.DynamicMemoryUsage(), is_64_bit ? 32U : 16U);

    // We should be able to add COINS_UNTIL_CRITICAL coins to the cache before going CRITICAL.
    // This is contingent not only on the dynamic memory usage of the Coins
    // that we're adding (COIN_SIZE bytes per), but also on how much memory the
    // cacheCoins (unordered_map) preallocates.
    constexpr int COINS_UNTIL_CRITICAL{3};

    // no coin added, so we have plenty of space left.
    BOOST_CHECK_EQUAL(
        chainstate.GetCoinsCacheSizeState(MAX_COINS_CACHE_BYTES, /*max_relaypool_size_bytes*/ 0),
        CoinsCacheSizeState::OK);

    for (int i{0}; i < COINS_UNTIL_CRITICAL; ++i) {
        const COutPoint res = AddTestCoin(m_rng, view);
        print_view_mem_usage(view);
        BOOST_CHECK_EQUAL(view.AccessCoin(res).DynamicMemoryUsage(), COIN_SIZE);

        // adding first coin causes the MemoryResource to allocate one 256 KiB chunk of memory,
        // pushing us immediately over to LARGE
        BOOST_CHECK_EQUAL(
            chainstate.GetCoinsCacheSizeState(MAX_COINS_CACHE_BYTES, /*max_relaypool_size_bytes=*/ 0),
            CoinsCacheSizeState::LARGE);
    }

    // Adding some additional coins will push us over the edge to CRITICAL.
    for (int i{0}; i < 4; ++i) {
        AddTestCoin(m_rng, view);
        print_view_mem_usage(view);
        if (chainstate.GetCoinsCacheSizeState(MAX_COINS_CACHE_BYTES, /*max_relaypool_size_bytes=*/0) ==
            CoinsCacheSizeState::CRITICAL) {
            break;
        }
    }

    BOOST_CHECK_EQUAL(
        chainstate.GetCoinsCacheSizeState(MAX_COINS_CACHE_BYTES, /*max_relaypool_size_bytes=*/0),
        CoinsCacheSizeState::CRITICAL);

    // Passing non-zero max relaypool usage (512 KiB) should allow us more headroom.
    BOOST_CHECK_EQUAL(
        chainstate.GetCoinsCacheSizeState(MAX_COINS_CACHE_BYTES, /*max_relaypool_size_bytes=*/ 1 << 19),
        CoinsCacheSizeState::OK);

    for (int i{0}; i < 3; ++i) {
        AddTestCoin(m_rng, view);
        print_view_mem_usage(view);
        BOOST_CHECK_EQUAL(
            chainstate.GetCoinsCacheSizeState(MAX_COINS_CACHE_BYTES, /*max_relaypool_size_bytes=*/ 1 << 19),
            CoinsCacheSizeState::OK);
    }

    // Adding another coin with the additional relaypool room will put us >90%
    // but not yet critical.
    AddTestCoin(m_rng, view);
    print_view_mem_usage(view);

    // Only perform these checks on 64 bit hosts; I haven't done the math for 32.
    if (is_64_bit) {
        float usage_percentage = (float)view.DynamicMemoryUsage() / (MAX_COINS_CACHE_BYTES + (1 << 10));
        BOOST_TEST_MESSAGE("CoinsTip usage percentage: " << usage_percentage);
        BOOST_CHECK(usage_percentage >= 0.9);
        BOOST_CHECK(usage_percentage < 1);
        BOOST_CHECK_EQUAL(
            chainstate.GetCoinsCacheSizeState(MAX_COINS_CACHE_BYTES, /*max_relaypool_size_bytes*/ 1 << 10), // 1024
            CoinsCacheSizeState::LARGE);
    }

    // Using the default max_* values permits way more coins to be added.
    for (int i{0}; i < 1000; ++i) {
        AddTestCoin(m_rng, view);
        BOOST_CHECK_EQUAL(
            chainstate.GetCoinsCacheSizeState(),
            CoinsCacheSizeState::OK);
    }

    // Flushing the view does take us back to OK because ReallocateCache() is called

    BOOST_CHECK_EQUAL(
        chainstate.GetCoinsCacheSizeState(MAX_COINS_CACHE_BYTES, 0),
        CoinsCacheSizeState::CRITICAL);

    view.SetBestBlock(m_rng.rand256());
    BOOST_CHECK(view.Flush());
    print_view_mem_usage(view);

    BOOST_CHECK_EQUAL(
        chainstate.GetCoinsCacheSizeState(MAX_COINS_CACHE_BYTES, 0),
        CoinsCacheSizeState::OK);
}

BOOST_AUTO_TEST_SUITE_END()
