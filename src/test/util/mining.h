// Copyright (c) 2019-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_TEST_UTIL_MINING_H
#define QUICKSILVER_TEST_UTIL_MINING_H

#include <node/miner.h>

#include <memory>
#include <string>
#include <vector>

class CBlock;
class CBlockHeader;
class COutPoint;
class CScript;
namespace Consensus {
struct Params;
} // namespace Consensus
namespace node {
struct NodeContext;
} // namespace node

/** Quicksilver: solve the Cuckatoo block PoW for `header` (sets nCycle, grinding
 *  nNonce as needed) so it satisfies CheckProofOfWork. Replaces the old SHA256d
 *  nonce-grind in tests. Fast at sandbox EDGEBITS-19. */
void SolveBlockPoW(CBlockHeader& header, const Consensus::Params& params);

/** Returns the generated coin */
COutPoint MineBlock(const node::NodeContext&,
                    const node::BlockAssembler::Options& assembler_options);

/**
 * Returns the generated coin (or Null if the block was invalid).
 * It is recommended to call RegenerateCommitments before mining the block to avoid merkle tree mismatches.
 **/
COutPoint MineBlock(const node::NodeContext&, std::shared_ptr<CBlock>& block);

/** Prepare a block to be mined */
std::shared_ptr<CBlock> PrepareBlock(const node::NodeContext&);
std::shared_ptr<CBlock> PrepareBlock(const node::NodeContext& node,
                                     const node::BlockAssembler::Options& assembler_options);

/** RPC-like helper function, returns the generated coin */
COutPoint generatetoaddress(const node::NodeContext&, const std::string& address);

#endif // QUICKSILVER_TEST_UTIL_MINING_H
