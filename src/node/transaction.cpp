// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/validation.h>
#include <index/txindex.h>
#include <net.h>
#include <net_processing.h>
#include <node/blockstorage.h>
#include <node/context.h>
#include <node/types.h>
#include <txrelaypool.h>
#include <validation.h>
#include <validationinterface.h>
#include <node/transaction.h>

#include <future>

namespace node {
static TransactionError HandleATMPError(const TxValidationState& state, std::string& err_string_out)
{
    err_string_out = state.ToString();
    if (state.IsInvalid()) {
        if (state.GetResult() == TxValidationResult::TX_MISSING_INPUTS) {
            return TransactionError::MISSING_INPUTS;
        }
        // Separate "never acceptable" from "not acceptable yet". Only consensus
        // invalidity is a property of the bytes themselves; every other rejection
        // is relative to this node's policy or the current chain state, and the
        // same transaction may well be accepted on a later attempt.
        if (state.GetResult() == TxValidationResult::TX_CONSENSUS) {
            return TransactionError::CONSENSUS_INVALID;
        }
        return TransactionError::RELAYPOOL_REJECTED;
    } else {
        return TransactionError::RELAYPOOL_ERROR;
    }
}

TransactionError BroadcastTransaction(NodeContext& node, const CTransactionRef tx, std::string& err_string, bool relay, bool wait_callback)
{
    // BroadcastTransaction can be called by RPC or by the vault.
    // chainman, relaypool and peerman are initialized before the RPC server and vault are started
    // and reset after the RPC sever and vault are stopped.
    assert(node.chainman);
    assert(node.relaypool);
    assert(node.peerman);

    std::promise<void> promise;
    Txid txid = tx->GetHash();
    uint256 wtxid = tx->GetWitnessHash();
    bool callback_set = false;

    {
        LOCK(cs_main);

        // If the transaction is already confirmed in the chain, don't do anything
        // and return early.
        CCoinsViewCache &view = node.chainman->ActiveChainstate().CoinsTip();
        for (size_t o = 0; o < tx->vout.size(); o++) {
            const Coin& existingCoin = view.AccessCoin(COutPoint(txid, o));
            // IsSpent doesn't mean the coin is spent, it means the output doesn't exist.
            // So if the output does exist, then this transaction exists in the chain.
            if (!existingCoin.IsSpent()) return TransactionError::ALREADY_IN_UTXO_SET;
        }

        if (auto relaypool_tx = node.relaypool->get(txid); relaypool_tx) {
            // There's already a transaction in the relaypool with this txid. Don't
            // try to submit this transaction to the relaypool (since it'll be
            // rejected as a TX_CONFLICT), but do attempt to reannounce the relaypool
            // transaction if relay=true.
            //
            // The relaypool transaction may have the same or different witness (and
            // wtxid) as this transaction. Use the relaypool's wtxid for reannouncement.
            wtxid = relaypool_tx->GetWitnessHash();
        } else {
            // Transaction is not already in the relaypool.
            // Try to submit the transaction to the relaypool.
            const RelayPoolAcceptResult result = node.chainman->ProcessTransaction(tx, /*test_accept=*/ false);
            if (result.m_result_type != RelayPoolAcceptResult::ResultType::VALID) {
                return HandleATMPError(result.m_state, err_string);
            }

            // Transaction was accepted to the relaypool.

            if (relay) {
                // the relaypool tracks locally submitted transactions to make a
                // best-effort of initial broadcast
                node.relaypool->AddUnbroadcastTx(txid);
            }

            if (wait_callback && node.validation_signals) {
                // For transactions broadcast from outside the vault, make sure
                // that the vault has been notified of the transaction before
                // continuing.
                //
                // This prevents a race where a user might call sendrawtransaction
                // with a transaction to/from their vault, immediately call some
                // vault RPC, and get a stale result because callbacks have not
                // yet been processed.
                node.validation_signals->CallFunctionInValidationInterfaceQueue([&promise] {
                    promise.set_value();
                });
                callback_set = true;
            }
        }
    } // cs_main

    if (callback_set) {
        // Wait until Validation Interface clients have been notified of the
        // transaction entering the relaypool.
        promise.get_future().wait();
    }

    if (relay) {
        node.peerman->RelayTransaction(txid, wtxid);
    }

    return TransactionError::OK;
}

CTransactionRef GetTransaction(const CBlockIndex* const block_index, const CTxRelayPool* const relaypool, const uint256& hash, uint256& hashBlock, const BlockManager& blockman)
{
    if (relaypool && !block_index) {
        CTransactionRef ptx = relaypool->get(hash);
        if (ptx) return ptx;
    }
    if (g_txindex) {
        CTransactionRef tx;
        uint256 block_hash;
        if (g_txindex->FindTx(hash, block_hash, tx)) {
            if (!block_index || block_index->GetBlockHash() == block_hash) {
                // Don't return the transaction if the provided block hash doesn't match.
                // The case where a transaction appears in multiple blocks (e.g. reorgs or
                // a duplicate transaction id) is handled by the block lookup below.
                hashBlock = block_hash;
                return tx;
            }
        }
    }
    if (block_index) {
        CBlock block;
        if (blockman.ReadBlock(block, *block_index)) {
            for (const auto& tx : block.vtx) {
                if (tx->GetHash() == hash) {
                    hashBlock = block_index->GetBlockHash();
                    return tx;
                }
            }
        }
    }
    return nullptr;
}
} // namespace node
