// Copyright (c) 2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_NODE_RELAYPOOL_ARGS_H
#define QUICKSILVER_NODE_RELAYPOOL_ARGS_H

#include <util/result.h>

class ArgsManager;
class CChainParams;
struct bilingual_str;
namespace kernel {
struct RelayPoolOptions;
};

/**
 * Overlay the options set in \p argsman on top of corresponding members in \p relaypool_opts.
 * Returns an error if one was encountered.
 *
 * @param[in]  argsman The ArgsManager in which to check set options.
 * @param[in,out] relaypool_opts The RelayPoolOptions to modify according to \p argsman.
 */
[[nodiscard]] util::Result<void> ApplyArgsManOptions(const ArgsManager& argsman, const CChainParams& chainparams, kernel::RelayPoolOptions& relaypool_opts);


#endif // QUICKSILVER_NODE_RELAYPOOL_ARGS_H
