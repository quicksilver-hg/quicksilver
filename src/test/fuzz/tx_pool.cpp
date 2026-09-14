// Copyright (c) 2021-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/validation.h>
#include <node/context.h>
#include <node/relaypool_args.h>
#include <node/miner.h>
#include <policy/truc_policy.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <test/fuzz/util/relaypool.h>
#include <test/util/mining.h>
#include <test/util/script.h>
#include <test/util/setup_common.h>
#include <test/util/txrelaypool.h>
#include <util/check.h>
#include <util/translation.h>
#include <validation.h>
#include <validationinterface.h>

using node::BlockAssembler;
using node::NodeContext;
using util::ToString;

namespace {

const TestingSetup* g_setup;
std::vector<COutPoint> g_outpoints_coinbase_init_mature;
std::vector<COutPoint> g_outpoints_coinbase_init_immature;

void initialize_tx_pool()
{
    static const auto testing_setup = MakeNoLogFileContext<const TestingSetup>();
    g_setup = testing_setup.get();

    BlockAssembler::Options options;
    options.coinbase_output_script = P2WSH_OP_TRUE;

    for (int i = 0; i < 2 * COINBASE_MATURITY; ++i) {
        COutPoint prevout{MineBlock(g_setup->m_node, options)};
        // Remember the txids to avoid expensive disk access later on
        auto& outpoints = i < COINBASE_MATURITY ?
                              g_outpoints_coinbase_init_mature :
                              g_outpoints_coinbase_init_immature;
        outpoints.push_back(prevout);
    }
    g_setup->m_node.validation_signals->SyncWithValidationInterfaceQueue();
}

struct TransactionsDelta final : public CValidationInterface {
    std::set<CTransactionRef>& m_removed;
    std::set<CTransactionRef>& m_added;

    explicit TransactionsDelta(std::set<CTransactionRef>& r, std::set<CTransactionRef>& a)
        : m_removed{r}, m_added{a} {}

    void TransactionAddedToRelayPool(const CTransactionRef& tx, uint64_t /* relaypool_sequence */) override
    {
        Assert(m_added.insert(tx).second);
    }

    void TransactionRemovedFromRelayPool(const CTransactionRef& tx, RelayPoolRemovalReason reason, uint64_t /* relaypool_sequence */) override
    {
        Assert(m_removed.insert(tx).second);
    }
};

void SetRelayPoolConstraints(ArgsManager& args, FuzzedDataProvider& fuzzed_data_provider)
{
    args.ForceSetArg("-limitancestorcount",
                     ToString(fuzzed_data_provider.ConsumeIntegralInRange<unsigned>(0, 50)));
    args.ForceSetArg("-limitancestorsize",
                     ToString(fuzzed_data_provider.ConsumeIntegralInRange<unsigned>(0, 202)));
    args.ForceSetArg("-limitdescendantcount",
                     ToString(fuzzed_data_provider.ConsumeIntegralInRange<unsigned>(0, 50)));
    args.ForceSetArg("-limitdescendantsize",
                     ToString(fuzzed_data_provider.ConsumeIntegralInRange<unsigned>(0, 202)));
    args.ForceSetArg("-maxrelaypool",
                     ToString(fuzzed_data_provider.ConsumeIntegralInRange<unsigned>(0, 200)));
    args.ForceSetArg("-relaypoolexpiry",
                     ToString(fuzzed_data_provider.ConsumeIntegralInRange<unsigned>(0, 999)));
}

void Finish(FuzzedDataProvider& fuzzed_data_provider, CTxRelayPool& tx_pool, Chainstate& chainstate)
{
    WITH_LOCK(::cs_main, tx_pool.check(chainstate.CoinsTip(), chainstate.m_chain.Height() + 1));
    {
        BlockAssembler::Options options;
        options.nBlockMaxWeight = fuzzed_data_provider.ConsumeIntegralInRange(0U, MAX_BLOCK_WEIGHT);
        auto assembler = BlockAssembler{chainstate, &tx_pool, options};
        auto block_template = assembler.CreateNewBlock();
        Assert(block_template->block.vtx.size() >= 1);
    }
    const auto info_all = tx_pool.infoAll();
    if (!info_all.empty()) {
        const auto& tx_to_remove = *PickValue(fuzzed_data_provider, info_all).tx;
        WITH_LOCK(tx_pool.cs, tx_pool.removeRecursive(tx_to_remove, RelayPoolRemovalReason::BLOCK /* dummy */));
        assert(tx_pool.size() < info_all.size());
        WITH_LOCK(::cs_main, tx_pool.check(chainstate.CoinsTip(), chainstate.m_chain.Height() + 1));
    }
    g_setup->m_node.validation_signals->SyncWithValidationInterfaceQueue();
}

void MockTime(FuzzedDataProvider& fuzzed_data_provider, const Chainstate& chainstate)
{
    const auto time = ConsumeTime(fuzzed_data_provider,
                                  chainstate.m_chain.Tip()->GetMedianTimePast() + 1,
                                  std::numeric_limits<decltype(chainstate.m_chain.Tip()->nTime)>::max());
    SetMockTime(time);
}

std::unique_ptr<CTxRelayPool> MakeRelayPool(FuzzedDataProvider& fuzzed_data_provider, const NodeContext& node)
{
    // Take the default options for tests...
    CTxRelayPool::Options relaypool_opts{RelayPoolOptionsForTest(node)};

    // ...override specific options for this specific fuzz suite
    relaypool_opts.check_ratio = 1;
    relaypool_opts.require_standard = fuzzed_data_provider.ConsumeBool();

    // ...and construct a CTxRelayPool from it
    bilingual_str error;
    auto relaypool{std::make_unique<CTxRelayPool>(std::move(relaypool_opts), error)};
    // ... ignore the error since it might be beneficial to fuzz even when the
    // relaypool size is unreasonably small
    Assert(error.empty() || error.original.starts_with("-maxrelaypool must be at least "));
    return relaypool;
}

void CheckATMPInvariants(const RelayPoolAcceptResult& res, bool txid_in_relaypool, bool wtxid_in_relaypool)
{

    switch (res.m_result_type) {
    case RelayPoolAcceptResult::ResultType::VALID:
    {
        Assert(txid_in_relaypool);
        Assert(wtxid_in_relaypool);
        Assert(res.m_state.IsValid());
        Assert(!res.m_state.IsInvalid());
        Assert(res.m_vsize);
        Assert(res.m_package_wtxids);
        Assert(!res.m_other_wtxid);
        break;
    }
    case RelayPoolAcceptResult::ResultType::INVALID:
    {
        // It may be already in the relaypool since in ATMP cases we don't set RELAYPOOL_ENTRY or DIFFERENT_WITNESS
        Assert(!res.m_state.IsValid());
        Assert(res.m_state.IsInvalid());

        Assert(!res.m_vsize);
        Assert(!res.m_package_wtxids);
        Assert(!res.m_other_wtxid);
        break;
    }
    case RelayPoolAcceptResult::ResultType::RELAYPOOL_ENTRY:
    {
        Assert(res.m_vsize);
        break;
    }
    case RelayPoolAcceptResult::ResultType::DIFFERENT_WITNESS:
    {
        // ATMP never sets this; only set in package settings
        Assert(false);
        break;
    }
    }
}

FUZZ_TARGET(tx_pool_standard, .init = initialize_tx_pool)
{
    SeedRandomStateForTest(SeedRand::ZEROS);
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());
    const auto& node = g_setup->m_node;
    auto& chainstate{static_cast<DummyChainState&>(node.chainman->ActiveChainstate())};

    MockTime(fuzzed_data_provider, chainstate);

    // All replaceable outpoints
    std::set<COutPoint> outpoints_replaceable;
    // All outpoints counting toward the total supply (subset of outpoints_replaceable)
    std::set<COutPoint> outpoints_supply;
    for (const auto& outpoint : g_outpoints_coinbase_init_mature) {
        Assert(outpoints_supply.insert(outpoint).second);
    }
    outpoints_replaceable = outpoints_supply;

    // The sum of the values of all spendable outpoints
    constexpr CAmount SUPPLY_TOTAL{COINBASE_MATURITY * 50 * COIN};

    SetRelayPoolConstraints(*node.args, fuzzed_data_provider);
    auto tx_pool_{MakeRelayPool(fuzzed_data_provider, node)};
    CTxRelayPool& tx_pool = *tx_pool_;

    chainstate.SetRelayPool(&tx_pool);

    // Helper to query an amount
    const CCoinsViewRelayPool amount_view{WITH_LOCK(::cs_main, return &chainstate.CoinsTip()), tx_pool};
    const auto GetAmount = [&](const COutPoint& outpoint) {
        auto coin{amount_view.GetCoin(outpoint).value()};
        return coin.out.nValue;
    };

    LIMITED_WHILE(fuzzed_data_provider.ConsumeBool(), 300)
    {
        {
            CAmount supply_now{0};
            for (const auto& op : outpoints_supply) {
                supply_now += GetAmount(op);
            }
            Assert(supply_now == SUPPLY_TOTAL);
        }
        Assert(!outpoints_supply.empty());

        // Create transaction to add to the relaypool
        const CTransactionRef tx = [&] {
            CMutableTransaction tx_mut;
            tx_mut.version = fuzzed_data_provider.ConsumeBool() ? TRUC_VERSION : CTransaction::CURRENT_VERSION;
            tx_mut.nLockTime = fuzzed_data_provider.ConsumeBool() ? 0 : fuzzed_data_provider.ConsumeIntegral<uint32_t>();
            const auto num_in = fuzzed_data_provider.ConsumeIntegralInRange<int>(1, outpoints_replaceable.size());
            const auto num_out = fuzzed_data_provider.ConsumeIntegralInRange<int>(1, outpoints_replaceable.size() * 2);

            CAmount amount_in{0};
            for (int i = 0; i < num_in; ++i) {
                // Pop random outpoint
                auto pop = outpoints_replaceable.begin();
                std::advance(pop, fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, outpoints_replaceable.size() - 1));
                const auto outpoint = *pop;
                outpoints_replaceable.erase(pop);
                amount_in += GetAmount(outpoint);

                // Create input
                const auto sequence = ConsumeSequence(fuzzed_data_provider);
                const auto script_sig = CScript{};
                const auto script_wit_stack = std::vector<std::vector<uint8_t>>{WITNESS_STACK_ELEM_OP_TRUE};
                CTxIn in;
                in.prevout = outpoint;
                in.nSequence = sequence;
                in.scriptSig = script_sig;
                in.scriptWitness.stack = script_wit_stack;

                tx_mut.vin.push_back(in);
            }
            const auto leftover = fuzzed_data_provider.ConsumeIntegralInRange<CAmount>(-1000, amount_in);
            const auto amount_out = (amount_in - leftover) / num_out;
            for (int i = 0; i < num_out; ++i) {
                tx_mut.vout.emplace_back(amount_out, P2WSH_OP_TRUE);
            }
            auto tx = MakeTransactionRef(tx_mut);
            // Restore previously removed outpoints
            for (const auto& in : tx->vin) {
                Assert(outpoints_replaceable.insert(in.prevout).second);
            }
            return tx;
        }();

        if (fuzzed_data_provider.ConsumeBool()) {
            MockTime(fuzzed_data_provider, chainstate);
        }
        // Remember all removed and added transactions
        std::set<CTransactionRef> removed;
        std::set<CTransactionRef> added;
        auto txr = std::make_shared<TransactionsDelta>(removed, added);
        node.validation_signals->RegisterSharedValidationInterface(txr);
        const bool bypass_limits = fuzzed_data_provider.ConsumeBool();

        // Make sure ProcessNewPackage on one transaction works.
        // The result is not guaranteed to be the same as what is returned by ATMP.
        const auto result_package = WITH_LOCK(::cs_main,
                                    return ProcessNewPackage(chainstate, tx_pool, {tx}, true));
        // If something went wrong due to a package-specific policy, it might not return a
        // validation result for the transaction.
        if (result_package.m_state.GetResult() != PackageValidationResult::PCKG_POLICY) {
            auto it = result_package.m_tx_results.find(tx->GetWitnessHash());
            Assert(it != result_package.m_tx_results.end());
            Assert(it->second.m_result_type == RelayPoolAcceptResult::ResultType::VALID ||
                   it->second.m_result_type == RelayPoolAcceptResult::ResultType::INVALID);
        }

        const auto res = WITH_LOCK(::cs_main, return AcceptToMemoryPool(chainstate, tx, GetTime(), bypass_limits, /*test_accept=*/false));
        const bool accepted = res.m_result_type == RelayPoolAcceptResult::ResultType::VALID;
        node.validation_signals->SyncWithValidationInterfaceQueue();
        node.validation_signals->UnregisterSharedValidationInterface(txr);

        bool txid_in_relaypool = tx_pool.exists(GenTxid::Txid(tx->GetHash()));
        bool wtxid_in_relaypool = tx_pool.exists(GenTxid::Wtxid(tx->GetWitnessHash()));
        CheckATMPInvariants(res, txid_in_relaypool, wtxid_in_relaypool);

        Assert(accepted != added.empty());
        if (accepted) {
            Assert(added.size() == 1); // For now, no package acceptance
            Assert(tx == *added.begin());
            CheckRelayPoolTRUCInvariants(tx_pool);
        } else {
            // Do not consider rejected transaction removed
            removed.erase(tx);
        }

        // Helper to insert spent and created outpoints of a tx into collections
        using Sets = std::vector<std::reference_wrapper<std::set<COutPoint>>>;
        const auto insert_tx = [](Sets created_by_tx, Sets consumed_by_tx, const auto& tx) {
            for (size_t i{0}; i < tx.vout.size(); ++i) {
                for (auto& set : created_by_tx) {
                    Assert(set.get().emplace(tx.GetHash(), i).second);
                }
            }
            for (const auto& in : tx.vin) {
                for (auto& set : consumed_by_tx) {
                    Assert(set.get().insert(in.prevout).second);
                }
            }
        };
        // Add created outpoints, remove spent outpoints
        {
            // Outpoints that no longer exist at all
            std::set<COutPoint> consumed_erased;
            // Outpoints that no longer count toward the total supply
            std::set<COutPoint> consumed_supply;
            for (const auto& removed_tx : removed) {
                insert_tx(/*created_by_tx=*/{consumed_erased}, /*consumed_by_tx=*/{outpoints_supply}, /*tx=*/*removed_tx);
            }
            for (const auto& added_tx : added) {
                insert_tx(/*created_by_tx=*/{outpoints_supply, outpoints_replaceable}, /*consumed_by_tx=*/{consumed_supply}, /*tx=*/*added_tx);
            }
            for (const auto& p : consumed_erased) {
                Assert(outpoints_supply.erase(p) == 1);
                Assert(outpoints_replaceable.erase(p) == 1);
            }
            for (const auto& p : consumed_supply) {
                Assert(outpoints_supply.erase(p) == 1);
            }
        }
    }
    Finish(fuzzed_data_provider, tx_pool, chainstate);
}

FUZZ_TARGET(tx_pool, .init = initialize_tx_pool)
{
    SeedRandomStateForTest(SeedRand::ZEROS);
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());
    const auto& node = g_setup->m_node;
    auto& chainstate{static_cast<DummyChainState&>(node.chainman->ActiveChainstate())};

    MockTime(fuzzed_data_provider, chainstate);

    std::vector<Txid> txids;
    txids.reserve(g_outpoints_coinbase_init_mature.size());
    for (const auto& outpoint : g_outpoints_coinbase_init_mature) {
        txids.push_back(outpoint.hash);
    }
    for (int i{0}; i <= 3; ++i) {
        // Add some immature and non-existent outpoints
        txids.push_back(g_outpoints_coinbase_init_immature.at(i).hash);
        txids.push_back(Txid::FromUint256(ConsumeUInt256(fuzzed_data_provider)));
    }

    SetRelayPoolConstraints(*node.args, fuzzed_data_provider);
    auto tx_pool_{MakeRelayPool(fuzzed_data_provider, node)};
    CTxRelayPool& tx_pool = *tx_pool_;

    chainstate.SetRelayPool(&tx_pool);

    LIMITED_WHILE(fuzzed_data_provider.ConsumeBool(), 300)
    {
        const auto mut_tx = ConsumeTransaction(fuzzed_data_provider, txids);

        if (fuzzed_data_provider.ConsumeBool()) {
            MockTime(fuzzed_data_provider, chainstate);
        }
        const auto tx = MakeTransactionRef(mut_tx);
        const bool bypass_limits = fuzzed_data_provider.ConsumeBool();
        const auto res = WITH_LOCK(::cs_main, return AcceptToMemoryPool(chainstate, tx, GetTime(), bypass_limits, /*test_accept=*/false));
        const bool accepted = res.m_result_type == RelayPoolAcceptResult::ResultType::VALID;
        if (accepted) {
            txids.push_back(tx->GetHash());
            CheckRelayPoolTRUCInvariants(tx_pool);
        }
    }
    Finish(fuzzed_data_provider, tx_pool, chainstate);
}
} // namespace
