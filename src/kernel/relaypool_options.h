// Copyright (c) 2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef QUICKSILVER_KERNEL_RELAYPOOL_OPTIONS_H
#define QUICKSILVER_KERNEL_RELAYPOOL_OPTIONS_H

#include <kernel/relaypool_limits.h>

#include <policy/policy.h>

#include <chrono>
#include <cstdint>
#include <optional>

class ValidationSignals;

/** Default for -maxrelaypool, maximum megabytes of relaypool memory usage */
static constexpr unsigned int DEFAULT_MAX_RELAYPOOL_SIZE_MB{300};
/** Default for -maxrelaypool when blocksonly is set */
static constexpr unsigned int DEFAULT_BLOCKSONLY_MAX_RELAYPOOL_SIZE_MB{5};
/** Default for -relaypoolexpiry, expiration time for relaypool transactions in hours */
static constexpr unsigned int DEFAULT_RELAYPOOL_EXPIRY_HOURS{336};
/** Default for -acceptnonstdtxn */
static constexpr bool DEFAULT_ACCEPT_NON_STD_TXN{false};

namespace kernel {
/**
 * Options struct containing options for constructing a CTxRelayPool. Default
 * constructor populates the struct with sane default values which can be
 * modified.
 *
 * Most of the time, this struct should be referenced as CTxRelayPool::Options.
 */
struct RelayPoolOptions {
    /* The ratio used to determine how often sanity checks will run.  */
    int check_ratio{0};
    int64_t max_size_bytes{DEFAULT_MAX_RELAYPOOL_SIZE_MB * 1'000'000};
    std::chrono::seconds expiry{std::chrono::hours{DEFAULT_RELAYPOOL_EXPIRY_HOURS}};
    /**
     * A data carrying output is an unspendable output containing data. The script
     * type is designated as TxoutType::NULL_DATA.
     *
     * Maximum size of TxoutType::NULL_DATA scripts that this node considers standard.
     * If nullopt, any size is nonstandard.
     */
    std::optional<unsigned> max_datacarrier_bytes{DEFAULT_ACCEPT_DATACARRIER ? std::optional{MAX_OP_RETURN_RELAY} : std::nullopt};
    bool permit_bare_multisig{DEFAULT_PERMIT_BAREMULTISIG};
    bool require_standard{true};
    RelayPoolLimits limits{};

    ValidationSignals* signals{nullptr};
};
} // namespace kernel

#endif // QUICKSILVER_KERNEL_RELAYPOOL_OPTIONS_H
