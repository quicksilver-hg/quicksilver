// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_NODE_PRUNE_ARGS_H
#define QUICKSILVER_NODE_PRUNE_ARGS_H

#include <cstdint>

class CChainParams;

namespace node {
uint64_t PruneTargetFloor(const CChainParams& chainparams);
} // namespace node

#endif // QUICKSILVER_NODE_PRUNE_ARGS_H
