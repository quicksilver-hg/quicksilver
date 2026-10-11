// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <agent/allotmentspend.h>

#include <addresstype.h>
#include <arith_uint256.h>
#include <chain.h>
#include <consensus/amount.h>
#include <consensus/params.h>
#include <crypto/cuckatoo/cuckatoo.h>
#include <crypto/cuckatoo/gpu_solver.h>
#include <key.h>
#include <key_io.h>
#include <pow.h>
#include <primitives/transaction.h>
#include <psqt.h>
#include <script/descriptor.h>
#include <script/interpreter.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <tinyformat.h>
#include <util/result.h>
#include <util/strencodings.h>
#include <util/translation.h>

#include <algorithm>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace agent {
namespace {

util::Result<void> ImportAgentDescriptor(AllotmentSpendContext& context)
{
    // Exactly one public 2-of-2 leaf, A first and C second. Keep this boundary
    // narrower than the descriptor parser: ranged keys, extra leaves, alternate
    // thresholds, private descriptors and duplicate role keys are not allotments.
    static const std::regex shape{R"(^tr\(([0-9a-f]{64}),multi_a\(2,([0-9a-f]{64}),([0-9a-f]{64})\)\)#[0-9a-z]{8}$)"};
    std::smatch roles;
    const std::string& descriptor{context.bundle.funding_descriptor};
    const std::string shape_error{"funding_descriptor must be a canonical tr(V,multi_a(2,A,C)) descriptor with distinct public keys"};
    if (!std::regex_match(descriptor, roles, shape)) {
        return util::Error{Untranslated(shape_error)};
    }
    const XOnlyPubKey vault_key{ParseHex(roles[1].str())};
    const XOnlyPubKey agent_key{ParseHex(roles[2].str())};
    const XOnlyPubKey cosigner_key{ParseHex(roles[3].str())};
    if (!vault_key.IsFullyValid() || !agent_key.IsFullyValid() || !cosigner_key.IsFullyValid() ||
        vault_key == agent_key || vault_key == cosigner_key || agent_key == cosigner_key) {
        return util::Error{Untranslated(shape_error)};
    }
    FlatSigningProvider parsed;
    std::string error;
    auto descs{Parse(descriptor, parsed, error, /*require_checksum=*/true)};
    if (descs.size() != 1 || descs[0]->ToString() != descriptor || !parsed.keys.empty()) {
        return util::Error{Untranslated(shape_error)};
    }
    std::vector<CScript> scripts;
    if (!descs[0]->Expand(0, parsed, scripts, context.signing_provider) || scripts.size() != 1) {
        return util::Error{Untranslated(shape_error)};
    }
    if (scripts[0] != GetScriptForDestination(context.funding_destination)) {
        return util::Error{Untranslated("funding_descriptor does not match the bundle funding address")};
    }
    const CKey key{DecodeSecret(context.bundle.agent_secret)};
    if (!key.IsValid()) {
        return util::Error{Untranslated("agent_secret_wif is not a valid private key for this chain")};
    }
    const CPubKey pubkey{key.GetPubKey()};
    if (XOnlyPubKey(pubkey) != agent_key) {
        return util::Error{Untranslated("agent_secret_wif does not match the descriptor agent key")};
    }
    context.funding_script = scripts[0];
    context.signing_provider.keys.emplace(pubkey.GetID(), key);
    context.signing_provider.pubkeys.emplace(pubkey.GetID(), pubkey);
    return {};
}

// The generic dummy signer accepts every key, including Taproot key-path keys,
// and its Schnorr dummy is 64 bytes even in MAXIMUM mode. Allotments must price
// their A+C leaf and allow the 65-byte explicit-sighash form of each signature.
class MaximumAllotmentSignatureCreator final : public BaseSignatureCreator {
    const std::map<std::pair<XOnlyPubKey, uint256>, std::vector<unsigned char>>& m_allowed;
public:
    explicit MaximumAllotmentSignatureCreator(const SignatureData& leaf_data) : m_allowed{leaf_data.taproot_script_sigs} {}
    const BaseSignatureChecker& Checker() const override { return DUMMY_MAXIMUM_SIGNATURE_CREATOR.Checker(); }
    bool CreateSig(const SigningProvider&, std::vector<unsigned char>&, const CKeyID&, const CScript&, SigVersion) const override { return false; }
    bool CreateSchnorrSig(const SigningProvider& provider, std::vector<unsigned char>& sig, const XOnlyPubKey& pubkey,
                          const uint256* leaf_hash, const uint256* merkle_root, SigVersion version) const override
    {
        if (version != SigVersion::TAPSCRIPT || leaf_hash == nullptr || !m_allowed.contains({pubkey, *leaf_hash})) return false;
        if (!DUMMY_MAXIMUM_SIGNATURE_CREATOR.CreateSchnorrSig(provider, sig, pubkey, leaf_hash, merkle_root, version)) return false;
        sig.resize(65);
        sig.back() = SIGHASH_ALL;
        return true;
    }
};

util::Result<CMutableTransaction> MaximumAllotmentSpend(const AllotmentSpendContext& context,
                                                       const PartiallySignedQuicksilverTransaction& psqt)
{
    CMutableTransaction maximum{*psqt.tx};
    const HidingSigningProvider public_provider{&context.signing_provider, /*hide_secret=*/true, /*hide_origin=*/false};
    for (size_t i{0}; i < maximum.vin.size(); ++i) {
        SignatureData allowed;
        psqt.inputs[i].FillSignatureData(allowed);
        // Restrict dummy signatures to the sole canonical leaf and its A/C keys.
        const auto& leaf{*psqt.inputs[i].m_tap_scripts.begin()};
        const uint256 leaf_hash{psqt.inputs[i].m_tap_script_sigs.begin()->first.second};
        for (const auto offset : {1, 35}) {
            const XOnlyPubKey pubkey{Span{leaf.first.first}.subspan(offset, 32)};
            allowed.taproot_script_sigs.try_emplace({pubkey, leaf_hash}, std::vector<unsigned char>{});
        }
        const MaximumAllotmentSignatureCreator creator{allowed};
        SignatureData dummy;
        if (!ProduceSignature(public_provider, creator, context.funding_script, dummy) || dummy.scriptWitness.stack.size() != 4) {
            return util::Error{Untranslated("Could not price the allotment script-path witness.")};
        }
        UpdateInput(maximum.vin[i], dummy);
    }
    return maximum;
}

util::Result<void> ProveAgentSpend(CMutableTransaction& transaction,
                                   const CBlockIndex* anchor,
                                   const Consensus::Params& consensus,
                                   const uint256& proof_target,
                                   const cuckatoo::SolverCancelCallback& cancel,
                                   bool allow_cpu_txpow)
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

    // The agent grinds before the cosigner signs, so price the largest witness
    // the canonical A+C leaf can carry. A larger size only lowers the target;
    // a proof found for it still holds for the real, smaller witness. The proof
    // pre-image omits signatures and witness, so co-signing preserves the proof.
    const arith_uint256 target{UintToArith256(proof_target)};

    const std::vector<unsigned char> preimage{CTransaction(transaction).PowPreimage(anchor->GetBlockHash())};
    cuckatoo::Cycle cycle{};
    uint32_t nonce{0};
    const bool cpu_fallback{AllowsAllotmentCpuFallback(consensus.nTxEdgeBits, allow_cpu_txpow)};
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
                                                                            const cuckatoo::SolverCancelCallback& cancel,
                                                                            bool allow_cpu_txpow)
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

    PartiallySignedQuicksilverTransaction psqt{transaction};
    for (size_t i{0}; i < inputs.size(); ++i) {
        psqt.inputs[i].witness_utxo = CTxOut{inputs[i].amount, context.funding_script};
    }
    const auto txdata{PrecomputePSQTData(psqt)};
    for (size_t i{0}; i < inputs.size(); ++i) {
        // False is expected: A signs its half, C is absent, and nothing finalizes.
        SignPSQTInput(context.signing_provider, psqt, i, &txdata, SIGHASH_DEFAULT, nullptr, /*finalize=*/false);
        const auto& input{psqt.inputs[i]};
        if (input.m_tap_script_sigs.size() != 1 || input.m_tap_scripts.size() != 1 || !input.m_tap_key_sig.empty()) {
            return util::Error{Untranslated("Agent allotment signing failed.")};
        }
        const auto& leaf{*input.m_tap_scripts.begin()};
        transaction.vin[i].scriptWitness.stack = {{}, input.m_tap_script_sigs.begin()->second, leaf.first.first, *leaf.second.begin()};
    }

    uint256 proof_target;
    if (prove) {
        auto maximum{MaximumAllotmentSpend(context, psqt)};
        if (!maximum) return util::Error{util::ErrorString(maximum)};
        proof_target = GetTxPowTarget(consensus, anchor, CTransaction{*maximum});
        auto proof_result{ProveAgentSpend(transaction, anchor, consensus, proof_target, cancel, allow_cpu_txpow)};
        if (!proof_result) return util::Error{util::ErrorString(proof_result)};
        psqt.tx->nAnchorHeight = transaction.nAnchorHeight;
        psqt.tx->nCycle = transaction.nCycle;
        psqt.tx->nPowNonce = transaction.nPowNonce;
    }

    return AllotmentSignedSpend{
        .transaction = CTransaction{transaction},
        .psqt = std::move(psqt),
        .proof_target = proof_target,
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
    auto key_result{ImportAgentDescriptor(context)};
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
        request.cancel,
        request.allow_cpu_txpow);
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
        request.cancel,
        request.allow_cpu_txpow);
}

} // namespace agent
