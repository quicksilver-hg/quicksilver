// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <agent/txpeer.h>

#include <protocol.h>

#include <cassert>
#include <utility>

namespace agent {

const char* TxPeerResultCodeString(TxPeerResultCode code)
{
    switch (code) {
    case TxPeerResultCode::INV_SENT:
        return "inv-sent";
    case TxPeerResultCode::GETDATA_SENT:
        return "getdata-sent";
    case TxPeerResultCode::TX_SENT:
        return "tx-sent";
    case TxPeerResultCode::TX_NOT_FOUND:
        return "tx-not-found";
    case TxPeerResultCode::INV_ALREADY_KNOWN:
        return "inv-already-known";
    case TxPeerResultCode::TX_RECEIVED:
        return "tx-received";
    case TxPeerResultCode::IGNORED_MESSAGE:
        return "ignored-message";
    case TxPeerResultCode::DECODE_FAILED:
        return "decode-failed";
    } // no default case, so the compiler can warn on missing enum values
    assert(false);
    return "unknown";
}

TxPeer::TxPeer(size_t max_tx_inventory)
    : m_max_tx_inventory{max_tx_inventory}
{
}

TxPeerResult TxPeer::Result(TxPeerResultCode code,
                            std::optional<TxInventoryMessageDecodeResult> inventory_message,
                            std::optional<TxMessageDecodeResult> tx_message,
                            std::vector<CInv> announced_inventory,
                            std::vector<CInv> requested_inventory,
                            std::vector<CInv> served_inventory,
                            std::vector<CInv> matched_requests,
                            size_t duplicate_count,
                            size_t missing_count) const
{
    return {code,
            std::move(inventory_message),
            std::move(tx_message),
            std::move(announced_inventory),
            std::move(requested_inventory),
            std::move(served_inventory),
            std::move(matched_requests),
            duplicate_count,
            missing_count};
}

TxPeerAction TxPeer::QueueResult(TxPeerResult result, std::optional<CSerializedNetMsg> outbound_message)
{
    const bool queued_outbound_message{outbound_message.has_value()};
    if (outbound_message.has_value()) {
        m_outbound_messages.push_back(std::move(*outbound_message));
    }
    return {std::move(result), queued_outbound_message};
}

TxPeerAction TxPeer::AnnounceTransaction(const CTransaction& transaction, bool prefer_wtxid)
{
    CTransactionRef transaction_ref{MakeTransactionRef(transaction)};
    StoreRelayTransaction(transaction_ref);
    MarkTransactionKnown(*transaction_ref);

    std::vector<CInv> announced_inventory;
    if (prefer_wtxid) {
        announced_inventory.emplace_back(MSG_WTX, transaction_ref->GetWitnessHash().ToUint256());
    } else {
        announced_inventory.emplace_back(MSG_TX, transaction_ref->GetHash().ToUint256());
    }
    CSerializedNetMsg inv{MakeTxInvMessage(announced_inventory)};
    return QueueResult(Result(TxPeerResultCode::INV_SENT,
                              std::nullopt,
                              std::nullopt,
                              announced_inventory),
                       std::move(inv));
}

TxPeerAction TxPeer::SendTransaction(const CTransaction& transaction)
{
    CTransactionRef transaction_ref{MakeTransactionRef(transaction)};
    StoreRelayTransaction(transaction_ref);
    MarkTransactionKnown(*transaction_ref);

    std::vector<CInv> served_inventory{CInv{MSG_WTX, transaction_ref->GetWitnessHash().ToUint256()}};
    CSerializedNetMsg tx{MakeTxMessage(*transaction_ref)};
    return QueueResult(Result(TxPeerResultCode::TX_SENT,
                              std::nullopt,
                              std::nullopt,
                              {},
                              {},
                              served_inventory),
                       std::move(tx));
}

TxPeerAction TxPeer::ProcessMessage(const CSerializedNetMsg& message)
{
    if (message.m_type == NetMsgType::INV) {
        TxInventoryMessageDecodeResult decoded{DecodeTxInvMessage(message, m_max_tx_inventory)};
        if (!decoded.ok()) {
            return QueueResult(Result(TxPeerResultCode::DECODE_FAILED, std::move(decoded)));
        }

        std::vector<CInv> requested_inventory;
        size_t duplicate_count{0};
        for (const CInv& inv : decoded.inventory) {
            if (m_known_inventory.contains(inv) || m_requested_inventory.contains(inv)) {
                ++duplicate_count;
                continue;
            }
            m_requested_inventory.insert(inv);
            requested_inventory.push_back(inv);
        }

        if (requested_inventory.empty()) {
            return QueueResult(Result(TxPeerResultCode::INV_ALREADY_KNOWN,
                                      std::move(decoded),
                                      std::nullopt,
                                      {},
                                      {},
                                      {},
                                      {},
                                      duplicate_count));
        }

        CSerializedNetMsg getdata{MakeTxGetDataMessage(requested_inventory)};
        return QueueResult(Result(TxPeerResultCode::GETDATA_SENT,
                                  std::move(decoded),
                                  std::nullopt,
                                  {},
                                  std::move(requested_inventory),
                                  {},
                                  {},
                                  duplicate_count),
                           std::move(getdata));
    }

    if (message.m_type == NetMsgType::GETDATA) {
        TxInventoryMessageDecodeResult decoded{DecodeTxGetDataMessage(message, m_max_tx_inventory)};
        if (!decoded.ok()) {
            return QueueResult(Result(TxPeerResultCode::DECODE_FAILED, std::move(decoded)));
        }

        std::vector<CInv> served_inventory;
        size_t missing_count{0};
        std::set<uint256> served_txids;
        for (const CInv& inv : decoded.inventory) {
            const auto transaction_it{m_relay_transactions.find(inv)};
            if (transaction_it == m_relay_transactions.end()) {
                ++missing_count;
                continue;
            }
            const uint256 txid{transaction_it->second->GetHash().ToUint256()};
            if (!served_txids.insert(txid).second) {
                continue;
            }
            served_inventory.push_back(inv);
            m_outbound_messages.push_back(MakeTxMessage(*transaction_it->second));
        }

        const TxPeerResultCode code{
            served_inventory.empty() ? TxPeerResultCode::TX_NOT_FOUND : TxPeerResultCode::TX_SENT};
        const bool queued_outbound_message{!served_inventory.empty()};
        return {Result(code,
                       std::move(decoded),
                       std::nullopt,
                       {},
                       {},
                       std::move(served_inventory),
                       {},
                       0,
                       missing_count),
                queued_outbound_message};
    }

    if (message.m_type == NetMsgType::TX) {
        TxMessageDecodeResult decoded{DecodeTxMessage(message)};
        if (!decoded.ok()) {
            return QueueResult(Result(TxPeerResultCode::DECODE_FAILED, std::nullopt, std::move(decoded)));
        }

        assert(decoded.transaction);
        std::vector<CInv> matched_requests{MarkTransactionKnown(*decoded.transaction)};
        return QueueResult(Result(TxPeerResultCode::TX_RECEIVED,
                                  std::nullopt,
                                  std::move(decoded),
                                  {},
                                  {},
                                  {},
                                  std::move(matched_requests)));
    }

    return QueueResult(Result(TxPeerResultCode::IGNORED_MESSAGE));
}

std::vector<CInv> TxPeer::MarkTransactionKnown(const CTransaction& transaction)
{
    const uint256 txid{transaction.GetHash().ToUint256()};
    const uint256 wtxid{transaction.GetWitnessHash().ToUint256()};
    const std::vector<CInv> known_inventory{
        CInv{MSG_TX, txid},
        CInv{MSG_WITNESS_TX, txid},
        CInv{MSG_WTX, wtxid},
    };

    std::vector<CInv> matched_requests;
    for (const CInv& inv : known_inventory) {
        m_known_inventory.insert(inv);
        if (m_requested_inventory.erase(inv) > 0) {
            matched_requests.push_back(inv);
        }
    }
    return matched_requests;
}

void TxPeer::StoreRelayTransaction(CTransactionRef transaction)
{
    assert(transaction);
    const uint256 txid{transaction->GetHash().ToUint256()};
    const uint256 wtxid{transaction->GetWitnessHash().ToUint256()};
    m_relay_transactions[CInv{MSG_TX, txid}] = transaction;
    m_relay_transactions[CInv{MSG_WITNESS_TX, txid}] = transaction;
    m_relay_transactions[CInv{MSG_WTX, wtxid}] = transaction;
}

std::optional<CSerializedNetMsg> TxPeer::PopOutboundMessage()
{
    if (m_outbound_messages.empty()) {
        return std::nullopt;
    }

    CSerializedNetMsg message{std::move(m_outbound_messages.front())};
    m_outbound_messages.pop_front();
    return message;
}

std::vector<CSerializedNetMsg> TxPeer::DrainOutboundMessages()
{
    std::vector<CSerializedNetMsg> messages;
    messages.reserve(m_outbound_messages.size());
    while (!m_outbound_messages.empty()) {
        messages.push_back(std::move(m_outbound_messages.front()));
        m_outbound_messages.pop_front();
    }
    return messages;
}

} // namespace agent
