// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <agent/peertransport.h>

#include <quicksilver-build-config.h> // IWYU pragma: keep

#include <chainparams.h>
#include <clientversion.h>
#include <common/system.h>
#include <compat/compat.h>
#include <hash.h>
#include <netbase.h>
#include <netmessagemaker.h>
#include <node/protocol_version.h>
#include <protocol.h>
#include <streams.h>
#include <tinyformat.h>
#include <util/result.h>
#include <util/strencodings.h>
#include <util/threadinterrupt.h>
#include <util/time.h>
#include <util/translation.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <memory>
#include <vector>

namespace agent {
namespace {

constexpr int MAX_AGENT_PEER_HANDSHAKE_MESSAGES{64};

std::vector<unsigned char> FrameWireMessage(const CSerializedNetMsg& message)
{
    const uint256 payload_hash{Hash(message.data)};
    CMessageHeader header{Params().MessageStart(), message.m_type.c_str(), static_cast<unsigned int>(message.data.size())};
    std::memcpy(header.pchChecksum, payload_hash.begin(), CMessageHeader::CHECKSUM_SIZE);

    std::vector<unsigned char> bytes;
    bytes.reserve(CMessageHeader::HEADER_SIZE + message.data.size());
    VectorWriter{bytes, 0, header};
    bytes.insert(bytes.end(), message.data.begin(), message.data.end());
    return bytes;
}

util::Result<size_t> SendWireMessage(const Sock& sock,
                                     const CSerializedNetMsg& message,
                                     std::chrono::milliseconds timeout)
{
    const std::vector<unsigned char> bytes{FrameWireMessage(message)};
    CThreadInterrupt interrupt;
    try {
        sock.SendComplete(bytes, timeout, interrupt);
    } catch (const std::exception& e) {
        return util::Error{Untranslated(strprintf("Could not send %s message: %s", message.m_type, e.what()))};
    }
    return bytes.size();
}

util::Result<std::vector<unsigned char>> ReceiveBytes(const Sock& sock,
                                                      size_t byte_count,
                                                      std::chrono::milliseconds timeout)
{
    std::vector<unsigned char> bytes(byte_count);
    size_t received{0};
    const auto deadline{GetTime<std::chrono::milliseconds>() + timeout};
    while (received < byte_count) {
        const auto now{GetTime<std::chrono::milliseconds>()};
        if (now >= deadline) {
            return util::Error{Untranslated(strprintf("Timed out waiting for %s bytes from peer", util::ToString(byte_count - received)))};
        }

        Sock::Event occurred{};
        const auto wait_time{std::min(deadline - now, std::chrono::milliseconds{250})};
        if (!sock.Wait(wait_time, Sock::RECV, &occurred)) {
            return util::Error{Untranslated(strprintf("Peer socket wait failed: %s", NetworkErrorString(WSAGetLastError())))};
        }
        if (occurred == 0) {
            continue;
        }
        if (occurred & Sock::ERR) {
            return util::Error{Untranslated("Peer socket reported an error")};
        }

        const ssize_t read_count{sock.Recv(bytes.data() + received, byte_count - received, MSG_DONTWAIT)};
        if (read_count > 0) {
            received += static_cast<size_t>(read_count);
            continue;
        }
        if (read_count == 0) {
            return util::Error{Untranslated("Peer closed connection")};
        }
        const int error{WSAGetLastError()};
        if (error != WSAEAGAIN && error != WSAEINTR && error != WSAEWOULDBLOCK && error != WSAEINPROGRESS) {
            return util::Error{Untranslated(strprintf("Could not receive from peer: %s", NetworkErrorString(error)))};
        }
    }
    return bytes;
}

util::Result<CSerializedNetMsg> ReceiveWireMessage(const Sock& sock, std::chrono::milliseconds timeout)
{
    auto header_bytes{ReceiveBytes(sock, CMessageHeader::HEADER_SIZE, timeout)};
    if (!header_bytes) return util::Error{util::ErrorString(header_bytes)};

    CMessageHeader header;
    try {
        DataStream stream{Span<const unsigned char>{header_bytes->data(), header_bytes->size()}};
        stream >> header;
    } catch (const std::exception& e) {
        return util::Error{Untranslated(strprintf("Could not decode peer message header: %s", e.what()))};
    }

    if (header.pchMessageStart != Params().MessageStart()) {
        return util::Error{Untranslated("Peer sent a message for a different network")};
    }
    if (!header.IsMessageTypeValid()) {
        return util::Error{Untranslated("Peer sent an invalid message type")};
    }
    if (header.nMessageSize > MAX_PROTOCOL_MESSAGE_LENGTH) {
        return util::Error{Untranslated(strprintf("Peer message %s is too large: %s bytes", header.GetMessageType(), util::ToString(header.nMessageSize)))};
    }

    auto payload{ReceiveBytes(sock, header.nMessageSize, timeout)};
    if (!payload) return util::Error{util::ErrorString(payload)};

    const uint256 payload_hash{Hash(*payload)};
    if (std::memcmp(payload_hash.begin(), header.pchChecksum, CMessageHeader::CHECKSUM_SIZE) != 0) {
        return util::Error{Untranslated(strprintf("Peer message %s has an invalid checksum", header.GetMessageType()))};
    }

    CSerializedNetMsg message;
    message.m_type = header.GetMessageType();
    message.data = std::move(*payload);
    return message;
}

CSerializedNetMsg MakeAgentVersionMessage(const CService& peer)
{
    const int64_t now{count_seconds(GetTime<std::chrono::seconds>())};
    const uint64_t nonce{static_cast<uint64_t>(count_microseconds(GetTime<std::chrono::microseconds>()))};
    return NetMsg::Make(NetMsgType::VERSION,
                        PROTOCOL_VERSION,
                        uint64_t{NODE_NONE},
                        now,
                        uint64_t{NODE_NONE},
                        CNetAddr::V1(peer),
                        uint64_t{NODE_NONE},
                        CNetAddr::V1(CService{}),
                        nonce,
                        FormatSubVersion(CLIENT_NAME, CLIENT_VERSION, {"agent"}),
                        int32_t{0},
                        true);
}

util::Result<void> SendHandshakeFeatureMessages(const Sock& sock, std::chrono::milliseconds timeout)
{
    auto sent_wtxidrelay{SendWireMessage(sock, NetMsg::Make(NetMsgType::WTXIDRELAY), timeout)};
    if (!sent_wtxidrelay) return util::Error{util::ErrorString(sent_wtxidrelay)};
    auto sent_sendaddrv2{SendWireMessage(sock, NetMsg::Make(NetMsgType::SENDADDRV2), timeout)};
    if (!sent_sendaddrv2) return util::Error{util::ErrorString(sent_sendaddrv2)};
    auto sent_verack{SendWireMessage(sock, NetMsg::Make(NetMsgType::VERACK), timeout)};
    if (!sent_verack) return util::Error{util::ErrorString(sent_verack)};
    return {};
}

struct PeerHandshakeResult {
    bool peer_version_received{false};
    bool peer_verack_received{false};
    bool local_verack_sent{false};
    std::string error;
};

PeerHandshakeResult CompletePeerHandshake(const Sock& sock, const CService& peer, std::chrono::milliseconds timeout)
{
    PeerHandshakeResult result;

    auto sent_version{SendWireMessage(sock, MakeAgentVersionMessage(peer), timeout)};
    if (!sent_version) {
        result.error = util::ErrorString(sent_version).original;
        return result;
    }

    for (int message_count{0}; message_count < MAX_AGENT_PEER_HANDSHAKE_MESSAGES && !(result.peer_version_received && result.peer_verack_received); ++message_count) {
        auto peer_message{ReceiveWireMessage(sock, timeout)};
        if (!peer_message) {
            result.error = util::ErrorString(peer_message).original;
            return result;
        }
        if (peer_message->m_type == NetMsgType::VERSION) {
            if (result.peer_version_received) {
                result.error = "peer sent duplicate version message";
                return result;
            }
            result.peer_version_received = true;
            auto sent_features{SendHandshakeFeatureMessages(sock, timeout)};
            if (!sent_features) {
                result.error = util::ErrorString(sent_features).original;
                return result;
            }
            result.local_verack_sent = true;
            continue;
        }
        if (peer_message->m_type == NetMsgType::VERACK) {
            result.peer_verack_received = true;
            continue;
        }
        if (peer_message->m_type == NetMsgType::PING && peer_message->data.size() == sizeof(uint64_t)) {
            CSerializedNetMsg pong;
            pong.m_type = NetMsgType::PONG;
            pong.data = peer_message->data;
            auto sent_pong{SendWireMessage(sock, pong, timeout)};
            if (!sent_pong) {
                result.error = util::ErrorString(sent_pong).original;
                return result;
            }
        }
    }
    if (!result.peer_version_received || !result.peer_verack_received || !result.local_verack_sent) {
        result.error = "peer handshake did not complete";
    }
    return result;
}

} // namespace

std::vector<CService> FixedSeedPeers()
{
    return FixedSeedPeers(Params());
}

std::vector<CService> FixedSeedPeers(const CChainParams& params)
{
    std::vector<CService> peers;
    ParamsStream stream{DataStream{params.FixedSeeds()}, CAddress::V2_NETWORK};
    while (!stream.eof()) {
        CService peer;
        stream >> peer;
        if (peer.IsValid() && peer.GetPort() != 0) {
            peers.push_back(peer);
        }
    }
    return peers;
}

std::unique_ptr<Sock> ConnectToPeer(const CService& peer)
{
    Proxy proxy;
    if (GetProxy(peer.GetNetwork(), proxy)) {
        bool proxy_connection_failed{false};
        return ConnectThroughProxy(proxy,
                                   peer.ToStringAddr(),
                                   peer.GetPort(),
                                   proxy_connection_failed);
    }
    return ConnectDirectly(peer, /*manual_connection=*/true);
}

PeerTransactionRelayResult SendTransactionToOnePeer(const CService& peer,
                                                    const CSerializedNetMsg& tx_payload,
                                                    std::chrono::milliseconds timeout)
{
    PeerTransactionRelayResult result;
    result.peer = peer;

    std::unique_ptr<Sock> sock{ConnectToPeer(peer)};
    if (!sock) {
        result.error = strprintf("could not connect to peer %s", peer.ToStringAddrPort());
        return result;
    }
    result.connected = true;

    const PeerHandshakeResult handshake{CompletePeerHandshake(*sock, peer, timeout)};
    result.peer_version_received = handshake.peer_version_received;
    result.peer_verack_received = handshake.peer_verack_received;
    result.local_verack_sent = handshake.local_verack_sent;
    if (!handshake.error.empty()) {
        result.error = handshake.error;
        return result;
    }

    auto sent_tx{SendWireMessage(*sock, tx_payload, timeout)};
    if (!sent_tx) {
        result.error = util::ErrorString(sent_tx).original;
        return result;
    }
    result.sent_tx = true;
    result.wire_tx_bytes = *sent_tx;
    return result;
}

} // namespace agent
