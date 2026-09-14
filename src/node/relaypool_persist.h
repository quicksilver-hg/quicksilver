// Copyright (c) 2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_NODE_RELAYPOOL_PERSIST_H
#define QUICKSILVER_NODE_RELAYPOOL_PERSIST_H

#include <util/fs.h>

class Chainstate;
class CTxRelayPool;

namespace node {

/** Dump the relaypool to a file. */
bool DumpRelayPool(const CTxRelayPool& pool, const fs::path& dump_path,
                 fsbridge::FopenFn mockable_fopen_function = fsbridge::fopen,
                 bool skip_file_commit = false);

struct ImportRelayPoolOptions {
    fsbridge::FopenFn mockable_fopen_function{fsbridge::fopen};
    bool use_current_time{false};
    bool apply_unbroadcast_set{true};
};
/** Import the file and attempt to add its contents to the relaypool. */
bool LoadRelayPool(CTxRelayPool& pool, const fs::path& load_path,
                 Chainstate& active_chainstate,
                 ImportRelayPoolOptions&& opts);

} // namespace node


#endif // QUICKSILVER_NODE_RELAYPOOL_PERSIST_H
