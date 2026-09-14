// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef QUICKSILVER_AGENT_ALLOTMENTPOLICY_H
#define QUICKSILVER_AGENT_ALLOTMENTPOLICY_H

#include <consensus/amount.h>
#include <util/result.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace agent {

enum class AllotmentPolicyResultCode {
    ALLOWED,
    INVALID_POLICY,
    INVALID_AMOUNT,
    INSUFFICIENT_FUNDS,
    DAILY_LIMIT_EXCEEDED,
};

struct AllotmentPolicy {
    CAmount funding_limit{0};
    CAmount daily_limit{0};
};

struct AllotmentPolicyState {
    CAmount funding_available{0};
    CAmount spent_today{0};
};

struct AllotmentPolicyArtifact {
    std::string id;
    std::string label;
    std::string chain;
    std::string genesis_hash;
    std::string funding_address;
    AllotmentPolicy policy;
    CAmount funding_available{0};
    int64_t risk_accepted_time{0};
    int64_t request_created_time{0};
    std::string policy_status;
    bool backend_created{false};
};

struct AllotmentFundingOutputArtifact {
    std::string txid;
    uint32_t vout{0};
    CAmount amount{0};
};

struct AllotmentPolicyBundleArtifact {
    AllotmentPolicyArtifact policy_request;
    std::string policy_request_json;
    std::string funding_address;
    std::string funding_secret;
    std::string policy_enforcement;
    std::vector<AllotmentFundingOutputArtifact> funding_outputs;
};

struct AllotmentPaymentReceiptArtifact {
    std::string chain;
    std::string genesis_hash;
    std::string funding_address;
    AllotmentFundingOutputArtifact funding_output;
    int64_t received_time{0};
    std::string payment_id;
    std::string label;
    std::string memo;
    std::string payer;
};

struct AllotmentPolicyCheck {
    AllotmentPolicyResultCode code{AllotmentPolicyResultCode::INVALID_POLICY};
    CAmount daily_remaining{0};

    bool allowed() const { return code == AllotmentPolicyResultCode::ALLOWED; }
};

const char* AllotmentPolicyResultCodeString(AllotmentPolicyResultCode code);
util::Result<AllotmentPolicyArtifact> DecodeAllotmentPolicyRequest(std::string_view request_json,
                                                                       std::string_view expected_chain,
                                                                       std::string_view expected_genesis_hash);
util::Result<AllotmentPolicyBundleArtifact> DecodeAllotmentPolicyBundle(std::string_view bundle_json,
                                                                            std::string_view expected_chain,
                                                                            std::string_view expected_genesis_hash);
util::Result<AllotmentPaymentReceiptArtifact> DecodeAllotmentPaymentReceipt(std::string_view receipt_json,
                                                                                std::string_view expected_chain,
                                                                                std::string_view expected_genesis_hash,
                                                                                std::string_view expected_funding_address = {});
AllotmentPolicyCheck CheckAllotmentSpend(const AllotmentPolicy& policy,
                                             const AllotmentPolicyState& state,
                                             CAmount amount);

} // namespace agent

#endif // QUICKSILVER_AGENT_ALLOTMENTPOLICY_H
