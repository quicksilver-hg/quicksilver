// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef QUICKSILVER_AGENT_HEADERDRIVER_H
#define QUICKSILVER_AGENT_HEADERDRIVER_H

#include <agent/headermessages.h>
#include <agent/headersync.h>
#include <net.h>
#include <uint256.h>

#include <optional>

namespace agent {

enum class HeaderSyncDriverResultCode {
    GETHEADERS_SENT,
    HEADERS_ACCEPTED,
    PEER_SYNCED,
    WAITING_FOR_HEADERS,
    IGNORED_MESSAGE,
    DECODE_FAILED,
    SYNC_FAILED,
};

struct HeaderSyncDriverResult {
    HeaderSyncDriverResultCode code;
    std::optional<CSerializedNetMsg> outbound_message;
    std::optional<HeaderMessageProcessResult> header_message;

    bool ok() const
    {
        return code == HeaderSyncDriverResultCode::GETHEADERS_SENT ||
               code == HeaderSyncDriverResultCode::HEADERS_ACCEPTED ||
               code == HeaderSyncDriverResultCode::PEER_SYNCED ||
               code == HeaderSyncDriverResultCode::WAITING_FOR_HEADERS ||
               code == HeaderSyncDriverResultCode::IGNORED_MESSAGE;
    }
};

const char* HeaderSyncDriverResultCodeString(HeaderSyncDriverResultCode code);

class HeaderSyncDriver
{
public:
    explicit HeaderSyncDriver(HeaderSyncSession& sync, uint256 stop_hash = uint256{});

    HeaderSyncDriverResult RequestHeaders();
    HeaderSyncDriverResult RequestHeaders(const uint256& stop_hash);
    HeaderSyncDriverResult ProcessMessage(const CSerializedNetMsg& message);

    bool WaitingForHeaders() const { return m_sync.HasRequestInFlight(); }
    bool PeerSynced() const { return m_sync.PeerSynced(); }
    const uint256& StopHash() const { return m_stop_hash; }

private:
    HeaderSyncDriverResult Result(HeaderSyncDriverResultCode code,
                                  std::optional<CSerializedNetMsg> outbound_message = std::nullopt,
                                  std::optional<HeaderMessageProcessResult> header_message = std::nullopt) const;

    CSerializedNetMsg MakeNextRequest();

    HeaderSyncSession& m_sync;
    uint256 m_stop_hash;
};

} // namespace agent

#endif // QUICKSILVER_AGENT_HEADERDRIVER_H
