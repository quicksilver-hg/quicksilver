// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <agent/txmessages.h>

#include <netmessagemaker.h>
#include <serialize.h>
#include <span.h>
#include <streams.h>

#include <cassert>
#include <ios>
#include <utility>

namespace agent {

namespace {

TxInventoryMessageDecodeResult InventoryDecodeResult(TxMessageResultCode code,
                                                     std::vector<CInv> inventory = {},
                                                     size_t announced_count = 0,
                                                     size_t decoded_count = 0,
                                                     size_t skipped_count = 0)
{
    return {code, std::move(inventory), announced_count, decoded_count, skipped_count};
}

TxMessageDecodeResult TransactionDecodeResult(TxMessageResultCode code, CTransactionRef transaction = {})
{
    return {code, std::move(transaction)};
}

//! With skip_non_transaction, entries that are not transactions (a node's inv also
//! announces blocks) are read and dropped; without it, the first one fails the message.
TxInventoryMessageDecodeResult DecodeTxInventoryMessage(const CSerializedNetMsg& message,
                                                        const char* expected_message_type,
                                                        size_t max_tx_inventory,
                                                        bool skip_non_transaction)
{
    if (message.m_type != expected_message_type) {
        return InventoryDecodeResult(TxMessageResultCode::WRONG_MESSAGE_TYPE);
    }

    try {
        DataStream stream{MakeByteSpan(message.data)};
        const uint64_t announced_count{ReadCompactSize(stream)};
        if (announced_count > max_tx_inventory) {
            return InventoryDecodeResult(TxMessageResultCode::TOO_MANY_INVENTORY, {}, announced_count);
        }

        std::vector<CInv> inventory;
        inventory.reserve(announced_count);
        size_t skipped_count{0};
        for (uint64_t i{0}; i < announced_count; ++i) {
            CInv inv;
            stream >> inv;
            if (!inv.IsGenTxMsg() && skip_non_transaction) {
                ++skipped_count;
                continue;
            }
            if (!inv.IsGenTxMsg()) {
                return InventoryDecodeResult(TxMessageResultCode::NON_TRANSACTION_INVENTORY,
                                             std::move(inventory),
                                             announced_count,
                                             i + 1);
            }
            inventory.push_back(inv);
        }

        if (!stream.empty()) {
            return InventoryDecodeResult(TxMessageResultCode::TRAILING_DATA,
                                         std::move(inventory),
                                         announced_count,
                                         announced_count,
                                         skipped_count);
        }

        return InventoryDecodeResult(TxMessageResultCode::DECODED,
                                     std::move(inventory),
                                     announced_count,
                                     announced_count,
                                     skipped_count);
    } catch (const std::ios_base::failure&) {
        return InventoryDecodeResult(TxMessageResultCode::DESERIALIZE_FAILED);
    }
}

} // namespace

const char* TxMessageResultCodeString(TxMessageResultCode code)
{
    switch (code) {
    case TxMessageResultCode::DECODED:
        return "decoded";
    case TxMessageResultCode::WRONG_MESSAGE_TYPE:
        return "wrong-message-type";
    case TxMessageResultCode::TOO_MANY_INVENTORY:
        return "too-many-inventory";
    case TxMessageResultCode::NON_TRANSACTION_INVENTORY:
        return "non-transaction-inventory";
    case TxMessageResultCode::TRAILING_DATA:
        return "trailing-data";
    case TxMessageResultCode::DESERIALIZE_FAILED:
        return "deserialize-failed";
    } // no default case, so the compiler can warn on missing enum values
    assert(false);
    return "unknown";
}

namespace {
CSerializedNetMsg MakeInventoryMessage(const char* message_type, std::span<const CInv> inventory)
{
    CSerializedNetMsg message;
    message.m_type = message_type;
    VectorWriter writer{message.data, 0};
    WriteCompactSize(writer, inventory.size());
    for (const CInv& inv : inventory) {
        writer << inv;
    }
    return message;
}
} // namespace

CSerializedNetMsg MakeTxInvMessage(std::span<const CInv> inventory)
{
    return MakeInventoryMessage(NetMsgType::INV, inventory);
}

CSerializedNetMsg MakeTxGetDataMessage(std::span<const CInv> inventory)
{
    return MakeInventoryMessage(NetMsgType::GETDATA, inventory);
}

CSerializedNetMsg MakeTxNotFoundMessage(std::span<const CInv> inventory)
{
    return MakeInventoryMessage(NetMsgType::NOTFOUND, inventory);
}

CSerializedNetMsg MakeTxMessage(const CTransaction& transaction)
{
    return NetMsg::Make(NetMsgType::TX, TX_WITH_WITNESS(transaction));
}

TxInventoryMessageDecodeResult DecodeTxInvMessage(const CSerializedNetMsg& message, size_t max_tx_inventory)
{
    return DecodeTxInventoryMessage(message, NetMsgType::INV, max_tx_inventory, /*skip_non_transaction=*/true);
}

TxInventoryMessageDecodeResult DecodeTxGetDataMessage(const CSerializedNetMsg& message, size_t max_tx_inventory)
{
    return DecodeTxInventoryMessage(message, NetMsgType::GETDATA, max_tx_inventory, /*skip_non_transaction=*/false);
}

TxInventoryMessageDecodeResult DecodeTxNotFoundMessage(const CSerializedNetMsg& message, size_t max_tx_inventory)
{
    return DecodeTxInventoryMessage(message, NetMsgType::NOTFOUND, max_tx_inventory, /*skip_non_transaction=*/true);
}

TxMessageDecodeResult DecodeTxMessage(const CSerializedNetMsg& message)
{
    if (message.m_type != NetMsgType::TX) {
        return TransactionDecodeResult(TxMessageResultCode::WRONG_MESSAGE_TYPE);
    }

    try {
        DataStream stream{MakeByteSpan(message.data)};
        CTransactionRef transaction;
        stream >> TX_WITH_WITNESS(transaction);
        if (!stream.empty()) {
            return TransactionDecodeResult(TxMessageResultCode::TRAILING_DATA, std::move(transaction));
        }
        return TransactionDecodeResult(TxMessageResultCode::DECODED, std::move(transaction));
    } catch (const std::ios_base::failure&) {
        return TransactionDecodeResult(TxMessageResultCode::DESERIALIZE_FAILED);
    }
}

} // namespace agent
