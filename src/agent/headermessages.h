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
    std::vector<CBlockHeader> headers;
    size_t announced_count{0};
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
CSerializedNetMsg MakeHeadersMessage(std::span<const CBlockHeader> headers);
HeaderMessageDecodeResult DecodeHeadersMessage(const CSerializedNetMsg& message, size_t max_headers_result = DEFAULT_MAX_HEADERS_RESULTS);
HeaderMessageProcessResult ProcessHeadersMessage(HeaderSyncSession& sync,
                                                 const CSerializedNetMsg& message,
                                                 size_t max_headers_result = DEFAULT_MAX_HEADERS_RESULTS);

} // namespace agent

#endif // QUICKSILVER_AGENT_HEADERMESSAGES_H
