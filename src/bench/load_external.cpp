// Copyright (c) 2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <bench/data/quicksilver_block.raw.h>
#include <chainparams.h>
#include <flatfile.h>
#include <node/blockstorage.h>
#include <span.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/check.h>
#include <util/fs.h>
#include <validation.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

/**
 * The LoadExternalBlockFile() function is used during -reindex and -loadblock.
 *
 * Create a test file that's similar to a datadir/blocks/blk?????.dat file,
 * It contains MAX_BLOCKFILE_SIZE / (record size) copies of the same block: 524 with the
 * current fixture block (255,978 bytes plus 8 bytes of framing per record).
 * For each block in the file, LoadExternalBlockFile() won't find its parent,
 * and so will skip the block. (In the real system, it will re-read the block
 * from disk later when it encounters its parent.)
 *
 * This benchmark measures the performance of deserializing the block (or just
 * its header, beginning with PR 16981).
 */
static void LoadExternalBlockFile(benchmark::Bench& bench)
{
    const auto testing_setup{MakeNoLogFileContext<const TestingSetup>(ChainType::MAIN)};

    // Create a single block as in the blocks files (magic bytes, block size,
    // block data) as a stream object.
    const fs::path blkfile{testing_setup.get()->m_path_root / "blk.dat"};
    DataStream ss{};
    auto params{testing_setup->m_node.chainman->GetParams()};
    ss << params.MessageStart();
    ss << static_cast<uint32_t>(benchmark::data::quicksilver_block.size());
    // We can't use the streaming serialization (ss << benchmark::data::quicksilver_block)
    // because that first writes a compact size.
    ss << Span{benchmark::data::quicksilver_block};

    // Create the test file.
    const size_t n_records{node::MAX_BLOCKFILE_SIZE / ss.size()};
    {
        // "wb+" is "binary, O_RDWR | O_CREAT | O_TRUNC".
        FILE* file{fsbridge::fopen(blkfile, "wb+")};
        if (!file) throw std::runtime_error("cannot create test file");
        // Make the test block file about 128 MB in length.
        for (size_t i = 0; i < n_records; ++i) {
            if (fwrite(ss.data(), 1, ss.size(), file) != ss.size()) {
                throw std::runtime_error("write to test file failed\n");
            }
        }
        if (fclose(file) != 0) throw std::runtime_error("close of test file failed");
    }

    FlatFilePos pos;
    // Every record must be read and set aside as having an unknown parent, or the
    // benchmark times something other than the header scan it describes.
    {
        std::multimap<uint256, FlatFilePos> blocks_with_unknown_parent;
        AutoFile file{fsbridge::fopen(blkfile, "rb")};
        if (file.IsNull()) throw std::runtime_error("cannot open test file");
        testing_setup->m_node.chainman->LoadExternalBlockFile(file, &pos, &blocks_with_unknown_parent);
        Assert(blocks_with_unknown_parent.size() == n_records);
    }
    bench.run([&] {
        // A fresh map each run, so every sample scans the same state; a map kept across
        // runs would gain n_records entries per run.
        std::multimap<uint256, FlatFilePos> blocks_with_unknown_parent;
        // "rb" is "binary, O_RDONLY", positioned to the start of the file.
        // The file will be closed by LoadExternalBlockFile().
        AutoFile file{fsbridge::fopen(blkfile, "rb")};
        if (file.IsNull()) throw std::runtime_error("cannot open test file");
        testing_setup->m_node.chainman->LoadExternalBlockFile(file, &pos, &blocks_with_unknown_parent);
    });
    fs::remove(blkfile);
}

BENCHMARK(LoadExternalBlockFile, benchmark::PriorityLevel::HIGH);
