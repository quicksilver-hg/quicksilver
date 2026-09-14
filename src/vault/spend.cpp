// Copyright (c) 2021-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <algorithm>
#include <arith_uint256.h>
#include <chainparams.h>
#include <common/args.h>
#include <common/messages.h>
#include <common/system.h>
#include <consensus/amount.h>
#include <consensus/validation.h>
#include <crypto/cuckatoo/cuckatoo.h>
#include <interfaces/chain.h>
#include <key_io.h>
#include <node/types.h>
#include <numeric>
#include <policy/policy.h>
#include <pow.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <script/signingprovider.h>
#include <script/solver.h>
#include <univalue.h>
#include <util/check.h>
#include <util/moneystr.h>
#include <util/string.h>
#include <util/trace.h>
#include <util/translation.h>
#include <vault/coincontrol.h>
#include <vault/receive.h>
#include <vault/scriptpubkeyman.h>
#include <vault/spend.h>
#include <vault/transaction.h>
#include <vault/types.h>
#include <vault/vault.h>

#include <cmath>
#include <limits>

using common::TransactionErrorString;
using interfaces::FoundBlock;
using node::TransactionError;

TRACEPOINT_SEMAPHORE(coin_selection, selected_coins);
TRACEPOINT_SEMAPHORE(coin_selection, normal_create_tx_internal);
TRACEPOINT_SEMAPHORE(coin_selection, attempting_aps_create_tx);
TRACEPOINT_SEMAPHORE(coin_selection, aps_create_tx_internal);

namespace vault {
static constexpr size_t OUTPUT_GROUP_MAX_ENTRIES{100};

//! Weight and with-witness byte size of one signed input. Two numbers because
//! weight is 3*base + total: it cannot be divided back into bytes after the fact,
//! so the split has to be carried from where it is known.
struct InputSize {
    int64_t weight{0};
    int64_t bytes{0};
};

/** Whether the descriptor represents, directly or not, a witness program. */
static bool IsSegwit(const Descriptor& desc) {
    if (const auto typ = desc.GetOutputType()) return *typ != OutputType::BASE58;
    return false;
}

/** Whether to assume ECDSA signatures' will be high-r. */
static bool UseMaxSig(const std::optional<CTxIn>& txin, const CCoinControl* coin_control) {
    // Use max sig if this particular input is external, so the transaction weight
    // budget accounts for the largest expected signature.
    return coin_control && txin && coin_control->IsExternalSelected(txin->prevout);
}

/** Get the size of an input (in witness units) once it's signed.
 *
 * @param desc The output script descriptor of the coin spent by this input.
 * @param txin Optionally the txin to estimate the size of. Used to determine the size of ECDSA signatures.
 * @param coin_control Information about the context to determine the size of ECDSA signatures.
 * @param tx_is_segwit Whether the transaction has at least a single input spending a segwit coin.
 * @param can_grind_r Whether the signer will be able to grind the R of the signature.
 */
static std::optional<InputSize> MaxInputSize(const Descriptor& desc, const std::optional<CTxIn>& txin,
                                             const CCoinControl* coin_control, const bool tx_is_segwit,
                                             const bool can_grind_r) {
    if (const auto sat_weight = desc.MaxSatisfactionWeight(!can_grind_r || UseMaxSig(txin, coin_control))) {
        if (const auto elems_count = desc.MaxSatisfactionElems()) {
            const bool is_segwit = IsSegwit(desc);
            // Account for the size of the scriptsig and the number of elements on the witness stack. Note
            // that if any input in the transaction is spending a witness program, we need to specify the
            // witness stack size for every input regardless of whether it is segwit itself.
            const int64_t scriptsig_len = is_segwit ? 1 : GetSizeOfCompactSize(*sat_weight / WITNESS_SCALE_FACTOR);
            const int64_t witstack_len = is_segwit ? GetSizeOfCompactSize(*elems_count) : (tx_is_segwit ? 1 : 0);
            // previous txid + previous vout + sequence + scriptsig len: always non-witness.
            const int64_t nonwitness = 32 + 4 + 4 + scriptsig_len;
            // sat_weight is in WEIGHT units, and that is where witness and non-witness
            // part company: for a segwit descriptor it is witness bytes at 1x, for a
            // legacy one it is scriptSig bytes at 4x. Splitting it here is what lets the
            // caller report real bytes instead of paying the 4x weight proxy.
            const int64_t sat_bytes = is_segwit ? *sat_weight : *sat_weight / WITNESS_SCALE_FACTOR;
            return InputSize{
                /*weight=*/nonwitness * WITNESS_SCALE_FACTOR + witstack_len + *sat_weight,
                /*bytes=*/nonwitness + witstack_len + sat_bytes};
        }
    }

    return {};
}

int CalculateMaximumSignedInputSize(const CTxOut& txout, const SigningProvider* provider, bool can_grind_r, const CCoinControl* coin_control)
{
    if (!provider) return -1;

    if (const auto desc = InferDescriptor(txout.scriptPubKey, *provider)) {
        if (const auto size = MaxInputSize(*desc, {}, coin_control, true, can_grind_r)) {
            return static_cast<int>(GetVirtualTransactionSize(size->weight, 0, 0));
        }
    }

    return -1;
}

int CalculateMaximumSignedInputSize(const CTxOut& txout, const CVault* vault, const CCoinControl* coin_control)
{
    const std::unique_ptr<SigningProvider> provider = vault->GetSolvingProvider(txout.scriptPubKey);
    return CalculateMaximumSignedInputSize(txout, provider.get(), vault->CanGrindR(), coin_control);
}

/** Infer a descriptor for the given output script. */
static std::unique_ptr<Descriptor> GetDescriptor(const CVault* vault, const CCoinControl* coin_control,
                                                 const CScript script_pubkey)
{
    MultiSigningProvider providers;
    for (const auto spkman: vault->GetScriptPubKeyMans(script_pubkey)) {
        providers.AddProvider(spkman->GetSolvingProvider(script_pubkey));
    }
    if (coin_control) {
        providers.AddProvider(std::make_unique<FlatSigningProvider>(coin_control->m_external_provider));
    }
    return InferDescriptor(script_pubkey, providers);
}

/** Infer the maximum size of this input after it will be signed. */
static std::optional<InputSize> GetSignedTxinSize(const CVault* vault, const CCoinControl* coin_control,
                                                  const CTxIn& txin, const CTxOut& txo, const bool tx_is_segwit,
                                                  const bool can_grind_r)
{
    // If weight was provided, use that. It carries no witness/non-witness split, so
    // the byte bound falls back to the weight itself — correct because bytes <=
    // weight always, and conservative only for callers who supply their own weights.
    std::optional<int64_t> weight;
    if (coin_control && (weight = coin_control->GetInputWeight(txin.prevout))) {
        return InputSize{*weight, *weight};
    }

    // Otherwise, use the maximum satisfaction size provided by the descriptor.
    std::unique_ptr<Descriptor> desc{GetDescriptor(vault, coin_control, txo.scriptPubKey)};
    if (desc) return MaxInputSize(*desc, {txin}, coin_control, tx_is_segwit, can_grind_r);

    return {};
}

// txouts needs to be in the order of tx.vin
TxSize CalculateMaximumSignedTxSize(const CTransaction &tx, const CVault *vault, const std::vector<CTxOut>& txouts, const CCoinControl* coin_control)
{
    // version + nLockTime + input count + output count, plus Quicksilver's per-tx PoW
    // tail: nAnchorHeight (4) + nPowNonce (4) + 42 * nCycle (168) = 176 bytes, all
    // non-witness. Omitting it understated every estimate by 704 weight, which was
    // harmless only while size did not affect validity. After the Stage 2 flag day an
    // understated estimate is an under-grind, and an under-ground transaction is
    // invalid.
    const int64_t header_bytes = 4 + 4 + 176 + GetSizeOfCompactSize(tx.vin.size()) + GetSizeOfCompactSize(tx.vout.size());
    int64_t weight = header_bytes * WITNESS_SCALE_FACTOR;
    // Tracked alongside weight, not derived from it: weight is 3*base + total and
    // cannot be divided back into bytes. The Stage 2 size term prices bytes.
    int64_t bytes = header_bytes;
    // Whether any input spends a witness program. Necessary to run before the next loop over the
    // inputs in order to accurately compute the compactSize length for the witness data per input.
    bool is_segwit = std::any_of(txouts.begin(), txouts.end(), [&](const CTxOut& txo) {
        std::unique_ptr<Descriptor> desc{GetDescriptor(vault, coin_control, txo.scriptPubKey)};
        if (desc) return IsSegwit(*desc);
        return false;
    });
    // Segwit marker and flag (witness data: one weight unit per byte)
    if (is_segwit) {
        weight += 2;
        bytes += 2;
    }

    // Add the size of the transaction outputs.
    for (const auto& txo : tx.vout) {
        const int64_t out_bytes = GetSerializeSize(txo);
        weight += out_bytes * WITNESS_SCALE_FACTOR;
        bytes += out_bytes;
    }

    // Add the size of the transaction inputs as if they were signed.
    for (uint32_t i = 0; i < txouts.size(); i++) {
        const auto txin_size = GetSignedTxinSize(vault, coin_control, tx.vin[i], txouts[i], is_segwit, vault->CanGrindR());
        if (!txin_size) return TxSize{-1, -1, -1};
        assert(txin_size->weight > -1);
        weight += txin_size->weight;
        bytes += txin_size->bytes;
    }

    // It's ok to use 0 as the number of sigops since we never create any pathological transaction.
    return TxSize{GetVirtualTransactionSize(weight, 0, 0), weight, bytes};
}

TxSize CalculateMaximumSignedTxSize(const CTransaction &tx, const CVault *vault, const CCoinControl* coin_control)
{
    std::vector<CTxOut> txouts;
    // Look up the inputs. The inputs are either in the vault, or in coin_control.
    for (const CTxIn& input : tx.vin) {
        const auto mi = vault->mapVault.find(input.prevout.hash);
        // Can not estimate size without knowing the input details
        if (mi != vault->mapVault.end()) {
            assert(input.prevout.n < mi->second.tx->vout.size());
            txouts.emplace_back(mi->second.tx->vout.at(input.prevout.n));
        } else if (coin_control) {
            const auto& txout{coin_control->GetExternalOutput(input.prevout)};
            if (!txout) return TxSize{-1, -1, -1};
            txouts.emplace_back(*txout);
        } else {
            return TxSize{-1, -1, -1};
        }
    }
    return CalculateMaximumSignedTxSize(tx, vault, txouts, coin_control);
}

size_t CoinsResult::Size() const
{
    size_t size{0};
    for (const auto& it : coins) {
        size += it.second.size();
    }
    return size;
}

std::vector<COutput> CoinsResult::All() const
{
    std::vector<COutput> all;
    all.reserve(coins.size());
    for (const auto& it : coins) {
        all.insert(all.end(), it.second.begin(), it.second.end());
    }
    return all;
}

void CoinsResult::Clear() {
    coins.clear();
    total_amount = 0;
}

void CoinsResult::Erase(const std::unordered_set<COutPoint, SaltedOutpointHasher>& coins_to_remove)
{
    for (auto& [type, vec] : coins) {
        auto remove_it = std::remove_if(vec.begin(), vec.end(), [&](const COutput& coin) {
            // remove it if it's on the set
            if (coins_to_remove.count(coin.outpoint) == 0) return false;

            // update cached amounts
            total_amount -= coin.txout.nValue;
            return true;
        });
        vec.erase(remove_it, vec.end());
    }
}

void CoinsResult::Shuffle(FastRandomContext& rng_fast)
{
    for (auto& it : coins) {
        std::shuffle(it.second.begin(), it.second.end(), rng_fast);
    }
}

void CoinsResult::Add(OutputType type, const COutput& out)
{
    coins[type].emplace_back(out);
    total_amount += out.txout.nValue;
}

static OutputType GetOutputType(TxoutType type, bool is_from_p2sh)
{
    switch (type) {
        case TxoutType::WITNESS_V1_TAPROOT:
            return OutputType::BECH32M;
        case TxoutType::WITNESS_V0_KEYHASH:
        case TxoutType::WITNESS_V0_SCRIPTHASH:
            return is_from_p2sh ? OutputType::BASE58 : OutputType::BECH32;
        case TxoutType::SCRIPTHASH:
        case TxoutType::PUBKEYHASH:
            return OutputType::BASE58;
        default:
            return OutputType::UNKNOWN;
    }
}

// Fetch and validate the coin control selected inputs.
// Coins could be internal (from the vault) or external.
util::Result<PreSelectedInputs> FetchSelectedInputs(const CVault& vault, const CCoinControl& coin_control)
{
    PreSelectedInputs result;
    const bool can_grind_r = vault.CanGrindR();
    // Quicksilver feeless: raw selected value is used directly, with no MiniMiner pass.
    for (const COutPoint& outpoint : coin_control.ListSelected()) {
        int64_t input_bytes = coin_control.GetInputWeight(outpoint).value_or(-1);
        if (input_bytes != -1) {
            input_bytes = GetVirtualTransactionSize(input_bytes, 0, 0);
        }
        CTxOut txout;
        if (auto ptr_wtx = vault.GetVaultTx(outpoint.hash)) {
            // Clearly invalid input, fail
            if (ptr_wtx->tx->vout.size() <= outpoint.n) {
                return util::Error{strprintf(_("Invalid pre-selected input %s"), outpoint.ToString())};
            }
            txout = ptr_wtx->tx->vout.at(outpoint.n);
            if (input_bytes == -1) {
                input_bytes = CalculateMaximumSignedInputSize(txout, &vault, &coin_control);
            }
        } else {
            // The input is external. We did not find the tx in mapVault.
            const auto out{coin_control.GetExternalOutput(outpoint)};
            if (!out) {
                return util::Error{strprintf(_("Not found pre-selected input %s"), outpoint.ToString())};
            }

            txout = *out;
        }

        if (input_bytes == -1) {
            input_bytes = CalculateMaximumSignedInputSize(txout, &coin_control.m_external_provider, can_grind_r, &coin_control);
        }

        if (input_bytes == -1) {
            return util::Error{strprintf(_("Not solvable pre-selected input %s"), outpoint.ToString())}; // Not solvable, cannot estimate input size
        }

        /* Set some defaults for depth, solvable, safe, time, and from_me as these don't matter for preset inputs since no selection is being done. */
        COutput output(outpoint, txout, /*depth=*/ 0, input_bytes, /*solvable=*/ true, /*safe=*/ true, /*time=*/ 0, /*from_me=*/ false);
        result.Insert(output);
    }
    return result;
}

CoinsResult AvailableCoins(const CVault& vault,
                           const CCoinControl* coinControl,
                           const CoinFilterParams& params)
{
    AssertLockHeld(vault.cs_vault);

    CoinsResult result;
    // Either the VAULT_FLAG_AVOID_REUSE flag is not set (in which case we always allow), or we default to avoiding, and only in the case where
    // a coin control object is provided, and has the avoid address reuse flag set to false, do we allow already used addresses
    bool allow_used_addresses = !vault.IsVaultFlagSet(VAULT_FLAG_AVOID_REUSE) || (coinControl && !coinControl->m_avoid_address_reuse);
    const int min_depth = {coinControl ? coinControl->m_min_depth : DEFAULT_MIN_DEPTH};
    const int max_depth = {coinControl ? coinControl->m_max_depth : DEFAULT_MAX_DEPTH};
    const bool only_safe = {coinControl ? !coinControl->m_include_unsafe_inputs : true};
    const bool can_grind_r = vault.CanGrindR();
    std::vector<COutPoint> outpoints;

    std::set<uint256> trusted_parents;
    for (const auto& entry : vault.mapVault)
    {
        const uint256& txid = entry.first;
        const CVaultTx& wtx = entry.second;

        if (vault.IsTxImmatureCoinBase(wtx) && !params.include_immature_coinbase)
            continue;

        int nDepth = vault.GetTxDepthInMainChain(wtx);
        if (nDepth < 0)
            continue;

        // We should not consider coins which aren't at least in our relaypool
        // It's possible for these to be conflicted via ancestors which we may never be able to detect
        if (nDepth == 0 && !wtx.InRelayPool())
            continue;

        bool safeTx = CachedTxIsTrusted(vault, wtx, trusted_parents);

        if (only_safe && !safeTx) {
            continue;
        }

        if (nDepth < min_depth || nDepth > max_depth) {
            continue;
        }

        bool tx_from_me = CachedTxIsFromMe(vault, wtx, ISMINE_SPENDABLE);

        for (unsigned int i = 0; i < wtx.tx->vout.size(); i++) {
            const CTxOut& output = wtx.tx->vout[i];
            const COutPoint outpoint(Txid::FromUint256(txid), i);

            if (output.nValue < params.min_amount || output.nValue > params.max_amount)
                continue;

            // Skip manually selected coins (the caller can fetch them directly)
            if (coinControl && coinControl->HasSelected() && coinControl->IsSelected(outpoint))
                continue;

            if (vault.IsLockedCoin(outpoint) && params.skip_locked)
                continue;

            if (vault.IsSpent(outpoint))
                continue;

            isminetype mine = vault.IsMine(output);

            if (mine == ISMINE_NO) {
                continue;
            }

            if (!allow_used_addresses && vault.IsSpentKey(output.scriptPubKey)) {
                continue;
            }

            std::unique_ptr<SigningProvider> provider = vault.GetSolvingProvider(output.scriptPubKey);

            int input_bytes = CalculateMaximumSignedInputSize(output, provider.get(), can_grind_r, coinControl);
            // Because CalculateMaximumSignedInputSize infers a solvable descriptor to get the satisfaction size,
            // it is safe to assume that this input is solvable if input_bytes is greater than -1.
            bool solvable = input_bytes > -1;
            // Obtain script type
            std::vector<std::vector<uint8_t>> script_solutions;
            TxoutType type = Solver(output.scriptPubKey, script_solutions);

            // If the output is P2SH and solvable, inspect its redeemScript to
            // classify the underlying base58 script. If the output is not solvable, it will be classified
            // as a base58 P2SH, since we have no way of knowing otherwise without the redeemScript
            bool is_from_p2sh{false};
            if (type == TxoutType::SCRIPTHASH && solvable) {
                CScript script;
                if (!provider->GetCScript(CScriptID(uint160(script_solutions[0])), script)) continue;
                type = Solver(script, script_solutions);
                is_from_p2sh = true;
            }

            result.Add(GetOutputType(type, is_from_p2sh),
                       COutput(outpoint, output, nDepth, input_bytes, solvable, safeTx, wtx.GetTxTime(), tx_from_me));

            outpoints.push_back(outpoint);

            // Checks the sum amount of all UTXO's.
            if (params.min_sum_amount != MAX_MONEY) {
                if (result.GetTotalAmount() >= params.min_sum_amount) {
                    return result;
                }
            }

            // Checks the maximum number of UTXO's.
            if (params.max_count > 0 && result.Size() >= params.max_count) {
                return result;
            }
        }
    }

    // Quicksilver feeless: raw selected value is used directly, with no MiniMiner pass.

    return result;
}

const CTxOut& FindNonChangeParentOutput(const CVault& vault, const COutPoint& outpoint)
{
    AssertLockHeld(vault.cs_vault);
    const CVaultTx* wtx{Assert(vault.GetVaultTx(outpoint.hash))};

    const CTransaction* ptx = wtx->tx.get();
    int n = outpoint.n;
    while (OutputIsChange(vault, ptx->vout[n]) && ptx->vin.size() > 0) {
        const COutPoint& prevout = ptx->vin[0].prevout;
        const CVaultTx* it = vault.GetVaultTx(prevout.hash);
        if (!it || it->tx->vout.size() <= prevout.n ||
            !vault.IsMine(it->tx->vout[prevout.n])) {
            break;
        }
        ptx = it->tx.get();
        n = prevout.n;
    }
    return ptx->vout[n];
}

std::map<CTxDestination, std::vector<COutput>> ListCoins(const CVault& vault)
{
    AssertLockHeld(vault.cs_vault);

    std::map<CTxDestination, std::vector<COutput>> result;

    CCoinControl coin_control;
    CoinFilterParams coins_params;
    coins_params.skip_locked = false;
    for (const COutput& coin : AvailableCoins(vault, &coin_control, coins_params).All()) {
        CTxDestination address;
        if (coin.solvable) {
            if (!ExtractDestination(FindNonChangeParentOutput(vault, coin.outpoint).scriptPubKey, address)) {
                // Group bare P2PK outputs by the address of their public key.
                if (auto pk_dest = std::get_if<PubKeyDestination>(&address)) {
                    address = PKHash(pk_dest->GetPubKey());
                } else {
                    continue;
                }
            }
            result[address].emplace_back(coin);
        }
    }
    return result;
}

FilteredOutputGroups GroupOutputs(const CVault& vault,
                          const CoinsResult& coins,
                          const CoinSelectionParams& coin_sel_params,
                          const std::vector<SelectionFilter>& filters,
                          std::vector<OutputGroup>& ret_discarded_groups)
{
    FilteredOutputGroups filtered_groups;

    if (!coin_sel_params.m_avoid_partial_spends) {
        // Allowing partial spends means no grouping. Each COutput gets its own OutputGroup
        for (const auto& [type, outputs] : coins.coins) {
            for (const COutput& output : outputs) {
                // Get relaypool info
                size_t ancestors, descendants;
                vault.chain().getTransactionAncestry(output.outpoint.hash, ancestors, descendants);

                // Create a new group per output and add it to the all groups vector
                OutputGroup group;
                group.Insert(std::make_shared<COutput>(output), ancestors, descendants);

                // Each filter maps to a different set of groups
                bool accepted = false;
                for (const auto& sel_filter : filters) {
                    const auto& filter = sel_filter.filter;
                    if (!group.EligibleForSpending(filter)) continue;
                    filtered_groups[filter].Push(group, type, /*insert_positive=*/true, /*insert_mixed=*/true);
                    accepted = true;
                }
                if (!accepted) ret_discarded_groups.emplace_back(group);
            }
        }
        return filtered_groups;
    }

    // We want to combine COutputs that have the same scriptPubKey into single OutputGroups
    // except when there are more than OUTPUT_GROUP_MAX_ENTRIES COutputs grouped in an OutputGroup.
    // To do this, we maintain a map where the key is the scriptPubKey and the value is a vector of OutputGroups.
    // For each COutput, we check if the scriptPubKey is in the map, and if it is, the COutput is added
    // to the last OutputGroup in the vector for the scriptPubKey. When the last OutputGroup has
    // OUTPUT_GROUP_MAX_ENTRIES COutputs, a new OutputGroup is added to the end of the vector.
    typedef std::map<std::pair<CScript, OutputType>, std::vector<OutputGroup>> ScriptPubKeyToOutgroup;
    const auto& insert_output = [&](
            const std::shared_ptr<COutput>& output, OutputType type, size_t ancestors, size_t descendants,
            ScriptPubKeyToOutgroup& groups_map) {
        std::vector<OutputGroup>& groups = groups_map[std::make_pair(output->txout.scriptPubKey,type)];

        if (groups.size() == 0) {
            // No OutputGroups for this scriptPubKey yet, add one
            groups.emplace_back();
        }

        // Get the last OutputGroup in the vector so that we can add the COutput to it
        // A pointer is used here so that group can be reassigned later if it is full.
        OutputGroup* group = &groups.back();

        // Check if this OutputGroup is full. We limit to OUTPUT_GROUP_MAX_ENTRIES when using -avoidpartialspends
        // to avoid building an oversized transaction from one reused script.
        if (group->m_outputs.size() >= OUTPUT_GROUP_MAX_ENTRIES) {
            // The last output group is full, add a new group to the vector and use that group for the insertion
            groups.emplace_back();
            group = &groups.back();
        }

        group->Insert(output, ancestors, descendants);
    };

    ScriptPubKeyToOutgroup spk_to_groups_map;
    ScriptPubKeyToOutgroup spk_to_positive_groups_map;
    for (const auto& [type, outs] : coins.coins) {
        for (const COutput& output : outs) {
            size_t ancestors, descendants;
            vault.chain().getTransactionAncestry(output.outpoint.hash, ancestors, descendants);

            const auto& shared_output = std::make_shared<COutput>(output);
            // Filter for positive only before adding the output
            if (output.txout.nValue > 0) {
                insert_output(shared_output, type, ancestors, descendants, spk_to_positive_groups_map);
            }

            // 'All' groups
            insert_output(shared_output, type, ancestors, descendants, spk_to_groups_map);
        }
    }

    // Now we go through the entire maps and pull out the OutputGroups
    const auto& push_output_groups = [&](const ScriptPubKeyToOutgroup& groups_map, bool positive_only) {
        for (const auto& [script, groups] : groups_map) {
            // Go through the vector backwards. This allows for the first item we deal with being the partial group.
            for (auto group_it = groups.rbegin(); group_it != groups.rend(); group_it++) {
                const OutputGroup& group = *group_it;

                // Each filter maps to a different set of groups
                bool accepted = false;
                for (const auto& sel_filter : filters) {
                    const auto& filter = sel_filter.filter;
                    if (!group.EligibleForSpending(filter)) continue;

                    // Don't include partial groups if there are full groups too and we don't want partial groups
                    if (group_it == groups.rbegin() && groups.size() > 1 && !filter.m_include_partial_groups) {
                        continue;
                    }

                    OutputType type = script.second;
                    // Either insert the group into the positive-only groups or the mixed ones.
                    filtered_groups[filter].Push(group, type, positive_only, /*insert_mixed=*/!positive_only);
                    accepted = true;
                }
                if (!accepted) ret_discarded_groups.emplace_back(group);
            }
        }
    };

    push_output_groups(spk_to_groups_map, /*positive_only=*/ false);
    push_output_groups(spk_to_positive_groups_map, /*positive_only=*/ true);

    return filtered_groups;
}

FilteredOutputGroups GroupOutputs(const CVault& vault,
                                  const CoinsResult& coins,
                                  const CoinSelectionParams& params,
                                  const std::vector<SelectionFilter>& filters)
{
    std::vector<OutputGroup> unused;
    return GroupOutputs(vault, coins, params, filters, unused);
}

// Returns true if the result contains an error and the message is not empty
static bool HasErrorMsg(const util::Result<SelectionResult>& res) { return !util::ErrorString(res).empty(); }

static bool IsPreferredSelection(const SelectionResult& candidate, const SelectionResult& current)
{
    if (candidate.GetAlgo() != current.GetAlgo()) return candidate.GetAlgo() == SelectionAlgorithm::EXACT;
    if (candidate.GetWeight() != current.GetWeight()) return candidate.GetWeight() < current.GetWeight();
    if (candidate.GetInputSet().size() != current.GetInputSet().size()) return candidate.GetInputSet().size() < current.GetInputSet().size();
    return candidate.GetSelectedValue() > current.GetSelectedValue();
}

util::Result<SelectionResult> AttemptSelection(const CAmount& nTargetValue, OutputGroupTypeMap& groups,
                               const CoinSelectionParams& coin_selection_params, bool allow_mixed_output_types)
{
    std::vector<SelectionResult> results;
    std::vector<util::Result<SelectionResult>> res_detailed_errors;
    for (auto& [type, group] : groups.groups_by_type) {
        auto result{ChooseSelectionResult(nTargetValue, group, coin_selection_params)};
        if (result) results.push_back(*result);
        else if (HasErrorMsg(result)) res_detailed_errors.emplace_back(std::move(result));
    }

    // If allowed, also consider the mixed output-type pool. Exact matches from any eligible bucket
    // win before a lower-weight largest-first fallback that would create change.
    if (allow_mixed_output_types && groups.TypesCount() > 1) {
        auto mixed_result{ChooseSelectionResult(nTargetValue, groups.all_groups, coin_selection_params)};
        if (mixed_result) results.push_back(*mixed_result);
        else if (HasErrorMsg(mixed_result)) res_detailed_errors.emplace_back(std::move(mixed_result));
    }

    if (!results.empty()) {
        return *std::min_element(results.begin(), results.end(), [](const SelectionResult& a, const SelectionResult& b) {
            return IsPreferredSelection(a, b);
        });
    }

    if (!res_detailed_errors.empty()) return std::move(res_detailed_errors.front());
    return util::Error();
};

util::Result<SelectionResult> ChooseSelectionResult(const CAmount& nTargetValue, Groups& groups, const CoinSelectionParams& coin_selection_params)
{
    int max_transaction_weight = coin_selection_params.m_max_tx_weight.value_or(MAX_STANDARD_TX_WEIGHT);
    int tx_weight_no_input = coin_selection_params.tx_noinputs_size * WITNESS_SCALE_FACTOR;
    int no_change_selection_weight = max_transaction_weight - tx_weight_no_input;
    if (no_change_selection_weight <= 0) {
        return util::Error{_("Maximum transaction weight is less than transaction weight without inputs")};
    }
    const int change_weight{coin_selection_params.change_output_size * WITNESS_SCALE_FACTOR};
    const int fallback_selection_weight{no_change_selection_weight - change_weight};

    return SelectCoinsExactOrLargest(groups.positive_group, nTargetValue, no_change_selection_weight, fallback_selection_weight);
}

util::Result<SelectionResult> SelectCoins(const CVault& vault, CoinsResult& available_coins, const PreSelectedInputs& pre_set_inputs,
                                          const CAmount& nTargetValue, const CCoinControl& coin_control,
                                          const CoinSelectionParams& coin_selection_params)
{
    // Deduct preset inputs amount from the search target
    CAmount selection_target = nTargetValue - pre_set_inputs.total_amount;

    // Return if automatic coin selection is disabled, and we don't cover the selection target
    if (!coin_control.m_allow_other_inputs && selection_target > 0) {
        return util::Error{_("The preselected coins total amount does not cover the transaction target. "
                             "Please allow other inputs to be automatically selected or include more coins manually")};
    }

    // Return if we can cover the target only with the preset inputs
    if (selection_target <= 0) {
        SelectionResult result(nTargetValue, SelectionAlgorithm::MANUAL);
        result.AddInputs(pre_set_inputs.coins);
        return result;
    }

    // Return early if we cannot cover the target with the vault's UTXO.
    CAmount available_coins_total_amount = available_coins.GetTotalAmount();
    if (selection_target > available_coins_total_amount) {
        return util::Error(); // Insufficient funds
    }

    // Start vault Coin Selection procedure
    auto op_selection_result = AutomaticCoinSelection(vault, available_coins, selection_target, coin_selection_params);
    if (!op_selection_result) return op_selection_result;

    // If needed, add preset inputs to the automatic coin selection result
    if (!pre_set_inputs.coins.empty()) {
        SelectionResult preselected(pre_set_inputs.total_amount, SelectionAlgorithm::MANUAL);
        preselected.AddInputs(pre_set_inputs.coins);
        op_selection_result->Merge(preselected);

        // Verify we haven't exceeded the maximum allowed weight
        int max_inputs_weight = coin_selection_params.m_max_tx_weight.value_or(MAX_STANDARD_TX_WEIGHT) - (coin_selection_params.tx_noinputs_size * WITNESS_SCALE_FACTOR);
        if (op_selection_result->GetWeight() > max_inputs_weight) {
            return util::Error{_("The combination of the pre-selected inputs and the vault automatic inputs selection exceeds the transaction maximum weight. "
                                 "Please try sending a smaller amount or manually consolidating your vault's UTXOs")};
        }
    }
    return op_selection_result;
}

util::Result<SelectionResult> AutomaticCoinSelection(const CVault& vault, CoinsResult& available_coins, const CAmount& value_to_select, const CoinSelectionParams& coin_selection_params)
{
    unsigned int limit_ancestor_count = 0;
    unsigned int limit_descendant_count = 0;
    vault.chain().getPackageLimits(limit_ancestor_count, limit_descendant_count);
    const size_t max_ancestors = (size_t)std::max<int64_t>(1, limit_ancestor_count);
    const size_t max_descendants = (size_t)std::max<int64_t>(1, limit_descendant_count);
    const bool fRejectLongChains = gArgs.GetBoolArg("-vaultrejectlongchains", DEFAULT_VAULT_REJECT_LONG_CHAINS);

    // Cases where we have 101+ outputs all pointing to the same destination may result in
    // privacy leaks as they will potentially be deterministically sorted. We solve that by
    // explicitly shuffling the outputs before processing
    if (coin_selection_params.m_avoid_partial_spends && available_coins.Size() > OUTPUT_GROUP_MAX_ENTRIES) {
        available_coins.Shuffle(coin_selection_params.rng_fast);
    }

    // Coin selection attempts exact raw-value funding first, then largest-first funding with
    // change. If an attempt fails, more attempts may be made using a more permissive
    // CoinEligibilityFilter.
    {
        // Place coins eligibility filters on a scope increasing order.
        std::vector<SelectionFilter> ordered_filters{
                // If possible, fund the transaction with confirmed UTXOs only. Prefer at least six
                // confirmations on outputs received from other vaults and only spend confirmed change.
                {CoinEligibilityFilter(1, 6, 0), /*allow_mixed_output_types=*/false},
                {CoinEligibilityFilter(1, 1, 0)},
        };
        // Fall back to using zero confirmation change (but with as few ancestors in the relaypool as
        // possible) if we cannot fund the transaction otherwise.
        if (vault.m_spend_zero_conf_change) {
            ordered_filters.push_back({CoinEligibilityFilter(0, 1, 2)});
            ordered_filters.push_back({CoinEligibilityFilter(0, 1, std::min(size_t{4}, max_ancestors/3), std::min(size_t{4}, max_descendants/3))});
            ordered_filters.push_back({CoinEligibilityFilter(0, 1, max_ancestors/2, max_descendants/2)});
            // If partial groups are allowed, relax the requirement of spending OutputGroups (groups
            // of UTXOs sent to the same address, which are obviously controlled by a single vault)
            // in their entirety.
            ordered_filters.push_back({CoinEligibilityFilter(0, 1, max_ancestors-1, max_descendants-1, /*include_partial=*/true)});
            // Try with unsafe inputs if they are allowed. This may spend unconfirmed outputs
            // received from other vaults.
            if (coin_selection_params.m_include_unsafe_inputs) {
                ordered_filters.push_back({CoinEligibilityFilter(/*conf_mine=*/0, /*conf_theirs*/0, max_ancestors-1, max_descendants-1, /*include_partial=*/true)});
            }
            // Try with unlimited ancestors/descendants. The transaction will still need to meet
            // relaypool ancestor/descendant policy to be accepted to relaypool and broadcasted, but
            // OutputGroups use heuristics that may overestimate ancestor/descendant counts.
            if (!fRejectLongChains) {
                ordered_filters.push_back({CoinEligibilityFilter(0, 1, std::numeric_limits<uint64_t>::max(),
                                                                   std::numeric_limits<uint64_t>::max(),
                                                                   /*include_partial=*/true)});
            }
        }

        // Group outputs and map them by coin eligibility filter
        std::vector<OutputGroup> discarded_groups;
        FilteredOutputGroups filtered_groups = GroupOutputs(vault, available_coins, coin_selection_params, ordered_filters, discarded_groups);

        // Check if we still have enough balance after applying filters (some coins might be discarded)
        CAmount total_discarded = 0;
        CAmount total_unconf_long_chain = 0;
        for (const auto& group : discarded_groups) {
            total_discarded += group.GetSelectionAmount();
            if (group.m_ancestors >= max_ancestors || group.m_descendants >= max_descendants) total_unconf_long_chain += group.GetSelectionAmount();
        }

        if (CAmount total_amount = available_coins.GetTotalAmount() - total_discarded < value_to_select) {
            // Special case, too-long-relaypool cluster.
            if (total_amount + total_unconf_long_chain > value_to_select) {
                return util::Error{_("Unconfirmed UTXOs are available, but spending them creates a chain of transactions that will be rejected by the relay pool")};
            }
            return util::Error{}; // General "Insufficient Funds"
        }

        // Walk-through the filters until the solution gets found.
        // If no solution is found, return the first detailed error (if any).
        // future: add "error level" so the worst one can be picked instead.
        std::vector<util::Result<SelectionResult>> res_detailed_errors;
        for (const auto& select_filter : ordered_filters) {
            auto it = filtered_groups.find(select_filter.filter);
            if (it == filtered_groups.end()) continue;
            if (auto res{AttemptSelection(value_to_select, it->second,
                                          coin_selection_params, select_filter.allow_mixed_output_types)}) {
                return res; // result found
            } else {
                // If any specific error message appears here, then something particularly wrong might have happened.
                // Save the error and continue the selection process. So if no solutions gets found, we can return
                // the detailed error to the upper layers.
                if (HasErrorMsg(res)) res_detailed_errors.emplace_back(std::move(res));
            }
        }

        // Return right away if we have a detailed error
        if (!res_detailed_errors.empty()) return std::move(res_detailed_errors.front());


        // General "Insufficient Funds"
        return util::Error{};
    }
}

static bool IsCurrentForLocktimeFreshness(interfaces::Chain& chain, const uint256& block_hash)
{
    if (chain.isInitialBlockDownload()) {
        return false;
    }
    constexpr int64_t MAX_LOCKTIME_FRESHNESS_TIP_AGE = 8 * 60 * 60; // in seconds
    int64_t block_time;
    CHECK_NONFATAL(chain.findBlock(block_hash, FoundBlock().time(block_time)));
    if (block_time < (GetTime() - MAX_LOCKTIME_FRESHNESS_TIP_AGE)) {
        return false;
    }
    return true;
}

/**
 * Set a height-based locktime for new transactions (uses the height of the
 * current chain tip unless we are not synced with the current chain
 */
static void SetFreshVaultLocktime(CMutableTransaction& tx, FastRandomContext& rng_fast,
                                   interfaces::Chain& chain, const uint256& block_hash, int block_height)
{
    // All inputs must be added by now
    assert(!tx.vin.empty());
    // Prefer a recent height-based locktime for vault-created transactions.
    // This keeps default vault spends pinned near the active tip instead of
    // remaining valid at much older heights, while preserving the existing
    // privacy jitter for delayed broadcasts.
    if (IsCurrentForLocktimeFreshness(chain, block_hash)) {
        tx.nLockTime = block_height;

        // Occasionally pick a nLockTime even further back, so that transactions
        // that are delayed after signing for whatever reason have better privacy.
        if (rng_fast.randrange(10) == 0) {
            tx.nLockTime = std::max(0, int(tx.nLockTime) - int(rng_fast.randrange(100)));
        }
    } else {
        // If our chain is lagging behind, we cannot use tip-relative locktime
        // without leaking a potentially unique nLockTime fingerprint.
        tx.nLockTime = 0;
    }
    // Sanity check all values
    assert(tx.nLockTime < LOCKTIME_THRESHOLD); // Type must be block height
    assert(tx.nLockTime <= uint64_t(block_height));
    for (const auto& in : tx.vin) {
        // Can not be FINAL for locktime to work
        assert(in.nSequence != CTxIn::SEQUENCE_FINAL);
        // MAX NONFINAL disables BIP68 while leaving nLockTime effective
        if (in.nSequence == CTxIn::MAX_SEQUENCE_NONFINAL) continue;
        // The vault does not support any other sequence-use right now.
        assert(false);
    }
}

size_t GetSerializeSizeForRecipient(const CRecipient& recipient)
{
    return ::GetSerializeSize(CTxOut(recipient.nAmount, GetScriptForDestination(recipient.dest)));
}

// Quicksilver is feeless/exact-change: no dust. (IsDust recipient helper removed.)

util::Result<int> TxPowAnchorHeight(CVault& vault)
{
    const std::optional<int> tip_height = vault.chain().getHeight();
    if (!tip_height) {
        return util::Error{_("Cannot anchor per-tx proof-of-work: no chain tip")};
    }
    return std::max(0, *tip_height - VAULT_ANCHOR_DEPTH);
}

std::optional<bilingual_str> GrindTransactionPow(CVault& vault, CMutableTransaction& txNew,
                                                 uint64_t bytes, int anchor_height,
                                                 const cuckatoo::SolverProgressCallback& tx_proof_progress,
                                                 const cuckatoo::SolverCancelCallback& tx_proof_cancel)
{
    const Consensus::Params& cp = Params().GetConsensus();
    txNew.nAnchorHeight = static_cast<uint32_t>(anchor_height);
    if (tx_proof_cancel && tx_proof_cancel()) return _("Transfer proof-of-work canceled");

    // Stage 2: required work depends on the transaction's serialized bytes, and
    // the pre-image covers neither scriptSig nor the witness -- so we are grinding
    // before the bytes we are charged for exist. tx_sizes.bytes is an upper bound
    // on the finished serialization, computed with maximum-size dummy signatures.
    // Over-grinding is always valid; under-grinding yields a transaction the
    // network rejects, and there is no rounding slack to hide in because the
    // charge moves with every byte.
    const uint256 target = vault.chain().txPowTarget(
        anchor_height, bytes, txNew.vout.size(), txNew.vin.size());

    // The sender and consensus must derive the same target from the same anchor. When
    // they disagree the transaction is rejected as tx-pow-invalid after the grind has
    // already been paid for, so log every input to the computation on both sides --
    // the pair of lines is what names the divergence.
    vault.VaultLogPrintf("tx-pow grind: anchor=%d bytes=%u nout=%u nin=%u target=%s\n",
                         anchor_height, static_cast<unsigned>(bytes),
                         static_cast<unsigned>(txNew.vout.size()),
                         static_cast<unsigned>(txNew.vin.size()), target.ToString());

    // Sandbox tests can explicitly disable cycle verification while retaining the
    // transaction-specific target check. Creation must honor the same boundary:
    // solving a real E19 cycle here made send tests intermittently take tens of
    // seconds, and large-output weight-limit fixtures could spend minutes proving a
    // setup transaction even though those tests do not exercise cycle validity.
    // Hashing an arbitrary cycle until it meets the live target preserves the part
    // of the rule fTxPowNoCycle deliberately leaves enabled. This flag is ignored by
    // publictest and mainnet when chain parameters are constructed.
    if (cp.fTxPowNoCycle) {
        txNew.nPowNonce = 0;
        for (uint32_t i = 0; i < txNew.nCycle.size(); ++i) txNew.nCycle[i] = i + 1;
        while (UintToArith256(cuckatoo::CuckatooProofHash(txNew.nCycle)) > UintToArith256(target)) {
            if (tx_proof_cancel && tx_proof_cancel()) return _("Transfer proof-of-work canceled");
            txNew.nCycle[0] += static_cast<uint32_t>(txNew.nCycle.size());
        }
        return std::nullopt;
    }

    const uint256 anchor_hash = vault.chain().getBlockHash(anchor_height);
    // Consensus rebuilds this pre-image from the anchor it resolves at submit time. If
    // the two hashes differ the cycle cannot verify, however good the proof is.
    vault.VaultLogPrintf("tx-pow grind: anchor=%d anchor_hash=%s\n", anchor_height,
                         anchor_hash.ToString());
    const auto pre = CTransaction(txNew).PowPreimage(anchor_hash);

    cuckatoo::Cycle cyc{};
    uint32_t won = 0;
    // Sandbox remains fast on E19. Live and publictest E28 transfers may also
    // fall back to the CPU so a missing or failed accelerator never strands a
    // user, although that path can take substantially longer.
    const bool cpu_ok = AllowsTxPowCpuFallback(cp.nTxEdgeBits);
    uint32_t start_nonce = 0;
    bool proved = false;
    while (!proved) {
        // Polled here as well as inside the solver: a sweep that finds a cycle which
        // misses the target comes back around, and a caller that asked to stop should
        // not be made to wait out another one.
        if (tx_proof_cancel && tx_proof_cancel()) return _("Transfer proof-of-work canceled");
        if (!cuckatoo::CuckatooSolveBytes(pre.data(), pre.size(), cp.nTxEdgeBits, start_nonce, 1u << 24, cyc, won, cpu_ok, tx_proof_progress, tx_proof_cancel)) {
            // A cancelled solve also returns false, so ask before blaming the hardware:
            // reporting a missing GPU solver to someone who pressed Cancel sends them
            // looking for a fault that is not there.
            if (tx_proof_cancel && tx_proof_cancel()) return _("Transfer proof-of-work canceled");
            if (!cpu_ok) {
                return _("This transfer could not be proved. Quicksilver charges no fees, so every transfer is paid for with proof-of-work, and that work needs a CUDA-capable graphics card. Open the GPU solver setting, choose the solver executable for your card, and restart. Nothing was sent and your funds are untouched.");
            }
            return _("Could not produce per-tx proof-of-work");
        }
        // A cycle is necessary but no longer sufficient: it must also meet the
        // target this transaction's size implies. That check was latent before --
        // required work was 1 everywhere, so any cycle passed. It is not 1 now.
        if (UintToArith256(cuckatoo::CuckatooProofHash(cyc)) <= UintToArith256(target)) {
            proved = true;
            break;
        }
        if (won == std::numeric_limits<uint32_t>::max()) {
            return _("Could not produce per-tx proof-of-work");
        }
        start_nonce = won + 1;
    }
    txNew.nCycle = cyc;
    txNew.nPowNonce = won;
    return std::nullopt;
}

//! What the proof-of-work grind and the finalize step need out of the build step.
//!
//! The grind runs for tens of seconds to minutes at mainnet edge bits, and it must
//! not run with cs_vault held — see CreateTransaction below. So the build step ends
//! here, hands this back, and releases the lock.
struct BuiltTransaction {
    CMutableTransaction txNew;
    std::optional<unsigned int> change_pos;
    //! Upper bound on the finished serialization, what the proof is ground against.
    uint64_t pow_bytes;
    int pow_anchor_height;
    //! Kept alive across the grind so the change key is still reserved when the
    //! finalize step keeps it. Must be destroyed with cs_vault held: returning an
    //! unkept key to the pool touches the vault.
    std::unique_ptr<ReserveDestination> reservedest;
    //! Carried purely so the coin-selection log line reads the same as it always has.
    std::string algo_name;
    size_t inputs;
    int nBytes;
};

static util::Result<BuiltTransaction> BuildTransactionForProof(
        CVault& vault,
        const std::vector<CRecipient>& vecSend,
        std::optional<unsigned int> change_pos,
        const CCoinControl& coin_control,
        bool sign) EXCLUSIVE_LOCKS_REQUIRED(vault.cs_vault)
{
    AssertLockHeld(vault.cs_vault);

    FastRandomContext rng_fast;
    CMutableTransaction txNew; // The resulting transaction that we make

    if (coin_control.m_version) {
        txNew.version = coin_control.m_version.value();
    }

    CoinSelectionParams coin_selection_params{rng_fast}; // Parameters for coin selection, init with dummy
    coin_selection_params.m_avoid_partial_spends = coin_control.m_avoid_partial_spends;
    coin_selection_params.m_include_unsafe_inputs = coin_control.m_include_unsafe_inputs;
    coin_selection_params.m_max_tx_weight = coin_control.m_max_tx_weight.value_or(MAX_STANDARD_TX_WEIGHT);
    int minimum_tx_weight = MIN_STANDARD_TX_NONWITNESS_SIZE * WITNESS_SCALE_FACTOR;
    if (coin_selection_params.m_max_tx_weight.value() < minimum_tx_weight || coin_selection_params.m_max_tx_weight.value() > MAX_STANDARD_TX_WEIGHT) {
        return util::Error{strprintf(_("Maximum transaction weight must be between %d and %d"), minimum_tx_weight, MAX_STANDARD_TX_WEIGHT)};
    }
    // Static vsize overhead + outputs vsize. 4 nVersion, 4 nLocktime, 1 input count, 1 witness overhead (dummy, flag, stack size)
    coin_selection_params.tx_noinputs_size = 10 + GetSizeOfCompactSize(vecSend.size()); // bytes for output count

    CAmount recipients_sum = 0;
    const OutputType change_type = vault.TransactionChangeType(coin_control.m_change_type ? *coin_control.m_change_type : vault.m_default_change_type, vecSend);
    auto reservedest = std::make_unique<ReserveDestination>(&vault, change_type);
    for (const auto& recipient : vecSend) {
        // Quicksilver is feeless/exact-change: no dust floor on recipient amounts.

        // Include this recipient in the transaction's no-input size estimate.
        coin_selection_params.tx_noinputs_size += GetSerializeSizeForRecipient(recipient);
        recipients_sum += recipient.nAmount;
    }

    // Create change script that will be used if we need change
    CScript scriptChange;
    bilingual_str error; // possible error str

    // coin control: send change to custom address
    if (!std::get_if<CNoDestination>(&coin_control.destChange)) {
        scriptChange = GetScriptForDestination(coin_control.destChange);
    } else { // no coin control: send change to newly generated address
        // Note: We use a new key here to keep it from being obvious which side is the change.
        //  The drawback is that by not reusing a previous key, the change may be lost if a
        //  backup is restored, if the backup doesn't have the new private key for the change.
        //  If we reused the old key, it would be possible to add code to look for and
        //  rediscover unknown transactions that were written with keys of ours to recover
        //  post-backup change.

        // Reserve a new key pair from key pool. If it fails, provide a dummy
        // destination in case we don't need change.
        CTxDestination dest;
        auto op_dest = reservedest->GetReservedDestination(true);
        if (!op_dest) {
            error = _("Transaction needs a change address, but we can't generate it.") + Untranslated(" ") + util::ErrorString(op_dest);
        } else {
            dest = *op_dest;
            scriptChange = GetScriptForDestination(dest);
        }
        // A valid destination implies a change script (and
        // vice-versa). An empty change script will abort later, if the
        // change keypool ran out, but change is required.
        CHECK_NONFATAL(IsValidDestination(dest) != scriptChange.empty());
    }
    CTxOut change_prototype_txout(0, scriptChange);
    coin_selection_params.change_output_size = GetSerializeSize(change_prototype_txout);

    CAmount selection_target = recipients_sum;

    // This can only happen if requested destinations are value of 0 (e.g. OP_RETURN)
    // and no pre-selected inputs. This will result in 0-input transaction, which is consensus-invalid anyways.
    if (selection_target == 0 && !coin_control.HasSelected()) {
        return util::Error{_("Transaction requires one destination of non-0 value or a pre-selected input")};
    }

    // Fetch manually selected coins
    PreSelectedInputs preset_inputs;
    if (coin_control.HasSelected()) {
        auto res_fetch_inputs = FetchSelectedInputs(vault, coin_control);
        if (!res_fetch_inputs) return util::Error{util::ErrorString(res_fetch_inputs)};
        preset_inputs = *res_fetch_inputs;
    }

    // Fetch vault available coins if "other inputs" are
    // allowed (coins automatically selected by the vault)
    CoinsResult available_coins;
    if (coin_control.m_allow_other_inputs) {
        available_coins = AvailableCoins(vault, &coin_control);
    }

    // Choose coins to use
    auto select_coins_res = SelectCoins(vault, available_coins, preset_inputs, /*nTargetValue=*/selection_target, coin_control, coin_selection_params);
    if (!select_coins_res) {
        // 'SelectCoins' either returns a specific error message or, if empty, means a general "Insufficient funds".
        const bilingual_str& err = util::ErrorString(select_coins_res);
        return util::Error{err.empty() ?_("Insufficient funds") : err};
    }
    const SelectionResult& result = *select_coins_res;
    TRACEPOINT(coin_selection, selected_coins,
           vault.GetName().c_str(),
           GetAlgorithmName(result.GetAlgo()).c_str(),
           result.GetTarget(),
           0,
           result.GetSelectedValue());

    // vouts to the payees
    txNew.vout.reserve(vecSend.size() + 1); // + 1 because of possible later insert
    for (const auto& recipient : vecSend)
    {
        txNew.vout.emplace_back(recipient.nAmount, GetScriptForDestination(recipient.dest));
    }
    const CAmount change_amount = result.GetChange();
    if (change_amount > 0) {
        CTxOut newTxOut(change_amount, scriptChange);
        if (!change_pos) {
            // Insert change txn at random position:
            change_pos = rng_fast.randrange(txNew.vout.size() + 1);
        } else if ((unsigned int)*change_pos > txNew.vout.size()) {
            return util::Error{_("Transaction change output index out of range")};
        }
        txNew.vout.insert(txNew.vout.begin() + *change_pos, newTxOut);
    } else {
        change_pos = std::nullopt;
    }

    // Shuffle selected coins and fill in final vin
    std::vector<std::shared_ptr<COutput>> selected_coins = result.GetShuffledInputVector();

    if (coin_control.HasSelected() && coin_control.HasSelectedOrder()) {
        // When there are preselected inputs, we need to move them to be the first UTXOs
        // and have them be in the order selected. We can use stable_sort for this, where we
        // compare with the positions stored in coin_control. The COutputs that have positions
        // will be placed before those that don't, and those positions will be in order.
        std::stable_sort(selected_coins.begin(), selected_coins.end(),
            [&coin_control](const std::shared_ptr<COutput>& a, const std::shared_ptr<COutput>& b) {
                auto a_pos = coin_control.GetSelectionPos(a->outpoint);
                auto b_pos = coin_control.GetSelectionPos(b->outpoint);
                if (a_pos.has_value() && b_pos.has_value()) {
                    return a_pos.value() < b_pos.value();
                } else if (a_pos.has_value() && !b_pos.has_value()) {
                    return true;
                } else {
                    return false;
                }
            });
    }

    // The sequence number is set to non-maxint so that height-based locktime works.
    //
    bool use_fresh_locktime = true;
    // Never SEQUENCE_FINAL: a final sequence stops nLockTime binding, which is what
    // the fresh tip-relative locktime above depends on.
    const uint32_t default_sequence{CTxIn::MAX_SEQUENCE_NONFINAL};
    txNew.vin.reserve(selected_coins.size());
    for (const auto& coin : selected_coins) {
        std::optional<uint32_t> sequence = coin_control.GetSequence(coin->outpoint);
        if (sequence) {
            // If an input has a preset sequence, we cannot use default locktime handling.
            use_fresh_locktime = false;
        }
        txNew.vin.emplace_back(coin->outpoint, CScript{}, sequence.value_or(default_sequence));

        auto scripts = coin_control.GetScripts(coin->outpoint);
        if (scripts.first) {
            txNew.vin.back().scriptSig = *scripts.first;
        }
        if (scripts.second) {
            txNew.vin.back().scriptWitness = *scripts.second;
        }
    }
    if (coin_control.m_locktime) {
        txNew.nLockTime = coin_control.m_locktime.value();
        // If we have a locktime set, we cannot use default locktime handling.
        use_fresh_locktime = false;
    }
    if (use_fresh_locktime) {
        SetFreshVaultLocktime(txNew, rng_fast, vault.chain(), vault.GetLastBlockHash(), vault.GetLastBlockHeight());
    }

    TxSize tx_sizes = CalculateMaximumSignedTxSize(CTransaction(txNew), &vault, &coin_control);
    int nBytes = tx_sizes.vsize;
    if (nBytes == -1) {
        return util::Error{_("Missing solving data for estimating transaction size")};
    }
    const CAmount output_value = CalculateOutputValue(txNew);
    Assume(recipients_sum + change_amount == output_value);

    if (result.GetSelectedValue() != output_value) {
        return util::Error{_("Transaction inputs and outputs must preserve value exactly.")};
    }

    // Give up if change keypool ran out and change is required
    if (scriptChange.empty() && change_pos) {
        return util::Error{error};
    }

    if (sign && !vault.SignTransaction(txNew)) {
        return util::Error{_("Signing transaction failed")};
    }

    // Limit size. This runs BEFORE the proof-of-work grind below, not after it. The
    // per-transaction target scales with the transaction's serialized size, so an
    // oversized transaction asks for the most expensive grind the vault can be made
    // to perform — and is then refused here however that grind turns out. With the
    // order reversed, `send` on an over-weight transaction spent minutes grinding a
    // proof for a transaction it was always going to reject, and often enough ran
    // past the caller's timeout instead of returning the error it already knew.
    //
    // Checking here does not undercount: the grind writes only nAnchorHeight,
    // nCycle and nPowNonce, and those are a fixed word, a fixed 42-word array and a
    // fixed word, so they occupy the same bytes ground or not.
    if ((sign && GetTransactionWeight(CTransaction(txNew)) > MAX_STANDARD_TX_WEIGHT) ||
        (!sign && tx_sizes.weight > MAX_STANDARD_TX_WEIGHT))
    {
        return util::Error{_("Transaction too large")};
    }

    // Quicksilver: resolve the anchor the per-tx Cuckatoo proof will commit to. The
    // grind itself is the caller's job and runs *after* this function returns, with
    // cs_vault released — see CreateTransaction. Resolving the anchor here keeps it
    // under the same lock as coin selection, and keeps the size check ahead of the
    // grind so that a transaction the vault is about to refuse never costs one.
    // #5c-1: anchor the proof to the current chain tip and commit that block's hash,
    // so the proof cannot have been precomputed and is recent (within nMaxAnchorAge).
    const auto anchor_height_result = TxPowAnchorHeight(vault);
    if (!anchor_height_result) return util::Error{util::ErrorString(anchor_height_result)};
    const int pow_anchor_height = *anchor_height_result;
    return BuiltTransaction{
        .txNew = std::move(txNew),
        .change_pos = change_pos,
        .pow_bytes = static_cast<uint64_t>(tx_sizes.bytes),
        .pow_anchor_height = pow_anchor_height,
        .reservedest = std::move(reservedest),
        .algo_name = GetAlgorithmName(result.GetAlgo()),
        .inputs = result.GetInputSet().size(),
        .nBytes = nBytes,
    };
}

util::Result<CreatedTransactionResult> CreateTransaction(
        CVault& vault,
        const std::vector<CRecipient>& vecSend,
        std::optional<unsigned int> change_pos,
        const CCoinControl& coin_control,
        bool sign)
{
    return CreateTransaction(vault, vecSend, change_pos, coin_control, sign, {}, {});
}

util::Result<CreatedTransactionResult> CreateTransaction(
        CVault& vault,
        const std::vector<CRecipient>& vecSend,
        std::optional<unsigned int> change_pos,
        const CCoinControl& coin_control,
        bool sign,
        const std::function<void(uint32_t nonce)>& tx_proof_progress,
        const cuckatoo::SolverCancelCallback& tx_proof_cancel)
{
    if (vecSend.empty()) {
        return util::Error{_("Transaction must have at least one recipient")};
    }

    if (std::any_of(vecSend.cbegin(), vecSend.cend(), [](const auto& recipient){ return recipient.nAmount < 0; })) {
        return util::Error{_("Transaction amounts must not be negative")};
    }

    // Three phases, and the middle one deliberately holds no vault lock.
    //
    // The per-transaction proof-of-work runs for tens of seconds to minutes at
    // mainnet edge bits. Holding cs_vault across it stops every other vault
    // operation for that whole time — the desktop's own timers and transaction
    // table, and every RPC. Measured on a GUI test node before this was split: `getbalances`
    // took 235 s against 0.00 s on an idle node, and the GUI thread sat blocked
    // from the moment Transmit was clicked until the grind finished, so the
    // progress panel it had just put up never repainted and its Cancel button
    // could not be clicked.
    //
    // The grind does not need the lock. It reads the chain, not the vault, and
    // writes only nAnchorHeight, nCycle and nPowNonce on a transaction that is by
    // then entirely local to this call.
    const auto trace_result = [&vault](bool ok, const std::optional<unsigned int>& pos) {
        TRACEPOINT(coin_selection, normal_create_tx_internal,
               vault.GetName().c_str(),
               ok,
               pos.has_value() ? int32_t(*pos) : -1);
    };

    // Phase 1 — select, build and sign, with the vault locked.
    std::optional<BuiltTransaction> built;
    {
        LOCK(vault.cs_vault);
        auto res = BuildTransactionForProof(vault, vecSend, change_pos, coin_control, sign);
        if (!res) {
            trace_result(false, std::nullopt);
            return util::Error{util::ErrorString(res)};
        }
        built.emplace(std::move(*res));
    }

    // Phase 2 — grind the proof, unlocked.
    const auto grind_pow = [&](uint64_t bytes) -> std::optional<bilingual_str> {
        auto result = GrindTransactionPow(vault, built->txNew, bytes, built->pow_anchor_height, tx_proof_progress, tx_proof_cancel);
        return result;
    };
    std::optional<bilingual_str> grind_err{grind_pow(built->pow_bytes)};

    // The proof above was ground against an ESTIMATE of the final size. The estimate
    // is an upper bound by construction, but it is built from descriptor metadata
    // that has been wrong before: upstream's taproot estimator assumed a key-path
    // spend and underestimated script-path spends by orders of magnitude, which under
    // byte-priced work means a transaction the network rejects. Where we signed
    // locally we now know the true size, so check rather than trust.
    //
    // Regrinding here is safe: the signature hash does not cover nAnchorHeight,
    // nCycle or nPowNonce, so a new proof does not invalidate what was signed in
    // phase 1. In the PSQT/offline flow (sign == false) the final transaction is
    // assembled elsewhere and this check cannot run — there the estimate must simply
    // be right, which is why the taproot estimator was fixed rather than papered
    // over here.
    if (!grind_err && sign) {
        const CTransaction final_tx{built->txNew};
        const uint64_t final_bytes{static_cast<uint64_t>(::GetSerializeSize(TX_WITH_WITNESS(final_tx)))};
        const uint256 final_target = vault.chain().txPowTarget(
            built->pow_anchor_height, final_bytes, built->txNew.vout.size(), built->txNew.vin.size());
        // Keyed by txid, not by the proof hash: tests running -txpownocycle fabricate a
        // deterministic cycle, so hundreds of transactions share one proof hash and a
        // join on it silently collapses them into a single, wrong sample.
        vault.VaultLogPrintf("tx-pow final: tx=%s anchor=%d bytes=%u nout=%u nin=%u target=%s proof=%s\n",
                             final_tx.GetHash().ToString(),
                             built->pow_anchor_height, static_cast<unsigned>(final_bytes),
                             static_cast<unsigned>(built->txNew.vout.size()),
                             static_cast<unsigned>(built->txNew.vin.size()),
                             final_target.ToString(),
                             cuckatoo::CuckatooProofHash(built->txNew.nCycle).ToString());
        if (UintToArith256(cuckatoo::CuckatooProofHash(built->txNew.nCycle)) > UintToArith256(final_target)) {
            vault.VaultLogPrintf("tx-pow: size estimate ran low (%u estimated, %u actual); regrinding\n",
                                 static_cast<unsigned>(built->pow_bytes), static_cast<unsigned>(final_bytes));
            grind_err = grind_pow(final_bytes);
        }
    }

    // Phase 3 — finalize, with the vault locked again. `built` is reset explicitly
    // on every path out of here: destroying it returns an unkept change key to the
    // pool, and that touches the vault, so it must happen while the lock is held.
    LOCK(vault.cs_vault);

    if (grind_err) {
        built.reset();
        trace_result(false, std::nullopt);
        return util::Error{*grind_err};
    }
    CTransactionRef tx = MakeTransactionRef(std::move(built->txNew));

    if (gArgs.GetBoolArg("-vaultrejectlongchains", DEFAULT_VAULT_REJECT_LONG_CHAINS)) {
        // Lastly, ensure this tx will pass the relaypool's chain limits
        auto limits = vault.chain().checkChainLimits(tx);
        if (!limits) {
            built.reset();
            trace_result(false, std::nullopt);
            return util::Error{util::ErrorString(limits)};
        }
    }

    // Before we return success, we assume any change key will be used to prevent
    // accidental reuse.
    built->reservedest->KeepDestination();

    vault.VaultLogPrintf("Coin Selection: Algorithm:%s Inputs:%u\n", built->algo_name, built->inputs);
    vault.VaultLogPrintf("Transaction construction: Bytes:%u (exact value)\n", built->nBytes);

    const std::optional<unsigned int> final_change_pos{built->change_pos};
    built.reset();
    trace_result(true, final_change_pos);
    return CreatedTransactionResult(tx, final_change_pos);
}

util::Result<CreatedTransactionResult> FundTransaction(CVault& vault, const CMutableTransaction& tx, const std::vector<CRecipient>& vecSend, std::optional<unsigned int> change_pos, bool lockUnspents, CCoinControl coinControl)
{
    // We want to make sure tx.vout is not used now that we are passing outputs as a vector of recipients.
    // This sets us up to remove tx completely in a future PR in favor of passing the inputs directly.
    assert(tx.vout.empty());

    // Set the user desired locktime
    coinControl.m_locktime = tx.nLockTime;

    // Set the user desired version
    coinControl.m_version = tx.version;

    // The lock is deliberately NOT held across CreateTransaction. It used to be, to
    // close the race to the new locked unspents between selection and the LockCoin
    // calls below — but CreateTransaction grinds a proof-of-work that runs for
    // minutes at mainnet edge bits, and holding cs_vault across it freezes every
    // other vault operation for that whole time. Narrowing that race is not worth
    // stalling the vault, the GUI and every RPC to do it; the same exposure already
    // exists between any CreateTransaction and its commit.
    {
        LOCK(vault.cs_vault);

        // Fetch specified UTXOs from the UTXO set to get the scriptPubKeys and values of the outputs being selected
        // and to match with the given solving_data. Only used for non-vault outputs.
        std::map<COutPoint, Coin> coins;
        for (const CTxIn& txin : tx.vin) {
            coins[txin.prevout]; // Create empty map entry keyed by prevout.
        }
        vault.chain().findCoins(coins);

        for (const CTxIn& txin : tx.vin) {
            const auto& outPoint = txin.prevout;
            PreselectedInput& preset_txin = coinControl.Select(outPoint);
            if (!vault.IsMine(outPoint)) {
                if (coins[outPoint].out.IsNull()) {
                    return util::Error{_("Unable to find UTXO for external input")};
                }

                // The input was not in the vault, but is in the UTXO set, so select as external
                preset_txin.SetTxOut(coins[outPoint].out);
            }
            preset_txin.SetSequence(txin.nSequence);
            preset_txin.SetScriptSig(txin.scriptSig);
            preset_txin.SetScriptWitness(txin.scriptWitness);
        }
    }

    auto res = CreateTransaction(vault, vecSend, change_pos, coinControl, false);
    if (!res) {
        return res;
    }

    if (lockUnspents) {
        LOCK(vault.cs_vault);
        for (const CTxIn& txin : res->tx->vin) {
            vault.LockCoin(txin.prevout);
        }
    }

    return res;
}

// Defined here rather than in vault.cpp: building the bundle needs AvailableCoins, and
// vault including spend would close a vault -> spend -> vault cycle. Coin enumeration
// belongs to the spend layer, so the definition lives where the dependency already points.
util::Result<AgentAllotmentPolicyBundle> CVault::ExportAgentAllotmentPolicyBundle(const std::string& request_json)
{
    auto metadata{ValidateAgentAllotmentPolicyRequest(request_json)};
    if (!metadata) return util::Error{util::ErrorString(metadata)};

    const CTxDestination dest{DecodeDestination(metadata->funding_address)};
    if (!IsValidDestination(dest)) {
        return util::Error{Untranslated("Agent allotment policy request funding address is not valid for this chain.")};
    }

    // IsMine and GetScriptPubKeyMans both read m_cached_spks. Callers are bare,
    // so the lock is taken here and held through AvailableCoins / LockCoin.
    LOCK(cs_vault);

    if (!(IsMine(dest) & ISMINE_SPENDABLE)) {
        return util::Error{Untranslated("Agent allotment policy request funding address is not spendable by this vault.")};
    }

    const CScript funding_script{GetScriptForDestination(dest)};
    std::unique_ptr<FlatSigningProvider> provider;
    for (const auto spk_man : GetScriptPubKeyMans(funding_script)) {
        const auto desc_spk_man{dynamic_cast<DescriptorScriptPubKeyMan*>(spk_man)};
        if (!desc_spk_man) continue;
        provider = desc_spk_man->GetPrivateSigningProvider(funding_script);
        if (provider) break;
    }
    if (!provider) {
        return util::Error{Untranslated("Agent allotment policy request funding address key metadata is not available.")};
    }

    const CKeyID key_id{GetKeyForDestination(*provider, dest)};
    if (key_id.IsNull()) {
        return util::Error{Untranslated("Agent allotment policy request funding address does not map to a single exportable key.")};
    }

    CKey funding_key;
    if (!provider->GetKey(key_id, funding_key) || !funding_key.IsValid()) {
        return util::Error{Untranslated("Agent allotment policy request funding private key is not available.")};
    }

    std::vector<AgentAllotmentFundingOutput> funding_outputs;
    // Enumerate the agent's outputs INCLUDING ones this vault has already locked for
    // it. A previous export locks them (below), and a refreshed bundle for the same
    // agent must still list what that agent holds.
    CoinFilterParams coins_params;
    coins_params.skip_locked = false;
    std::vector<COutPoint> outpoints_to_lock;
    for (const COutput& coin : AvailableCoins(*this, /*coinControl=*/nullptr, coins_params).All()) {
        if (coin.txout.scriptPubKey != funding_script || !MoneyRange(coin.txout.nValue)) continue;
        funding_outputs.push_back(AgentAllotmentFundingOutput{
            .txid = coin.outpoint.hash.ToString(),
            .vout = coin.outpoint.n,
            .amount = coin.txout.nValue,
        });
        outpoints_to_lock.push_back(coin.outpoint);
    }

    // From here the agent holds a key that signs for these outputs, so both sides can
    // spend them. Lock them so ordinary coin selection in this vault cannot: keys are
    // shared by the user's decision, but the two spenders do not have to race for the
    // same coins. A collision is not a double-spend — it wastes a full grind, which is
    // 50 seconds on a GPU and about 16 minutes on a CPU. Coin control still shows them
    // as locked, so an explicit sweep remains possible.
    if (!outpoints_to_lock.empty()) {
        if (!RunWithinTxn(GetDatabase(), /*process_desc=*/"lock agent funding outputs",
                          [&](VaultBatch& batch) EXCLUSIVE_LOCKS_REQUIRED(cs_vault) {
                              for (const COutPoint& outpoint : outpoints_to_lock) {
                                  if (!LockCoin(outpoint, &batch)) return false;
                              }
                              return true;
                          })) {
            return util::Error{Untranslated("Agent allotment funding outputs could not be reserved for the agent.")};
        }
    }

    UniValue policy{UniValue::VOBJ};
    if (!policy.read(request_json) || !policy.isObject()) {
        return util::Error{Untranslated("Agent allotment policy request must be a JSON object.")};
    }

    AgentAllotmentPolicyBundle bundle;
    bundle.metadata = *metadata;
    bundle.policy_request = request_json;
    bundle.funding_secret = EncodeSecret(funding_key);
    bundle.funding_outputs = funding_outputs;

    UniValue bundle_json{UniValue::VOBJ};
    bundle_json.pushKV("type", "quicksilver.agent_allotment_key_bundle");
    bundle_json.pushKV("version", 1);
    bundle_json.pushKV("policy_request", policy);
    bundle_json.pushKV("funding_address", metadata->funding_address);
    bundle_json.pushKV("funding_secret_wif", bundle.funding_secret);
    UniValue funding_outputs_json{UniValue::VARR};
    for (const AgentAllotmentFundingOutput& output : bundle.funding_outputs) {
        UniValue output_json{UniValue::VOBJ};
        output_json.pushKV("txid", output.txid);
        output_json.pushKV("vout", static_cast<uint64_t>(output.vout));
        output_json.pushKV("amount_cinnabar", util::ToString(output.amount));
        funding_outputs_json.push_back(std::move(output_json));
    }
    bundle_json.pushKV("funding_outputs", std::move(funding_outputs_json));
    bundle_json.pushKV("policy_enforcement", metadata->policy_status == AgentAllotmentPolicyStatus::Enforced ? "enforced" : "pending_integration");
    bundle.bundle_json = bundle_json.write();
    return bundle;
}
} // namespace vault
