// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <agent/allotmentpolicy.h>

#include <consensus/amount.h>
#include <tinyformat.h>
#include <uint256.h>
#include <univalue.h>
#include <util/result.h>
#include <util/strencodings.h>
#include <util/translation.h>

#include <cassert>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agent {

const char* AllotmentPolicyResultCodeString(AllotmentPolicyResultCode code)
{
    switch (code) {
    case AllotmentPolicyResultCode::ALLOWED:
        return "allowed";
    case AllotmentPolicyResultCode::INVALID_POLICY:
        return "invalid-policy";
    case AllotmentPolicyResultCode::INVALID_AMOUNT:
        return "invalid-amount";
    case AllotmentPolicyResultCode::INSUFFICIENT_FUNDS:
        return "insufficient-funds";
    case AllotmentPolicyResultCode::DAILY_LIMIT_EXCEEDED:
        return "daily-limit-exceeded";
    } // no default case so the compiler will warn when a new code is added
    assert(false);
}

static util::Result<std::string> ReadPolicyStringField(const UniValue& policy, const std::string& key)
{
    const UniValue& field{policy.find_value(key)};
    if (!field.isStr()) {
        return util::Error{Untranslated(strprintf("Agent allotment policy request field '%s' must be a string.", key))};
    }
    if (field.get_str().empty()) {
        return util::Error{Untranslated(strprintf("Agent allotment policy request field '%s' must not be empty.", key))};
    }
    return field.get_str();
}

static util::Result<std::string> ReadOptionalPolicyStringField(const UniValue& policy, const std::string& key)
{
    const UniValue& field{policy.find_value(key)};
    if (field.isNull()) return std::string{};
    if (!field.isStr()) {
        return util::Error{Untranslated(strprintf("Agent allotment policy request field '%s' must be a string.", key))};
    }
    return field.get_str();
}

static util::Result<CAmount> ReadPolicyAmountField(const UniValue& policy, const std::string& key)
{
    auto field{ReadPolicyStringField(policy, key)};
    if (!field) return util::Error{util::ErrorString(field)};

    const auto amount{ToIntegral<CAmount>(*field)};
    if (!amount || !MoneyRange(*amount)) {
        return util::Error{Untranslated(strprintf("Agent allotment policy request field '%s' must be a valid nonnegative cinnabar amount.", key))};
    }
    return *amount;
}

static util::Result<int64_t> ReadPolicyTimeField(const UniValue& policy, const std::string& key)
{
    auto field{ReadPolicyStringField(policy, key)};
    if (!field) return util::Error{util::ErrorString(field)};

    const auto time{ToIntegral<int64_t>(*field)};
    if (!time || *time < 0) {
        return util::Error{Untranslated(strprintf("Agent allotment policy request field '%s' must be a valid nonnegative time.", key))};
    }
    return *time;
}

static util::Result<bool> ReadPolicyBoolField(const UniValue& policy, const std::string& key)
{
    const UniValue& field{policy.find_value(key)};
    if (!field.isBool()) {
        return util::Error{Untranslated(strprintf("Agent allotment policy request field '%s' must be a boolean.", key))};
    }
    return field.get_bool();
}

static util::Result<std::vector<AllotmentFundingOutputArtifact>> ReadBundleFundingOutputs(const UniValue& bundle)
{
    const UniValue& outputs{bundle.find_value("funding_outputs")};
    if (outputs.isNull()) {
        return std::vector<AllotmentFundingOutputArtifact>{};
    }
    if (!outputs.isArray()) {
        return util::Error{Untranslated("Agent allotment policy bundle field 'funding_outputs' must be an array.")};
    }

    std::vector<AllotmentFundingOutputArtifact> funding_outputs;
    funding_outputs.reserve(outputs.size());
    for (const UniValue& output : outputs.get_array().getValues()) {
        if (!output.isObject()) {
            return util::Error{Untranslated("Agent allotment policy bundle funding output must be an object.")};
        }

        auto txid{ReadPolicyStringField(output, "txid")};
        if (!txid) return util::Error{util::ErrorString(txid)};
        if (!uint256::FromHex(*txid)) {
            return util::Error{Untranslated("Agent allotment policy bundle funding output txid must be a transaction id hex string.")};
        }

        const UniValue& vout{output.find_value("vout")};
        const auto vout_index{vout.isNum() ? ToIntegral<uint32_t>(vout.getValStr()) : std::optional<uint32_t>{}};
        if (!vout_index.has_value()) {
            return util::Error{Untranslated("Agent allotment policy bundle funding output vout must be a uint32 value.")};
        }

        auto amount{ReadPolicyAmountField(output, "amount_cinnabar")};
        if (!amount) return util::Error{util::ErrorString(amount)};
        if (*amount <= 0) {
            return util::Error{Untranslated("Agent allotment policy bundle funding output amount must be greater than zero.")};
        }

        funding_outputs.push_back(AllotmentFundingOutputArtifact{
            .txid = *txid,
            .vout = *vout_index,
            .amount = *amount,
        });
    }
    return funding_outputs;
}

static util::Result<AllotmentFundingOutputArtifact> ReadPaymentReceiptFundingOutput(const UniValue& receipt)
{
    auto txid{ReadPolicyStringField(receipt, "txid")};
    if (!txid) return util::Error{util::ErrorString(txid)};
    if (!uint256::FromHex(*txid)) {
        return util::Error{Untranslated("Agent allotment payment receipt txid must be a transaction id hex string.")};
    }

    const UniValue& vout{receipt.find_value("vout")};
    const auto vout_index{vout.isNum() ? ToIntegral<uint32_t>(vout.getValStr()) : std::optional<uint32_t>{}};
    if (!vout_index.has_value()) {
        return util::Error{Untranslated("Agent allotment payment receipt vout must be a uint32 value.")};
    }

    auto amount{ReadPolicyAmountField(receipt, "amount_cinnabar")};
    if (!amount) return util::Error{util::ErrorString(amount)};
    if (*amount <= 0) {
        return util::Error{Untranslated("Agent allotment payment receipt amount must be greater than zero.")};
    }

    return AllotmentFundingOutputArtifact{
        .txid = *txid,
        .vout = *vout_index,
        .amount = *amount,
    };
}

util::Result<AllotmentPolicyArtifact> DecodeAllotmentPolicyRequest(std::string_view request_json,
                                                                       std::string_view expected_chain,
                                                                       std::string_view expected_genesis_hash)
{
    UniValue policy{UniValue::VOBJ};
    if (!policy.read(std::string{request_json}) || !policy.isObject()) {
        return util::Error{Untranslated("Agent allotment policy request must be a JSON object.")};
    }

    auto type{ReadPolicyStringField(policy, "type")};
    if (!type) return util::Error{util::ErrorString(type)};
    if (*type != "quicksilver.agent_allotment_policy_request") {
        return util::Error{Untranslated("Agent allotment policy request has an unknown type.")};
    }

    const UniValue& version{policy.find_value("version")};
    const auto policy_version{version.isNum() ? ToIntegral<int>(version.getValStr()) : std::optional<int>{}};
    if (!policy_version || *policy_version != 1) {
        return util::Error{Untranslated("Agent allotment policy request version is not supported.")};
    }

    AllotmentPolicyArtifact artifact;
    auto chain{ReadPolicyStringField(policy, "chain")};
    if (!chain) return util::Error{util::ErrorString(chain)};
    if (*chain != expected_chain) {
        return util::Error{Untranslated("Agent allotment policy request is for a different chain.")};
    }
    artifact.chain = *chain;

    auto genesis_hash{ReadPolicyStringField(policy, "genesis_hash")};
    if (!genesis_hash) return util::Error{util::ErrorString(genesis_hash)};
    if (*genesis_hash != expected_genesis_hash) {
        return util::Error{Untranslated("Agent allotment policy request is for a different genesis block.")};
    }
    artifact.genesis_hash = *genesis_hash;

    auto id{ReadPolicyStringField(policy, "id")};
    if (!id) return util::Error{util::ErrorString(id)};
    artifact.id = *id;

    auto label{ReadPolicyStringField(policy, "label")};
    if (!label) return util::Error{util::ErrorString(label)};
    artifact.label = *label;

    auto funding_address{ReadPolicyStringField(policy, "funding_address")};
    if (!funding_address) return util::Error{util::ErrorString(funding_address)};
    artifact.funding_address = *funding_address;

    auto funding_limit{ReadPolicyAmountField(policy, "funding_limit_cinnabar")};
    if (!funding_limit) return util::Error{util::ErrorString(funding_limit)};
    if (*funding_limit == 0) {
        return util::Error{Untranslated("Agent allotment policy request funding limit must be greater than zero.")};
    }
    artifact.policy.funding_limit = *funding_limit;

    auto funding_available{ReadPolicyAmountField(policy, "funding_available_cinnabar")};
    if (!funding_available) return util::Error{util::ErrorString(funding_available)};
    artifact.funding_available = *funding_available;

    auto daily_limit{ReadPolicyAmountField(policy, "daily_limit_cinnabar")};
    if (!daily_limit) return util::Error{util::ErrorString(daily_limit)};
    artifact.policy.daily_limit = *daily_limit;

    auto risk_accepted_time{ReadPolicyTimeField(policy, "risk_accepted_time")};
    if (!risk_accepted_time) return util::Error{util::ErrorString(risk_accepted_time)};
    artifact.risk_accepted_time = *risk_accepted_time;

    auto request_created_time{ReadPolicyTimeField(policy, "request_created_time")};
    if (!request_created_time) return util::Error{util::ErrorString(request_created_time)};
    artifact.request_created_time = *request_created_time;

    auto policy_status{ReadPolicyStringField(policy, "policy_status")};
    if (!policy_status) return util::Error{util::ErrorString(policy_status)};
    if (*policy_status != "pending_integration" && *policy_status != "enforced") {
        return util::Error{Untranslated("Agent allotment policy request has an unknown policy_status.")};
    }
    artifact.policy_status = *policy_status;

    auto backend_created{ReadPolicyBoolField(policy, "backend_created")};
    if (!backend_created) return util::Error{util::ErrorString(backend_created)};
    artifact.backend_created = *backend_created;

    return artifact;
}

util::Result<AllotmentPolicyBundleArtifact> DecodeAllotmentPolicyBundle(std::string_view bundle_json,
                                                                            std::string_view expected_chain,
                                                                            std::string_view expected_genesis_hash)
{
    UniValue bundle{UniValue::VOBJ};
    if (!bundle.read(std::string{bundle_json}) || !bundle.isObject()) {
        return util::Error{Untranslated("Agent allotment policy bundle must be a JSON object.")};
    }

    auto type{ReadPolicyStringField(bundle, "type")};
    if (!type) return util::Error{util::ErrorString(type)};
    if (*type != "quicksilver.agent_allotment_key_bundle") {
        return util::Error{Untranslated("Agent allotment policy bundle has an unknown type.")};
    }

    const UniValue& version{bundle.find_value("version")};
    const auto bundle_version{version.isNum() ? ToIntegral<int>(version.getValStr()) : std::optional<int>{}};
    if (!bundle_version || *bundle_version != 1) {
        return util::Error{Untranslated("Agent allotment policy bundle version is not supported.")};
    }

    const UniValue& policy_request{bundle.find_value("policy_request")};
    if (!policy_request.isObject()) {
        return util::Error{Untranslated("Agent allotment policy bundle field 'policy_request' must be an object.")};
    }
    const std::string policy_request_json{policy_request.write()};
    auto policy_artifact{DecodeAllotmentPolicyRequest(policy_request_json, expected_chain, expected_genesis_hash)};
    if (!policy_artifact) return util::Error{util::ErrorString(policy_artifact)};

    auto funding_address{ReadPolicyStringField(bundle, "funding_address")};
    if (!funding_address) return util::Error{util::ErrorString(funding_address)};
    if (*funding_address != policy_artifact->funding_address) {
        return util::Error{Untranslated("Agent allotment policy bundle funding address does not match the policy request.")};
    }

    auto funding_secret{ReadPolicyStringField(bundle, "funding_secret_wif")};
    if (!funding_secret) return util::Error{util::ErrorString(funding_secret)};

    auto policy_enforcement{ReadPolicyStringField(bundle, "policy_enforcement")};
    if (!policy_enforcement) return util::Error{util::ErrorString(policy_enforcement)};
    if (*policy_enforcement != "pending_integration" && *policy_enforcement != "enforced") {
        return util::Error{Untranslated("Agent allotment policy bundle has an unknown policy_enforcement.")};
    }
    if (*policy_enforcement != policy_artifact->policy_status) {
        return util::Error{Untranslated("Agent allotment policy bundle enforcement state does not match the policy request.")};
    }

    auto funding_outputs{ReadBundleFundingOutputs(bundle)};
    if (!funding_outputs) return util::Error{util::ErrorString(funding_outputs)};

    AllotmentPolicyBundleArtifact artifact;
    artifact.policy_request = *policy_artifact;
    artifact.policy_request_json = policy_request_json;
    artifact.funding_address = *funding_address;
    artifact.funding_secret = *funding_secret;
    artifact.policy_enforcement = *policy_enforcement;
    artifact.funding_outputs = *funding_outputs;
    return artifact;
}

util::Result<AllotmentPaymentReceiptArtifact> DecodeAllotmentPaymentReceipt(std::string_view receipt_json,
                                                                                std::string_view expected_chain,
                                                                                std::string_view expected_genesis_hash,
                                                                                std::string_view expected_funding_address)
{
    UniValue receipt{UniValue::VOBJ};
    if (!receipt.read(std::string{receipt_json}) || !receipt.isObject()) {
        return util::Error{Untranslated("Agent allotment payment receipt must be a JSON object.")};
    }

    auto type{ReadPolicyStringField(receipt, "type")};
    if (!type) return util::Error{util::ErrorString(type)};
    if (*type != "quicksilver.agent_payment_receipt") {
        return util::Error{Untranslated("Agent allotment payment receipt has an unknown type.")};
    }

    const UniValue& version{receipt.find_value("version")};
    const auto receipt_version{version.isNum() ? ToIntegral<int>(version.getValStr()) : std::optional<int>{}};
    if (!receipt_version || *receipt_version != 1) {
        return util::Error{Untranslated("Agent allotment payment receipt version is not supported.")};
    }

    AllotmentPaymentReceiptArtifact artifact;
    auto chain{ReadPolicyStringField(receipt, "chain")};
    if (!chain) return util::Error{util::ErrorString(chain)};
    if (*chain != expected_chain) {
        return util::Error{Untranslated("Agent allotment payment receipt is for a different chain.")};
    }
    artifact.chain = *chain;

    auto genesis_hash{ReadPolicyStringField(receipt, "genesis_hash")};
    if (!genesis_hash) return util::Error{util::ErrorString(genesis_hash)};
    if (*genesis_hash != expected_genesis_hash) {
        return util::Error{Untranslated("Agent allotment payment receipt is for a different genesis block.")};
    }
    artifact.genesis_hash = *genesis_hash;

    auto funding_address{ReadPolicyStringField(receipt, "funding_address")};
    if (!funding_address) return util::Error{util::ErrorString(funding_address)};
    if (!expected_funding_address.empty() && *funding_address != expected_funding_address) {
        return util::Error{Untranslated("Agent allotment payment receipt funding address does not match the bundle.")};
    }
    artifact.funding_address = *funding_address;

    auto funding_output{ReadPaymentReceiptFundingOutput(receipt)};
    if (!funding_output) return util::Error{util::ErrorString(funding_output)};
    artifact.funding_output = *funding_output;

    auto received_time{ReadPolicyTimeField(receipt, "received_time")};
    if (!received_time) return util::Error{util::ErrorString(received_time)};
    artifact.received_time = *received_time;

    auto payment_id{ReadOptionalPolicyStringField(receipt, "payment_id")};
    if (!payment_id) return util::Error{util::ErrorString(payment_id)};
    artifact.payment_id = *payment_id;

    auto label{ReadOptionalPolicyStringField(receipt, "label")};
    if (!label) return util::Error{util::ErrorString(label)};
    artifact.label = *label;

    auto memo{ReadOptionalPolicyStringField(receipt, "memo")};
    if (!memo) return util::Error{util::ErrorString(memo)};
    artifact.memo = *memo;

    auto payer{ReadOptionalPolicyStringField(receipt, "payer")};
    if (!payer) return util::Error{util::ErrorString(payer)};
    artifact.payer = *payer;

    return artifact;
}

static bool ValidNonnegativeAmount(CAmount amount)
{
    return amount >= 0 && MoneyRange(amount);
}

AllotmentPolicyCheck CheckAllotmentSpend(const AllotmentPolicy& policy,
                                             const AllotmentPolicyState& state,
                                             CAmount amount)
{
    if (policy.funding_limit <= 0 ||
        !MoneyRange(policy.funding_limit) ||
        !ValidNonnegativeAmount(policy.daily_limit) ||
        !ValidNonnegativeAmount(state.funding_available) ||
        !ValidNonnegativeAmount(state.spent_today)) {
        return {AllotmentPolicyResultCode::INVALID_POLICY, 0};
    }

    if (amount <= 0 || !MoneyRange(amount)) {
        return {AllotmentPolicyResultCode::INVALID_AMOUNT, 0};
    }

    if (amount > state.funding_available) {
        return {AllotmentPolicyResultCode::INSUFFICIENT_FUNDS, 0};
    }

    if (policy.daily_limit == 0) {
        return {AllotmentPolicyResultCode::ALLOWED, 0};
    }

    const CAmount daily_remaining{state.spent_today >= policy.daily_limit ? 0 : policy.daily_limit - state.spent_today};
    if (amount > daily_remaining) {
        return {AllotmentPolicyResultCode::DAILY_LIMIT_EXCEEDED, daily_remaining};
    }

    return {AllotmentPolicyResultCode::ALLOWED, daily_remaining - amount};
}

} // namespace agent
