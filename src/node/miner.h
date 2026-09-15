// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_NODE_MINER_H
#define QUICKSILVER_NODE_MINER_H

#include <node/types.h>
#include <policy/policy.h>
#include <primitives/block.h>
#include <txrelaypool.h>

#include <memory>
#include <optional>
#include <stdint.h>

#include <boost/multi_index/identity.hpp>
#include <boost/multi_index/indexed_by.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/tag.hpp>
#include <boost/multi_index_container.hpp>

class ArgsManager;
class CBlockIndex;
class CChainParams;
class CScript;
class Chainstate;
class ChainstateManager;

namespace Consensus { struct Params; };

namespace node {
struct CBlockTemplate
{
    CBlock block;
    std::vector<int64_t> vTxSigOpsCost;
    std::vector<unsigned char> vchCoinbaseCommitment;
};

// A comparator that sorts transactions based on number of ancestors.
// This is sufficient to sort an ancestor package in an order that is valid
// to appear in a block; surplus-work ranking only needs this topological sort.
struct CompareTxIterByAncestorCount {
    bool operator()(const CTxRelayPool::txiter& a, const CTxRelayPool::txiter& b) const
    {
        if (a->GetCountWithAncestors() != b->GetCountWithAncestors()) {
            return a->GetCountWithAncestors() < b->GetCountWithAncestors();
        }
        return CompareIteratorByHash()(a, b);
    }
};

/** Generate a new block, without valid proof-of-work */
class BlockAssembler
{
private:
    // The constructed block template
    std::unique_ptr<CBlockTemplate> pblocktemplate;

    // Information on the current status of the block
    uint64_t nBlockWeight;
    uint64_t nBlockTx;
    uint64_t nBlockSigOpsCost;
    std::unordered_set<Txid, SaltedTxidHasher> inBlock;

    // Chain context for the block
    int nHeight;
    int64_t m_lock_time_cutoff;

    const CChainParams& chainparams;
    const CTxRelayPool* const m_relaypool;
    Chainstate& m_chainstate;

public:
    struct Options : BlockCreateOptions {
        // Configuration parameters for the block size
        size_t nBlockMaxWeight{DEFAULT_BLOCK_MAX_WEIGHT};
        // Whether to call TestBlockValidity() at the end of CreateNewBlock().
        bool test_block_validity{true};
    };

    explicit BlockAssembler(Chainstate& chainstate, const CTxRelayPool* relaypool, const Options& options);

    /** Construct a new block template */
    std::unique_ptr<CBlockTemplate> CreateNewBlock();

    /** The number of transactions in the last assembled block (excluding coinbase transaction) */
    inline static std::optional<int64_t> m_last_block_num_txs{};
    /** The weight of the last assembled block (including reserved weight for block header, txs count and coinbase tx) */
    inline static std::optional<int64_t> m_last_block_weight{};

private:
    const Options m_options;

    // utility functions
    /** Clear the block's state and prepare for assembling a new block */
    void resetBlock();
    /** Add a tx to the block */
    void AddToBlock(CTxRelayPool::txiter iter);

    // Methods for how to add transactions to a block.
    /** Add transactions by surplus-work ranking, including unconfirmed ancestors
      * Increments nPackagesSelected / nDescendantsUpdated with corresponding
      * statistics from the package selection (for logging statistics).
      *
      * @pre BlockAssembler::m_relaypool must not be nullptr
    */
    void addPackageTxs(int& nPackagesSelected, int& nDescendantsUpdated)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_relaypool->cs);

    // helper functions for addPackageTxs()
    /** Remove confirmed (inBlock) entries from given set */
    void onlyUnconfirmed(CTxRelayPool::setEntries& testSet);
    /** Test if a new package would "fit" in the block */
    bool TestPackage(uint64_t packageSize, int64_t packageSigOpsCost) const;
    /** Perform checks on each transaction in a package:
      * locktime, premature-witness, serialized size (if necessary)
      * These checks should always succeed, and they're here
      * only as an extra check in case of suboptimal node configuration */
    bool TestPackageTransactions(const CTxRelayPool::setEntries& package) const;
    /** Return false if any tx in the package has a stale per-tx anchor at the candidate
     *  block height (parent = active tip), i.e. CheckTxAnchor(tx, tip, params) == nullptr.
     *  Keeps the assembler from building a block ConnectBlock would reject (bad-txns-pow-anchor). */
    bool TestPackageAnchors(const CTxRelayPool::setEntries& package) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    /** Sort the package in an order that is valid to appear in a block */
    void SortForBlock(const CTxRelayPool::setEntries& package, std::vector<CTxRelayPool::txiter>& sortedEntries);
};

/**
 * Get the minimum time a miner should use in the next block: one second past
 * the previous block's median time past, which is exactly the consensus limit.
 */
int64_t GetMinimumTime(const CBlockIndex* pindexPrev, const int64_t difficulty_adjustment_interval);

int64_t UpdateTime(CBlockHeader* pblock, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev);

/** Re-derive the header fields that depend on the block body after the block txs have
 *  changed: the witness commitment and merkle root, and the Quicksilver congestion
 *  multiplier (nCongestion), which folds in the block's own weight. Grind the PoW after
 *  calling this — nCongestion is inside the pre-pow, so it rebinds the cycle. */
void RegenerateCommitments(CBlock& block, ChainstateManager& chainman);

/** Apply -blockmaxweight and related options from ArgsManager to BlockAssembler options. */
void ApplyArgsManOptions(const ArgsManager& gArgs, BlockAssembler::Options& options);
} // namespace node

#endif // QUICKSILVER_NODE_MINER_H
