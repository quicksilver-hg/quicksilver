// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef QUICKSILVER_AGENT_ALLOTMENTSPEND_H
#define QUICKSILVER_AGENT_ALLOTMENTSPEND_H

#include <addresstype.h>
#include <agent/allotmentpolicy.h>
#include <crypto/cuckatoo/cuckatoo.h>
#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <script/signingprovider.h>
#include <uint256.h>
#include <util/result.h>

#include <cstdint>
#include <string>
#include <vector>

class CBlockIndex;
class CKey;
class CPubKey;

namespace Consensus {
struct Params;
} // namespace Consensus

namespace agent {

//! Policy: may an agent spend fall back to the built-in CPU solver at this graph size?
//!
//! True for the sandbox graph (edgebits 19), or when the operator opts in. Mainnet and
//! publictest use the E28 graph, and vault::AllowsTxPowCpuFallback allows the CPU there
//! because a transfer is one bounded solve the sender chose and waits out. An agent spend
//! is not: it starts on the agent's schedule, on a machine whose owner may be doing
//! something else, and the grind is long enough to be felt. So this refuses that graph
//! unless the operator has opted in (-allowcputxpow, or the desktop checkbox that sets it).
//! The refusal is not about capability -- the CPU can solve the graph -- it is about
//! consent to spend someone's machine while they are using it.
constexpr bool AllowsAllotmentCpuFallback(uint8_t edgebits, bool allow_cpu_txpow)
{
    return edgebits == 19 || (edgebits == 28 && allow_cpu_txpow);
}

struct AllotmentSpendContext {
    AllotmentPolicyBundleArtifact bundle;
    FlatSigningProvider signing_provider;
    CTxDestination funding_destination;
    CScript funding_script;
};

struct AllotmentSpendRequest {
    COutPoint prevout;
    CAmount prevout_value{0};
    CTxDestination destination;
    CAmount spend_amount{0};
    CAmount spent_today{0};
    bool prove{true};
    const CBlockIndex* anchor{nullptr};
    //! Polled between grind attempts and passed down to the solver. The prove
    //! loop is otherwise unbounded -- each iteration starts a fresh 2^24-attempt
    //! budget -- so without this there is no way to stop it at all.
    cuckatoo::SolverCancelCallback cancel{};
    //! Operator opt-in to CPU proving at a real graph size. Defaults to refusing: an agent
    //! spend starts without a human present, so the machine's owner has to have said yes
    //! first. See AllowsAllotmentCpuFallback.
    bool allow_cpu_txpow{false};
};

struct AllotmentBundleSpendRequest {
    CTxDestination destination;
    CAmount spend_amount{0};
    CAmount spent_today{0};
    bool prove{true};
    const CBlockIndex* anchor{nullptr};
    //! Polled between grind attempts and passed down to the solver. The prove
    //! loop is otherwise unbounded -- each iteration starts a fresh 2^24-attempt
    //! budget -- so without this there is no way to stop it at all.
    cuckatoo::SolverCancelCallback cancel{};
    //! Operator opt-in to CPU proving at a real graph size. Defaults to refusing: an agent
    //! spend starts without a human present, so the machine's owner has to have said yes
    //! first. See AllowsAllotmentCpuFallback.
    bool allow_cpu_txpow{false};
};

struct AllotmentSpendInput {
    COutPoint prevout;
    CAmount amount{0};
};

struct AllotmentSignedSpend {
    CTransaction transaction;
    AllotmentPolicyCheck policy_check;
    CAmount change_amount{0};
    CAmount input_amount{0};
    std::vector<AllotmentSpendInput> inputs;
};

util::Result<AllotmentSpendContext> ImportAllotmentBundle(std::string_view bundle_json,
                                                              std::string_view expected_chain,
                                                              std::string_view expected_genesis_hash);

util::Result<AllotmentSignedSpend> CreateSignedAllotmentSpend(const AllotmentSpendContext& context,
                                                                  const AllotmentSpendRequest& request,
                                                                  const Consensus::Params& consensus);
util::Result<AllotmentSignedSpend> CreateSignedAllotmentSpendFromBundleOutputs(const AllotmentSpendContext& context,
                                                                                   const AllotmentBundleSpendRequest& request,
                                                                                   const Consensus::Params& consensus);

} // namespace agent

#endif // QUICKSILVER_AGENT_ALLOTMENTSPEND_H
