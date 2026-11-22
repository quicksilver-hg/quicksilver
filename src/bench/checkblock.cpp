// Copyright (c) 2016-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <bench/data/quicksilver_block.raw.h>
#include <chainparams.h>
#include <common/args.h>
#include <consensus/validation.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <span.h>
#include <streams.h>
#include <util/chaintype.h>
#include <validation.h>

#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

// These are the two major time-sinks which happen after we have fully received
// a block off the wire, but before we can relay the block on to peers using
// compact block relay.

static void DeserializeBlockTest(benchmark::Bench& bench)
{
    DataStream stream(benchmark::data::quicksilver_block);
    std::byte a{0};
    stream.write({&a, 1}); // Prevent compaction

    bench.unit("block").run([&] {
        CBlock block;
        stream >> TX_WITH_WITNESS(block);
        bool rewound = stream.Rewind(benchmark::data::quicksilver_block.size());
        assert(rewound);
    });
}

// The fixture's 21 transactions are untouched by header-field congestion pricing, but its header widened
// with nCongestion (248 -> 252 bytes). The field was set to the value the EIP-1559
// recurrence gives for this block's own weight against a parent at the floor, so the
// header is self-consistent rather than carrying a placeholder. The trivial sandbox
// proof hash is over nCycle alone, so it is unaffected by the widening.
static void DeserializeAndCheckBlockTest(benchmark::Bench& bench)
{
    DataStream stream(benchmark::data::quicksilver_block);
    std::byte a{0};
    stream.write({&a, 1}); // Prevent compaction

    ArgsManager bench_args;
    // The fixture is a real sandbox block, so it is checked under sandbox rules --
    // CheckBlock verifies proof of work by default, and a sandbox block does not
    // satisfy main's. The figure is therefore transaction checks, the merkle root
    // and the sandbox proof hash: it excludes the Cuckatoo cycle verify that main
    // and publictest pay for the block, which no benchmark prices yet.
    const auto chainParams = CreateChainParams(bench_args, ChainType::SANDBOX);

    bench.unit("block").run([&] {
        CBlock block; // Note that CBlock caches its checked state, so we need to recreate it here
        stream >> TX_WITH_WITNESS(block);
        bool rewound = stream.Rewind(benchmark::data::quicksilver_block.size());
        assert(rewound);

        BlockValidationState validationState;
        bool checked = CheckBlock(block, validationState, chainParams->GetConsensus());
        assert(checked);
    });
}

BENCHMARK(DeserializeBlockTest, benchmark::PriorityLevel::HIGH);
BENCHMARK(DeserializeAndCheckBlockTest, benchmark::PriorityLevel::HIGH);
