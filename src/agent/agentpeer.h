// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef QUICKSILVER_AGENT_AGENTPEER_H
#define QUICKSILVER_AGENT_AGENTPEER_H

#include <agent/headerchain.h>
#include <agent/headerpeer.h>
#include <agent/headersync.h>
#include <agent/txmessages.h>
#include <agent/txpeer.h>
#include <net.h>
#include <uint256.h>

#include <cstddef>
#include <deque>
#include <optional>
#include <vector>

namespace agent {

enum class AgentPeerActionCode {
    HEADERS_STARTED,
    TRANSACTION_ANNOUNCED,
    TRANSACTION_SENT,
    HEADER_MESSAGE,
    TX_MESSAGE,
    IGNORED_MESSAGE,
};

struct AgentPeerAction {
    AgentPeerActionCode code;
    std::optional<HeaderSyncPeerAction> header_action;
    std::optional<TxPeerAction> tx_action;
    size_t queued_outbound_count{0};

    bool ok() const;
};

const char* AgentPeerActionCodeString(AgentPeerActionCode code);

class AgentPeer
{
public:
    explicit AgentPeer(HeaderChain& chain,
                       uint256 stop_hash = uint256{},
                       size_t max_headers_result = DEFAULT_MAX_HEADERS_RESULTS,
                       size_t max_tx_inventory = DEFAULT_MAX_TX_INVENTORY);

    AgentPeerAction StartHeaders();
    AgentPeerAction StartHeaders(const uint256& stop_hash);
    AgentPeerAction AnnounceTransaction(const CTransaction& transaction, bool prefer_wtxid = true);
    AgentPeerAction SendTransaction(const CTransaction& transaction);
    AgentPeerAction ProcessMessage(const CSerializedNetMsg& message);

    bool HasOutboundMessages() const { return !m_outbound_messages.empty(); }
    size_t OutboundMessageCount() const { return m_outbound_messages.size(); }
    std::optional<CSerializedNetMsg> PopOutboundMessage();
    std::vector<CSerializedNetMsg> DrainOutboundMessages();

    bool WaitingForHeaders() const { return m_header_peer.WaitingForHeaders(); }
    bool HeaderPeerSynced() const { return m_header_peer.PeerSynced(); }
    const uint256& HeaderStopHash() const { return m_header_peer.StopHash(); }

    size_t PendingTxRequestCount() const { return m_tx_peer.PendingRequestCount(); }
    size_t KnownTxInventoryCount() const { return m_tx_peer.KnownInventoryCount(); }
    size_t MaxTxInventory() const { return m_tx_peer.MaxTxInventory(); }

private:
    AgentPeerAction HeaderResult(AgentPeerActionCode code, HeaderSyncPeerAction action);
    AgentPeerAction TxResult(AgentPeerActionCode code, TxPeerAction action);
    AgentPeerAction IgnoredResult() const;
    size_t DrainPeerOutbound(std::vector<CSerializedNetMsg> messages);

    HeaderSyncPeer m_header_peer;
    TxPeer m_tx_peer;
    std::deque<CSerializedNetMsg> m_outbound_messages;
};

} // namespace agent

#endif // QUICKSILVER_AGENT_AGENTPEER_H
