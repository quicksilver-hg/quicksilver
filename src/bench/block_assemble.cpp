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
#include <util/check.h>
#include <test/util/mining.h>
#include <test/util/script.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <array>
#include <cassert>
#include <cstddef>
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
        // tip each transaction was built on: these span 200 blocks and nMaxAnchorAge
        // is 100, so anchoring at build time would leave the earliest ones too old.
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
            assert(res.m_result_type == RelayPoolAcceptResult::ResultType::VALID);
        }
    }

    bench.run([&] {
        PrepareBlock(test_setup->m_node, options);
    });
}
static void BlockAssemblerAddPackageTxns(benchmark::Bench& bench)
{
    FastRandomContext det_rand{true};
    auto testing_setup{MakeNoLogFileContext<TestChain100Setup>()};
    testing_setup->PopulateRelayPool(det_rand, /*num_transactions=*/1000, /*submit=*/true);
    BlockAssembler::Options assembler_options;
    assembler_options.test_block_validity = false;
    assembler_options.coinbase_output_script = P2WSH_OP_TRUE;

    bench.run([&] {
        PrepareBlock(testing_setup->m_node, assembler_options);
    });
}

BENCHMARK(AssembleBlock, benchmark::PriorityLevel::HIGH);
BENCHMARK(BlockAssemblerAddPackageTxns, benchmark::PriorityLevel::LOW);
