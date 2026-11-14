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
#include <uint256.h>

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

//! F-567: what reopening a saved backup found. Each failure names the first check that failed.
enum class BackupCheckResult {
    OK,                 //!< Every check that ran passed.
    UNREADABLE,         //!< Not a readable vault database for this network.
    DIFFERENT_VAULT,    //!< It holds none of this vault's descriptors.
    OLDER_COPY,         //!< An older copy of this vault: it lacks keys or active address sets the vault uses now.
    PROTECTION_DIFFERS, //!< Same keys, but its encryption or passphrase record differs from the vault's.
    WRONG_PASSPHRASE,   //!< The file is good; the typed passphrase does not open it.
};

struct BackupCheck {
    BackupCheckResult result{BackupCheckResult::UNREADABLE};
    //! The backup is encrypted (meaningful once the file was readable).
    bool encrypted{false};
    //! A typed passphrase was tried against the backup.
    bool passphrase_tested{false};
    //! The backup's fingerprint, set once the file was readable.
    uint256 fingerprint;

    //! The file is readable, this vault's, current and equally protected (checks 1-3).
    bool StepsPassed() const { return result == BackupCheckResult::OK || result == BackupCheckResult::WRONG_PASSPHRASE; }
};

//! F-567: the backup state Home shows.
enum class BackupState {
    NEEDED,          //!< No checked backup matches the vault's keys and protection.
    NOT_CHECKED,     //!< A backup was saved before checks existed; nothing has reopened it.
    DONE,            //!< A checked backup matches the vault.
    DONE_PASSPHRASE, //!< As DONE, and the typed passphrase opened it.
};

//! F-567: the record of the last backup check, kept only in the live vault.
struct BackupRecord {
    static constexpr int CURRENT_VERSION{1};

    int64_t saved_time{0};
    bool checked{false};
    bool passphrase_checked{false};
    //! Made at load from a pre-F-567 backup_recorded flag: nothing reopened that backup, and the fingerprint is the vault's at
    //! that load, so any later change to its keys or protection makes the row Needed.
    bool legacy{false};
    uint256 fingerprint;

    SERIALIZE_METHODS(BackupRecord, obj)
    {
        int version{CURRENT_VERSION};
        READWRITE(version);
        SER_READ(obj, if (version != CURRENT_VERSION) throw std::ios_base::failure("Unsupported backup record version"));
        READWRITE(obj.saved_time, obj.checked, obj.passphrase_checked, obj.legacy, obj.fingerprint);
    }
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
