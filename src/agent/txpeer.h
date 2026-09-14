// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef QUICKSILVER_AGENT_TXPEER_H
#define QUICKSILVER_AGENT_TXPEER_H

#include <agent/txmessages.h>
#include <net.h>
#include <primitives/transaction.h>
#include <protocol.h>

#include <cstddef>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace agent {

enum class TxPeerResultCode {
    INV_SENT,
    GETDATA_SENT,
    TX_SENT,
    TX_NOT_FOUND,
    INV_ALREADY_KNOWN,
    TX_RECEIVED,
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
    size_t duplicate_count{0};
    size_t missing_count{0};

    bool ok() const { return code != TxPeerResultCode::DECODE_FAILED; }
};

struct TxPeerAction {
    TxPeerResult result;
    // Queued messages are moved into TxPeer's outbound queue.
    bool queued_outbound_message{false};

    bool ok() const { return result.ok(); }
};

const char* TxPeerResultCodeString(TxPeerResultCode code);

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
                        size_t missing_count = 0) const;

    TxPeerAction QueueResult(TxPeerResult result, std::optional<CSerializedNetMsg> outbound_message = std::nullopt);
    std::vector<CInv> MarkTransactionKnown(const CTransaction& transaction);
    void StoreRelayTransaction(CTransactionRef transaction);

    size_t m_max_tx_inventory;
    std::set<CInv> m_requested_inventory;
    std::set<CInv> m_known_inventory;
    std::map<CInv, CTransactionRef> m_relay_transactions;
    std::deque<CSerializedNetMsg> m_outbound_messages;
};

} // namespace agent

#endif // QUICKSILVER_AGENT_TXPEER_H
