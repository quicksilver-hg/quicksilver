// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <coins.h>
#include <consensus/amount.h>
#include <kernel/cs_main.h>
#include <primitives/transaction.h>
#include <rpc/relaypool.h>
#include <script/script.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <test/util/txrelaypool.h>
#include <txrelaypool.h>
#include <uint256.h>
#include <univalue.h>
#include <util/check.h>

#include <cstdint>
#include <memory>
#include <vector>


static void AddTx(const CTransactionRef& tx, CTxRelayPool& pool) EXCLUSIVE_LOCKS_REQUIRED(cs_main, pool.cs)
{
    LockPoints lp;
    AddToRelayPool(pool, CTxRelayPoolEntry(tx, /*time=*/0, /*entry_height=*/1, /*entry_sequence=*/0, /*spends_coinbase=*/false, /*sigops_cost=*/4, lp));
}

static void RpcRelayPool(benchmark::Bench& bench)
{
    const auto testing_setup = MakeNoLogFileContext<const ChainTestingSetup>(ChainType::MAIN, {.extra_args = {"-checkrelaypool=1"}});
    CTxRelayPool& pool = *Assert(testing_setup->m_node.relaypool);
    LOCK2(cs_main, pool.cs);

    // Each transaction spends its own outpoint, held in a view so the pool can be checked.
    CCoinsView base_view;
    CCoinsViewCache coins{&base_view};
    for (int i = 0; i < 1000; ++i) {
        CMutableTransaction tx = CMutableTransaction();
        tx.vin.resize(1);
        tx.vin[0].prevout = COutPoint(Txid::FromUint256(uint256::ONE), static_cast<uint32_t>(i));
        coins.AddCoin(tx.vin[0].prevout, Coin{CTxOut{i, CScript() << OP_TRUE}, /*nHeightIn=*/1, /*fCoinBaseIn=*/false}, /*possible_overwrite=*/false);
        tx.vin[0].scriptSig = CScript() << OP_1;
        tx.vin[0].scriptWitness.stack.push_back({1});
        tx.vout.resize(1);
        tx.vout[0].scriptPubKey = CScript() << OP_1 << OP_EQUAL;
        tx.vout[0].nValue = i;
        const CTransactionRef tx_r{MakeTransactionRef(tx)};
        AddTx(tx_r, pool);
    }
    // All 1000 entries must be in the pool and consistent with its spend index.
    Assert(pool.size() == 1000);
    pool.check(coins, /*spendheight=*/2);

    bench.run([&] {
        (void)RelayPoolToJSON(pool, /*verbose=*/true);
    });
}

BENCHMARK(RpcRelayPool, benchmark::PriorityLevel::HIGH);
