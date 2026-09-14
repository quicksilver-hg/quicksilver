// Copyright (c) 2017-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_RPC_BLOCKCHAIN_H
#define QUICKSILVER_RPC_BLOCKCHAIN_H

#include <consensus/amount.h>
#include <core_io.h>
#include <sync.h>
#include <validation.h>

#include <any>
#include <stdint.h>

class CBlock;
class CBlockIndex;
class UniValue;
namespace node {
class BlockManager;
} // namespace node

/**
 * Get the difficulty of the net wrt to the given block index.
 *
 * @return A floating point number that is a multiple of the main net minimum
 * difficulty (4295032833 hashes).
 */
double GetDifficulty(const CBlockIndex& blockindex);

/** Block description to JSON */
UniValue blockToJSON(node::BlockManager& blockman, const CBlock& block, const CBlockIndex& tip, const CBlockIndex& blockindex, TxVerbosity verbosity, const uint256 pow_limit) LOCKS_EXCLUDED(cs_main);

/** Block header to JSON */
UniValue blockheaderToJSON(const CBlockIndex& tip, const CBlockIndex& blockindex, const uint256 pow_limit) LOCKS_EXCLUDED(cs_main);

//! Return height of highest block that has been pruned, or std::nullopt if no blocks have been pruned
std::optional<int> GetPruneHeight(const node::BlockManager& blockman, const CChain& chain) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
void CheckBlockDataAvailability(node::BlockManager& blockman, const CBlockIndex& blockindex, bool check_for_undo) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

#endif // QUICKSILVER_RPC_BLOCKCHAIN_H
