// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef QUICKSILVER_AGENT_HEADERSYNC_H
#define QUICKSILVER_AGENT_HEADERSYNC_H

#include <agent/headerchain.h>
#include <chain.h>
#include <primitives/block.h>
#include <uint256.h>

#include <cstddef>
#include <span>

namespace agent {

inline constexpr size_t DEFAULT_MAX_HEADERS_RESULTS{2000};

struct HeaderSyncRequest {
    CBlockLocator locator;
    uint256 stop_hash;
};

enum class HeaderSyncResultCode {
    HEADERS_ACCEPTED,
    DUPLICATE_HEADERS,
    NO_HEADERS,
    UNREQUESTED_HEADERS,
    TOO_MANY_HEADERS,
    STALE_HEADERS,
    INVALID_HEADER,
};

struct HeaderSyncProcessResult {
    HeaderSyncResultCode code;
    HeaderAcceptResult header_result;
    size_t accepted_count{0};
    size_t duplicate_count{0};
    bool request_more{false};
    bool peer_synced{false};

    bool ok() const
    {
        return code == HeaderSyncResultCode::HEADERS_ACCEPTED ||
               code == HeaderSyncResultCode::DUPLICATE_HEADERS ||
               code == HeaderSyncResultCode::NO_HEADERS;
    }
};

const char* HeaderSyncResultCodeString(HeaderSyncResultCode code);

class HeaderSyncSession
{
public:
    explicit HeaderSyncSession(HeaderChain& chain, size_t max_headers_result = DEFAULT_MAX_HEADERS_RESULTS);

    HeaderSyncRequest NextHeadersRequest(const uint256& stop_hash = uint256{});
    HeaderSyncProcessResult ProcessHeaders(std::span<const CBlockHeader> headers);

    bool HasRequestInFlight() const { return m_request_in_flight; }
    bool PeerSynced() const { return m_peer_synced; }
    size_t MaxHeadersResult() const { return m_max_headers_result; }
    const uint256& LastStopHash() const { return m_last_stop_hash; }

private:
    HeaderSyncProcessResult Result(HeaderSyncResultCode code,
                                   HeaderAcceptResult header_result,
                                   size_t accepted_count = 0,
                                   size_t duplicate_count = 0,
                                   bool request_more = false,
                                   bool peer_synced = false) const;

    HeaderChain& m_chain;
    size_t m_max_headers_result;
    bool m_request_in_flight{false};
    bool m_peer_synced{false};
    int m_last_request_tip_height{0};
    uint256 m_last_stop_hash;
};

} // namespace agent

#endif // QUICKSILVER_AGENT_HEADERSYNC_H
