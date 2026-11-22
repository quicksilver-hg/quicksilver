// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/consensus.h>
#include <node/miner.h>
#include <primitives/transaction.h>
#include <random.h>
#include <script/script.h>
#include <sync.h>
#include <txrelaypool.h>
#include <util/check.h>
#include <test/util/mining.h>
#include <test/util/script.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <deque>
#include <iostream>
#include <memory>
#include <vector>

using node::BlockAssembler;

static void AssembleBlock(benchmark::Bench& bench)
{
    const auto test_setup = MakeNoLogFileContext<const TestingSetup>(ChainType::SANDBOX);

    // Skip real Cuckatoo cycle solving and verification, as functional tests using
    // -txpownocycle do.
    // Anchor recency and the proof-hash target stay live; what is dropped is the cycle
    // solve, which this benchmark does not measure and which would dominate its runtime.
    const_cast<Consensus::Params&>(Params().GetConsensus()).fTxPowNoCycle = true;

    CScriptWitness witness;
    witness.stack.push_back(WITNESS_STACK_ELEM_OP_TRUE);
    BlockAssembler::Options options;
    options.coinbase_output_script = P2WSH_OP_TRUE;

    // Collect some loose transactions that spend the coinbases of our mined blocks
    constexpr size_t NUM_BLOCKS{200};
    std::array<CMutableTransaction, NUM_BLOCKS - COINBASE_MATURITY + 1> mutable_txs;
    for (size_t b{0}; b < NUM_BLOCKS; ++b) {
        CMutableTransaction tx;
        tx.vin.emplace_back(MineBlock(test_setup->m_node, options));
        tx.vin.back().scriptWitness = witness;
        tx.vout.emplace_back(0, P2WSH_OP_TRUE); // value set at submission, below
        if (NUM_BLOCKS - b >= COINBASE_MATURITY)
            mutable_txs.at(b) = std::move(tx);
    }
    {
        LOCK(::cs_main);

        // Quicksilver transactions carry their own proof of work, so prove each one
        // before submitting. The anchor has to be the tip we submit against, not the
        // tip each transaction was built on: these span 200 blocks and sandbox's
        // nMaxAnchorAge is 20, so anchoring at build time would leave most of them too old.
        const CBlockIndex& anchor{*Assert(test_setup->m_node.chainman->ActiveChain().Tip())};
        const Consensus::Params& consensus{Params().GetConsensus()};

        const CCoinsViewCache& coins{test_setup->m_node.chainman->ActiveChainstate().CoinsTip()};

        for (auto& tx : mutable_txs) {
            // Quicksilver is feeless: value in must equal value out, so pay the whole
            // coinbase forward. The inherited code sent 1337 and burned the rest as fee.
            tx.vout.at(0).nValue = coins.AccessCoin(tx.vin.at(0).prevout).out.nValue;
            // Prove after setting the value: the proof pre-image commits to the outputs.
            ProveTxPowForTest(tx, anchor, consensus);
            const RelayPoolAcceptResult res =
                test_setup->m_node.chainman->ProcessTransaction(MakeTransactionRef(tx));
            if (res.m_result_type != RelayPoolAcceptResult::ResultType::VALID) {
                std::cerr << "AssembleBlock: fixture transaction rejected: " << res.m_state.ToString() << std::endl;
            }
            Assert(res.m_result_type == RelayPoolAcceptResult::ResultType::VALID);
        }
    }

    // The template must hold every submitted transaction plus the coinbase; otherwise
    // the benchmark would time a smaller (or empty) assembly than its name claims.
    Assert(PrepareBlock(test_setup->m_node, options)->vtx.size() == 1 + mutable_txs.size());

    bench.run([&] {
        PrepareBlock(test_setup->m_node, options);
    });
}

/**
 * Stage num_transactions random transactions spending the setup's coinbases, the way
 * TestChain100Setup::PopulateRelayPool does, but anchored at the current tip. The
 * shared helper leaves nAnchorHeight at 0, which is outside sandbox's nMaxAnchorAge
 * window at height 100, so BlockAssembler would skip every package. Values are split
 * exactly (feeless). Staging is unchecked, as in the shared helper: no per-tx proof.
 */
static void PopulateAnchoredRelayPool(TestChain100Setup& setup, FastRandomContext& det_rand, size_t num_transactions)
{
    LOCK2(cs_main, setup.m_node.relaypool->cs);
    const uint32_t anchor_height{static_cast<uint32_t>(Assert(setup.m_node.chainman->ActiveChain().Tip())->nHeight)};
    std::deque<std::pair<COutPoint, CAmount>> unspent_prevouts;
    for (const auto& tx : setup.m_coinbase_txns) unspent_prevouts.emplace_back(COutPoint(tx->GetHash(), 0), tx->vout[0].nValue);
    while (num_transactions > 0 && !unspent_prevouts.empty()) {
        CMutableTransaction mtx;
        mtx.nAnchorHeight = anchor_height;
        const size_t num_inputs = det_rand.randrange(24) + 1;
        CAmount total_in{0};
        for (size_t n{0}; n < num_inputs && !unspent_prevouts.empty(); ++n) {
            const auto& [prevout, amount] = unspent_prevouts.front();
            mtx.vin.emplace_back(prevout, CScript());
            total_in += amount;
            unspent_prevouts.pop_front();
        }
        const size_t num_outputs = det_rand.randrange(24) + 1;
        const CAmount amount_per_output = total_in / num_outputs;
        const CAmount remainder = total_in - amount_per_output * num_outputs;
        for (size_t n{0}; n < num_outputs; ++n) {
            CScript spk = CScript() << CScriptNum(num_transactions + n);
            mtx.vout.emplace_back(amount_per_output + (n == 0 ? remainder : 0), spk);
        }
        CTransactionRef ptx = MakeTransactionRef(mtx);
        if (amount_per_output > 3000) {
            for (size_t n{0}; n < num_outputs; ++n) {
                unspent_prevouts.emplace_back(COutPoint(ptx->GetHash(), n), ptx->vout[n].nValue);
                std::swap(unspent_prevouts.back(), unspent_prevouts[det_rand.randrange(unspent_prevouts.size())]);
            }
        }
        LockPoints lp;
        auto changeset = setup.m_node.relaypool->GetChangeSet();
        changeset->StageAddition(ptx, /*time=*/0, /*entry_height=*/1, /*entry_sequence=*/0,
                                 /*spends_coinbase=*/false, /*sigops_cost=*/4, lp);
        changeset->Apply();
        --num_transactions;
    }
}
static void BlockAssemblerAddPackageTxns(benchmark::Bench& bench)
{
    FastRandomContext det_rand{true};
    auto testing_setup{MakeNoLogFileContext<TestChain100Setup>()};
    PopulateAnchoredRelayPool(*testing_setup, det_rand, /*num_transactions=*/1000);
    BlockAssembler::Options assembler_options;
    assembler_options.test_block_validity = false;
    assembler_options.coinbase_output_script = P2WSH_OP_TRUE;

    // test_block_validity is off, so nothing else would notice a coinbase-only template.
    Assert(PrepareBlock(testing_setup->m_node, assembler_options)->vtx.size() > 1);

    bench.run([&] {
        PrepareBlock(testing_setup->m_node, assembler_options);
    });
}

BENCHMARK(AssembleBlock, benchmark::PriorityLevel::HIGH);
BENCHMARK(BlockAssemblerAddPackageTxns, benchmark::PriorityLevel::LOW);
