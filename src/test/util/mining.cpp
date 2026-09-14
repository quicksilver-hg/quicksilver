// Copyright (c) 2019-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/mining.h>

#include <arith_uint256.h>
#include <chainparams.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <crypto/cuckatoo/cuckatoo.h>
#include <key_io.h>
#include <node/context.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <util/check.h>
#include <validation.h>
#include <validationinterface.h>

using node::BlockAssembler;
using node::NodeContext;

void SolveBlockPoW(CBlockHeader& header, const Consensus::Params& params)
{
    // Quicksilver: sandbox-only trivial block PoW (fBlockPowNoCycle). No real cycle
    // is required, so skip the expensive CuckatooSolve sweep and grind only the cheap
    // proof-hash threshold — this is what makes the 100-block maturity fixtures mine
    // instantly. CheckProofOfWorkImpl applies the matching skip on the verify side.
    if (params.fBlockPowNoCycle) {
        const auto target = DeriveTarget(header.nBits, params.powLimit);
        assert(target); // sandbox nBits/powLimit always derive a valid target
        for (uint32_t i = 0; i < header.nCycle.size(); ++i) header.nCycle[i] = i + 1;
        while (UintToArith256(cuckatoo::CuckatooProofHash(header.nCycle)) > *target) {
            header.nCycle[0] += header.nCycle.size();
        }
        assert(CheckProofOfWork(header, params));
        return;
    }
    // Cuckatoo: sweep nNonce until a 42-cycle is found whose blake2b meets the
    // target. The solver reuses one multi-threaded context across the sweep.
    while (true) {
        const auto pre = header.PrePowBytes();
        cuckatoo::Cycle cyc{};
        uint32_t won = 0;
        if (cuckatoo::CuckatooSolve(pre, params.nEdgeBits, header.nNonce, 1u << 20, cyc, won)) {
            header.nNonce = won;
            header.nCycle = cyc;
            if (CheckProofOfWork(header, params)) return;
            header.nNonce = won + 1;  // cycle found but above target; keep sweeping
        } else {
            header.nNonce += (1u << 20);  // no cycle in this window (improbable at sandbox)
            assert(header.nNonce != 0);
        }
    }
}

COutPoint generatetoaddress(const NodeContext& node, const std::string& address)
{
    const auto dest = DecodeDestination(address);
    assert(IsValidDestination(dest));
    BlockAssembler::Options assembler_options;
    assembler_options.coinbase_output_script = GetScriptForDestination(dest);

    return MineBlock(node, assembler_options);
}

COutPoint MineBlock(const NodeContext& node, const node::BlockAssembler::Options& assembler_options)
{
    auto block = PrepareBlock(node, assembler_options);
    auto valid = MineBlock(node, block);
    assert(!valid.IsNull());
    return valid;
}

struct BlockValidationStateCatcher : public CValidationInterface {
    const uint256 m_hash;
    std::optional<BlockValidationState> m_state;

    BlockValidationStateCatcher(const uint256& hash)
        : m_hash{hash},
          m_state{} {}

protected:
    void BlockChecked(const CBlock& block, const BlockValidationState& state) override
    {
        if (block.GetHash() != m_hash) return;
        m_state = state;
    }
};

COutPoint MineBlock(const NodeContext& node, std::shared_ptr<CBlock>& block)
{
    SolveBlockPoW(*block, Params().GetConsensus());

    auto& chainman{*Assert(node.chainman)};
    const auto old_height = WITH_LOCK(chainman.GetMutex(), return chainman.ActiveHeight());
    bool new_block;
    BlockValidationStateCatcher bvsc{block->GetHash()};
    node.validation_signals->RegisterValidationInterface(&bvsc);
    const bool processed{chainman.ProcessNewBlock(block, true, true, &new_block)};
    const bool duplicate{!new_block && processed};
    assert(!duplicate);
    node.validation_signals->UnregisterValidationInterface(&bvsc);
    node.validation_signals->SyncWithValidationInterfaceQueue();
    const bool was_valid{bvsc.m_state && bvsc.m_state->IsValid()};
    assert(old_height + was_valid == WITH_LOCK(chainman.GetMutex(), return chainman.ActiveHeight()));

    if (was_valid) return {block->vtx[0]->GetHash(), 0};
    return {};
}

std::shared_ptr<CBlock> PrepareBlock(const NodeContext& node,
                                     const BlockAssembler::Options& assembler_options)
{
    auto block = std::make_shared<CBlock>(
        BlockAssembler{Assert(node.chainman)->ActiveChainstate(), Assert(node.relaypool.get()), assembler_options}
            .CreateNewBlock()
            ->block);

    LOCK(cs_main);
    block->nTime = Assert(node.chainman)->ActiveChain().Tip()->GetMedianTimePast() + 1;
    block->hashMerkleRoot = BlockMerkleRoot(*block);

    return block;
}
std::shared_ptr<CBlock> PrepareBlock(const NodeContext& node, const CScript& coinbase_scriptPubKey)
{
    BlockAssembler::Options assembler_options;
    assembler_options.coinbase_output_script = coinbase_scriptPubKey;
    ApplyArgsManOptions(*node.args, assembler_options);
    return PrepareBlock(node, assembler_options);
}
