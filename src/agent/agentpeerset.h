// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef QUICKSILVER_AGENT_AGENTPEERSET_H
#define QUICKSILVER_AGENT_AGENTPEERSET_H

#include <agent/agentpeer.h>
#include <agent/headerchain.h>
#include <agent/headersync.h>
#include <agent/txmessages.h>
#include <net.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace agent {

using AgentPeerId = uint64_t;
inline constexpr AgentPeerId DEFAULT_AGENT_PEER_ID{0};

enum class AgentPeerSetResultCode {
    PEER_ADDED,
    PEER_REMOVED,
    PEER_ALREADY_EXISTS,
    PEER_NOT_FOUND,
    PEER_NOT_REMOVABLE,
    PEER_ACTION,
};

struct AgentPeerSetAction {
    AgentPeerSetResultCode code;
    AgentPeerId peer_id;
    std::optional<AgentPeerAction> peer_action;
    size_t queued_outbound_count{0};

    bool ok() const;
};

struct AgentPeerOutboundMessage {
    AgentPeerId peer_id;
    CSerializedNetMsg message;
};

const char* AgentPeerSetResultCodeString(AgentPeerSetResultCode code);

class AgentPeerSet
{
public:
    explicit AgentPeerSet(HeaderChain& chain,
                          uint256 stop_hash = uint256{},
                          size_t max_headers_result = DEFAULT_MAX_HEADERS_RESULTS,
                          size_t max_tx_inventory = DEFAULT_MAX_TX_INVENTORY);

    AgentPeerSet(const AgentPeerSet&) = delete;
    AgentPeerSet& operator=(const AgentPeerSet&) = delete;
    AgentPeerSet(AgentPeerSet&&) = delete;
    AgentPeerSet& operator=(AgentPeerSet&&) = delete;

    AgentPeerSetAction AddPeer(AgentPeerId peer_id);
    AgentPeerSetAction RemovePeer(AgentPeerId peer_id);

    AgentPeerSetAction StartHeaders(AgentPeerId peer_id);
    AgentPeerSetAction StartHeaders(AgentPeerId peer_id, const uint256& stop_hash);
    AgentPeerSetAction AnnounceTransaction(AgentPeerId peer_id,
                                           const CTransaction& transaction,
                                           bool prefer_wtxid = true);
    AgentPeerSetAction SendTransaction(AgentPeerId peer_id, const CTransaction& transaction);
    AgentPeerSetAction ProcessMessage(AgentPeerId peer_id, const CSerializedNetMsg& message);

    std::vector<AgentPeerSetAction> AnnounceTransactionToAll(const CTransaction& transaction,
                                                             bool prefer_wtxid = true);

    bool HasPeer(AgentPeerId peer_id) const;
    size_t PeerCount() const { return m_peers.size(); }
    std::vector<AgentPeerId> PeerIds() const;

    AgentPeer& GetPeer(AgentPeerId peer_id);
    const AgentPeer& GetPeer(AgentPeerId peer_id) const;

    bool HasOutboundMessages() const;
    bool HasOutboundMessages(AgentPeerId peer_id) const;
    size_t OutboundMessageCount() const;
    size_t OutboundMessageCount(AgentPeerId peer_id) const;
    std::optional<CSerializedNetMsg> PopOutboundMessage(AgentPeerId peer_id);
    std::vector<CSerializedNetMsg> DrainOutboundMessages(AgentPeerId peer_id);
    std::optional<AgentPeerOutboundMessage> PopNextOutboundMessage();
    std::vector<AgentPeerOutboundMessage> DrainOutboundMessages();

    bool WaitingForHeaders(AgentPeerId peer_id) const;
    bool HeaderPeerSynced(AgentPeerId peer_id) const;
    const uint256& HeaderStopHash(AgentPeerId peer_id) const;

    size_t PendingTxRequestCount(AgentPeerId peer_id) const;
    size_t KnownTxInventoryCount(AgentPeerId peer_id) const;
    size_t MaxTxInventory() const { return m_max_tx_inventory; }

private:
    AgentPeerSetAction Result(AgentPeerSetResultCode code, AgentPeerId peer_id) const;
    AgentPeerSetAction PeerAction(AgentPeerId peer_id, AgentPeerAction action) const;
    AgentPeer* FindPeer(AgentPeerId peer_id);
    const AgentPeer* FindPeer(AgentPeerId peer_id) const;

    HeaderChain& m_chain;
    uint256 m_stop_hash;
    size_t m_max_headers_result;
    size_t m_max_tx_inventory;
    std::map<AgentPeerId, std::unique_ptr<AgentPeer>> m_peers;
};

} // namespace agent

#endif // QUICKSILVER_AGENT_AGENTPEERSET_H
