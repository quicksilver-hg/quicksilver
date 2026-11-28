// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef QUICKSILVER_AGENT_AGENTCLIENT_H
#define QUICKSILVER_AGENT_AGENTCLIENT_H

#include <agent/agentpeer.h>
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

    //! Returns FILE_NOT_FOUND when the client was built without a header store path
    //! (the command-line agent always gives one).
    HeaderStoreResult SaveHeaders() const;

    AgentPeerAction StartHeaders();
    AgentPeerAction StartHeaders(const uint256& stop_hash);
    AgentPeerAction AnnounceTransaction(const CTransaction& transaction, bool prefer_wtxid = true);
    AgentPeerAction SendTransaction(const CTransaction& transaction);
    AgentPeerAction ProcessMessage(const CSerializedNetMsg& message);

    bool HasOutboundMessages() const { return m_peer.HasOutboundMessages(); }
    size_t OutboundMessageCount() const { return m_peer.OutboundMessageCount(); }
    std::optional<CSerializedNetMsg> PopOutboundMessage()
    {
        return m_peer.PopOutboundMessage();
    }
    std::vector<CSerializedNetMsg> DrainOutboundMessages()
    {
        return m_peer.DrainOutboundMessages();
    }

    bool WaitingForHeaders() const { return m_peer.WaitingForHeaders(); }
    bool HeaderPeerSynced() const { return m_peer.HeaderPeerSynced(); }
    const uint256& HeaderStopHash() const { return m_peer.HeaderStopHash(); }

    size_t PendingTxRequestCount() const { return m_peer.PendingTxRequestCount(); }
    size_t KnownTxInventoryCount() const { return m_peer.KnownTxInventoryCount(); }

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

    AgentClientOptions m_options;
    AgentClientLoadResult m_last_load_result;
    std::unique_ptr<HeaderChain> m_chain;
    //! The one peer the command-line agent talks to.
    AgentPeer m_peer;
};

} // namespace agent

#endif // QUICKSILVER_AGENT_AGENTCLIENT_H
