// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef QUICKSILVER_AGENT_MESSAGEIO_H
#define QUICKSILVER_AGENT_MESSAGEIO_H

#include <net.h>

#include <string>
#include <string_view>

namespace agent {

enum class AgentMessageIoResultCode {
    DECODED,
    INVALID_MESSAGE_TYPE,
    INVALID_HEX,
};

struct AgentMessageDecodeResult {
    AgentMessageIoResultCode code;
    CSerializedNetMsg message;

    bool ok() const { return code == AgentMessageIoResultCode::DECODED; }
};

const char* AgentMessageIoResultCodeString(AgentMessageIoResultCode code);

std::string AgentMessagePayloadHex(const CSerializedNetMsg& message);
AgentMessageDecodeResult DecodeAgentMessage(std::string_view message_type, std::string_view payload_hex);

} // namespace agent

#endif // QUICKSILVER_AGENT_MESSAGEIO_H
