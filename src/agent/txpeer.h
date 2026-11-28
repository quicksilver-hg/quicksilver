// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef QUICKSILVER_AGENT_TXPEER_H
#define QUICKSILVER_AGENT_TXPEER_H

#include <agent/txmessages.h>
#include <net.h>
#include <primitives/transaction.h>
#include <protocol.h>

#include <chrono>
#include <cstddef>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace agent {

//! A getdata the peer has neither answered nor refused (notfound) is dropped after
//! this long, so the transaction can be requested again. Matches the node's
//! GETDATA_TX_INTERVAL.
inline constexpr std::chrono::seconds TX_REQUEST_EXPIRY{60};

enum class TxPeerResultCode {
    INV_SENT,
    GETDATA_SENT,
    TX_SENT,
    TX_NOT_FOUND,
    INV_ALREADY_KNOWN,
    INV_NO_TRANSACTIONS,
    TX_RECEIVED,
    NOTFOUND_RECEIVED,
    IGNORED_MESSAGE,
    DECODE_FAILED,
};

struct TxPeerResult {
    TxPeerResultCode code;
    std::optional<TxInventoryMessageDecodeResult> inventory_message;
    std::optional<TxMessageDecodeResult> tx_message;
    std::vector<CInv> announced_inventory;
    std::vector<CInv> requested_inventory;
    std::vector<CInv> served_inventory;
    std::vector<CInv> matched_requests;
    //! inv: entries already known or already requested. getdata: repeats of an
    //! entry already served in the same message.
    size_t duplicate_count{0};
    //! getdata: entries the agent does not hold; they are answered with notfound.
    size_t missing_count{0};
    //! inv: new entries not requested because TxPeer already has max_tx_inventory
    //! requests pending.
    size_t over_limit_count{0};

    bool ok() const { return code != TxPeerResultCode::DECODE_FAILED; }
};

struct TxPeerAction {
    TxPeerResult result;
    // Queued messages are moved into TxPeer's outbound queue.
    bool queued_outbound_message{false};

    bool ok() const { return result.ok(); }
};

const char* TxPeerResultCodeString(TxPeerResultCode code);

/**
 * Transaction relay state for one peer.
 *
 * TxPeer serves only transactions handed to it locally (AnnounceTransaction,
 * SendTransaction). A transaction received from the peer is reported to the caller
 * and remembered as known so it is not requested again, but it is never stored for
 * getdata and never relayed: a getdata for it is answered with notfound.
 *
 * Bounded state: at most max_tx_inventory requests are pending, each expires after
 * TX_REQUEST_EXPIRY or when the peer sends notfound for it, and the known set keeps
 * the most recent 3 * max_tx_inventory entries (each transaction is known under
 * three inventory forms). The local relay set grows only with transactions the
 * caller hands it.
 */
class TxPeer
{
public:
    explicit TxPeer(size_t max_tx_inventory = DEFAULT_MAX_TX_INVENTORY);

    TxPeerAction AnnounceTransaction(const CTransaction& transaction, bool prefer_wtxid = true);
    TxPeerAction SendTransaction(const CTransaction& transaction);
    TxPeerAction ProcessMessage(const CSerializedNetMsg& message);

    bool HasOutboundMessages() const { return !m_outbound_messages.empty(); }
    size_t OutboundMessageCount() const { return m_outbound_messages.size(); }
    std::optional<CSerializedNetMsg> PopOutboundMessage();
    std::vector<CSerializedNetMsg> DrainOutboundMessages();

    size_t PendingRequestCount() const { return m_requested_inventory.size(); }
    size_t KnownInventoryCount() const { return m_known_inventory.size(); }
    size_t MaxTxInventory() const { return m_max_tx_inventory; }

private:
    TxPeerResult Result(TxPeerResultCode code,
                        std::optional<TxInventoryMessageDecodeResult> inventory_message = std::nullopt,
                        std::optional<TxMessageDecodeResult> tx_message = std::nullopt,
                        std::vector<CInv> announced_inventory = {},
                        std::vector<CInv> requested_inventory = {},
                        std::vector<CInv> served_inventory = {},
                        std::vector<CInv> matched_requests = {},
                        size_t duplicate_count = 0,
                        size_t missing_count = 0,
                        size_t over_limit_count = 0) const;

    TxPeerAction QueueResult(TxPeerResult result, std::optional<CSerializedNetMsg> outbound_message = std::nullopt);
    std::vector<CInv> MarkTransactionKnown(const CTransaction& transaction);
    void MarkInventoryKnown(const CInv& inv);
    void ExpireRequests();
    void StoreRelayTransaction(CTransactionRef transaction);

    size_t m_max_tx_inventory;
    //! Pending getdata entries and when each was requested.
    std::map<CInv, std::chrono::seconds> m_requested_inventory;
    std::set<CInv> m_known_inventory;
    //! m_known_inventory in insertion order, oldest first, for eviction.
    std::deque<CInv> m_known_order;
    std::map<CInv, CTransactionRef> m_relay_transactions;
    std::deque<CSerializedNetMsg> m_outbound_messages;
};

} // namespace agent

#endif // QUICKSILVER_AGENT_TXPEER_H
