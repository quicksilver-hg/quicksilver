// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//! @file vault/types.h is a home for public enum and struct type definitions
//! that are used by internally by vault code, but also used externally by node
//! or GUI code.
//!
//! This file is intended to define only simple types that do not have external
//! dependencies. More complicated public vault types like CCoinControl should
//! be defined in dedicated header files.

#ifndef QUICKSILVER_VAULT_TYPES_H
#define QUICKSILVER_VAULT_TYPES_H

#include <consensus/amount.h>
#include <serialize.h>

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace vault {
/**
 * IsMine() return codes.
 *
 * ISMINE_NO: the scriptPubKey is not in the vault;
 * ISMINE_SPENDABLE: the scriptPubKey matches a scriptPubKey in the vault.
 *
 * ISMINE_USED is never returned by IsMine(); it is a filter-only bit that
 * opts into counting already-used addresses when VAULT_FLAG_AVOID_REUSE is set.
 */
enum isminetype : unsigned int {
    ISMINE_NO = 0,
    ISMINE_SPENDABLE = 1 << 0,
    ISMINE_USED = 1 << 1,
    //! One past the largest representable filter combination. This sizes the
    //! per-filter caches in CachableAmount, so keep it stated explicitly
    //! rather than leaning on the implicit increment of the last enumerator.
    ISMINE_ENUM_ELEMENTS = (ISMINE_SPENDABLE | ISMINE_USED) + 1,
};
/** used for bitflags of isminetype */
using isminefilter = std::underlying_type<isminetype>::type;

/**
 * Address purpose field that has been been stored with vault sending and
 * receiving addresses. This field is not currently
 * used for any logic inside the vault, but it is still shown in RPC and GUI
 * interfaces and saved for new addresses. It is basically redundant with an
 * address's IsMine() result.
 */
enum class AddressPurpose {
    RECEIVE,
    SEND,
};

struct AgentAllotmentRecord {
    static constexpr int CURRENT_VERSION{2};

    int version{CURRENT_VERSION};
    std::string id;
    std::string label;
    CAmount funding_limit{0};
    CAmount daily_limit{0};
    int64_t risk_accepted_time{0};
    std::string funding_address;
    //! Public descriptor, with checksum. The vault holds the private keys.
    std::string funding_descriptor;
    //! 0 while the allotment is active. A nonzero value is the time it was stopped.
    int64_t stopped_time{0};

    SERIALIZE_METHODS(AgentAllotmentRecord, obj)
    {
        int version{CURRENT_VERSION};
        READWRITE(version);
        SER_READ(obj, if (version != CURRENT_VERSION) throw std::ios_base::failure("Unsupported agent allotment record version"));
        READWRITE(obj.id, obj.label, obj.funding_limit, obj.daily_limit, obj.risk_accepted_time, obj.funding_address, obj.funding_descriptor, obj.stopped_time);
        SER_READ(obj, obj.version = CURRENT_VERSION);
    }
};

struct AgentAllotmentPolicyRequestMetadata {
    std::string id;
    std::string label;
    std::string funding_address;
    std::string funding_descriptor;
    CAmount funding_limit{0};
    CAmount funding_available{0};
    CAmount daily_limit{0};
    int64_t risk_accepted_time{0};
    int64_t request_created_time{0};
};

struct AgentAllotmentFundingOutput {
    std::string txid;
    uint32_t vout{0};
    CAmount amount{0};
};

struct AgentAllotmentPolicyBundle {
    AgentAllotmentPolicyRequestMetadata metadata;
    std::string policy_request;
    //! Private key of the agent's key inside the funding descriptor. Not stored on the record.
    std::string agent_secret;
    std::string funding_descriptor;
    std::vector<AgentAllotmentFundingOutput> funding_outputs;
    std::string bundle_json;
};
} // namespace vault

#endif // QUICKSILVER_VAULT_TYPES_H
