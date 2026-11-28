// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef QUICKSILVER_AGENT_HEADERMESSAGES_H
#define QUICKSILVER_AGENT_HEADERMESSAGES_H

#include <agent/headersync.h>
#include <net.h>
#include <primitives/block.h>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace agent {

enum class HeaderMessageResultCode {
    DECODED,
    WRONG_MESSAGE_TYPE,
    TOO_MANY_HEADERS,
    NONEMPTY_HEADER_TX_COUNT,
    TRAILING_DATA,
    DESERIALIZE_FAILED,
};

struct HeaderMessageDecodeResult {
    HeaderMessageResultCode code;
    //! Headers accepted by the decoder, in payload order.
    std::vector<CBlockHeader> headers;
    //! The count the payload declared. Zero on WRONG_MESSAGE_TYPE and on
    //! DESERIALIZE_FAILED, which reports nothing it read.
    size_t announced_count{0};
    //! Headers read from the payload, the refused one included: all of them on
    //! DECODED and TRAILING_DATA; on NONEMPTY_HEADER_TX_COUNT the refused header's
    //! 1-based position, so `headers` then holds decoded_count - 1 entries; zero
    //! when reading stopped before any header (WRONG_MESSAGE_TYPE,
    //! TOO_MANY_HEADERS, DESERIALIZE_FAILED).
    size_t decoded_count{0};

    bool ok() const { return code == HeaderMessageResultCode::DECODED; }
};

struct HeaderMessageProcessResult {
    HeaderMessageDecodeResult decode;
    std::optional<HeaderSyncProcessResult> sync;

    bool ok() const { return decode.ok() && sync.has_value() && sync->ok(); }
};

const char* HeaderMessageResultCodeString(HeaderMessageResultCode code);

CSerializedNetMsg MakeGetHeadersMessage(const HeaderSyncRequest& request);
//! Refuses more than max_headers_result headers before reading any, so the
//! allocation is bounded by the cap (callers pass the node's 2000 or less).
HeaderMessageDecodeResult DecodeHeadersMessage(const CSerializedNetMsg& message, size_t max_headers_result = DEFAULT_MAX_HEADERS_RESULTS);
HeaderMessageProcessResult ProcessHeadersMessage(HeaderSyncSession& sync,
                                                 const CSerializedNetMsg& message,
                                                 size_t max_headers_result = DEFAULT_MAX_HEADERS_RESULTS);

} // namespace agent

#endif // QUICKSILVER_AGENT_HEADERMESSAGES_H
