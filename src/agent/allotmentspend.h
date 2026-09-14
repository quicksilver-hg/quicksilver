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
