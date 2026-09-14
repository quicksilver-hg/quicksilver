// Copyright (c) 2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/txrelaypool.h>

#include <chainparams.h>
#include <node/context.h>
#include <node/relaypool_args.h>
#include <policy/replacement.h>
#include <policy/truc_policy.h>
#include <txrelaypool.h>
#include <util/check.h>
#include <util/time.h>
#include <util/translation.h>
#include <validation.h>

#include <type_traits>
#include <utility>

namespace {

#define REMOVED_RESULT_PAYLOAD(name) m_base_##name

template <typename T, typename = void>
struct HasRemovedResultPayload : std::false_type {};

template <typename T>
struct HasRemovedResultPayload<T, std::void_t<decltype(std::declval<const T&>().REMOVED_RESULT_PAYLOAD(fees))>> : std::true_type {};

static_assert(!HasRemovedResultPayload<RelayPoolAcceptResult>::value,
              "RelayPoolAcceptResult must not expose removed base fee payload");

#undef REMOVED_RESULT_PAYLOAD

} // namespace

using node::NodeContext;

CTxRelayPool::Options RelayPoolOptionsForTest(const NodeContext& node)
{
    CTxRelayPool::Options relaypool_opts{
        // Default to always checking relaypool regardless of
        // chainparams.DefaultConsistencyChecks for tests
        .check_ratio = 1,
        .signals = node.validation_signals.get(),
    };
    const auto result{ApplyArgsManOptions(*node.args, ::Params(), relaypool_opts)};
    Assert(result);
    return relaypool_opts;
}

CTxRelayPoolEntry TestRelayPoolEntryHelper::FromTx(const CMutableTransaction& tx) const
{
    return FromTx(MakeTransactionRef(tx));
}

CTxRelayPoolEntry TestRelayPoolEntryHelper::FromTx(const CTransactionRef& tx) const
{
    return CTxRelayPoolEntry{tx, TicksSinceEpoch<std::chrono::seconds>(time), nHeight, m_sequence, spendsCoinbase, sigOpCost, lp, txwork_surplus};
}

std::optional<std::string> CheckPackageRelayPoolAcceptResult(const Package& txns,
                                                           const PackageRelayPoolAcceptResult& result,
                                                           bool expect_valid,
                                                           const CTxRelayPool* relaypool)
{
    if (expect_valid) {
        if (result.m_state.IsInvalid()) {
            return strprintf("Package validation unexpectedly failed: %s", result.m_state.ToString());
        }
    } else {
        if (result.m_state.IsValid()) {
            return strprintf("Package validation unexpectedly succeeded. %s", result.m_state.ToString());
        }
    }
    if (result.m_state.GetResult() != PackageValidationResult::PCKG_POLICY && txns.size() != result.m_tx_results.size()) {
        return strprintf("txns size %u does not match tx results size %u", txns.size(), result.m_tx_results.size());
    }
    for (const auto& tx : txns) {
        const auto& wtxid = tx->GetWitnessHash();
        if (result.m_tx_results.count(wtxid) == 0) {
            return strprintf("result not found for tx %s", wtxid.ToString());
        }

        const auto& atmp_result = result.m_tx_results.at(wtxid);
        const bool valid{atmp_result.m_result_type == RelayPoolAcceptResult::ResultType::VALID};
        if (expect_valid && atmp_result.m_state.IsInvalid()) {
            return strprintf("tx %s unexpectedly failed: %s", wtxid.ToString(), atmp_result.m_state.ToString());
        }

        // Each subpackage is allowed MAX_REPLACEMENT_CANDIDATES replacements (only checking individually here)
        if (atmp_result.m_replaced_transactions.size() > MAX_REPLACEMENT_CANDIDATES) {
            return strprintf("tx %s result replaced too many transactions",
                                wtxid.ToString());
        }

        // Replacements can't happen for subpackages larger than 2
        if (!atmp_result.m_replaced_transactions.empty() &&
            atmp_result.m_package_wtxids.has_value() && atmp_result.m_package_wtxids.value().size() > 2) {
             return strprintf("tx %s was part of a too-large package replacement subpackage",
                                wtxid.ToString());
        }

        if (!atmp_result.m_replaced_transactions.empty() && relaypool) {
            LOCK(relaypool->cs);
            // If replacements occurred and it used 2 transactions, this is a package replacement and should result in a cluster of size 2
            if (atmp_result.m_package_wtxids.has_value() && atmp_result.m_package_wtxids.value().size() == 2) {
                const auto cluster = relaypool->GatherClusters({tx->GetHash()});
                if (cluster.size() != 2) return strprintf("tx %s has too many ancestors or descendants for a package replacement", wtxid.ToString());
            }
        }

        // m_vsize should exist iff the result was VALID or RELAYPOOL_ENTRY.
        const bool relaypool_entry{atmp_result.m_result_type == RelayPoolAcceptResult::ResultType::RELAYPOOL_ENTRY};
        if (atmp_result.m_vsize.has_value() != (valid || relaypool_entry)) {
            return strprintf("tx %s result should %shave m_vsize", wtxid.ToString(), valid || relaypool_entry ? "" : "not ");
        }

        // m_other_wtxid should exist iff the result was DIFFERENT_WITNESS
        const bool diff_witness{atmp_result.m_result_type == RelayPoolAcceptResult::ResultType::DIFFERENT_WITNESS};
        if (atmp_result.m_other_wtxid.has_value() != diff_witness) {
            return strprintf("tx %s result should %shave m_other_wtxid", wtxid.ToString(), diff_witness ? "" : "not ");
        }

        // m_package_wtxids should exist iff the result was valid.
        if (atmp_result.m_package_wtxids.has_value() != valid) {
            return strprintf("tx %s result should %shave m_package_wtxids",
                                    wtxid.ToString(), valid ? "" : "not ");
        }

        if (relaypool) {
            // The tx by txid should be in the relaypool iff the result was not INVALID.
            const bool txid_in_relaypool{atmp_result.m_result_type != RelayPoolAcceptResult::ResultType::INVALID};
            if (relaypool->exists(GenTxid::Txid(tx->GetHash())) != txid_in_relaypool) {
                return strprintf("tx %s should %sbe in relay pool", wtxid.ToString(), txid_in_relaypool ? "" : "not ");
            }
            // Additionally, if the result was DIFFERENT_WITNESS, we shouldn't be able to find the tx in relaypool by wtxid.
            if (tx->HasWitness() && atmp_result.m_result_type == RelayPoolAcceptResult::ResultType::DIFFERENT_WITNESS) {
                if (relaypool->exists(GenTxid::Wtxid(wtxid))) {
                    return strprintf("wtxid %s should not be in relay pool", wtxid.ToString());
                }
            }
            for (const auto& tx_ref : atmp_result.m_replaced_transactions) {
                if (relaypool->exists(GenTxid::Txid(tx_ref->GetHash()))) {
                    return strprintf("tx %s should not be in relay pool as it was replaced", tx_ref->GetWitnessHash().ToString());
                }
            }
        }
    }
    return std::nullopt;
}

void CheckRelayPoolTRUCInvariants(const CTxRelayPool& tx_pool)
{
    LOCK(tx_pool.cs);
    for (const auto& tx_info : tx_pool.infoAll()) {
        const auto& entry = *Assert(tx_pool.GetEntry(tx_info.tx->GetHash()));
        if (tx_info.tx->version == TRUC_VERSION) {
            // Check that special maximum virtual size is respected
            Assert(entry.GetTxSize() <= TRUC_MAX_VSIZE);

            // Check that special TRUC ancestor/descendant limits and rules are always respected
            Assert(entry.GetCountWithDescendants() <= TRUC_DESCENDANT_LIMIT);
            Assert(entry.GetCountWithAncestors() <= TRUC_ANCESTOR_LIMIT);
            Assert(entry.GetSizeWithDescendants() <= TRUC_MAX_VSIZE + TRUC_CHILD_MAX_VSIZE);
            Assert(entry.GetSizeWithAncestors() <= TRUC_MAX_VSIZE + TRUC_CHILD_MAX_VSIZE);

            // If this transaction has at least 1 ancestor, it's a "child" and has restricted weight.
            if (entry.GetCountWithAncestors() > 1) {
                Assert(entry.GetTxSize() <= TRUC_CHILD_MAX_VSIZE);
                // All TRUC transactions must only have TRUC unconfirmed parents.
                const auto& parents = entry.GetRelayPoolParentsConst();
                Assert(parents.begin()->get().GetSharedTx()->version == TRUC_VERSION);
            }
        } else if (entry.GetCountWithAncestors() > 1) {
            // All non-TRUC transactions must only have non-TRUC unconfirmed parents.
            for (const auto& parent : entry.GetRelayPoolParentsConst()) {
                Assert(parent.get().GetSharedTx()->version != TRUC_VERSION);
            }
        }
    }
}

void AddToRelayPool(CTxRelayPool& tx_pool, const CTxRelayPoolEntry& entry)
{
    LOCK2(cs_main, tx_pool.cs);
    auto changeset = tx_pool.GetChangeSet();
    changeset->StageAddition(entry.GetSharedTx(), entry.GetTime().count(), entry.GetHeight(), entry.GetSequence(),
            entry.GetSpendsCoinbase(), entry.GetSigOpCost(), entry.GetLockPoints(),
            entry.GetTxWorkSurplusValue());
    changeset->Apply();
}
