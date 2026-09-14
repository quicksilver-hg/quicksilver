// Copyright (c) 2023 The Bitcoin Core developers
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

namespace {

const TestingSetup* g_setup;
std::vector<COutPoint> g_outpoints_coinbase_init_mature;

void initialize_tx_pool()
{
    static const auto testing_setup = MakeNoLogFileContext<const TestingSetup>();
    g_setup = testing_setup.get();

    BlockAssembler::Options options;
    options.coinbase_output_script = P2WSH_EMPTY;

    for (int i = 0; i < 2 * COINBASE_MATURITY; ++i) {
        COutPoint prevout{MineBlock(g_setup->m_node, options)};
        if (i < COINBASE_MATURITY) {
            // Remember the txids to avoid expensive disk access later on
            g_outpoints_coinbase_init_mature.push_back(prevout);
        }
    }
    g_setup->m_node.validation_signals->SyncWithValidationInterfaceQueue();
}

struct OutpointsUpdater final : public CValidationInterface {
    std::set<COutPoint>& m_relaypool_outpoints;

    explicit OutpointsUpdater(std::set<COutPoint>& r)
        : m_relaypool_outpoints{r} {}

    void TransactionAddedToRelayPool(const CTransactionRef& tx, uint64_t /* relaypool_sequence */) override
    {
        // for coins spent we always want to be able to replace so they're not removed

        // outputs from this tx can now be spent
        for (uint32_t index{0}; index < tx->vout.size(); ++index) {
            m_relaypool_outpoints.insert(COutPoint{tx->GetHash(), index});
        }
    }

    void TransactionRemovedFromRelayPool(const CTransactionRef& tx, RelayPoolRemovalReason reason, uint64_t /* relaypool_sequence */) override
    {
        // outpoints spent by this tx are now available
        for (const auto& input : tx->vin) {
            // Could already exist if this was a replacement
            m_relaypool_outpoints.insert(input.prevout);
        }
        // outpoints created by this tx no longer exist
        for (uint32_t index{0}; index < tx->vout.size(); ++index) {
            m_relaypool_outpoints.erase(COutPoint{tx->GetHash(), index});
        }
    }
};

struct TransactionsDelta final : public CValidationInterface {
    std::set<CTransactionRef>& m_added;

    explicit TransactionsDelta(std::set<CTransactionRef>& a)
        : m_added{a} {}

    void TransactionAddedToRelayPool(const CTransactionRef& tx, uint64_t /* relaypool_sequence */) override
    {
        // Transactions may be entered and booted any number of times
        m_added.insert(tx);
    }

    void TransactionRemovedFromRelayPool(const CTransactionRef& tx, RelayPoolRemovalReason reason, uint64_t /* relaypool_sequence */) override
    {
        // Transactions may be entered and booted any number of times
         m_added.erase(tx);
    }
};

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
    relaypool_opts.limits.ancestor_count = fuzzed_data_provider.ConsumeIntegralInRange<unsigned>(0, 50);
    relaypool_opts.limits.ancestor_size_vbytes = fuzzed_data_provider.ConsumeIntegralInRange<unsigned>(0, 202) * 1'000;
    relaypool_opts.limits.descendant_count = fuzzed_data_provider.ConsumeIntegralInRange<unsigned>(0, 50);
    relaypool_opts.limits.descendant_size_vbytes = fuzzed_data_provider.ConsumeIntegralInRange<unsigned>(0, 202) * 1'000;
    relaypool_opts.max_size_bytes = fuzzed_data_provider.ConsumeIntegralInRange<unsigned>(0, 200) * 1'000'000;
    relaypool_opts.expiry = std::chrono::hours{fuzzed_data_provider.ConsumeIntegralInRange<unsigned>(0, 999)};
    // Only interested in 2 cases: sigop cost 0 or when single legacy sigop cost is >> 1KvB
    nBytesPerSigOp = fuzzed_data_provider.ConsumeIntegralInRange<unsigned>(0, 1) * 10'000;

    relaypool_opts.check_ratio = 1;
    relaypool_opts.require_standard = fuzzed_data_provider.ConsumeBool();

    bilingual_str error;
    // ...and construct a CTxRelayPool from it
    auto relaypool{std::make_unique<CTxRelayPool>(std::move(relaypool_opts), error)};
    // ... ignore the error since it might be beneficial to fuzz even when the
    // relaypool size is unreasonably small
    Assert(error.empty() || error.original.starts_with("-maxrelaypool must be at least "));
    return relaypool;
}

FUZZ_TARGET(tx_package_eval, .init = initialize_tx_pool)
{
    SeedRandomStateForTest(SeedRand::ZEROS);
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());
    const auto& node = g_setup->m_node;
    auto& chainstate{static_cast<DummyChainState&>(node.chainman->ActiveChainstate())};

    MockTime(fuzzed_data_provider, chainstate);

    // All replaceable outpoints outside of the unsubmitted package
    std::set<COutPoint> relaypool_outpoints;
    std::map<COutPoint, CAmount> outpoints_value;
    for (const auto& outpoint : g_outpoints_coinbase_init_mature) {
        Assert(relaypool_outpoints.insert(outpoint).second);
        outpoints_value[outpoint] = 50 * COIN;
    }

    auto outpoints_updater = std::make_shared<OutpointsUpdater>(relaypool_outpoints);
    node.validation_signals->RegisterSharedValidationInterface(outpoints_updater);

    auto tx_pool_{MakeRelayPool(fuzzed_data_provider, node)};
    CTxRelayPool& tx_pool = *tx_pool_;

    chainstate.SetRelayPool(&tx_pool);

    LIMITED_WHILE(fuzzed_data_provider.remaining_bytes() > 0, 300)
    {
        Assert(!relaypool_outpoints.empty());

        std::vector<CTransactionRef> txs;

        // Make packages of 1-to-26 transactions
        const auto num_txs = (size_t) fuzzed_data_provider.ConsumeIntegralInRange<int>(1, 26);
        std::set<COutPoint> package_outpoints;
        while (txs.size() < num_txs) {
            // Create transaction to add to the relaypool
            txs.emplace_back([&] {
                CMutableTransaction tx_mut;
                tx_mut.version = fuzzed_data_provider.ConsumeBool() ? TRUC_VERSION : CTransaction::CURRENT_VERSION;
                tx_mut.nLockTime = fuzzed_data_provider.ConsumeBool() ? 0 : fuzzed_data_provider.ConsumeIntegral<uint32_t>();
                // Last transaction in a package needs to be a child of parents to get further in validation
                // so the last transaction to be generated(in a >1 package) must spend all package-made outputs
                // Note that this test currently only spends package outputs in last transaction.
                bool last_tx = num_txs > 1 && txs.size() == num_txs - 1;
                const auto num_in = last_tx ? package_outpoints.size()  : fuzzed_data_provider.ConsumeIntegralInRange<int>(1, relaypool_outpoints.size());
                auto num_out = fuzzed_data_provider.ConsumeIntegralInRange<int>(1, relaypool_outpoints.size() * 2);

                auto& outpoints = last_tx ? package_outpoints : relaypool_outpoints;

                Assert(!outpoints.empty());

                CAmount amount_in{0};
                for (size_t i = 0; i < num_in; ++i) {
                    // Pop random outpoint. We erase them to avoid double-spending
                    // while in this loop, but later add them back (unless last_tx).
                    auto pop = outpoints.begin();
                    std::advance(pop, fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, outpoints.size() - 1));
                    const auto outpoint = *pop;
                    outpoints.erase(pop);
                    // no need to update or erase from outpoints_value
                    amount_in += outpoints_value.at(outpoint);

                    // Create input
                    const auto sequence = ConsumeSequence(fuzzed_data_provider);
                    const auto script_sig = CScript{};
                    const auto script_wit_stack = fuzzed_data_provider.ConsumeBool() ? P2WSH_EMPTY_TRUE_STACK : P2WSH_EMPTY_TWO_STACK;

                    CTxIn in;
                    in.prevout = outpoint;
                    in.nSequence = sequence;
                    in.scriptSig = script_sig;
                    in.scriptWitness.stack = script_wit_stack;

                    tx_mut.vin.push_back(in);
                }

                // Duplicate an input
                bool dup_input = fuzzed_data_provider.ConsumeBool();
                if (dup_input) {
                    tx_mut.vin.push_back(tx_mut.vin.back());
                }

                // Refer to a non-existent input
                if (fuzzed_data_provider.ConsumeBool()) {
                    tx_mut.vin.emplace_back();
                }

                // Make a p2pk output to make sigops adjusted vsize to violate TRUC rules, potentially, which is never spent
                if (last_tx && amount_in > 1000 && fuzzed_data_provider.ConsumeBool()) {
                    tx_mut.vout.emplace_back(1000, CScript() << std::vector<unsigned char>(33, 0x02) << OP_CHECKSIG);
                    // Don't add any other outputs.
                    num_out = 1;
                    amount_in -= 1000;
                }

                const auto leftover = fuzzed_data_provider.ConsumeIntegralInRange<CAmount>(0, amount_in);
                const auto amount_out = (amount_in - leftover) / num_out;
                for (int i = 0; i < num_out; ++i) {
                    tx_mut.vout.emplace_back(amount_out, P2WSH_EMPTY);
                }
                auto tx = MakeTransactionRef(tx_mut);
                // Restore previously removed outpoints, except in-package outpoints
                if (!last_tx) {
                    for (const auto& in : tx->vin) {
                        // It's a fake input, or a new input, or a duplicate
                        Assert(in == CTxIn() || outpoints.insert(in.prevout).second || dup_input);
                    }
                    // Cache the in-package outpoints being made
                    for (size_t i = 0; i < tx->vout.size(); ++i) {
                        package_outpoints.emplace(tx->GetHash(), i);
                    }
                }
                // We need newly-created values for the duration of this run
                for (size_t i = 0; i < tx->vout.size(); ++i) {
                    outpoints_value[COutPoint(tx->GetHash(), i)] = tx->vout[i].nValue;
                }
                return tx;
            }());
        }

        if (fuzzed_data_provider.ConsumeBool()) {
            MockTime(fuzzed_data_provider, chainstate);
        }
        // Remember all added transactions
        std::set<CTransactionRef> added;
        auto txr = std::make_shared<TransactionsDelta>(added);
        node.validation_signals->RegisterSharedValidationInterface(txr);

        // When there are multiple transactions in the package, we call ProcessNewPackage(txs, test_accept=false)
        // and AcceptToMemoryPool(txs.back(), test_accept=true). When there is only 1 transaction, we might flip it
        // (the package is a test accept and ATMP is a submission).
        auto single_submit = txs.size() == 1 && fuzzed_data_provider.ConsumeBool();

        const auto result_package = WITH_LOCK(::cs_main,
                                    return ProcessNewPackage(chainstate, tx_pool, txs, /*test_accept=*/single_submit));

        // Always set bypass_limits to false because it is not supported in ProcessNewPackage and
        // can be a source of divergence.
        const auto res = WITH_LOCK(::cs_main, return AcceptToMemoryPool(chainstate, txs.back(), GetTime(),
                                   /*bypass_limits=*/false, /*test_accept=*/!single_submit));
        const bool passed = res.m_result_type == RelayPoolAcceptResult::ResultType::VALID;

        node.validation_signals->SyncWithValidationInterfaceQueue();
        node.validation_signals->UnregisterSharedValidationInterface(txr);

        // There is only 1 transaction in the package. We did a test-package-accept and a ATMP
        if (single_submit) {
            Assert(passed != added.empty());
            Assert(passed == res.m_state.IsValid());
            if (passed) {
                Assert(added.size() == 1);
                Assert(txs.back() == *added.begin());
            }
        } else if (result_package.m_state.GetResult() != PackageValidationResult::PCKG_POLICY) {
            // We don't know anything about the validity since transactions were randomly generated, so
            // just use result_package.m_state here. This makes the expect_valid check meaningless, but
            // we can still verify that the contents of m_tx_results are consistent with m_state.
            const bool expect_valid{result_package.m_state.IsValid()};
            Assert(!CheckPackageRelayPoolAcceptResult(txs, result_package, expect_valid, &tx_pool));
        } else {
            // This is empty if it fails early checks, or "full" if transactions are looked at deeper
            Assert(result_package.m_tx_results.size() == txs.size() || result_package.m_tx_results.empty());
        }

        CheckRelayPoolTRUCInvariants(tx_pool);
    }

    node.validation_signals->UnregisterSharedValidationInterface(outpoints_updater);

    WITH_LOCK(::cs_main, tx_pool.check(chainstate.CoinsTip(), chainstate.m_chain.Height() + 1));
}
} // namespace
