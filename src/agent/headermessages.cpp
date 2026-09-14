// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <agent/headermessages.h>

#include <netmessagemaker.h>
#include <protocol.h>
#include <serialize.h>
#include <span.h>
#include <streams.h>

#include <cassert>
#include <ios>
#include <utility>

namespace agent {

namespace {

HeaderMessageDecodeResult DecodeResult(HeaderMessageResultCode code,
                                       std::vector<CBlockHeader> headers = {},
                                       size_t announced_count = 0,
                                       size_t decoded_count = 0)
{
    return {code, std::move(headers), announced_count, decoded_count};
}

} // namespace

const char* HeaderMessageResultCodeString(HeaderMessageResultCode code)
{
    switch (code) {
    case HeaderMessageResultCode::DECODED:
        return "decoded";
    case HeaderMessageResultCode::WRONG_MESSAGE_TYPE:
        return "wrong-message-type";
    case HeaderMessageResultCode::TOO_MANY_HEADERS:
        return "too-many-headers";
    case HeaderMessageResultCode::NONEMPTY_HEADER_TX_COUNT:
        return "nonempty-header-tx-count";
    case HeaderMessageResultCode::TRAILING_DATA:
        return "trailing-data";
    case HeaderMessageResultCode::DESERIALIZE_FAILED:
        return "deserialize-failed";
    } // no default case, so the compiler can warn on missing enum values
    assert(false);
    return "unknown";
}

CSerializedNetMsg MakeGetHeadersMessage(const HeaderSyncRequest& request)
{
    return NetMsg::Make(NetMsgType::GETHEADERS, request.locator, request.stop_hash);
}

CSerializedNetMsg MakeHeadersMessage(std::span<const CBlockHeader> headers)
{
    CSerializedNetMsg message;
    message.m_type = NetMsgType::HEADERS;
    VectorWriter writer{message.data, 0};
    WriteCompactSize(writer, headers.size());
    for (const CBlockHeader& header : headers) {
        writer << header;
        WriteCompactSize(writer, 0);
    }
    return message;
}

HeaderMessageDecodeResult DecodeHeadersMessage(const CSerializedNetMsg& message, size_t max_headers_result)
{
    if (message.m_type != NetMsgType::HEADERS) {
        return DecodeResult(HeaderMessageResultCode::WRONG_MESSAGE_TYPE);
    }

    try {
        DataStream stream{MakeByteSpan(message.data)};
        const uint64_t announced_count{ReadCompactSize(stream)};
        if (announced_count > max_headers_result) {
            return DecodeResult(HeaderMessageResultCode::TOO_MANY_HEADERS, {}, announced_count);
        }

        std::vector<CBlockHeader> headers;
        headers.reserve(announced_count);
        for (uint64_t i{0}; i < announced_count; ++i) {
            CBlockHeader header;
            stream >> header;
            const uint64_t tx_count{ReadCompactSize(stream)};
            if (tx_count != 0) {
                return DecodeResult(HeaderMessageResultCode::NONEMPTY_HEADER_TX_COUNT, std::move(headers), announced_count, i + 1);
            }
            headers.push_back(header);
        }

        if (!stream.empty()) {
            return DecodeResult(HeaderMessageResultCode::TRAILING_DATA, std::move(headers), announced_count, headers.size());
        }

        return DecodeResult(HeaderMessageResultCode::DECODED, std::move(headers), announced_count, announced_count);
    } catch (const std::ios_base::failure&) {
        return DecodeResult(HeaderMessageResultCode::DESERIALIZE_FAILED);
    }
}

HeaderMessageProcessResult ProcessHeadersMessage(HeaderSyncSession& sync, const CSerializedNetMsg& message, size_t max_headers_result)
{
    HeaderMessageDecodeResult decoded{DecodeHeadersMessage(message, max_headers_result)};
    if (!decoded.ok()) {
        return {std::move(decoded), std::nullopt};
    }

    HeaderSyncProcessResult processed{sync.ProcessHeaders(decoded.headers)};
    return {std::move(decoded), std::move(processed)};
}

} // namespace agent
