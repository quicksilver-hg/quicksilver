// Copyright (c) 2018-present The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Trust anchors (F-565 (a)): a per-chain, append-only list of {height, hash,
// chain_work}. ContextualCheckBlockHeader refuses a header at an anchor's height
// whose hash differs ("checkpoint-mismatch") and a header below the newest anchor
// already held in the block index ("bad-fork-prior-to-checkpoint"). The newest
// anchor also supplies the default assumed-valid block and minimum chain work.
//
// Every case drives headers through ChainstateManager::ProcessNewBlockHeaders (or
// full blocks through ProcessNewBlock) on a separate sandbox chainman built from
// SandboxOptions{.trust_anchors = ...}. The fixture's own chainman carries no
// anchors and only generates the blocks. Its block maker follows
// MinerTestingSetup::Block in validation_block_tests.cpp.

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <common/args.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <kernel/chainparams.h>
#include <node/chainstate.h>
#include <node/chainstatemanager_args.h>
#include <node/kernel_notifications.h>
#include <node/miner.h>
#include <test/util/mining.h>
#include <test/util/script.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>
#include <util/task_runner.h>
#include <validation.h>
#include <validationinterface.h>

#include <boost/test/unit_test.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using node::BlockAssembler;

namespace {

//! Records the state ProcessNewBlock reports for a block it refuses.
class CheckedRecorder final : public CValidationInterface
{
public:
    std::optional<BlockValidationState> last_state;
    uint256 last_hash;

protected:
    void BlockChecked(const CBlock& block, const BlockValidationState& state) override
    {
        last_hash = block.GetHash();
        last_state = state;
    }
};

//! A sandbox chainman built from its own chain params, with its own block files.
//! Members are destroyed in reverse order: chainman before the params and the
//! signals it refers to.
struct AnchoredNode {
    std::unique_ptr<const CChainParams> params;
    std::unique_ptr<ValidationSignals> signals;
    std::shared_ptr<CheckedRecorder> recorder;
    std::unique_ptr<ChainstateManager> chainman;

    ~AnchoredNode()
    {
        if (signals && recorder) signals->UnregisterSharedValidationInterface(recorder);
        chainman.reset();
    }
};

struct TrustAnchorSetup : public SandboxingSetup {
    int m_node_count{0};
    int m_coinbase_counter{0};
    int64_t m_time{Params().GenesisBlock().nTime};

    //! An unsolved block on prev_hash, valid at its height, with a unique coinbase.
    std::shared_ptr<CBlock> Block(const uint256& prev_hash)
    {
        BlockAssembler::Options options;
        options.coinbase_output_script = CScript{} << m_coinbase_counter++ << OP_TRUE;
        auto ptemplate = BlockAssembler{m_node.chainman->ActiveChainstate(), m_node.relaypool.get(), options}.CreateNewBlock();
        auto pblock = std::make_shared<CBlock>(ptemplate->block);
        pblock->hashPrevBlock = prev_hash;
        pblock->nTime = ++m_time;

        const int height{WITH_LOCK(::cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(prev_hash)->nHeight + 1)};
        CMutableTransaction coinbase(*pblock->vtx[0]);
        coinbase.vout.resize(2);
        coinbase.vout[1].scriptPubKey = P2WSH_OP_TRUE;
        coinbase.vout[1].nValue = GetBlockSubsidy(height, Params().GetConsensus());
        coinbase.vout[0].nValue = 0;
        coinbase.vin[0].scriptWitness.SetNull();
        coinbase.vin[0].scriptSig = CScript{} << height << OP_0;
        pblock->vtx[0] = MakeTransactionRef(std::move(coinbase));
        return pblock;
    }

    //! Solve a block and record its header in the generator chainman, so later
    //! blocks can find their height.
    std::shared_ptr<const CBlock> GoodBlock(const uint256& prev_hash)
    {
        auto pblock = Block(prev_hash);
        const CBlockIndex* prev{WITH_LOCK(::cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(prev_hash))};
        m_node.chainman->GenerateCoinbaseCommitment(*pblock, prev);
        pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
        SolveBlockPoW(*pblock, Params().GetConsensus());
        BlockValidationState state;
        BOOST_REQUIRE(m_node.chainman->ProcessNewBlockHeaders({{pblock->GetBlockHeader()}}, true, state));
        return pblock;
    }

    //! blocks[i] is at height first_height + i, building on prev_hash.
    std::vector<std::shared_ptr<const CBlock>> MineChain(const uint256& prev_hash, int count)
    {
        std::vector<std::shared_ptr<const CBlock>> blocks;
        uint256 prev{prev_hash};
        for (int i = 0; i < count; ++i) {
            blocks.push_back(GoodBlock(prev));
            prev = blocks.back()->GetHash();
        }
        return blocks;
    }

    //! Chain work at a block, read from the generator chainman.
    uint256 WorkAt(const uint256& hash)
    {
        LOCK(::cs_main);
        return ArithToUint256(Assert(m_node.chainman->m_blockman.LookupBlockIndex(hash))->nChainWork);
    }

    //! A fresh chainman holding only genesis, over sandbox params with these anchors.
    std::unique_ptr<AnchoredNode> MakeNode(std::vector<TrustAnchor> anchors,
                                           const std::vector<std::pair<std::string, std::string>>& args = {})
    {
        auto node = std::make_unique<AnchoredNode>();
        CChainParams::SandboxOptions opts{};
        opts.trust_anchors = std::move(anchors);
        node->params = CChainParams::Sandbox(opts);
        node->signals = std::make_unique<ValidationSignals>(std::make_unique<util::ImmediateTaskRunner>());
        node->recorder = std::make_shared<CheckedRecorder>();
        node->signals->RegisterSharedValidationInterface(node->recorder);

        const fs::path dir{m_args.GetDataDirNet() / fs::u8path(strprintf("anchored%d", m_node_count++))};
        fs::create_directories(dir / "blocks"); // init.cpp creates the blocks dir for a real node
        ChainstateManager::Options chainman_opts{
            .chainparams = *node->params,
            .datadir = dir,
            .check_block_index = 1,
            .notifications = *m_node.notifications,
            .signals = node->signals.get(),
            .worker_threads_num = 0,
        };
        ArgsManager local_args;
        for (const auto& [name, value] : args) local_args.ForceSetArg(name, value);
        BOOST_REQUIRE(node::ApplyArgsManOptions(local_args, chainman_opts));
        const node::BlockManager::Options blockman_opts{
            .chainparams = chainman_opts.chainparams,
            .blocks_dir = dir / "blocks",
            .notifications = chainman_opts.notifications,
            .block_tree_db_params = DBParams{
                .path = dir / "blocks" / "index",
                .cache_bytes = m_kernel_cache_sizes.block_tree_db,
                .memory_only = true,
            },
        };
        node->chainman = std::make_unique<ChainstateManager>(*Assert(m_node.shutdown_signal), chainman_opts, blockman_opts);

        node::ChainstateLoadOptions load_opts;
        load_opts.coins_db_in_memory = true;
        auto [status, error] = node::LoadChainstate(*node->chainman, m_kernel_cache_sizes, load_opts);
        BOOST_REQUIRE(status == node::ChainstateLoadStatus::SUCCESS);
        std::tie(status, error) = node::VerifyLoadedChainstate(*node->chainman, load_opts);
        BOOST_REQUIRE(status == node::ChainstateLoadStatus::SUCCESS);
        BlockValidationState state;
        BOOST_REQUIRE(node->chainman->ActiveChainstate().ActivateBestChain(state));
        BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return node->chainman->ActiveHeight()), 0);
        return node;
    }

    static std::vector<CBlockHeader> Headers(const std::vector<std::shared_ptr<const CBlock>>& blocks, size_t first, size_t last)
    {
        std::vector<CBlockHeader> headers;
        for (size_t i = first; i <= last; ++i) headers.push_back(blocks.at(i)->GetBlockHeader());
        return headers;
    }

    static bool Known(const AnchoredNode& node, const uint256& hash)
    {
        LOCK(::cs_main);
        return node.chainman->m_blockman.LookupBlockIndex(hash) != nullptr;
    }

    // Chain A: A[h] is at height h; A[0] is genesis. Fork B forks from genesis.
    std::vector<std::shared_ptr<const CBlock>> A;
    std::vector<std::shared_ptr<const CBlock>> B;

    TrustAnchorSetup()
    {
        A.push_back(std::make_shared<const CBlock>(Params().GenesisBlock()));
        for (auto& block : MineChain(A[0]->GetHash(), 12)) A.push_back(block);
        B.push_back(A[0]);
        for (auto& block : MineChain(A[0]->GetHash(), 8)) B.push_back(block);
        for (int h = 1; h <= 8; ++h) BOOST_REQUIRE(A[h]->GetHash() != B[h]->GetHash());
    }

    TrustAnchor AnchorAt(int height) { return TrustAnchor{height, A[height]->GetHash(), WorkAt(A[height]->GetHash())}; }

    //! A node holding A's headers 1..10, anchored at A[5].
    std::unique_ptr<AnchoredNode> NodeWithChainA(std::vector<TrustAnchor> anchors)
    {
        auto node = MakeNode(std::move(anchors));
        BlockValidationState state;
        BOOST_REQUIRE(node->chainman->ProcessNewBlockHeaders(Headers(A, 1, 10), true, state));
        BOOST_REQUIRE(Known(*node, A[10]->GetHash()));
        return node;
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(trust_anchor_tests, TrustAnchorSetup)

BOOST_AUTO_TEST_CASE(anchor_mismatch_rejects_fresh_node)
{
    auto node = MakeNode({AnchorAt(5)});
    BlockValidationState state;
    BOOST_CHECK(!node->chainman->ProcessNewBlockHeaders(Headers(B, 1, 8), true, state));
    BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_CHECKPOINT);
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "checkpoint-mismatch");
    for (int h = 5; h <= 8; ++h) BOOST_CHECK_MESSAGE(!Known(*node, B[h]->GetHash()), "B[" << h << "] entered the block index");
}

BOOST_AUTO_TEST_CASE(anchor_match_accepts)
{
    auto node = MakeNode({AnchorAt(5)});
    BlockValidationState state;
    BOOST_CHECK(node->chainman->ProcessNewBlockHeaders(Headers(A, 1, 10), true, state));
    BOOST_CHECK(state.IsValid());
    BOOST_CHECK(Known(*node, A[10]->GetHash()));
}

BOOST_AUTO_TEST_CASE(fork_below_known_anchor_rejected)
{
    auto node = NodeWithChainA({AnchorAt(5)});
    const auto fork{GoodBlock(A[2]->GetHash())}; // height 3
    BlockValidationState state;
    BOOST_CHECK(!node->chainman->ProcessNewBlockHeaders({{fork->GetBlockHeader()}}, true, state));
    BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_CHECKPOINT);
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-fork-prior-to-checkpoint");
    BOOST_CHECK(!Known(*node, fork->GetHash()));
}

BOOST_AUTO_TEST_CASE(fork_above_known_anchor_accepted)
{
    auto node = NodeWithChainA({AnchorAt(5)});
    const auto fork{GoodBlock(A[6]->GetHash())}; // height 7
    BlockValidationState state;
    BOOST_CHECK(node->chainman->ProcessNewBlockHeaders({{fork->GetBlockHeader()}}, true, state));
    BOOST_CHECK(state.IsValid());
    BOOST_CHECK(Known(*node, fork->GetHash()));
}

BOOST_AUTO_TEST_CASE(no_anchors_no_rejection)
{
    auto node = NodeWithChainA({});
    const auto fork{GoodBlock(A[2]->GetHash())}; // height 3
    BlockValidationState state;
    BOOST_CHECK(node->chainman->ProcessNewBlockHeaders({{fork->GetBlockHeader()}}, true, state));
    BOOST_CHECK(state.IsValid());
    BOOST_CHECK(Known(*node, fork->GetHash()));
    // Defaults stay null without anchors.
    BOOST_CHECK(node->params->GetConsensus().defaultAssumeValid.IsNull());
    BOOST_CHECK(node->params->GetConsensus().nMinimumChainWork.IsNull());
}

BOOST_AUTO_TEST_CASE(newest_anchor_derives_assumevalid_and_minchainwork)
{
    const TrustAnchor a5{AnchorAt(5)};
    const TrustAnchor a9{AnchorAt(9)};
    BOOST_REQUIRE(UintToArith256(a5.chain_work) < UintToArith256(a9.chain_work));
    CChainParams::SandboxOptions opts{};
    opts.trust_anchors = {a5, a9};
    const auto params{CChainParams::Sandbox(opts)};
    BOOST_REQUIRE_EQUAL(params->TrustAnchors().size(), 2U);
    BOOST_CHECK_EQUAL(params->TrustAnchors().back().height, 9);
    BOOST_CHECK_EQUAL(params->GetConsensus().defaultAssumeValid, A[9]->GetHash());
    BOOST_CHECK_EQUAL(params->GetConsensus().nMinimumChainWork, a9.chain_work);

    // A chainman over those params takes them as its defaults.
    auto node = MakeNode({a5, a9});
    BOOST_CHECK_EQUAL(node->chainman->AssumedValidBlock(), A[9]->GetHash());
    BOOST_CHECK(node->chainman->MinimumChainWork() == UintToArith256(a9.chain_work));
}

// Review focus 3: -assumevalid=0 overrides the derived default, and the anchor
// check still runs under it.
BOOST_AUTO_TEST_CASE(assumevalid_override_keeps_anchor_check)
{
    auto node = MakeNode({AnchorAt(5)}, {{"-assumevalid", "0"}});
    BOOST_CHECK(node->chainman->AssumedValidBlock().IsNull());
    BlockValidationState state;
    BOOST_CHECK(!node->chainman->ProcessNewBlockHeaders(Headers(B, 1, 8), true, state));
    BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_CHECKPOINT);
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "checkpoint-mismatch");
    BOOST_CHECK(!Known(*node, B[5]->GetHash()));
}

// Review focus 4: a full block at a mismatching anchor height is refused the same
// way, because AcceptBlock goes through AcceptBlockHeader.
BOOST_AUTO_TEST_CASE(full_block_at_mismatching_anchor_rejected)
{
    auto node = MakeNode({AnchorAt(5)});
    BlockValidationState state;
    BOOST_REQUIRE(node->chainman->ProcessNewBlockHeaders(Headers(B, 1, 4), true, state));
    BOOST_CHECK(!node->chainman->ProcessNewBlock(B[5], /*force_processing=*/true, /*min_pow_checked=*/true, /*new_block=*/nullptr));
    BOOST_REQUIRE(node->recorder->last_state.has_value());
    BOOST_CHECK_EQUAL(node->recorder->last_hash, B[5]->GetHash());
    BOOST_CHECK(node->recorder->last_state->GetResult() == BlockValidationResult::BLOCK_CHECKPOINT);
    BOOST_CHECK_EQUAL(node->recorder->last_state->GetRejectReason(), "checkpoint-mismatch");
    BOOST_CHECK(!Known(*node, B[5]->GetHash()));
}

BOOST_AUTO_TEST_SUITE_END()
