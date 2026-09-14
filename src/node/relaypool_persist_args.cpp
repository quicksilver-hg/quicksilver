// Copyright (c) 2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/relaypool_persist_args.h>

#include <common/args.h>
#include <util/fs.h>
#include <validation.h>

namespace node {

bool ShouldPersistRelayPool(const ArgsManager& argsman)
{
    return argsman.GetBoolArg("-persistrelaypool", DEFAULT_PERSIST_RELAYPOOL);
}

fs::path RelayPoolPath(const ArgsManager& argsman)
{
    return argsman.GetDataDirNet() / "relaypool.dat";
}

} // namespace node
