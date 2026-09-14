// Copyright (c) 2024 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_INTERFACES_MINING_H
#define QUICKSILVER_INTERFACES_MINING_H

#include <interfaces/types.h>       // for BlockRef
#include <node/types.h>             // for BlockCreateOptions
#include <primitives/block.h>       // for CBlock, CBlockHeader
#include <primitives/transaction.h> // for CTransactionRef
#include <stdint.h>                 // for int64_t
#include <uint256.h>                // for uint256
#include <util/time.h>              // for MillisecondsDouble

#include <array>    // for std::array (Cuckatoo 42-cycle)
#include <memory>   // for unique_ptr, shared_ptr
#include <optional> // for optional
#include <vector>   // for vector

namespace node {
struct NodeContext;
} // namespace node

class BlockValidationState;
class CScript;

namespace interfaces {

//! Block template interface
class BlockTemplate
{
public:
    virtual ~BlockTemplate() = default;

    virtual CBlock getBlock() = 0;

    virtual std::vector<int64_t> getTxSigops() = 0;

    virtual std::vector<unsigned char> getCoinbaseCommitment() = 0;

    /**
     * Construct and broadcast the block.
     *
     * Quicksilver: the block PoW is Cuckatoo, so a solution carries the winning
     * 42-cycle (`cycle`) alongside the grind nonce — `nonce` alone cannot satisfy
     * `CheckProofOfWork`. An external miner submits both.
     *
     * Quicksilver (#5c-1 Phase 2): `congestion` is likewise submitted rather than
     * carried over from the template. It is a header field inside the pre-pow, and
     * consensus recomputes it from the FINAL block weight — which the miner changes
     * here by supplying its own `coinbase`. Reusing the template's value would be
     * wrong whenever that coinbase differs in size, and recomputing it on this side
     * would invalidate the cycle the miner already ground. The miner owns the value.
     *
     * @returns if the block was processed, independent of block validity
     */
    virtual bool submitSolution(uint32_t version, uint32_t timestamp, uint32_t congestion, uint32_t nonce, const std::array<uint32_t, 42>& cycle, CTransactionRef coinbase) = 0;
};

//! Interface giving clients (RPC, Stratum v2 Template Provider in the future)
//! ability to create block templates.
class Mining
{
public:
    virtual ~Mining() = default;

    //! If this chain is exclusively used for testing
    virtual bool isTestChain() = 0;

    //! Returns whether IBD is still in progress.
    virtual bool isInitialBlockDownload() = 0;

    //! Returns the hash and height for the tip of this chain
    virtual std::optional<BlockRef> getTip() = 0;

    //! Monotonic relay-pool update counter. Reading it does not take the pool
    //! lock, so miners can use it from a solver cancellation predicate.
    virtual unsigned int getTransactionsUpdated() = 0;

    /**
     * Waits for the connected tip to change. During node initialization, this will
     * wait until the tip is connected.
     *
     * @param[in] current_tip block hash of the current chain tip. Function waits
     *                        for the chain tip to differ from this.
     * @param[in] timeout     how long to wait for a new tip
     * @returns               Hash and height of the current chain tip after this call.
     */
    virtual BlockRef waitTipChanged(uint256 current_tip, MillisecondsDouble timeout = MillisecondsDouble::max()) = 0;

   /**
     * Construct a new block template
     *
     * @param[in] options options for creating the block
     * @returns a block template
     */
    virtual std::unique_ptr<BlockTemplate> createNewBlock(const node::BlockCreateOptions& options = {}) = 0;

    //! Get internal node context. Useful for RPC and testing,
    //! but not accessible across processes.
    virtual node::NodeContext* context() { return nullptr; }
};

//! Return implementation of Mining interface.
std::unique_ptr<Mining> MakeMining(node::NodeContext& node);

} // namespace interfaces

#endif // QUICKSILVER_INTERFACES_MINING_H
