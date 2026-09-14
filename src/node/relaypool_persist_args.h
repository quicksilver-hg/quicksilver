// Copyright (c) 2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_NODE_RELAYPOOL_PERSIST_ARGS_H
#define QUICKSILVER_NODE_RELAYPOOL_PERSIST_ARGS_H

#include <util/fs.h>

class ArgsManager;

namespace node {

/**
 * Default for -persistrelaypool, indicating whether the node should attempt to
 * automatically load the relaypool on start and save to disk on shutdown
 */
static constexpr bool DEFAULT_PERSIST_RELAYPOOL{true};

bool ShouldPersistRelayPool(const ArgsManager& argsman);
fs::path RelayPoolPath(const ArgsManager& argsman);

} // namespace node

#endif // QUICKSILVER_NODE_RELAYPOOL_PERSIST_ARGS_H
