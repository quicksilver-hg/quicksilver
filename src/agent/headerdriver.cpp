// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <agent/headerdriver.h>

#include <protocol.h>

#include <cassert>
#include <optional>
#include <utility>

namespace agent {

const char* HeaderSyncDriverResultCodeString(HeaderSyncDriverResultCode code)
{
    switch (code) {
    case HeaderSyncDriverResultCode::GETHEADERS_SENT:
        return "getheaders-sent";
    case HeaderSyncDriverResultCode::HEADERS_ACCEPTED:
        return "headers-accepted";
    case HeaderSyncDriverResultCode::PEER_SYNCED:
        return "peer-synced";
    case HeaderSyncDriverResultCode::WAITING_FOR_HEADERS:
        return "waiting-for-headers";
    case HeaderSyncDriverResultCode::IGNORED_MESSAGE:
        return "ignored-message";
    case HeaderSyncDriverResultCode::DECODE_FAILED:
        return "decode-failed";
    case HeaderSyncDriverResultCode::SYNC_FAILED:
        return "sync-failed";
    } // no default case, so the compiler can warn on missing enum values
    assert(false);
    return "unknown";
}

HeaderSyncDriver::HeaderSyncDriver(HeaderSyncSession& sync, uint256 stop_hash)
    : m_sync{sync},
      m_stop_hash{std::move(stop_hash)}
{
}

HeaderSyncDriverResult HeaderSyncDriver::Result(HeaderSyncDriverResultCode code,
                                                std::optional<CSerializedNetMsg> outbound_message,
                                                std::optional<HeaderMessageProcessResult> header_message) const
{
    return {code, std::move(outbound_message), std::move(header_message)};
}

CSerializedNetMsg HeaderSyncDriver::MakeNextRequest()
{
    return MakeGetHeadersMessage(m_sync.NextHeadersRequest(m_stop_hash));
}

HeaderSyncDriverResult HeaderSyncDriver::RequestHeaders()
{
    if (m_sync.HasRequestInFlight()) {
        return Result(HeaderSyncDriverResultCode::WAITING_FOR_HEADERS);
    }

    return Result(HeaderSyncDriverResultCode::GETHEADERS_SENT, MakeNextRequest());
}

HeaderSyncDriverResult HeaderSyncDriver::RequestHeaders(const uint256& stop_hash)
{
    if (m_sync.HasRequestInFlight()) {
        return Result(HeaderSyncDriverResultCode::WAITING_FOR_HEADERS);
    }

    m_stop_hash = stop_hash;
    return RequestHeaders();
}

HeaderSyncDriverResult HeaderSyncDriver::ProcessMessage(const CSerializedNetMsg& message)
{
    if (message.m_type != NetMsgType::HEADERS) {
        return Result(HeaderSyncDriverResultCode::IGNORED_MESSAGE);
    }

    HeaderMessageProcessResult processed{ProcessHeadersMessage(m_sync, message, m_sync.MaxHeadersResult())};
    if (!processed.decode.ok()) {
        return Result(HeaderSyncDriverResultCode::DECODE_FAILED, std::nullopt, std::move(processed));
    }

    assert(processed.sync.has_value());
    if (!processed.sync->ok()) {
        return Result(HeaderSyncDriverResultCode::SYNC_FAILED, std::nullopt, std::move(processed));
    }

    if (processed.sync->request_more) {
        return Result(HeaderSyncDriverResultCode::HEADERS_ACCEPTED, MakeNextRequest(), std::move(processed));
    }

    if (processed.sync->peer_synced) {
        return Result(HeaderSyncDriverResultCode::PEER_SYNCED, std::nullopt, std::move(processed));
    }

    return Result(HeaderSyncDriverResultCode::HEADERS_ACCEPTED, std::nullopt, std::move(processed));
}

} // namespace agent
