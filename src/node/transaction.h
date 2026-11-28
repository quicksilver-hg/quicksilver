// Copyright (c) 2017-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_NODE_TRANSACTION_H
#define QUICKSILVER_NODE_TRANSACTION_H

#include <common/messages.h>
#include <node/types.h>
#include <primitives/transaction.h>

#include <string>

class CBlockIndex;
class CTxRelayPool;

namespace node {
class BlockManager;
struct NodeContext;

/** Default burn limit for sendrawtransaction and submitpackage.
 * Both RPCs accept a maxburnamount override.
 */
static const CAmount DEFAULT_MAX_BURN_AMOUNT{0};

/**
 * Submit a transaction to the relaypool and (optionally) relay it to all P2P peers.
 *
 * RelayPool submission can be synchronous (will await relaypool entry notification
 * over the CValidationInterface) or asynchronous (will submit and not wait for
 * notification), depending on the value of wait_callback. wait_callback MUST
 * NOT be set while cs_main, the relaypool mutex or cs_vault are held to avoid
 * deadlock.
 *
 * @param[in]  node reference to node context
 * @param[in]  tx the transaction to broadcast
 * @param[out] err_string reference to std::string to fill with error string if available
 * @param[in]  relay flag if both relaypool insertion and p2p relay are requested
 * @param[in]  wait_callback wait until callbacks have been processed to avoid stale result due to a sequentially RPC.
 * @returns The submission status.
 */
[[nodiscard]] TransactionError BroadcastTransaction(NodeContext& node, CTransactionRef tx, std::string& err_string, bool relay, bool wait_callback);

/**
 * Return transaction with a given hash.
 * If relaypool is provided and block_index is not provided, check it first for the tx.
 * If -txindex is available, check it next for the tx.
 * Finally, if block_index is provided, check for tx by reading entire block from disk.
 *
 * @param[in]  block_index     The block to read from disk, or nullptr
 * @param[in]  relaypool         If provided, check relaypool for tx
 * @param[in]  hash            The txid
 * @param[out] hashBlock       The block hash, if the tx was found via -txindex or block_index
 * @returns                    The tx if found, otherwise nullptr
 */
CTransactionRef GetTransaction(const CBlockIndex* const block_index, const CTxRelayPool* const relaypool, const uint256& hash, uint256& hashBlock, const BlockManager& blockman);
} // namespace node

#endif // QUICKSILVER_NODE_TRANSACTION_H
