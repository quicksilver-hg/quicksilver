// Copyright (c) 2021-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <consensus/amount.h>
#include <crypto/cuckatoo/cuckatoo.h>
#include <pow.h>
#include <script/solver.h>
#include <validation.h>
#include <vault/coincontrol.h>
#include <vault/spend.h>
#include <vault/test/util.h>
#include <vault/test/vault_test_fixture.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <thread>

namespace vault {
BOOST_FIXTURE_TEST_SUITE(spend_tests, VaultTestingSetup)

struct NoCycleTestChain100Setup : TestChain100Setup {
    NoCycleTestChain100Setup()
        : TestChain100Setup{ChainType::SANDBOX, {.extra_args = {"-txpownocycle=1"}}}
    {
    }
};

BOOST_FIXTURE_TEST_CASE(ExactValueChange, TestChain100Setup)
{
    CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    auto vault = CreateSyncedVault(*m_node.chain, WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain()), coinbaseKey);
    const CAmount input_amount{WITH_LOCK(vault->cs_vault, return AvailableCoins(*vault).GetTotalAmount())};
    BOOST_REQUIRE_GT(input_amount, 0);

    // Quicksilver is feeless: tiny remainders remain exact-value change.
    auto check_tx = [&vault, input_amount](CAmount leftover_input_amount) {
        CRecipient recipient{PubKeyDestination({}), input_amount - leftover_input_amount};
        CCoinControl coin_control;
        coin_control.m_change_type = OutputType::BASE58;
        auto res = CreateTransaction(*vault, {recipient}, /*change_pos=*/std::nullopt, coin_control);
        BOOST_REQUIRE_MESSAGE(res, util::ErrorString(res).original);
        const auto& txr = *res;
        CAmount output_value{0};
        for (const auto& txout : txr.tx->vout) output_value += txout.nValue;
        BOOST_CHECK_EQUAL(output_value, input_amount);
        if (leftover_input_amount == 0) {
            BOOST_CHECK(!txr.change_pos);
            BOOST_REQUIRE_EQUAL(txr.tx->vout.size(), 1);
            BOOST_CHECK_EQUAL(txr.tx->vout[0].nValue, recipient.nAmount);
        } else {
            BOOST_REQUIRE(txr.change_pos);
            BOOST_REQUIRE_EQUAL(txr.tx->vout.size(), 2);
            BOOST_CHECK_EQUAL(txr.tx->vout[*txr.change_pos].nValue, leftover_input_amount);
            const unsigned int recipient_pos{*txr.change_pos == 0 ? 1U : 0U};
            BOOST_CHECK_EQUAL(txr.tx->vout[recipient_pos].nValue, recipient.nAmount);
        }
    };

    check_tx(0);
    check_tx(1);
    check_tx(123);
}

BOOST_FIXTURE_TEST_CASE(vault_duplicated_preset_inputs_test, TestChain100Setup)
{
    // Verify that the vault's Coin Selection process does not include pre-selected inputs twice in a transaction.

    // Add 4 spendable UTXO, 50 Hg each, to the vault (total balance 200 Hg)
    for (int i = 0; i < 4; i++) CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    auto vault = CreateSyncedVault(*m_node.chain, WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain()), coinbaseKey);

    LOCK(vault->cs_vault);
    auto available_coins = AvailableCoins(*vault);
    std::vector<COutput> coins = available_coins.All();
    // Preselect the first 3 UTXO (150 Hg total)
    std::set<COutPoint> preset_inputs = {coins[0].outpoint, coins[1].outpoint, coins[2].outpoint};

    // Try to create a tx that spends more than what preset inputs + vault selected inputs are covering for.
    // The vault can cover up to 200 Hg, and the tx target is 299 Hg.
    std::vector<CRecipient> recipients{{*Assert(vault->GetNewDestination(OutputType::BECH32, "dummy")),
                                           /*nAmount=*/299 * COIN}};
    CCoinControl coin_control;
    coin_control.m_allow_other_inputs = true;
    for (const auto& outpoint : preset_inputs) {
        coin_control.Select(outpoint);
    }

    // Attempt to send 299 Hg from a vault that only has 200 Hg. The vault should exclude
    // the preset inputs from the pool of available coins, realize that there is not enough
    // money to fund the 299 Hg payment, and fail with "Insufficient funds".
    //
    // If the vault does not properly exclude preset inputs from the pool of available coins
    // prior to coin selection, it may create a transaction that does not fund the full payment
    // amount.

    BOOST_CHECK(!CreateTransaction(*vault, recipients, /*change_pos=*/std::nullopt, coin_control));
}

// Stage 2: required work depends on serialized bytes, and PowPreimage covers neither
// scriptSig nor the witness -- so the grind happens before the bytes it is charged
// for exist. The vault must therefore grind against an UPPER BOUND on the final size,
// and the size estimate must actually be one. It was not: CalculateMaximumSignedTxSize
// never accounted for the per-tx PoW tail -- nAnchorHeight (4) + nPowNonce (4) +
// 42 * nCycle (168) = 176 bytes, or 704 weight -- so every estimate ran 704 short.
// Harmless while size did not affect validity; an under-grind afterwards.
BOOST_FIXTURE_TEST_CASE(MaximumSignedTxSizeIncludesThePowTail, TestChain100Setup)
{
    CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    auto vault = CreateSyncedVault(*m_node.chain, WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain()), coinbaseKey);

    CRecipient recipient{PubKeyDestination({}), 1 * COIN};
    CCoinControl coin_control;
    coin_control.m_change_type = OutputType::BASE58;
    auto res = CreateTransaction(*vault, {recipient}, /*change_pos=*/std::nullopt, coin_control);
    BOOST_REQUIRE_MESSAGE(res, util::ErrorString(res).original);
    const CTransactionRef tx = res->tx;

    LOCK(vault->cs_vault);
    const TxSize est = CalculateMaximumSignedTxSize(*tx, vault.get(), &coin_control);
    BOOST_REQUIRE_GT(est.weight, 0);
    // The estimate must be an UPPER bound on the transaction it estimates. Signatures
    // are estimated at their maximum, so the only way this can fail is a term missing
    // from the estimate entirely.
    BOOST_CHECK_GE(est.weight, GetTransactionWeight(*tx));
    // And the tail is 176 bytes of it, non-witness, so 704 weight.
    BOOST_CHECK_GE(est.weight, 176 * WITNESS_SCALE_FACTOR);

    // The byte bound is what the Stage 2 size term is actually charged against, and
    // it must be an upper bound on the finished serialization for the same reason.
    const int64_t actual_bytes{static_cast<int64_t>(::GetSerializeSize(TX_WITH_WITNESS(*tx)))};
    BOOST_CHECK_GE(est.bytes, actual_bytes);
    // It must also be a REAL byte count, not the weight proxy. weight is 3*base +
    // total, so for this zero-witness transaction weight is exactly 4x bytes, and
    // grinding against weight would have charged four times the required work.
    BOOST_CHECK_LT(est.bytes, est.weight);
    BOOST_CHECK_EQUAL(est.weight, est.bytes * WITNESS_SCALE_FACTOR);
}

// The proof must hold against the FINISHED transaction, not against the estimate it
// was ground from. The size estimate is an upper bound by construction, but it is
// built from descriptor metadata that has been wrong before -- upstream's taproot
// estimator assumed a key-path spend and underestimated script-path spends by orders
// of magnitude. Where the vault signs locally it knows the true size, so it should
// check rather than trust: an estimate that ever slips low must cost a regrind, not
// produce a transaction the network rejects.
BOOST_FIXTURE_TEST_CASE(SignedTransactionCarriesEnoughWork, TestChain100Setup)
{
    CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    auto vault = CreateSyncedVault(*m_node.chain, WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain()), coinbaseKey);

    CRecipient recipient{PubKeyDestination({}), 1 * COIN};
    CCoinControl coin_control;
    coin_control.m_change_type = OutputType::BASE58;
    auto res = CreateTransaction(*vault, {recipient}, /*change_pos=*/std::nullopt, coin_control);
    BOOST_REQUIRE_MESSAGE(res, util::ErrorString(res).original);
    const CTransactionRef tx = res->tx;

    // Recompute the requirement from the signed transaction exactly as consensus
    // does, and confirm the proof it carries clears it.
    const Consensus::Params& cp = Params().GetConsensus();
    LOCK(cs_main);
    const CBlockIndex* anchor{m_node.chainman->ActiveChain()[int(tx->nAnchorHeight)]};
    BOOST_REQUIRE(anchor != nullptr);
    const uint256 target{GetTxPowTarget(cp, anchor, *tx)};
    BOOST_CHECK_MESSAGE(
        UintToArith256(cuckatoo::CuckatooProofHash(tx->nCycle)) <= UintToArith256(target),
        "signed transaction carries less work than its own bytes require");
}

//! The per-transaction proof-of-work must not be ground while cs_vault is held.
//!
//! At mainnet edge bits the grind runs for tens of seconds to minutes. Holding the
//! vault lock across it makes every other vault operation — the GUI's own timers,
//! the transaction table, and every RPC — block for that whole time, which was
//! observed on a GUI test node as a frozen desktop and a `getbalances` that took 235 s against
//! 0.00 s idle. Nothing about the grind needs the lock: it reads the chain, not the
//! vault, and writes only nAnchorHeight, nCycle and nPowNonce on a local transaction.
BOOST_FIXTURE_TEST_CASE(create_transaction_does_not_hold_vault_lock_while_grinding, TestChain100Setup)
{
    CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    auto vault = CreateSyncedVault(*m_node.chain, WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain()), coinbaseKey);

    std::atomic<bool> probed{false};
    std::atomic<bool> lock_was_available{false};

    // Runs on the grinding thread, once, partway through the proof-of-work.
    const auto progress = [&](uint32_t) {
        if (probed.exchange(true)) return;
        // cs_vault is recursive, so probing it from this thread would succeed even
        // while it is held. Ask from a thread that holds nothing.
        std::thread probe{[&] {
            TRY_LOCK(vault->cs_vault, locked);
            lock_was_available = bool(locked);
        }};
        probe.join();
    };

    CRecipient recipient{PubKeyDestination({}), 1 * COIN};
    CCoinControl coin_control;
    coin_control.m_change_type = OutputType::BASE58;
    auto res = CreateTransaction(*vault, {recipient}, /*change_pos=*/std::nullopt, coin_control,
                                 /*sign=*/true, progress, /*tx_proof_cancel=*/{});
    BOOST_REQUIRE_MESSAGE(res, util::ErrorString(res).original);

    // Guards the guard: without this, a grind that never reported progress would
    // leave lock_was_available false and look like the very bug being tested for.
    BOOST_REQUIRE_MESSAGE(probed.load(), "the grind never reported progress, so the lock was never probed");
    BOOST_CHECK_MESSAGE(lock_was_available.load(),
                        "cs_vault was held during the proof-of-work grind; every other vault "
                        "operation blocks for the grind's full duration");
}

// fTxPowNoCycle is an opt-in sandbox boundary for tests whose subject is transaction
// handling rather than the Cuckatoo solver. It already skips cycle verification; the
// creation path must skip the real solver too, while still satisfying the same
// transaction-specific proof-hash target that validation keeps enabled.
BOOST_FIXTURE_TEST_CASE(no_cycle_mode_skips_solver_but_meets_target, NoCycleTestChain100Setup)
{
    auto vault = CreateSyncedVault(*m_node.chain,
                                   WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain()),
                                   coinbaseKey);

    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(ArithToUint256(1)), 0});
    tx.vout.emplace_back(COIN, CScript{});
    const auto anchor_height = TxPowAnchorHeight(*vault);
    BOOST_REQUIRE(anchor_height);

    bool solver_reported_progress{false};
    constexpr uint64_t charged_bytes{300'000};
    constexpr size_t charged_outputs{1'501};
    const uint256 target = vault->chain().txPowTarget(*anchor_height, charged_bytes, charged_outputs, tx.vin.size());
    const auto error = GrindTransactionPow(*vault, tx, charged_bytes, *anchor_height,
                                           [&](uint32_t) { solver_reported_progress = true; });

    BOOST_CHECK(!error);
    BOOST_CHECK(!solver_reported_progress);
    BOOST_CHECK_EQUAL(tx.nAnchorHeight, static_cast<uint32_t>(*anchor_height));
    BOOST_CHECK_LE(UintToArith256(cuckatoo::CuckatooProofHash(tx.nCycle)), UintToArith256(target));
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace vault
