// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef QUICKSILVER_AGENT_PEERTRANSPORT_H
#define QUICKSILVER_AGENT_PEERTRANSPORT_H

#include <net.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

class CChainParams;

namespace agent {

inline constexpr int64_t DEFAULT_AGENT_PEER_TIMEOUT_MS{10'000};

struct PeerTransactionRelayResult {
    CService peer;
    bool connected{false};
    bool peer_version_received{false};
    bool peer_verack_received{false};
    bool local_verack_sent{false};
    bool sent_tx{false};
    size_t wire_tx_bytes{0};
    std::string error;
};

/** Decode the active network's BIP155 fixed-seed blob into usable endpoints. */
std::vector<CService> FixedSeedPeers();
std::vector<CService> FixedSeedPeers(const CChainParams& params);

/** Connect directly or through the proxy configured for the peer's network. */
std::unique_ptr<Sock> ConnectToPeer(const CService& peer);

PeerTransactionRelayResult SendTransactionToOnePeer(const CService& peer,
                                                    const CSerializedNetMsg& tx_payload,
                                                    std::chrono::milliseconds timeout);

} // namespace agent

#endif // QUICKSILVER_AGENT_PEERTRANSPORT_H
