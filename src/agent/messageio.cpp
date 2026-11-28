// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <agent/messageio.h>

#include <protocol.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <cassert>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace agent {

namespace {

AgentMessageDecodeResult DecodeResult(AgentMessageIoResultCode code, CSerializedNetMsg message = {})
{
    return {code, std::move(message)};
}

bool ValidMessageType(std::string_view message_type)
{
    const std::string_view trimmed{util::TrimStringView(message_type)};
    if (trimmed.empty() || trimmed.size() > CMessageHeader::MESSAGE_TYPE_SIZE) return false;
    // A NUL would end the type early on the wire; the header check below would then
    // see only the part before it.
    if (trimmed.find('\0') != std::string_view::npos) return false;
    // Same rule the node applies to a received header: printable ASCII only.
    const std::string type{trimmed};
    return CMessageHeader{MessageStartChars{}, type.c_str(), 0}.IsMessageTypeValid();
}

} // namespace

const char* AgentMessageIoResultCodeString(AgentMessageIoResultCode code)
{
    switch (code) {
    case AgentMessageIoResultCode::DECODED:
        return "decoded";
    case AgentMessageIoResultCode::INVALID_MESSAGE_TYPE:
        return "invalid-message-type";
    case AgentMessageIoResultCode::INVALID_HEX:
        return "invalid-hex";
    } // no default case, so the compiler can warn on missing enum values
    assert(false);
    return "unknown";
}

std::string AgentMessagePayloadHex(const CSerializedNetMsg& message)
{
    return HexStr(message.data);
}

AgentMessageDecodeResult DecodeAgentMessage(std::string_view message_type, std::string_view payload_hex)
{
    if (!ValidMessageType(message_type)) {
        return DecodeResult(AgentMessageIoResultCode::INVALID_MESSAGE_TYPE);
    }

    std::optional<std::vector<unsigned char>> payload{TryParseHex<unsigned char>(payload_hex)};
    if (!payload.has_value()) {
        return DecodeResult(AgentMessageIoResultCode::INVALID_HEX);
    }

    CSerializedNetMsg message;
    message.m_type = std::string{util::TrimStringView(message_type)};
    message.data = std::move(*payload);
    return DecodeResult(AgentMessageIoResultCode::DECODED, std::move(message));
}

} // namespace agent
