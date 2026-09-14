// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef QUICKSILVER_AGENT_HEADERPEER_H
#define QUICKSILVER_AGENT_HEADERPEER_H

#include <agent/headerchain.h>
#include <agent/headerdriver.h>
#include <agent/headersync.h>
#include <net.h>
#include <uint256.h>

#include <cstddef>
#include <deque>
#include <optional>
#include <vector>

namespace agent {

struct HeaderSyncPeerAction {
    HeaderSyncDriverResult driver_result;
    // Queued messages are moved into HeaderSyncPeer's outbound queue.
    bool queued_outbound_message{false};

    bool ok() const { return driver_result.ok(); }
};

class HeaderSyncPeer
{
public:
    explicit HeaderSyncPeer(HeaderChain& chain,
                            uint256 stop_hash = uint256{},
                            size_t max_headers_result = DEFAULT_MAX_HEADERS_RESULTS);

    HeaderSyncPeerAction StartHeaders();
    HeaderSyncPeerAction StartHeaders(const uint256& stop_hash);
    HeaderSyncPeerAction ProcessMessage(const CSerializedNetMsg& message);

    bool HasOutboundMessages() const { return !m_outbound_messages.empty(); }
    size_t OutboundMessageCount() const { return m_outbound_messages.size(); }
    std::optional<CSerializedNetMsg> PopOutboundMessage();
    std::vector<CSerializedNetMsg> DrainOutboundMessages();

    bool WaitingForHeaders() const { return m_driver.WaitingForHeaders(); }
    bool PeerSynced() const { return m_driver.PeerSynced(); }
    const uint256& StopHash() const { return m_driver.StopHash(); }

private:
    HeaderSyncPeerAction QueueResult(HeaderSyncDriverResult result);

    HeaderSyncSession m_sync;
    HeaderSyncDriver m_driver;
    std::deque<CSerializedNetMsg> m_outbound_messages;
};

} // namespace agent

#endif // QUICKSILVER_AGENT_HEADERPEER_H
