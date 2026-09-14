// Copyright (c) 2016-present The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef QUICKSILVER_KERNEL_RELAYPOOL_REMOVAL_REASON_H
#define QUICKSILVER_KERNEL_RELAYPOOL_REMOVAL_REASON_H

#include <string>

/** Reason why a transaction was removed from the relaypool,
 * this is passed to the notification signal.
 */
enum class RelayPoolRemovalReason {
    EXPIRY,      //!< Expired from relaypool
    SIZELIMIT,   //!< Removed in size limiting
    REORG,       //!< Removed for reorganization
    BLOCK,       //!< Removed for block
    CONFLICT,    //!< Removed for conflict with in-block transaction
    REPLACED,    //!< Removed for replacement
    STALE_ANCHOR, //!< Per-tx PoW anchor aged out of the recency window (#7)
};

std::string RemovalReasonToString(const RelayPoolRemovalReason& r) noexcept;

#endif // QUICKSILVER_KERNEL_RELAYPOOL_REMOVAL_REASON_H
