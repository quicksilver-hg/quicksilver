// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/miner.h>

#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <common/args.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <deploymentstatus.h>
#include <logging.h>
#include <policy/policy.h>
#include <pow.h>
#include <primitives/transaction.h>
#include <util/moneystr.h>
#include <util/time.h>
#include <validation.h>

#include <algorithm>
#include <utility>

namespace node {

int64_t GetMinimumTime(const CBlockIndex* pindexPrev, const int64_t difficulty_adjustment_interval)
{
    // Quicksilver has no timewarp adjustment: the retarget anchor spans a full
    // interval, so median-time-past is the only lower bound consensus imposes.
    // The interval parameter is retained for call-site compatibility.
    (void)difficulty_adjustment_interval;
    return pindexPrev->GetMedianTimePast() + 1;
}

int64_t UpdateTime(CBlockHeader* pblock, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev)
{
    int64_t nOldTime = pblock->nTime;
    int64_t nNewTime{std::max<int64_t>(GetMinimumTime(pindexPrev, consensusParams.DifficultyAdjustmentInterval()),
                                       TicksSinceEpoch<std::chrono::seconds>(NodeClock::now()))};

    if (nOldTime < nNewTime) {
        pblock->nTime = nNewTime;
    }

    // Updating time can change work required on networks with min-difficulty blocks:
    if (consensusParams.fPowAllowMinDifficultyBlocks) {
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, consensusParams);
    }

    return nNewTime - nOldTime;
}

void RegenerateCommitments(CBlock& block, ChainstateManager& chainman)
{
    CMutableTransaction tx{*block.vtx.at(0)};
    tx.vout.erase(tx.vout.begin() + GetWitnessCommitmentIndex(block));
    block.vtx.at(0) = MakeTransactionRef(tx);

    const CBlockIndex* prev_block = WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(block.hashPrevBlock));
    chainman.GenerateCoinbaseCommitment(block, prev_block);

    block.hashMerkleRoot = BlockMerkleRoot(block);

    // Quicksilver (#5c-1 Phase 2): nCongestion folds in this block's own weight, so a
    // body change invalidates it exactly as it invalidates the merkle root above. This
    // is why it is recomputed here and not left to the assembler: every caller of this
    // function has just changed vtx, and a stale value is rejected as "bad-congestion".
    // Callers must grind the PoW after this — nCongestion sits inside the pre-pow.
    if (prev_block != nullptr) {
        block.nCongestion = static_cast<uint32_t>(
            NextCongestionMultiplier(prev_block->m_congestion, GetBlockWeight(block),
                                     chainman.GetConsensus()));
    }
}

static BlockAssembler::Options ClampOptions(BlockAssembler::Options options)
{
    Assert(options.block_reserved_weight <= MAX_BLOCK_WEIGHT);
    Assert(options.block_reserved_weight >= MINIMUM_BLOCK_RESERVED_WEIGHT);
    Assert(options.coinbase_output_max_additional_sigops <= MAX_BLOCK_SIGOPS_COST);
    // Limit weight to between block_reserved_weight and MAX_BLOCK_WEIGHT for sanity:
    // block_reserved_weight can safely exceed -blockmaxweight, but the rest of the block template will be empty.
    options.nBlockMaxWeight = std::clamp<size_t>(options.nBlockMaxWeight, options.block_reserved_weight, MAX_BLOCK_WEIGHT);
    return options;
}

BlockAssembler::BlockAssembler(Chainstate& chainstate, const CTxRelayPool* relaypool, const Options& options)
    : chainparams{chainstate.m_chainman.GetParams()},
      m_relaypool{options.use_relaypool ? relaypool : nullptr},
      m_chainstate{chainstate},
      m_options{ClampOptions(options)}
{
}

void ApplyArgsManOptions(const ArgsManager& args, BlockAssembler::Options& options)
{
    // Block resource limits
    options.nBlockMaxWeight = args.GetIntArg("-blockmaxweight", options.nBlockMaxWeight);
    options.block_reserved_weight = args.GetIntArg("-blockreservedweight", options.block_reserved_weight);
}

void BlockAssembler::resetBlock()
{
    inBlock.clear();

    // Reserve space for fixed-size block header, txs count, and coinbase tx.
    nBlockWeight = m_options.block_reserved_weight;
    nBlockSigOpsCost = m_options.coinbase_output_max_additional_sigops;

    // These counters do not include coinbase tx
    nBlockTx = 0;
}

std::unique_ptr<CBlockTemplate> BlockAssembler::CreateNewBlock()
{
    const auto time_start{SteadyClock::now()};

    resetBlock();

    pblocktemplate.reset(new CBlockTemplate());
    CBlock* const pblock = &pblocktemplate->block; // pointer for convenience

    // Add dummy coinbase tx as first transaction
    pblock->vtx.emplace_back();
    pblocktemplate->vTxSigOpsCost.push_back(-1); // updated at end

    LOCK(::cs_main);
    CBlockIndex* pindexPrev = m_chainstate.m_chain.Tip();
    assert(pindexPrev != nullptr);
    nHeight = pindexPrev->nHeight + 1;

    pblock->nVersion = m_chainstate.m_chainman.m_versionbitscache.ComputeBlockVersion(pindexPrev, chainparams.GetConsensus());
    // -sandbox only: allow overriding block.nVersion with
    // -blockversion=N to test forking scenarios
    if (chainparams.MineBlocksOnDemand()) {
        pblock->nVersion = gArgs.GetIntArg("-blockversion", pblock->nVersion);
    }

    pblock->nTime = TicksSinceEpoch<std::chrono::seconds>(NodeClock::now());
    m_lock_time_cutoff = pindexPrev->GetMedianTimePast();

    int nPackagesSelected = 0;
    int nDescendantsUpdated = 0;
    if (m_relaypool) {
        addPackageTxs(nPackagesSelected, nDescendantsUpdated);
    }

    const auto time_1{SteadyClock::now()};

    m_last_block_num_txs = nBlockTx;
    m_last_block_weight = nBlockWeight;

    // Create coinbase transaction.
    CMutableTransaction coinbaseTx;
    coinbaseTx.vin.resize(1);
    coinbaseTx.vin[0].prevout.SetNull();
    coinbaseTx.vout.resize(1);
    coinbaseTx.vout[0].scriptPubKey = m_options.coinbase_output_script;
    // Quicksilver Frame-B mint (#4): claim nTxPowMint for each included tx carrying a valid
    // per-tx PoW. Consensus only caps the claim; the assembler must actually take it. Stale-
    // anchor txs are already excluded from pblock->vtx by TestPackageAnchors (#7), so this
    // sum is exactly the mint over the anchor-valid included set. vtx[0] is still the dummy
    // coinbase (mints nothing).
    coinbaseTx.vout[0].nValue = GetBlockSubsidy(nHeight, chainparams.GetConsensus())
                              + GetBlockMintAllowance(*pblock, chainparams.GetConsensus(), pindexPrev);
    coinbaseTx.vin[0].scriptSig = CScript() << nHeight << OP_0;
    pblock->vtx[0] = MakeTransactionRef(std::move(coinbaseTx));
    pblocktemplate->vchCoinbaseCommitment = m_chainstate.m_chainman.GenerateCoinbaseCommitment(*pblock, pindexPrev);
    LogInfo(HgLog::FORGE, "assembled weight=%u txs=%u sigops=%d\n", GetBlockWeight(*pblock), nBlockTx, nBlockSigOpsCost);

    // Fill in header
    pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
    UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);
    pblock->nBits          = GetNextWorkRequired(pindexPrev, pblock, chainparams.GetConsensus());
    // Quicksilver (#5c-1 Phase 2): the congestion multiplier is a header field that
    // consensus recomputes at ConnectBlock from the parent's m and THIS block's weight.
    // It must therefore be set after the body is final — the coinbase above is the last
    // thing to move it, since GenerateCoinbaseCommitment appends the witness-commitment
    // output. Any later change to vtx invalidates this value (and, because it sits in the
    // pre-pow, also invalidates any cycle already ground against it).
    pblock->nCongestion    = static_cast<uint32_t>(
        NextCongestionMultiplier(pindexPrev->m_congestion, GetBlockWeight(*pblock),
                                 chainparams.GetConsensus()));
    pblock->nNonce         = 0;
    pblocktemplate->vTxSigOpsCost[0] = WITNESS_SCALE_FACTOR * GetLegacySigOpCount(*pblock->vtx[0]);

    BlockValidationState state;
    if (m_options.test_block_validity && !TestBlockValidity(state, chainparams, m_chainstate, *pblock, pindexPrev,
                                                            /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/false)) {
        throw std::runtime_error(strprintf("%s: TestBlockValidity failed: %s", __func__, state.ToString()));
    }
    const auto time_2{SteadyClock::now()};

    LogDebug(HgLog::BENCH, "CreateNewBlock() packages: %.2fms (%d packages, %d updated descendants), validity: %.2fms (total %.2fms)\n",
             Ticks<MillisecondsDouble>(time_1 - time_start), nPackagesSelected, nDescendantsUpdated,
             Ticks<MillisecondsDouble>(time_2 - time_1),
             Ticks<MillisecondsDouble>(time_2 - time_start));

    return std::move(pblocktemplate);
}

void BlockAssembler::onlyUnconfirmed(CTxRelayPool::setEntries& testSet)
{
    for (CTxRelayPool::setEntries::iterator iit = testSet.begin(); iit != testSet.end(); ) {
        // Only test txs not already in the block
        if (inBlock.count((*iit)->GetSharedTx()->GetHash())) {
            testSet.erase(iit++);
        } else {
            iit++;
        }
    }
}

bool BlockAssembler::TestPackage(uint64_t packageSize, int64_t packageSigOpsCost) const
{
    // TODO: switch to weight-based accounting for packages instead of vsize-based accounting.
    if (nBlockWeight + WITNESS_SCALE_FACTOR * packageSize >= m_options.nBlockMaxWeight) {
        return false;
    }
    if (nBlockSigOpsCost + packageSigOpsCost >= MAX_BLOCK_SIGOPS_COST) {
        return false;
    }
    return true;
}

// Perform transaction-level checks before adding to block:
// - transaction finality (locktime)
bool BlockAssembler::TestPackageTransactions(const CTxRelayPool::setEntries& package) const
{
    for (CTxRelayPool::txiter it : package) {
        if (!IsFinalTx(it->GetTx(), nHeight, m_lock_time_cutoff)) {
            return false;
        }
    }
    return true;
}

bool BlockAssembler::TestPackageAnchors(const CTxRelayPool::setEntries& package) const
{
    const CBlockIndex* tip{m_chainstate.m_chain.Tip()};
    const Consensus::Params& params{chainparams.GetConsensus()};
    for (CTxRelayPool::txiter it : package) {
        if (CheckTxAnchor(it->GetTx(), tip, params) == nullptr) return false;
    }
    return true;
}

void BlockAssembler::AddToBlock(CTxRelayPool::txiter iter)
{
    pblocktemplate->block.vtx.emplace_back(iter->GetSharedTx());
    pblocktemplate->vTxSigOpsCost.push_back(iter->GetSigOpCost());
    nBlockWeight += iter->GetTxWeight();
    ++nBlockTx;
    nBlockSigOpsCost += iter->GetSigOpCost();
    inBlock.insert(iter->GetSharedTx()->GetHash());
}

void BlockAssembler::SortForBlock(const CTxRelayPool::setEntries& package, std::vector<CTxRelayPool::txiter>& sortedEntries)
{
    // Sort package by ancestor count
    // If a transaction A depends on transaction B, then A's ancestor count
    // must be greater than B's.  So this is sufficient to validly order the
    // transactions for block inclusion.
    sortedEntries.clear();
    sortedEntries.insert(sortedEntries.begin(), package.begin(), package.end());
    std::sort(sortedEntries.begin(), sortedEntries.end(), CompareTxIterByAncestorCount());
}

// #5c-2 transaction selection: order the relaypool by per-tx surplus-work rate
// (surplus/vsize) — the policy inclusion ranking. This is PER-TX individual
// scoring (no package re-ranking by a child's surplus): we rank each tx by its own rate, but a
// selected tx still pulls in its not-yet-included in-relaypool ancestors so the
// block stays topologically valid (a child can never precede its parent).
void BlockAssembler::addPackageTxs(int& nPackagesSelected, int& nDescendantsUpdated)
{
    const auto& relaypool{*Assert(m_relaypool)};
    LOCK(relaypool.cs);

    // Limit the number of attempts to add transactions to the block when it is
    // close to full; a simple heuristic to finish quickly on a large relaypool.
    const int64_t MAX_CONSECUTIVE_FAILURES = 1000;
    int64_t nConsecutiveFailed = 0;

    // Walk highest surplus-work rate first. We never remove from the relaypool while
    // assembling (txs stay in mapTx), so the index is not invalidated as we go.
    for (auto mi = relaypool.mapTx.get<txwork_rate>().begin();
         mi != relaypool.mapTx.get<txwork_rate>().end(); ++mi) {
        CTxRelayPool::txiter iter = relaypool.mapTx.project<0>(mi);

        // Skip txs already pulled into the block as some earlier tx's ancestor.
        if (inBlock.count(iter->GetSharedTx()->GetHash())) continue;

        // Gather this tx's in-relaypool ancestors (for topological correctness),
        // dropping any already in the block.
        auto ancestors{relaypool.AssumeCalculateRelayPoolAncestors(__func__, *iter, CTxRelayPool::Limits::NoLimits(), /*fSearchForParents=*/false)};
        onlyUnconfirmed(ancestors);
        ancestors.insert(iter);

        uint64_t packageSize = 0;
        int64_t packageSigOpsCost = 0;
        for (CTxRelayPool::txiter anc : ancestors) {
            packageSize += anc->GetTxSize();
            packageSigOpsCost += anc->GetSigOpCost();
        }

        if (!TestPackage(packageSize, packageSigOpsCost)) {
            ++nConsecutiveFailed;
            if (nConsecutiveFailed > MAX_CONSECUTIVE_FAILURES && nBlockWeight >
                    m_options.nBlockMaxWeight - m_options.block_reserved_weight) {
                // Give up if we're close to full and haven't succeeded in a while.
                break;
            }
            continue;
        }

        // Test if all tx's are final.
        if (!TestPackageTransactions(ancestors)) continue;

        // Quicksilver #7: skip any package whose per-tx anchor has gone stale at this
        // height; ConnectBlock would otherwise reject the block (bad-txns-pow-anchor).
        if (!TestPackageAnchors(ancestors)) continue;

        // This transaction will make it in; reset the failed counter.
        nConsecutiveFailed = 0;

        // Sort the ancestor set into a valid (parent-before-child) block order.
        std::vector<CTxRelayPool::txiter> sortedEntries;
        SortForBlock(ancestors, sortedEntries);
        for (CTxRelayPool::txiter e : sortedEntries) AddToBlock(e);

        ++nPackagesSelected;
    }

    // No package re-ranking under per-tx scoring.
    nDescendantsUpdated = 0;
}
} // namespace node
