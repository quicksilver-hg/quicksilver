// Copyright (c) 2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/relaypool_args.h>

#include <kernel/relaypool_limits.h>
#include <kernel/relaypool_options.h>

#include <common/args.h>
#include <common/messages.h>
#include <kernel/chainparams.h>
#include <policy/policy.h>
#include <tinyformat.h>
#include <util/translation.h>

#include <chrono>
#include <limits>

using kernel::RelayPoolLimits;
using kernel::RelayPoolOptions;

//! Maximum relaypool size on 32-bit systems.
static constexpr int MAX_32BIT_RELAYPOOL_MB{500};

namespace {
void ApplyArgsManOptions(const ArgsManager& argsman, RelayPoolLimits& relaypool_limits)
{
    relaypool_limits.ancestor_count = argsman.GetIntArg("-limitancestorcount", relaypool_limits.ancestor_count);

    if (auto vkb = argsman.GetIntArg("-limitancestorsize")) relaypool_limits.ancestor_size_vbytes = *vkb * 1'000;

    relaypool_limits.descendant_count = argsman.GetIntArg("-limitdescendantcount", relaypool_limits.descendant_count);

    if (auto vkb = argsman.GetIntArg("-limitdescendantsize")) relaypool_limits.descendant_size_vbytes = *vkb * 1'000;
}
}

util::Result<void> ApplyArgsManOptions(const ArgsManager& argsman, const CChainParams& chainparams, RelayPoolOptions& relaypool_opts)
{
    relaypool_opts.check_ratio = argsman.GetIntArg("-checkrelaypool", relaypool_opts.check_ratio);

    if (auto mb = argsman.GetIntArg("-maxrelaypool")) {
        constexpr bool is_32bit{sizeof(void*) == 4};
        if (is_32bit && *mb > MAX_32BIT_RELAYPOOL_MB) {
            return util::Error{Untranslated(strprintf("-maxrelaypool is set to %i but can't be over %i MB on 32-bit systems", *mb, MAX_32BIT_RELAYPOOL_MB))};
        }
        relaypool_opts.max_size_bytes = *mb * 1'000'000;
    }

    if (auto hours = argsman.GetIntArg("-relaypoolexpiry")) relaypool_opts.expiry = std::chrono::hours{*hours};

    relaypool_opts.permit_bare_multisig = argsman.GetBoolArg("-permitbaremultisig", DEFAULT_PERMIT_BAREMULTISIG);

    if (argsman.GetBoolArg("-datacarrier", DEFAULT_ACCEPT_DATACARRIER)) {
        const int64_t size{argsman.GetIntArg("-datacarriersize", MAX_OP_RETURN_RELAY)};
        if (size < 0 || size > std::numeric_limits<unsigned>::max()) {
            return util::Error{Untranslated("-datacarriersize must be between 0 and 4294967295")};
        }
        relaypool_opts.max_datacarrier_bytes = static_cast<unsigned>(size);
    } else {
        relaypool_opts.max_datacarrier_bytes = std::nullopt;
    }

    relaypool_opts.require_standard = !argsman.GetBoolArg("-acceptnonstdtxn", DEFAULT_ACCEPT_NON_STD_TXN);
    if (!chainparams.IsTestChain() && !relaypool_opts.require_standard) {
        return util::Error{Untranslated(strprintf("acceptnonstdtxn is not currently supported for %s chain", chainparams.GetChainTypeString()))};
    }

    ApplyArgsManOptions(argsman, relaypool_opts.limits);

    return {};
}
