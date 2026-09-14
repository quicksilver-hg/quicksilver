// Copyright (c) 2016-present The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <kernel/relaypool_removal_reason.h>

#include <cassert>
#include <string>

std::string RemovalReasonToString(const RelayPoolRemovalReason& r) noexcept
{
    switch (r) {
        case RelayPoolRemovalReason::EXPIRY: return "expiry";
        case RelayPoolRemovalReason::SIZELIMIT: return "sizelimit";
        case RelayPoolRemovalReason::REORG: return "reorg";
        case RelayPoolRemovalReason::BLOCK: return "block";
        case RelayPoolRemovalReason::CONFLICT: return "conflict";
        case RelayPoolRemovalReason::REPLACED: return "replaced";
        case RelayPoolRemovalReason::STALE_ANCHOR: return "stale anchor";
    }
    assert(false);
}
