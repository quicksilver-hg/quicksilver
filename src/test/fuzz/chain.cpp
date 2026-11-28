// Copyright (c) 2020-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <primitives/block.h>
#include <sync.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>

#include <cassert>
#include <cstdint>
#include <optional>
#include <vector>

FUZZ_TARGET(chain)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());
    std::optional<CDiskBlockIndex> disk_block_index = ConsumeDeserializable<CDiskBlockIndex>(fuzzed_data_provider);
    if (!disk_block_index) {
        return;
    }

    const uint256 zero{};
    disk_block_index->phashBlock = &zero;
    {
        LOCK(::cs_main);
        (void)disk_block_index->ConstructBlockHash();
        (void)disk_block_index->GetBlockPos();
        (void)disk_block_index->GetBlockTime();
        (void)disk_block_index->GetBlockTimeMax();
        (void)disk_block_index->GetMedianTimePast();
        (void)disk_block_index->GetUndoPos();
        (void)disk_block_index->HaveNumChainTxs();
        (void)disk_block_index->IsValid();
    }

    // S0392-B-01: compare the reconstructed fields and hash, not only call them.
    // The expected header is assembled here from the index's own fields.
    CBlockHeader expected;
    expected.nVersion = disk_block_index->nVersion;
    expected.hashPrevBlock = disk_block_index->hashPrev;
    expected.hashMerkleRoot = disk_block_index->hashMerkleRoot;
    expected.nTime = disk_block_index->nTime;
    expected.nBits = disk_block_index->nBits;
    expected.nCongestion = static_cast<uint32_t>(disk_block_index->m_congestion);
    expected.nNonce = disk_block_index->nNonce;
    expected.nCycle = disk_block_index->nCycle;
    assert(disk_block_index->ConstructBlockHash() == expected.GetHash());

    const CBlockHeader block_header = disk_block_index->GetBlockHeader();
    // No pprev here, so the in-memory header has a null previous hash.
    assert(block_header.hashPrevBlock.IsNull());
    assert(block_header.nVersion == expected.nVersion);
    assert(block_header.hashMerkleRoot == expected.hashMerkleRoot);
    assert(block_header.nTime == expected.nTime);
    assert(block_header.nBits == expected.nBits);
    assert(block_header.nCongestion == expected.nCongestion);
    assert(block_header.nNonce == expected.nNonce);
    assert(block_header.nCycle == expected.nCycle);

    const CDiskBlockIndex copy{*disk_block_index};
    assert(copy.hashPrev == disk_block_index->hashPrev);
    assert(copy.ConstructBlockHash() == expected.GetHash());
    (void)disk_block_index->BuildSkip();

    LIMITED_WHILE(fuzzed_data_provider.ConsumeBool(), 10000) {
        const BlockStatus block_status = fuzzed_data_provider.PickValueInArray({
            BlockStatus::BLOCK_VALID_UNKNOWN,
            BlockStatus::BLOCK_VALID_TREE,
            BlockStatus::BLOCK_VALID_TRANSACTIONS,
            BlockStatus::BLOCK_VALID_CHAIN,
            BlockStatus::BLOCK_VALID_SCRIPTS,
            BlockStatus::BLOCK_VALID_MASK,
            BlockStatus::BLOCK_HAVE_DATA,
            BlockStatus::BLOCK_HAVE_UNDO,
            BlockStatus::BLOCK_HAVE_MASK,
            BlockStatus::BLOCK_FAILED_VALID,
            BlockStatus::BLOCK_FAILED_CHILD,
            BlockStatus::BLOCK_FAILED_MASK,
            BlockStatus::BLOCK_OPT_WITNESS,
        });
        if (block_status & ~BLOCK_VALID_MASK) {
            continue;
        }
        WITH_LOCK(::cs_main, (void)disk_block_index->RaiseValidity(block_status));
    }

    CBlockIndex block_index{block_header};
    block_index.phashBlock = &zero;
    (void)block_index.GetBlockHash();
    (void)block_index.ToString();
    // An index built from a header gives that header back, byte for byte.
    assert(block_index.GetBlockHeader().GetHash() == block_header.GetHash());
}
