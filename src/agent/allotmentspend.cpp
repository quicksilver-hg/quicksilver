// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <agent/allotmentspend.h>

#include <addresstype.h>
#include <arith_uint256.h>
#include <chain.h>
#include <coins.h>
#include <consensus/amount.h>
#include <consensus/params.h>
#include <crypto/cuckatoo/cuckatoo.h>
#include <crypto/cuckatoo/gpu_solver.h>
#include <key.h>
#include <key_io.h>
#include <pow.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <tinyformat.h>
#include <util/result.h>
#include <util/translation.h>

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace agent {
namespace {

util::Result<void> AddFundingKey(AllotmentSpendContext& context, const std::string& funding_secret)
{
    const CKey key{DecodeSecret(funding_secret)};
    if (!key.IsValid()) {
        return util::Error{Untranslated("funding_secret_wif is not a valid private key for this chain")};
    }

    const CPubKey pubkey{key.GetPubKey()};
    const CKeyID key_id{pubkey.GetID()};
    if (const auto* pkhash{std::get_if<PKHash>(&context.funding_destination)}) {
        if (ToKeyID(*pkhash) != key_id) {
            return util::Error{Untranslated("funding_secret_wif does not match the bundle funding address")};
        }
    } else if (const auto* witness_hash{std::get_if<WitnessV0KeyHash>(&context.funding_destination)}) {
        if (ToKeyID(*witness_hash) != key_id) {
            return util::Error{Untranslated("funding_secret_wif does not match the bundle funding address")};
        }
    } else {
        return util::Error{Untranslated("funding_address does not map to an importable single-key agent address")};
    }

    context.signing_provider.keys.emplace(key_id, key);
    context.signing_provider.pubkeys.emplace(key_id, pubkey);
    return {};
}

util::Result<void> ProveAgentSpend(CMutableTransaction& transaction,
                                   const CBlockIndex* anchor,
                                   const Consensus::Params& consensus,
                                   const cuckatoo::SolverCancelCallback& cancel)
{
    if (anchor == nullptr || anchor->nHeight < 0) {
        return util::Error{Untranslated("Per-transaction proof-of-work requires a validated anchor.")};
    }
    // F-51: this used to refuse outright, because a header-only chain carried no
    // congestion multiplier and proving against the wrong target is worse than not
    // proving at all. nCongestion is now a header field (#5c-1 Phase 2), so the anchor
    // index carries the real m. See doc/design/agent-client.md for what that is and is
    // not worth: the value is unvalidated SPV-grade — a full node would have rejected
    // the block at connect if it were wrong — and both failure directions are bounded
    // (too high wastes work, too low gets the transaction rejected; neither loses funds).
    transaction.nAnchorHeight = static_cast<uint32_t>(anchor->nHeight);

    // Required work is per-transaction and moves with the serialized bytes, so the
    // target has to be taken from this transaction rather than the chain alone. The
    // caller grinds after signing, so the bytes charged for already exist and no
    // upper-bound estimate is needed: the pre-image covers neither scriptSig nor the
    // witness, so a proof found now stays valid over what was already signed.
    const arith_uint256 target{
        UintToArith256(GetTxPowTarget(consensus, anchor, CTransaction(transaction)))};

    const std::vector<unsigned char> preimage{CTransaction(transaction).PowPreimage(anchor->GetBlockHash())};
    cuckatoo::Cycle cycle{};
    uint32_t nonce{0};
    const bool cpu_fallback{consensus.nTxEdgeBits == 19};
    uint32_t start_nonce{0};
    while (true) {
        // Checked at the top of every iteration, not only inside the solver: this
        // loop is unbounded -- each pass starts a fresh 2^24-attempt budget and it
        // exits only on a cycle that beats the target -- so a cancel arriving
        // between attempts would otherwise be swallowed and the grind continue.
        if (cancel && cancel()) {
            return util::Error{Untranslated("Per-transaction proof-of-work was cancelled.")};
        }
        cuckatoo::GpuSolveStatus solver_status{cuckatoo::GpuSolveStatus::kNoCycle};
        if (!cuckatoo::CuckatooSolveBytes(preimage.data(), preimage.size(), consensus.nTxEdgeBits, start_nonce, 1u << 24, cycle, nonce, cpu_fallback,
                                          /*progress=*/{}, cancel, &solver_status)) {
            // A cancelled solve is not a failure to find a proof, and SolverFault()
            // deliberately returns nullopt for it -- so without this the operator
            // would be told the grind could not produce a proof.
            if (solver_status == cuckatoo::GpuSolveStatus::kCancelled) {
                return util::Error{Untranslated("Per-transaction proof-of-work was cancelled.")};
            }
            // "No solver configured" and "the solver ran and failed" used to read
            // identically, which sent an operator with a working -cuckatoosolver
            // and a dead card looking for a configuration mistake.
            if (const auto fault = cuckatoo::SolverFault(solver_status, cpu_fallback)) {
                return util::Error{Untranslated("Per-transaction proof-of-work failed. " + *fault)};
            }
            return util::Error{Untranslated("Could not produce per-tx proof-of-work")};
        }
        // A cycle is necessary but not sufficient: it must also meet the target this
        // transaction's size implies. Required work was 1 before the Stage 2 flag day,
        // so the first cycle always passed; it does not now, and a single attempt
        // would fail almost every time.
        if (UintToArith256(cuckatoo::CuckatooProofHash(cycle)) <= target) break;
        if (nonce == std::numeric_limits<uint32_t>::max()) {
            return util::Error{Untranslated("Could not produce per-tx proof-of-work")};
        }
        start_nonce = nonce + 1;
    }

    transaction.nCycle = cycle;
    transaction.nPowNonce = nonce;
    return {};
}

util::Result<AllotmentSignedSpend> CreateSignedAllotmentSpendWithInputs(const AllotmentSpendContext& context,
                                                                            const std::vector<AllotmentSpendInput>& inputs,
                                                                            const CTxDestination& destination,
                                                                            CAmount spend_amount,
                                                                            CAmount spent_today,
                                                                            bool prove,
                                                                            const CBlockIndex* anchor,
                                                                            const Consensus::Params& consensus,
                                                                            const cuckatoo::SolverCancelCallback& cancel)
{
    if (inputs.empty()) {
        return util::Error{Untranslated("Agent allotment spend requires at least one funding output.")};
    }
    if (!IsValidDestination(destination)) {
        return util::Error{Untranslated("Agent allotment spend destination is not valid for this chain.")};
    }

    const AllotmentPolicyState state{
        .funding_available = context.bundle.policy_request.funding_available,
        .spent_today = spent_today,
    };
    const AllotmentPolicyCheck check{CheckAllotmentSpend(context.bundle.policy_request.policy, state, spend_amount)};
    if (!check.allowed()) {
        return util::Error{Untranslated(strprintf("Agent allotment policy rejected spend: %s", AllotmentPolicyResultCodeString(check.code)))};
    }

    std::set<COutPoint> seen_inputs;
    CAmount input_amount{0};
    for (const AllotmentSpendInput& input : inputs) {
        if (input.prevout.IsNull()) {
            return util::Error{Untranslated("Agent allotment spend prevout must not be null.")};
        }
        if (!seen_inputs.insert(input.prevout).second) {
            return util::Error{Untranslated("Agent allotment spend must not include duplicate funding outputs.")};
        }
        if (input.amount <= 0 || !MoneyRange(input.amount)) {
            return util::Error{Untranslated("Agent allotment spend prevout amount must be a valid positive cinnabar amount.")};
        }
        if (!MoneyRange(input_amount + input.amount)) {
            return util::Error{Untranslated("Agent allotment spend input amount is out of range.")};
        }
        input_amount += input.amount;
    }
    if (spend_amount > input_amount) {
        return util::Error{Untranslated("Agent allotment spend amount exceeds the selected funding outputs.")};
    }

    CMutableTransaction transaction;
    transaction.vin.reserve(inputs.size());
    for (const AllotmentSpendInput& input : inputs) {
        transaction.vin.emplace_back(input.prevout);
    }
    transaction.vout.emplace_back(spend_amount, GetScriptForDestination(destination));

    const CAmount change_amount{input_amount - spend_amount};
    if (change_amount > 0) {
        transaction.vout.emplace_back(change_amount, context.funding_script);
    }

    std::map<COutPoint, Coin> coins;
    for (const AllotmentSpendInput& input : inputs) {
        coins.emplace(input.prevout, Coin(CTxOut(input.amount, context.funding_script), /*nHeightIn=*/1, /*fCoinBaseIn=*/false));
    }
    std::map<int, bilingual_str> input_errors;
    if (!SignTransaction(transaction, &context.signing_provider, coins, SIGHASH_ALL, input_errors)) {
        if (!input_errors.empty()) {
            return util::Error{Untranslated(strprintf("Agent allotment signing failed for input %d: %s", input_errors.begin()->first, input_errors.begin()->second.original))};
        }
        return util::Error{Untranslated("Agent allotment signing failed.")};
    }

    // Prove after signing, not before. Required work now depends on the serialized
    // size, and scriptSig and the witness are part of that size but are deliberately
    // outside the proof pre-image. Grinding here charges the true final size instead
    // of an estimate, and cannot invalidate the signatures just made.
    if (prove) {
        auto proof_result{ProveAgentSpend(transaction, anchor, consensus, cancel)};
        if (!proof_result) return util::Error{util::ErrorString(proof_result)};
    }

    return AllotmentSignedSpend{
        .transaction = CTransaction{transaction},
        .policy_check = check,
        .change_amount = change_amount,
        .input_amount = input_amount,
        .inputs = inputs,
    };
}

util::Result<std::vector<AllotmentSpendInput>> SelectBundleFundingOutputs(const AllotmentSpendContext& context,
                                                                            CAmount spend_amount)
{
    if (context.bundle.funding_outputs.empty()) {
        return util::Error{Untranslated("Agent allotment bundle does not include funding outputs; pass -prevtxid, -prevout, and -prevamount or copy a fresh desktop bundle.")};
    }

    std::vector<AllotmentFundingOutputArtifact> outputs{context.bundle.funding_outputs};
    std::sort(outputs.begin(), outputs.end(), [](const auto& a, const auto& b) {
        if (a.amount != b.amount) return a.amount > b.amount;
        if (a.txid != b.txid) return a.txid < b.txid;
        return a.vout < b.vout;
    });

    std::vector<AllotmentSpendInput> selected;
    CAmount selected_amount{0};
    for (const AllotmentFundingOutputArtifact& output : outputs) {
        auto txid{Txid::FromHex(output.txid)};
        if (!txid.has_value()) {
            return util::Error{Untranslated("Agent allotment bundle funding output txid must be a transaction id hex string.")};
        }
        selected.push_back(AllotmentSpendInput{
            .prevout = COutPoint{*txid, output.vout},
            .amount = output.amount,
        });
        if (!MoneyRange(selected_amount + output.amount)) {
            return util::Error{Untranslated("Agent allotment bundle funding output amount is out of range.")};
        }
        selected_amount += output.amount;
        if (selected_amount >= spend_amount) break;
    }

    if (selected_amount < spend_amount) {
        return util::Error{Untranslated("Agent allotment bundle does not include enough spendable funding outputs for this spend.")};
    }
    return selected;
}

} // namespace

util::Result<AllotmentSpendContext> ImportAllotmentBundle(std::string_view bundle_json,
                                                              std::string_view expected_chain,
                                                              std::string_view expected_genesis_hash)
{
    auto bundle{DecodeAllotmentPolicyBundle(bundle_json, expected_chain, expected_genesis_hash)};
    if (!bundle) return util::Error{util::ErrorString(bundle)};

    CTxDestination funding_destination{DecodeDestination(bundle->funding_address)};
    if (!IsValidDestination(funding_destination)) {
        return util::Error{Untranslated("funding_address is not valid for this chain")};
    }

    AllotmentSpendContext context;
    context.bundle = *bundle;
    context.funding_destination = funding_destination;
    context.funding_script = GetScriptForDestination(context.funding_destination);

    auto key_result{AddFundingKey(context, bundle->funding_secret)};
    if (!key_result) return util::Error{util::ErrorString(key_result)};

    return context;
}

util::Result<AllotmentSignedSpend> CreateSignedAllotmentSpend(const AllotmentSpendContext& context,
                                                                  const AllotmentSpendRequest& request,
                                                                  const Consensus::Params& consensus)
{
    return CreateSignedAllotmentSpendWithInputs(
        context,
        {AllotmentSpendInput{.prevout = request.prevout, .amount = request.prevout_value}},
        request.destination,
        request.spend_amount,
        request.spent_today,
        request.prove,
        request.anchor,
        consensus,
        request.cancel);
}

util::Result<AllotmentSignedSpend> CreateSignedAllotmentSpendFromBundleOutputs(const AllotmentSpendContext& context,
                                                                                   const AllotmentBundleSpendRequest& request,
                                                                                   const Consensus::Params& consensus)
{
    auto selected{SelectBundleFundingOutputs(context, request.spend_amount)};
    if (!selected) return util::Error{util::ErrorString(selected)};

    return CreateSignedAllotmentSpendWithInputs(
        context,
        *selected,
        request.destination,
        request.spend_amount,
        request.spent_today,
        request.prove,
        request.anchor,
        consensus,
        request.cancel);
}

} // namespace agent
