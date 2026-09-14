// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <agent/agentpeerset.h>

#include <cassert>
#include <utility>

namespace agent {

bool AgentPeerSetAction::ok() const
{
    if (code == AgentPeerSetResultCode::PEER_ACTION) {
        return peer_action.has_value() && peer_action->ok();
    }
    return code == AgentPeerSetResultCode::PEER_ADDED || code == AgentPeerSetResultCode::PEER_REMOVED;
}

const char* AgentPeerSetResultCodeString(AgentPeerSetResultCode code)
{
    switch (code) {
    case AgentPeerSetResultCode::PEER_ADDED:
        return "peer-added";
    case AgentPeerSetResultCode::PEER_REMOVED:
        return "peer-removed";
    case AgentPeerSetResultCode::PEER_ALREADY_EXISTS:
        return "peer-already-exists";
    case AgentPeerSetResultCode::PEER_NOT_FOUND:
        return "peer-not-found";
    case AgentPeerSetResultCode::PEER_NOT_REMOVABLE:
        return "peer-not-removable";
    case AgentPeerSetResultCode::PEER_ACTION:
        return "peer-action";
    } // no default case, so the compiler can warn on missing enum values
    assert(false);
    return "unknown";
}

AgentPeerSet::AgentPeerSet(HeaderChain& chain,
                           uint256 stop_hash,
                           size_t max_headers_result,
                           size_t max_tx_inventory)
    : m_chain{chain},
      m_stop_hash{std::move(stop_hash)},
      m_max_headers_result{max_headers_result},
      m_max_tx_inventory{max_tx_inventory}
{
    m_peers.emplace(DEFAULT_AGENT_PEER_ID,
                    std::make_unique<AgentPeer>(m_chain, m_stop_hash, m_max_headers_result, m_max_tx_inventory));
}

AgentPeerSetAction AgentPeerSet::AddPeer(AgentPeerId peer_id)
{
    if (m_peers.contains(peer_id)) {
        return Result(AgentPeerSetResultCode::PEER_ALREADY_EXISTS, peer_id);
    }

    m_peers.emplace(peer_id,
                    std::make_unique<AgentPeer>(m_chain, m_stop_hash, m_max_headers_result, m_max_tx_inventory));
    return Result(AgentPeerSetResultCode::PEER_ADDED, peer_id);
}

AgentPeerSetAction AgentPeerSet::RemovePeer(AgentPeerId peer_id)
{
    if (peer_id == DEFAULT_AGENT_PEER_ID) {
        return Result(AgentPeerSetResultCode::PEER_NOT_REMOVABLE, peer_id);
    }
    if (m_peers.erase(peer_id) == 0) {
        return Result(AgentPeerSetResultCode::PEER_NOT_FOUND, peer_id);
    }
    return Result(AgentPeerSetResultCode::PEER_REMOVED, peer_id);
}

AgentPeerSetAction AgentPeerSet::StartHeaders(AgentPeerId peer_id)
{
    AgentPeer* peer{FindPeer(peer_id)};
    if (peer == nullptr) {
        return Result(AgentPeerSetResultCode::PEER_NOT_FOUND, peer_id);
    }
    return PeerAction(peer_id, peer->StartHeaders());
}

AgentPeerSetAction AgentPeerSet::StartHeaders(AgentPeerId peer_id, const uint256& stop_hash)
{
    AgentPeer* peer{FindPeer(peer_id)};
    if (peer == nullptr) {
        return Result(AgentPeerSetResultCode::PEER_NOT_FOUND, peer_id);
    }
    return PeerAction(peer_id, peer->StartHeaders(stop_hash));
}

AgentPeerSetAction AgentPeerSet::AnnounceTransaction(AgentPeerId peer_id,
                                                     const CTransaction& transaction,
                                                     bool prefer_wtxid)
{
    AgentPeer* peer{FindPeer(peer_id)};
    if (peer == nullptr) {
        return Result(AgentPeerSetResultCode::PEER_NOT_FOUND, peer_id);
    }
    return PeerAction(peer_id, peer->AnnounceTransaction(transaction, prefer_wtxid));
}

AgentPeerSetAction AgentPeerSet::SendTransaction(AgentPeerId peer_id, const CTransaction& transaction)
{
    AgentPeer* peer{FindPeer(peer_id)};
    if (peer == nullptr) {
        return Result(AgentPeerSetResultCode::PEER_NOT_FOUND, peer_id);
    }
    return PeerAction(peer_id, peer->SendTransaction(transaction));
}

AgentPeerSetAction AgentPeerSet::ProcessMessage(AgentPeerId peer_id, const CSerializedNetMsg& message)
{
    AgentPeer* peer{FindPeer(peer_id)};
    if (peer == nullptr) {
        return Result(AgentPeerSetResultCode::PEER_NOT_FOUND, peer_id);
    }
    return PeerAction(peer_id, peer->ProcessMessage(message));
}

std::vector<AgentPeerSetAction> AgentPeerSet::AnnounceTransactionToAll(const CTransaction& transaction,
                                                                       bool prefer_wtxid)
{
    std::vector<AgentPeerSetAction> actions;
    actions.reserve(m_peers.size());
    for (const auto& [peer_id, peer] : m_peers) {
        actions.push_back(PeerAction(peer_id, peer->AnnounceTransaction(transaction, prefer_wtxid)));
    }
    return actions;
}

bool AgentPeerSet::HasPeer(AgentPeerId peer_id) const
{
    return FindPeer(peer_id) != nullptr;
}

std::vector<AgentPeerId> AgentPeerSet::PeerIds() const
{
    std::vector<AgentPeerId> peer_ids;
    peer_ids.reserve(m_peers.size());
    for (const auto& peer_entry : m_peers) {
        peer_ids.push_back(peer_entry.first);
    }
    return peer_ids;
}

AgentPeer& AgentPeerSet::GetPeer(AgentPeerId peer_id)
{
    AgentPeer* peer{FindPeer(peer_id)};
    assert(peer != nullptr);
    return *peer;
}

const AgentPeer& AgentPeerSet::GetPeer(AgentPeerId peer_id) const
{
    const AgentPeer* peer{FindPeer(peer_id)};
    assert(peer != nullptr);
    return *peer;
}

bool AgentPeerSet::HasOutboundMessages() const
{
    for (const auto& peer_entry : m_peers) {
        if (peer_entry.second->HasOutboundMessages()) {
            return true;
        }
    }
    return false;
}

bool AgentPeerSet::HasOutboundMessages(AgentPeerId peer_id) const
{
    const AgentPeer* peer{FindPeer(peer_id)};
    return peer != nullptr && peer->HasOutboundMessages();
}

size_t AgentPeerSet::OutboundMessageCount() const
{
    size_t count{0};
    for (const auto& peer_entry : m_peers) {
        count += peer_entry.second->OutboundMessageCount();
    }
    return count;
}

size_t AgentPeerSet::OutboundMessageCount(AgentPeerId peer_id) const
{
    const AgentPeer* peer{FindPeer(peer_id)};
    return peer == nullptr ? 0 : peer->OutboundMessageCount();
}

std::optional<CSerializedNetMsg> AgentPeerSet::PopOutboundMessage(AgentPeerId peer_id)
{
    AgentPeer* peer{FindPeer(peer_id)};
    if (peer == nullptr) {
        return std::nullopt;
    }
    return peer->PopOutboundMessage();
}

std::vector<CSerializedNetMsg> AgentPeerSet::DrainOutboundMessages(AgentPeerId peer_id)
{
    AgentPeer* peer{FindPeer(peer_id)};
    if (peer == nullptr) {
        return {};
    }
    return peer->DrainOutboundMessages();
}

std::optional<AgentPeerOutboundMessage> AgentPeerSet::PopNextOutboundMessage()
{
    for (auto& [peer_id, peer] : m_peers) {
        std::optional<CSerializedNetMsg> message{peer->PopOutboundMessage()};
        if (message.has_value()) {
            return AgentPeerOutboundMessage{peer_id, std::move(*message)};
        }
    }
    return std::nullopt;
}

std::vector<AgentPeerOutboundMessage> AgentPeerSet::DrainOutboundMessages()
{
    std::vector<AgentPeerOutboundMessage> messages;
    messages.reserve(OutboundMessageCount());
    while (std::optional<AgentPeerOutboundMessage> message{PopNextOutboundMessage()}) {
        messages.push_back(std::move(*message));
    }
    return messages;
}

bool AgentPeerSet::WaitingForHeaders(AgentPeerId peer_id) const
{
    const AgentPeer* peer{FindPeer(peer_id)};
    return peer != nullptr && peer->WaitingForHeaders();
}

bool AgentPeerSet::HeaderPeerSynced(AgentPeerId peer_id) const
{
    const AgentPeer* peer{FindPeer(peer_id)};
    return peer != nullptr && peer->HeaderPeerSynced();
}

const uint256& AgentPeerSet::HeaderStopHash(AgentPeerId peer_id) const
{
    return GetPeer(peer_id).HeaderStopHash();
}

size_t AgentPeerSet::PendingTxRequestCount(AgentPeerId peer_id) const
{
    const AgentPeer* peer{FindPeer(peer_id)};
    return peer == nullptr ? 0 : peer->PendingTxRequestCount();
}

size_t AgentPeerSet::KnownTxInventoryCount(AgentPeerId peer_id) const
{
    const AgentPeer* peer{FindPeer(peer_id)};
    return peer == nullptr ? 0 : peer->KnownTxInventoryCount();
}

AgentPeerSetAction AgentPeerSet::Result(AgentPeerSetResultCode code, AgentPeerId peer_id) const
{
    return {code, peer_id, std::nullopt, 0};
}

AgentPeerSetAction AgentPeerSet::PeerAction(AgentPeerId peer_id, AgentPeerAction action) const
{
    const size_t queued_count{action.queued_outbound_count};
    return {AgentPeerSetResultCode::PEER_ACTION, peer_id, std::move(action), queued_count};
}

AgentPeer* AgentPeerSet::FindPeer(AgentPeerId peer_id)
{
    const auto it{m_peers.find(peer_id)};
    return it == m_peers.end() ? nullptr : it->second.get();
}

const AgentPeer* AgentPeerSet::FindPeer(AgentPeerId peer_id) const
{
    const auto it{m_peers.find(peer_id)};
    return it == m_peers.end() ? nullptr : it->second.get();
}

} // namespace agent
