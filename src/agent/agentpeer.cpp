// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <agent/agentpeer.h>

#include <protocol.h>

#include <cassert>
#include <utility>

namespace agent {

bool AgentPeerAction::ok() const
{
    if (header_action.has_value()) {
        return header_action->ok();
    }
    if (tx_action.has_value()) {
        return tx_action->ok();
    }
    return code == AgentPeerActionCode::IGNORED_MESSAGE;
}

const char* AgentPeerActionCodeString(AgentPeerActionCode code)
{
    switch (code) {
    case AgentPeerActionCode::HEADERS_STARTED:
        return "headers-started";
    case AgentPeerActionCode::TRANSACTION_ANNOUNCED:
        return "transaction-announced";
    case AgentPeerActionCode::TRANSACTION_SENT:
        return "transaction-sent";
    case AgentPeerActionCode::HEADER_MESSAGE:
        return "header-message";
    case AgentPeerActionCode::TX_MESSAGE:
        return "tx-message";
    case AgentPeerActionCode::IGNORED_MESSAGE:
        return "ignored-message";
    } // no default case, so the compiler can warn on missing enum values
    assert(false);
    return "unknown";
}

AgentPeer::AgentPeer(HeaderChain& chain, uint256 stop_hash, size_t max_headers_result, size_t max_tx_inventory)
    : m_header_peer{chain, std::move(stop_hash), max_headers_result},
      m_tx_peer{max_tx_inventory}
{
}

AgentPeerAction AgentPeer::StartHeaders()
{
    return HeaderResult(AgentPeerActionCode::HEADERS_STARTED, m_header_peer.StartHeaders());
}

AgentPeerAction AgentPeer::StartHeaders(const uint256& stop_hash)
{
    return HeaderResult(AgentPeerActionCode::HEADERS_STARTED, m_header_peer.StartHeaders(stop_hash));
}

AgentPeerAction AgentPeer::AnnounceTransaction(const CTransaction& transaction, bool prefer_wtxid)
{
    return TxResult(AgentPeerActionCode::TRANSACTION_ANNOUNCED, m_tx_peer.AnnounceTransaction(transaction, prefer_wtxid));
}

AgentPeerAction AgentPeer::SendTransaction(const CTransaction& transaction)
{
    return TxResult(AgentPeerActionCode::TRANSACTION_SENT, m_tx_peer.SendTransaction(transaction));
}

AgentPeerAction AgentPeer::ProcessMessage(const CSerializedNetMsg& message)
{
    if (message.m_type == NetMsgType::HEADERS) {
        return HeaderResult(AgentPeerActionCode::HEADER_MESSAGE, m_header_peer.ProcessMessage(message));
    }

    if (message.m_type == NetMsgType::INV || message.m_type == NetMsgType::GETDATA || message.m_type == NetMsgType::TX) {
        return TxResult(AgentPeerActionCode::TX_MESSAGE, m_tx_peer.ProcessMessage(message));
    }

    return IgnoredResult();
}

AgentPeerAction AgentPeer::HeaderResult(AgentPeerActionCode code, HeaderSyncPeerAction action)
{
    const size_t queued_count{DrainPeerOutbound(m_header_peer.DrainOutboundMessages())};
    return {code, std::move(action), std::nullopt, queued_count};
}

AgentPeerAction AgentPeer::TxResult(AgentPeerActionCode code, TxPeerAction action)
{
    const size_t queued_count{DrainPeerOutbound(m_tx_peer.DrainOutboundMessages())};
    return {code, std::nullopt, std::move(action), queued_count};
}

AgentPeerAction AgentPeer::IgnoredResult() const
{
    return {AgentPeerActionCode::IGNORED_MESSAGE, std::nullopt, std::nullopt, 0};
}

size_t AgentPeer::DrainPeerOutbound(std::vector<CSerializedNetMsg> messages)
{
    const size_t queued_count{messages.size()};
    for (CSerializedNetMsg& message : messages) {
        m_outbound_messages.push_back(std::move(message));
    }
    return queued_count;
}

std::optional<CSerializedNetMsg> AgentPeer::PopOutboundMessage()
{
    if (m_outbound_messages.empty()) {
        return std::nullopt;
    }

    CSerializedNetMsg message{std::move(m_outbound_messages.front())};
    m_outbound_messages.pop_front();
    return message;
}

std::vector<CSerializedNetMsg> AgentPeer::DrainOutboundMessages()
{
    std::vector<CSerializedNetMsg> messages;
    messages.reserve(m_outbound_messages.size());
    while (!m_outbound_messages.empty()) {
        messages.push_back(std::move(m_outbound_messages.front()));
        m_outbound_messages.pop_front();
    }
    return messages;
}

} // namespace agent
