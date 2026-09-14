// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef QUICKSILVER_AGENT_TXMESSAGES_H
#define QUICKSILVER_AGENT_TXMESSAGES_H

#include <net.h>
#include <primitives/transaction.h>
#include <protocol.h>

#include <cstddef>
#include <span>
#include <vector>

namespace agent {

inline constexpr size_t DEFAULT_MAX_TX_INVENTORY{50000};

enum class TxMessageResultCode {
    DECODED,
    WRONG_MESSAGE_TYPE,
    TOO_MANY_INVENTORY,
    NON_TRANSACTION_INVENTORY,
    TRAILING_DATA,
    DESERIALIZE_FAILED,
};

struct TxInventoryMessageDecodeResult {
    TxMessageResultCode code;
    std::vector<CInv> inventory;
    size_t announced_count{0};
    size_t decoded_count{0};

    bool ok() const { return code == TxMessageResultCode::DECODED; }
};

struct TxMessageDecodeResult {
    TxMessageResultCode code;
    CTransactionRef transaction;

    bool ok() const { return code == TxMessageResultCode::DECODED; }
};

const char* TxMessageResultCodeString(TxMessageResultCode code);

CInv TransactionInventory(const GenTxid& txid);
std::vector<CInv> TransactionInventory(std::span<const GenTxid> txids);

CSerializedNetMsg MakeTxInvMessage(std::span<const CInv> inventory);
CSerializedNetMsg MakeTxGetDataMessage(std::span<const CInv> inventory);
CSerializedNetMsg MakeTxMessage(const CTransaction& transaction);

TxInventoryMessageDecodeResult DecodeTxInvMessage(const CSerializedNetMsg& message,
                                                  size_t max_tx_inventory = DEFAULT_MAX_TX_INVENTORY);
TxInventoryMessageDecodeResult DecodeTxGetDataMessage(const CSerializedNetMsg& message,
                                                      size_t max_tx_inventory = DEFAULT_MAX_TX_INVENTORY);
TxMessageDecodeResult DecodeTxMessage(const CSerializedNetMsg& message);

} // namespace agent

#endif // QUICKSILVER_AGENT_TXMESSAGES_H
