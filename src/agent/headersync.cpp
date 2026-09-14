// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <agent/headersync.h>

#include <util/check.h>

#include <algorithm>
#include <cassert>

namespace agent {

const char* HeaderSyncResultCodeString(HeaderSyncResultCode code)
{
    switch (code) {
    case HeaderSyncResultCode::HEADERS_ACCEPTED:
        return "headers-accepted";
    case HeaderSyncResultCode::DUPLICATE_HEADERS:
        return "duplicate-headers";
    case HeaderSyncResultCode::NO_HEADERS:
        return "no-headers";
    case HeaderSyncResultCode::UNREQUESTED_HEADERS:
        return "unrequested-headers";
    case HeaderSyncResultCode::TOO_MANY_HEADERS:
        return "too-many-headers";
    case HeaderSyncResultCode::STALE_HEADERS:
        return "stale-headers";
    case HeaderSyncResultCode::INVALID_HEADER:
        return "invalid-header";
    } // no default case, so the compiler can warn on missing enum values
    assert(false);
    return "unknown";
}

HeaderSyncSession::HeaderSyncSession(HeaderChain& chain, size_t max_headers_result)
    : m_chain{chain},
      m_max_headers_result{max_headers_result}
{
    Assume(m_max_headers_result > 0);
}

HeaderSyncRequest HeaderSyncSession::NextHeadersRequest(const uint256& stop_hash)
{
    m_request_in_flight = true;
    m_peer_synced = false;
    m_last_request_tip_height = m_chain.Height();
    m_last_stop_hash = stop_hash;
    return {m_chain.GetLocator(), stop_hash};
}

HeaderSyncProcessResult HeaderSyncSession::Result(HeaderSyncResultCode code,
                                                  HeaderAcceptResult header_result,
                                                  size_t accepted_count,
                                                  size_t duplicate_count,
                                                  bool request_more,
                                                  bool peer_synced) const
{
    return {code, header_result, accepted_count, duplicate_count, request_more, peer_synced};
}

HeaderSyncProcessResult HeaderSyncSession::ProcessHeaders(std::span<const CBlockHeader> headers)
{
    HeaderAcceptResult last_result{HeaderAcceptCode::DUPLICATE, m_chain.Tip().GetBlockHash(), m_chain.Height()};
    if (!m_request_in_flight) {
        return Result(HeaderSyncResultCode::UNREQUESTED_HEADERS, last_result);
    }

    if (headers.size() > m_max_headers_result) {
        m_request_in_flight = false;
        return Result(HeaderSyncResultCode::TOO_MANY_HEADERS, last_result);
    }

    if (headers.empty()) {
        m_request_in_flight = false;
        m_peer_synced = true;
        return Result(HeaderSyncResultCode::NO_HEADERS, last_result, 0, 0, false, true);
    }

    size_t accepted_count{0};
    size_t duplicate_count{0};
    int highest_duplicate_height{m_last_request_tip_height};
    for (const CBlockHeader& header : headers) {
        last_result = m_chain.AcceptHeader(header);
        if (last_result.accepted()) {
            ++accepted_count;
        } else if (last_result.code == HeaderAcceptCode::DUPLICATE) {
            ++duplicate_count;
            highest_duplicate_height = std::max(highest_duplicate_height, last_result.height);
        } else {
            m_request_in_flight = false;
            return Result(HeaderSyncResultCode::INVALID_HEADER, last_result, accepted_count, duplicate_count);
        }
    }

    m_request_in_flight = false;
    if (accepted_count == 0) {
        if (duplicate_count == headers.size() && highest_duplicate_height > m_last_request_tip_height) {
            const bool request_more{headers.size() == m_max_headers_result};
            m_peer_synced = !request_more;
            return Result(HeaderSyncResultCode::DUPLICATE_HEADERS,
                          last_result,
                          accepted_count,
                          duplicate_count,
                          request_more,
                          m_peer_synced);
        }
        return Result(HeaderSyncResultCode::STALE_HEADERS, last_result, accepted_count, duplicate_count);
    }

    const bool request_more{headers.size() == m_max_headers_result};
    m_peer_synced = !request_more;
    return Result(HeaderSyncResultCode::HEADERS_ACCEPTED,
                  last_result,
                  accepted_count,
                  duplicate_count,
                  request_more,
                  m_peer_synced);
}

} // namespace agent
