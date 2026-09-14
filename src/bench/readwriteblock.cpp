// Copyright (c) 2023 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <bench/data/quicksilver_block.raw.h>
#include <flatfile.h>
#include <node/blockstorage.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <span.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>

// The fixture is a real sandbox block at height 151, so these benchmarks run
// under sandbox params: ReadBlock re-checks proof of work on the way in, and a
// sandbox block does not satisfy main's.
static CBlock CreateTestBlock()
{
    DataStream stream{benchmark::data::quicksilver_block};
    CBlock block;
    stream >> TX_WITH_WITNESS(block);
    return block;
}

static void SaveBlockBench(benchmark::Bench& bench)
{
    const auto testing_setup{MakeNoLogFileContext<const TestingSetup>(ChainType::SANDBOX)};
    auto& blockman{testing_setup->m_node.chainman->m_blockman};
    const CBlock block{CreateTestBlock()};
    bench.run([&] {
        const auto pos{blockman.WriteBlock(block, 151)};
        assert(!pos.IsNull());
    });
}

static void ReadBlockBench(benchmark::Bench& bench)
{
    const auto testing_setup{MakeNoLogFileContext<const TestingSetup>(ChainType::SANDBOX)};
    auto& blockman{testing_setup->m_node.chainman->m_blockman};
    const auto pos{blockman.WriteBlock(CreateTestBlock(), 151)};
    CBlock block;
    bench.run([&] {
        const auto success{blockman.ReadBlock(block, pos)};
        assert(success);
    });
}

static void ReadRawBlockBench(benchmark::Bench& bench)
{
    const auto testing_setup{MakeNoLogFileContext<const TestingSetup>(ChainType::SANDBOX)};
    auto& blockman{testing_setup->m_node.chainman->m_blockman};
    const auto pos{blockman.WriteBlock(CreateTestBlock(), 151)};
    std::vector<uint8_t> block_data;
    blockman.ReadRawBlock(block_data, pos); // warmup
    bench.run([&] {
        const auto success{blockman.ReadRawBlock(block_data, pos)};
        assert(success);
    });
}

BENCHMARK(SaveBlockBench, benchmark::PriorityLevel::HIGH);
BENCHMARK(ReadBlockBench, benchmark::PriorityLevel::HIGH);
BENCHMARK(ReadRawBlockBench, benchmark::PriorityLevel::HIGH);
