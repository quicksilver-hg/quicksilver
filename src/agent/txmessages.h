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
    //! The transaction entries kept, in payload order.
    std::vector<CInv> inventory;
    //! The count the payload declared. Zero on WRONG_MESSAGE_TYPE and on
    //! DESERIALIZE_FAILED, which reports nothing it read.
    size_t announced_count{0};
    //! Entries read from the payload, the refused one included: all of them on
    //! DECODED and TRAILING_DATA, the refused entry's 1-based position on
    //! NON_TRANSACTION_INVENTORY, and zero when reading stopped before any entry
    //! (WRONG_MESSAGE_TYPE, TOO_MANY_INVENTORY, DESERIALIZE_FAILED).
    size_t decoded_count{0};
    //! Non-transaction entries read and dropped (inv and notfound only).
    size_t skipped_count{0};

    bool ok() const { return code == TxMessageResultCode::DECODED; }
};

struct TxMessageDecodeResult {
    TxMessageResultCode code;
    CTransactionRef transaction;

    bool ok() const { return code == TxMessageResultCode::DECODED; }
};

const char* TxMessageResultCodeString(TxMessageResultCode code);


CSerializedNetMsg MakeTxInvMessage(std::span<const CInv> inventory);
CSerializedNetMsg MakeTxGetDataMessage(std::span<const CInv> inventory);
CSerializedNetMsg MakeTxNotFoundMessage(std::span<const CInv> inventory);
CSerializedNetMsg MakeTxMessage(const CTransaction& transaction);

//! An inv may announce blocks as well as transactions; non-transaction entries are
//! skipped (counted in skipped_count), not a reason to refuse the message.
TxInventoryMessageDecodeResult DecodeTxInvMessage(const CSerializedNetMsg& message,
                                                  size_t max_tx_inventory = DEFAULT_MAX_TX_INVENTORY);
//! The agent only ever announces transactions, so a getdata for anything else is refused.
TxInventoryMessageDecodeResult DecodeTxGetDataMessage(const CSerializedNetMsg& message,
                                                      size_t max_tx_inventory = DEFAULT_MAX_TX_INVENTORY);
//! Non-transaction entries are skipped, as for inv.
TxInventoryMessageDecodeResult DecodeTxNotFoundMessage(const CSerializedNetMsg& message,
                                                       size_t max_tx_inventory = DEFAULT_MAX_TX_INVENTORY);
TxMessageDecodeResult DecodeTxMessage(const CSerializedNetMsg& message);

} // namespace agent

#endif // QUICKSILVER_AGENT_TXMESSAGES_H
