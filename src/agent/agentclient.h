// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef QUICKSILVER_AGENT_AGENTCLIENT_H
#define QUICKSILVER_AGENT_AGENTCLIENT_H

#include <agent/agentpeer.h>
#include <agent/agentpeerset.h>
#include <agent/headerchain.h>
#include <agent/headerstore.h>
#include <agent/headersync.h>
#include <agent/txmessages.h>
#include <consensus/params.h>
#include <net.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <uint256.h>
#include <util/fs.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace agent {

struct AgentClientOptions {
    std::optional<fs::path> header_store_path{};
    uint256 header_stop_hash{};
    size_t max_headers_result{DEFAULT_MAX_HEADERS_RESULTS};
    size_t max_tx_inventory{DEFAULT_MAX_TX_INVENTORY};
};

struct AgentClientLoadResult {
    HeaderStoreResult status;
    HeaderAcceptResult invalid_header;

    bool ok() const;
};

class AgentClient
{
public:
    explicit AgentClient(const Consensus::Params& params,
                         const CBlockHeader& genesis,
                         AgentClientOptions options = {});

    AgentClient(const AgentClient&) = delete;
    AgentClient& operator=(const AgentClient&) = delete;
    AgentClient(AgentClient&&) = delete;
    AgentClient& operator=(AgentClient&&) = delete;

    const AgentClientLoadResult& LastLoadResult() const { return m_last_load_result; }

    HeaderChain& Headers() { return *m_chain; }
    const HeaderChain& Headers() const { return *m_chain; }
    int HeaderHeight() const { return m_chain->Height(); }
    const CBlockIndex& HeaderTip() const { return m_chain->Tip(); }

    HeaderStoreResult SaveHeaders() const;

    AgentPeerSetAction AddPeer(AgentPeerId peer_id) { return m_peers.AddPeer(peer_id); }
    size_t PeerCount() const { return m_peers.PeerCount(); }
    bool HasPeer(AgentPeerId peer_id) const { return m_peers.HasPeer(peer_id); }

    AgentPeerAction StartHeaders();
    AgentPeerAction StartHeaders(const uint256& stop_hash);
    AgentPeerSetAction StartHeaders(AgentPeerId peer_id) { return m_peers.StartHeaders(peer_id); }
    AgentPeerSetAction StartHeaders(AgentPeerId peer_id, const uint256& stop_hash)
    {
        return m_peers.StartHeaders(peer_id, stop_hash);
    }
    AgentPeerAction AnnounceTransaction(const CTransaction& transaction, bool prefer_wtxid = true);
    AgentPeerSetAction AnnounceTransaction(AgentPeerId peer_id,
                                           const CTransaction& transaction,
                                           bool prefer_wtxid = true)
    {
        return m_peers.AnnounceTransaction(peer_id, transaction, prefer_wtxid);
    }
    AgentPeerAction SendTransaction(const CTransaction& transaction);
    AgentPeerSetAction SendTransaction(AgentPeerId peer_id, const CTransaction& transaction)
    {
        return m_peers.SendTransaction(peer_id, transaction);
    }
    AgentPeerAction ProcessMessage(const CSerializedNetMsg& message);
    AgentPeerSetAction ProcessMessage(AgentPeerId peer_id, const CSerializedNetMsg& message)
    {
        return m_peers.ProcessMessage(peer_id, message);
    }

    bool HasOutboundMessages() const { return m_peers.HasOutboundMessages(DEFAULT_AGENT_PEER_ID); }
    bool HasOutboundMessages(AgentPeerId peer_id) const { return m_peers.HasOutboundMessages(peer_id); }
    size_t OutboundMessageCount() const { return m_peers.OutboundMessageCount(DEFAULT_AGENT_PEER_ID); }
    size_t OutboundMessageCount(AgentPeerId peer_id) const { return m_peers.OutboundMessageCount(peer_id); }
    std::optional<CSerializedNetMsg> PopOutboundMessage()
    {
        return m_peers.PopOutboundMessage(DEFAULT_AGENT_PEER_ID);
    }
    std::optional<CSerializedNetMsg> PopOutboundMessage(AgentPeerId peer_id)
    {
        return m_peers.PopOutboundMessage(peer_id);
    }
    std::vector<CSerializedNetMsg> DrainOutboundMessages()
    {
        return m_peers.DrainOutboundMessages(DEFAULT_AGENT_PEER_ID);
    }
    std::vector<CSerializedNetMsg> DrainOutboundMessages(AgentPeerId peer_id)
    {
        return m_peers.DrainOutboundMessages(peer_id);
    }

    bool WaitingForHeaders() const { return m_peers.WaitingForHeaders(DEFAULT_AGENT_PEER_ID); }
    bool WaitingForHeaders(AgentPeerId peer_id) const { return m_peers.WaitingForHeaders(peer_id); }
    bool HeaderPeerSynced() const { return m_peers.HeaderPeerSynced(DEFAULT_AGENT_PEER_ID); }
    bool HeaderPeerSynced(AgentPeerId peer_id) const { return m_peers.HeaderPeerSynced(peer_id); }
    const uint256& HeaderStopHash() const { return m_peers.HeaderStopHash(DEFAULT_AGENT_PEER_ID); }
    const uint256& HeaderStopHash(AgentPeerId peer_id) const { return m_peers.HeaderStopHash(peer_id); }

    size_t PendingTxRequestCount() const { return m_peers.PendingTxRequestCount(DEFAULT_AGENT_PEER_ID); }
    size_t PendingTxRequestCount(AgentPeerId peer_id) const { return m_peers.PendingTxRequestCount(peer_id); }
    size_t KnownTxInventoryCount() const { return m_peers.KnownTxInventoryCount(DEFAULT_AGENT_PEER_ID); }
    size_t KnownTxInventoryCount(AgentPeerId peer_id) const { return m_peers.KnownTxInventoryCount(peer_id); }

private:
    struct LoadedHeaders {
        std::unique_ptr<HeaderChain> chain;
        AgentClientLoadResult result;
    };

    AgentClient(AgentClientOptions options, LoadedHeaders loaded);
    static LoadedHeaders LoadHeaders(const Consensus::Params& params,
                                     const CBlockHeader& genesis,
                                     const std::optional<fs::path>& header_store_path);
    static AgentClientLoadResult FreshLoadResult(const CBlockHeader& genesis);
    static AgentPeerAction DefaultPeerAction(AgentPeerSetAction action);

    AgentClientOptions m_options;
    AgentClientLoadResult m_last_load_result;
    std::unique_ptr<HeaderChain> m_chain;
    AgentPeerSet m_peers;
};

} // namespace agent

#endif // QUICKSILVER_AGENT_AGENTCLIENT_H
