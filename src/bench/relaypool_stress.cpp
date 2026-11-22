// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <coins.h>
#include <consensus/amount.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <random.h>
#include <script/script.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <test/util/txrelaypool.h>
#include <txrelaypool.h>
#include <uint256.h>
#include <util/check.h>
#include <validation.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>


static void AddTx(const CTransactionRef& tx, CTxRelayPool& pool) EXCLUSIVE_LOCKS_REQUIRED(cs_main, pool.cs)
{
    int64_t nTime = 0;
    unsigned int nHeight = 1;
    uint64_t sequence = 0;
    bool spendsCoinbase = false;
    unsigned int sigOpCost = 4;
    LockPoints lp;
    AddToRelayPool(pool, CTxRelayPoolEntry(tx, nTime, nHeight, sequence, spendsCoinbase, sigOpCost, lp));
}

struct Available {
    CTransactionRef ref;
    size_t vout_next{0}; // the next unspent output of ref
    size_t tx_count;
    Available(CTransactionRef& ref, size_t tx_count) : ref(ref), tx_count(tx_count){}
};

/** Split total exactly across tx's outputs (feeless: value in equals value out). */
static void SetFeelessOutputs(CMutableTransaction& tx, size_t num_outputs, CAmount total, size_t tx_counter)
{
    tx.vout.resize(num_outputs);
    const CAmount per_output{total / CAmount(num_outputs)};
    for (size_t n = 0; n < num_outputs; ++n) {
        tx.vout[n].scriptPubKey = CScript() << CScriptNum(tx_counter) << OP_EQUAL;
        tx.vout[n].nValue = per_output + (n == 0 ? total - per_output * CAmount(num_outputs) : 0);
    }
}

/**
 * Build a relay-pool topology that normal admission could produce: every outpoint is
 * spent at most once and every transaction is feeless. The base transactions spend
 * distinct outpoints outside the pool; base_coins receives each of them with its value,
 * so a coins view can be built for CTxRelayPool::check.
 */
static std::vector<CTransactionRef> CreateOrderedCoins(FastRandomContext& det_rand, int childTxs, int min_ancestors,
                                                       std::vector<std::pair<COutPoint, CAmount>>& base_coins)
{
    std::vector<Available> available_coins;
    std::vector<CTransactionRef> ordered_coins;
    // Create some base transactions
    size_t tx_counter = 1;
    for (auto x = 0; x < 100; ++x) {
        CMutableTransaction tx = CMutableTransaction();
        tx.vin.resize(1);
        tx.vin[0].prevout = COutPoint(Txid::FromUint256(uint256{static_cast<uint8_t>(x + 1)}), 0);
        tx.vin[0].scriptSig = CScript() << CScriptNum(tx_counter);
        tx.vin[0].scriptWitness.stack.push_back(CScriptNum(x).getvch());
        const size_t num_outputs = det_rand.randrange(10) + 2;
        const CAmount total{CAmount(num_outputs) * 10 * COIN};
        SetFeelessOutputs(tx, num_outputs, total, tx_counter);
        base_coins.emplace_back(tx.vin[0].prevout, total);
        ordered_coins.emplace_back(MakeTransactionRef(tx));
        available_coins.emplace_back(ordered_coins.back(), tx_counter++);
    }
    for (auto x = 0; x < childTxs && !available_coins.empty(); ++x) {
        CMutableTransaction tx = CMutableTransaction();
        CAmount total_in{0};
        size_t n_ancestors = det_rand.randrange(10)+1;
        for (size_t ancestor = 0; ancestor < n_ancestors && !available_coins.empty(); ++ancestor){
            size_t idx = det_rand.randrange(available_coins.size());
            Available& coin = available_coins[idx];
            const size_t left{coin.ref->vout.size() - coin.vout_next};
            Txid hash = coin.ref->GetHash();
            // biased towards taking min_ancestors parents, but maybe more
            size_t n_to_take = det_rand.randrange(2) == 0 ?
                               min_ancestors :
                               min_ancestors + det_rand.randrange(left);
            n_to_take = std::min(n_to_take, left);
            for (size_t i = 0; i < n_to_take; ++i) {
                tx.vin.emplace_back();
                tx.vin.back().prevout = COutPoint(hash, coin.vout_next);
                total_in += coin.ref->vout[coin.vout_next].nValue;
                ++coin.vout_next;
                tx.vin.back().scriptSig = CScript() << coin.tx_count;
                tx.vin.back().scriptWitness.stack.push_back(CScriptNum(coin.tx_count).getvch());
            }
            if (coin.vout_next == coin.ref->vout.size()) {
                // Every output is spent: drop this record (not the vector's last one).
                available_coins[idx] = std::move(available_coins.back());
                available_coins.pop_back();
            }
        }
        SetFeelessOutputs(tx, det_rand.randrange(10) + 2, total_in, tx_counter);
        ordered_coins.emplace_back(MakeTransactionRef(tx));
        available_coins.emplace_back(ordered_coins.back(), tx_counter++);
    }
    return ordered_coins;
}

static void ComplexRelayPool(benchmark::Bench& bench)
{
    FastRandomContext det_rand{true};
    int childTxs = 800;
    if (bench.complexityN() > 1) {
        childTxs = static_cast<int>(bench.complexityN());
    }
    std::vector<std::pair<COutPoint, CAmount>> base_coins;
    std::vector<CTransactionRef> ordered_coins = CreateOrderedCoins(det_rand, childTxs, /*min_ancestors=*/1, base_coins);
    const auto testing_setup = MakeNoLogFileContext<const TestingSetup>(ChainType::MAIN, {.extra_args = {"-checkrelaypool=1"}});
    CTxRelayPool& pool = *testing_setup.get()->m_node.relaypool;
    LOCK2(cs_main, pool.cs);

    // The fixture must be a pool that admission could have built: stage it once, run the
    // full consistency check (spend index, ancestry, feeless inputs) against a view that
    // holds the base transactions' inputs, then empty the pool for the timed runs.
    {
        CCoinsView base_view;
        CCoinsViewCache coins{&base_view};
        for (const auto& [outpoint, value] : base_coins) {
            coins.AddCoin(outpoint, Coin{CTxOut{value, CScript() << OP_TRUE}, /*nHeightIn=*/1, /*fCoinBaseIn=*/false}, /*possible_overwrite=*/false);
        }
        for (auto& tx : ordered_coins) AddTx(tx, pool);
        Assert(pool.size() == ordered_coins.size());
        pool.check(coins, /*spendheight=*/2);
        pool.TrimToSize(0);
        Assert(pool.size() == 0);
    }
    // bench.run calls this lambda before returning, while the LOCK2 above
    // still holds cs_main and pool.cs. AddTx requires both; TrimToSize
    // requires pool.cs. Clang treats the lambda as its own function and
    // does not carry the caller's locks in.
    bench.run([&]() NO_THREAD_SAFETY_ANALYSIS {
        for (auto& tx : ordered_coins) {
            AddTx(tx, pool);
        }
        pool.TrimToSize(pool.DynamicMemoryUsage() * 3 / 4);
        pool.TrimToSize(GetVirtualTransactionSize(*ordered_coins.front()));
    });
}

static void RelayPoolCheck(benchmark::Bench& bench)
{
    FastRandomContext det_rand{true};
    auto testing_setup = MakeNoLogFileContext<TestChain100Setup>(ChainType::SANDBOX, {.extra_args = {"-checkrelaypool=1"}});
    CTxRelayPool& pool = *testing_setup.get()->m_node.relaypool;
    LOCK2(cs_main, pool.cs);
    testing_setup->PopulateRelayPool(det_rand, 400, true);
    const CCoinsViewCache& coins_tip = testing_setup.get()->m_node.chainman->ActiveChainstate().CoinsTip();

    // Same boundary as ComplexRelayPool: the LOCK2 still holds cs_main,
    // which pool.check requires, and bench.run invokes this before returning.
    bench.run([&]() NO_THREAD_SAFETY_ANALYSIS {
        // Bump up the spendheight so we don't hit premature coinbase spend errors.
        pool.check(coins_tip, /*spendheight=*/300);
    });
}

BENCHMARK(ComplexRelayPool, benchmark::PriorityLevel::HIGH);
BENCHMARK(RelayPoolCheck, benchmark::PriorityLevel::HIGH);
