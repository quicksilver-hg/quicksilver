// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <agent/headerpeer.h>

#include <utility>

namespace agent {

HeaderSyncPeer::HeaderSyncPeer(HeaderChain& chain, uint256 stop_hash, size_t max_headers_result)
    : m_sync{chain, max_headers_result},
      m_driver{m_sync, std::move(stop_hash)}
{
}

HeaderSyncPeerAction HeaderSyncPeer::QueueResult(HeaderSyncDriverResult result)
{
    const bool queued_outbound_message{result.outbound_message.has_value()};
    if (result.outbound_message.has_value()) {
        m_outbound_messages.push_back(std::move(*result.outbound_message));
        result.outbound_message.reset();
    }
    return {std::move(result), queued_outbound_message};
}

HeaderSyncPeerAction HeaderSyncPeer::StartHeaders()
{
    return QueueResult(m_driver.RequestHeaders());
}

HeaderSyncPeerAction HeaderSyncPeer::StartHeaders(const uint256& stop_hash)
{
    return QueueResult(m_driver.RequestHeaders(stop_hash));
}

HeaderSyncPeerAction HeaderSyncPeer::ProcessMessage(const CSerializedNetMsg& message)
{
    return QueueResult(m_driver.ProcessMessage(message));
}

std::optional<CSerializedNetMsg> HeaderSyncPeer::PopOutboundMessage()
{
    if (m_outbound_messages.empty()) {
        return std::nullopt;
    }

    CSerializedNetMsg message{std::move(m_outbound_messages.front())};
    m_outbound_messages.pop_front();
    return message;
}

std::vector<CSerializedNetMsg> HeaderSyncPeer::DrainOutboundMessages()
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
