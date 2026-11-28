// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <agent/agentclient.h>

#include <utility>

namespace agent {

bool AgentClientLoadResult::ok() const
{
    return status == HeaderStoreResult::OK || status == HeaderStoreResult::FILE_NOT_FOUND;
}

AgentClient::AgentClient(const Consensus::Params& params, const CBlockHeader& genesis, AgentClientOptions options)
    : AgentClient{options, LoadHeaders(params, genesis, options.header_store_path)}
{
}

AgentClient::AgentClient(AgentClientOptions options, LoadedHeaders loaded)
    : m_options{std::move(options)},
      m_last_load_result{loaded.result},
      m_chain{std::move(loaded.chain)},
      m_peer{*m_chain, m_options.header_stop_hash, m_options.max_headers_result, m_options.max_tx_inventory}
{
}

HeaderStoreResult AgentClient::SaveHeaders() const
{
    if (!m_options.header_store_path.has_value()) {
        return HeaderStoreResult::FILE_NOT_FOUND;
    }
    return SaveHeaderChain(*m_chain, *m_options.header_store_path);
}

AgentPeerAction AgentClient::StartHeaders()
{
    return m_peer.StartHeaders();
}

AgentPeerAction AgentClient::StartHeaders(const uint256& stop_hash)
{
    return m_peer.StartHeaders(stop_hash);
}

AgentPeerAction AgentClient::AnnounceTransaction(const CTransaction& transaction, bool prefer_wtxid)
{
    return m_peer.AnnounceTransaction(transaction, prefer_wtxid);
}

AgentPeerAction AgentClient::SendTransaction(const CTransaction& transaction)
{
    return m_peer.SendTransaction(transaction);
}

AgentPeerAction AgentClient::ProcessMessage(const CSerializedNetMsg& message)
{
    return m_peer.ProcessMessage(message);
}

AgentClient::LoadedHeaders AgentClient::LoadHeaders(const Consensus::Params& params,
                                                    const CBlockHeader& genesis,
                                                    const std::optional<fs::path>& header_store_path)
{
    if (!header_store_path.has_value()) {
        return {std::make_unique<HeaderChain>(params, genesis), FreshLoadResult(genesis)};
    }

    HeaderStoreLoadResult loaded{LoadHeaderChain(params, genesis, *header_store_path)};
    AgentClientLoadResult result{loaded.status, loaded.invalid_header};
    return {std::move(loaded.chain), result};
}

AgentClientLoadResult AgentClient::FreshLoadResult(const CBlockHeader& genesis)
{
    return {HeaderStoreResult::FILE_NOT_FOUND, {HeaderAcceptCode::DUPLICATE, genesis.GetHash(), 0}};
}

} // namespace agent
