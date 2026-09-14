// Copyright (c) 2021-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_VAULT_SPEND_H
#define QUICKSILVER_VAULT_SPEND_H

#include <consensus/amount.h>
#include <crypto/cuckatoo/cuckatoo.h>
#include <util/result.h>
#include <vault/coinselection.h>
#include <vault/transaction.h>
#include <vault/vault.h>

#include <cstdint>
#include <functional>
#include <optional>

namespace vault {
/** Get the marginal bytes if spending the specified output from this transaction.
 * Use CoinControl to determine whether to expect signature grinding when calculating the size of the input spend. */
int CalculateMaximumSignedInputSize(const CTxOut& txout, const CVault* pvault, const CCoinControl* coin_control);
int CalculateMaximumSignedInputSize(const CTxOut& txout, const SigningProvider* pvault, bool can_grind_r, const CCoinControl* coin_control);
/** Quicksilver: how far back from the tip the vault anchors a per-tx proof.
 *
 * The proof commits to the anchor block's HASH, so if that block is later
 * orphaned the pre-image changes and the proof is dead — a full regrind. Anchoring
 * at the tip made a transaction vulnerable to any one-block reorg that replaced it,
 * and the exposure lasts the whole grind: about 7 blocks on GPU for an ordinary
 * payment, far longer for a large one. Shallow reorgs are ordinary at the launch
 * difficulty floor of 2 cycles per block.
 *
 * Six blocks costs 6 of the 100-block nMaxAnchorAge window, leaving ~7.8 hours of
 * slack, and buys immunity to any reorg shallower than 6. It opens no new surface:
 * a sender could already choose any anchor inside the window.
 */
static constexpr int VAULT_ANCHOR_DEPTH{6};

/** The height a per-tx proof should anchor to: VAULT_ANCHOR_DEPTH below the tip. */
util::Result<int> TxPowAnchorHeight(CVault& vault);

/** Whether this shipped graph size may fall back to built-in CPU solving. */
constexpr bool AllowsTxPowCpuFallback(uint8_t edgebits)
{
    return edgebits == 19 || edgebits == 28;
}

/** Quicksilver: grind `txNew`'s per-tx Cuckatoo proof and write the tail into it.
 *
 * Sets nAnchorHeight, nCycle and nPowNonce. `bytes` is the WITH-witness serialized
 * size the proof is priced against -- an upper bound before signing, or the exact
 * size after. Returns an error string on failure, std::nullopt on success.
 *
 * Every path that puts a transaction on the wire must call this. It is a free
 * function rather than a step inside CreateTransaction precisely because it was
 * one: sendall builds its transaction directly and so shipped without a proof,
 * producing transactions the node itself refused to broadcast while the RPC
 * reported success.
 *
 * `cancel` is polled from inside the solve loops, not merely between them. A grind
 * holds cs_vault for its whole duration and can run for minutes, so a caller that
 * wants to stop one -- a user abandoning a transfer, or a close that must not delete
 * the vault under a running worker -- has no other way to be let go of.
 */
std::optional<bilingual_str> GrindTransactionPow(CVault& vault, CMutableTransaction& txNew,
                                                 uint64_t bytes, int anchor_height,
                                                 const cuckatoo::SolverProgressCallback& progress = {},
                                                 const cuckatoo::SolverCancelCallback& cancel = {});

struct TxSize {
    int64_t vsize{-1};
    int64_t weight{-1};
    //! Upper bound on the WITH-witness serialized size once signed. Distinct from
    //! weight, which is 3*base + total and so overstates bytes by up to 4x. The
    //! Stage 2 size term prices bytes, and a sender grinds before its signature
    //! bytes exist, so this is the number it must grind against — using weight
    //! instead over-charges a zero-witness transaction by exactly 4x.
    int64_t bytes{-1};
};

/** Calculate the size of the transaction using CoinControl to determine
 * whether to expect signature grinding when calculating the size of the input spend. */
TxSize CalculateMaximumSignedTxSize(const CTransaction& tx, const CVault* vault, const std::vector<CTxOut>& txouts, const CCoinControl* coin_control = nullptr);
TxSize CalculateMaximumSignedTxSize(const CTransaction& tx, const CVault* vault, const CCoinControl* coin_control = nullptr) EXCLUSIVE_LOCKS_REQUIRED(vault->cs_vault);

/**
 * COutputs available for spending, stored by OutputType.
 * This struct is really just a wrapper around OutputType vectors with a convenient
 * method for concatenating and returning all COutputs as one vector.
 *
 * Size(), Clear(), Erase(), Shuffle(), and Add() methods are implemented to
 * allow easy interaction with the struct.
 */
struct CoinsResult {
    std::map<OutputType, std::vector<COutput>> coins;

    /** Concatenate and return all COutputs as one vector */
    std::vector<COutput> All() const;

    /** The following methods are provided so that CoinsResult can mimic a vector,
     * i.e., methods can work with individual OutputType vectors or on the entire object */
    size_t Size() const;
    void Clear();
    void Erase(const std::unordered_set<COutPoint, SaltedOutpointHasher>& coins_to_remove);
    void Shuffle(FastRandomContext& rng_fast);
    void Add(OutputType type, const COutput& out);

    CAmount GetTotalAmount() { return total_amount; }

private:
    /** Sum of all available coins raw value */
    CAmount total_amount{0};
};

struct CoinFilterParams {
    // Outputs below the minimum amount will not get selected
    CAmount min_amount{1};
    // Outputs above the maximum amount will not get selected
    CAmount max_amount{MAX_MONEY};
    // Return outputs until the minimum sum amount is covered
    CAmount min_sum_amount{MAX_MONEY};
    // Maximum number of outputs that can be returned
    uint64_t max_count{0};
    // By default, do not include immature coinbase outputs
    bool include_immature_coinbase{false};
    // By default, skip locked UTXOs
    bool skip_locked{true};
};

/**
 * Populate the CoinsResult struct with vectors of available COutputs, organized by OutputType.
 */
CoinsResult AvailableCoins(const CVault& vault,
                           const CCoinControl* coinControl = nullptr,
                           const CoinFilterParams& params = {}) EXCLUSIVE_LOCKS_REQUIRED(vault.cs_vault);

/**
 * Find non-change parent output.
 */
const CTxOut& FindNonChangeParentOutput(const CVault& vault, const COutPoint& outpoint) EXCLUSIVE_LOCKS_REQUIRED(vault.cs_vault);

/**
 * Return list of available coins and locked coins grouped by non-change output address.
 */
std::map<CTxDestination, std::vector<COutput>> ListCoins(const CVault& vault) EXCLUSIVE_LOCKS_REQUIRED(vault.cs_vault);

struct SelectionFilter {
    CoinEligibilityFilter filter;
    bool allow_mixed_output_types{true};
};

/**
* Group coins by the provided filters.
*/
FilteredOutputGroups GroupOutputs(const CVault& vault,
                          const CoinsResult& coins,
                          const CoinSelectionParams& coin_sel_params,
                          const std::vector<SelectionFilter>& filters);

/**
 * Attempt to find a valid input set. `ChooseSelectionResult()` is called on each
 * OutputType individually and, when allowed, over the mixed pool. Exact raw-value
 * matches are preferred before largest-first fallback results.
 *
 * @param[in]  nTargetValue              The target value
 * @param[in]  groups                    The grouped outputs mapped by coin eligibility filters
 * @param[in]  coin_selection_params     Parameters for the coin selection
 * @param[in]  allow_mixed_output_types  Relax restriction that SelectionResults must be of the same OutputType
 * returns                               If successful, a SelectionResult containing the input set
 *                                       If failed, returns (1) an empty error message if the target was not reached (general "Insufficient funds")
 *                                                  or (2) a specific error message if there was something particularly wrong (e.g. a selection
 *                                                  result that surpassed the tx max weight size).
 */
util::Result<SelectionResult> AttemptSelection(const CAmount& nTargetValue, OutputGroupTypeMap& groups,
                        const CoinSelectionParams& coin_selection_params, bool allow_mixed_output_types);

/**
 * Attempt to find a valid input set that meets the provided eligibility filter and target.
 * Exact raw-value matches are tried before largest-first fallback with change.
 *
 * @param[in]  nTargetValue              The target value
 * @param[in]  groups                    The struct containing the outputs grouped by script and divided by (1) positive only outputs and (2) all outputs (positive + negative).
 * @param[in]  coin_selection_params     Parameters for the coin selection
 * returns                               If successful, a SelectionResult containing the input set
 *                                       If failed, returns (1) an empty error message if the target was not reached (general "Insufficient funds")
 *                                                  or (2) a specific error message if there was something particularly wrong (e.g. a selection
 *                                                  result that surpassed the tx max weight size).
 */
util::Result<SelectionResult> ChooseSelectionResult(const CAmount& nTargetValue, Groups& groups, const CoinSelectionParams& coin_selection_params);

// User manually selected inputs that must be part of the transaction
struct PreSelectedInputs
{
    std::set<std::shared_ptr<COutput>> coins;
    // Sum of manually selected output values.
    CAmount total_amount{0};

    void Insert(const COutput& output)
    {
        total_amount += output.txout.nValue;
        coins.insert(std::make_shared<COutput>(output));
    }
};

/**
 * Fetch and validate coin control selected inputs.
 * Coins could be internal (from the vault) or external.
*/
util::Result<PreSelectedInputs> FetchSelectedInputs(const CVault& vault, const CCoinControl& coin_control) EXCLUSIVE_LOCKS_REQUIRED(vault.cs_vault);

/**
 * Select a set of coins such that nTargetValue is met; never select unconfirmed coins if they are not ours
 * @param[in]   vault                 The vault which provides data necessary to spend the selected coins
 * @param[in]   available_coins        The struct of coins, organized by OutputType, available for selection prior to filtering
 * @param[in]   nTargetValue           The target value
 * @param[in]   coin_selection_params  Parameters for this coin selection such as maximum transaction weight,
 *                                     whether to avoid partial spends, and whether to include unsafe inputs.
 * returns                             If successful, a SelectionResult containing the selected coins
 *                                     If failed, returns (1) an empty error message if the target was not reached (general "Insufficient funds")
 *                                                or (2) an specific error message if there was something particularly wrong (e.g. a selection
 *                                                result that surpassed the tx max weight size).
 */
util::Result<SelectionResult> AutomaticCoinSelection(const CVault& vault, CoinsResult& available_coins, const CAmount& nTargetValue,
                 const CoinSelectionParams& coin_selection_params) EXCLUSIVE_LOCKS_REQUIRED(vault.cs_vault);

/**
 * Select all coins from coin_control, and if coin_control 'm_allow_other_inputs=true', call 'AutomaticCoinSelection' to
 * select a set of coins such that nTargetValue - pre_set_inputs.total_amount is met.
 */
util::Result<SelectionResult> SelectCoins(const CVault& vault, CoinsResult& available_coins, const PreSelectedInputs& pre_set_inputs,
                                          const CAmount& nTargetValue, const CCoinControl& coin_control,
                                          const CoinSelectionParams& coin_selection_params) EXCLUSIVE_LOCKS_REQUIRED(vault.cs_vault);

struct CreatedTransactionResult
{
    CTransactionRef tx;
    std::optional<unsigned int> change_pos;

    CreatedTransactionResult(CTransactionRef tx, std::optional<unsigned int> change_pos)
        : tx(std::move(tx)), change_pos(change_pos) {}
};

/**
 * Create a new transaction paying the recipients with a set of coins
 * selected by SelectCoins(); Also create the change output, when needed
 * @note passing change_pos as std::nullopt will result in setting a random position
 */
util::Result<CreatedTransactionResult> CreateTransaction(CVault& vault, const std::vector<CRecipient>& vecSend, std::optional<unsigned int> change_pos, const CCoinControl& coin_control, bool sign = true);
util::Result<CreatedTransactionResult> CreateTransaction(CVault& vault, const std::vector<CRecipient>& vecSend, std::optional<unsigned int> change_pos, const CCoinControl& coin_control, bool sign, const std::function<void(uint32_t nonce)>& tx_proof_progress, const cuckatoo::SolverCancelCallback& tx_proof_cancel = {});

/**
 * Insert additional inputs into the transaction by
 * calling CreateTransaction();
 */
util::Result<CreatedTransactionResult> FundTransaction(CVault& vault, const CMutableTransaction& tx, const std::vector<CRecipient>& recipients, std::optional<unsigned int> change_pos, bool lockUnspents, CCoinControl);
} // namespace vault

#endif // QUICKSILVER_VAULT_SPEND_H
