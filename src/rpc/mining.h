// Copyright (c) 2020 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_RPC_MINING_H
#define QUICKSILVER_RPC_MINING_H

#include <arith_uint256.h>
#include <consensus/params.h>

class UniValue;

/** Default max iterations to try in RPC generatetodescriptor, generatetoaddress, and generateblock. */
static const uint64_t DEFAULT_MAX_TRIES{1000000};

/** Build the BIP22-extension "pow" object describing the Cuckatoo block PoW for
 *  an external miner: algorithm, edgebits, proofsize, proofhash, the proof-hash
 *  target, the 84-byte pre-pow size, the cycle encoding, the congestion recurrence
 *  parameters, and (sandbox only) a trivialcycle flag. Pure function of the
 *  consensus params and the target. */
UniValue BlockPowDescriptor(const Consensus::Params& params, const arith_uint256& hashTarget);

#endif // QUICKSILVER_RPC_MINING_H
