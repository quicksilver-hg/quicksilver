// Copyright (c) 2020-present The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <node/relaypool_args.h>
#include <policy/replacement.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <sync.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <test/fuzz/util/relaypool.h>
#include <test/util/setup_common.h>
#include <test/util/txrelaypool.h>
#include <tinyformat.h>
#include <txrelaypool.h>
#include <util/check.h>
#include <util/translation.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {
const BasicTestingSetup* g_setup;
} // namespace

const int NUM_ITERS = 10000;

void initialize_replacement()
{
    static const auto testing_setup = MakeNoLogFileContext<>();
    g_setup = testing_setup.get();
}

// This target exercises relaypool entry storage; it does not call replacement policy.
FUZZ_TARGET(relaypool_entry_storage, .init = initialize_replacement)
{
    SeedRandomStateForTest(SeedRand::ZEROS);
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());
    SetMockTime(ConsumeTime(fuzzed_data_provider));
    std::optional<CMutableTransaction> mtx = ConsumeDeserializable<CMutableTransaction>(fuzzed_data_provider, TX_WITH_WITNESS);
    if (!mtx) {
        return;
    }

    bilingual_str error;
    CTxRelayPool pool{RelayPoolOptionsForTest(g_setup->m_node), error};
    Assert(error.empty());

    LIMITED_WHILE(fuzzed_data_provider.ConsumeBool(), NUM_ITERS)
    {
        const std::optional<CMutableTransaction> another_mtx = ConsumeDeserializable<CMutableTransaction>(fuzzed_data_provider, TX_WITH_WITNESS);
        if (!another_mtx) {
            break;
        }
        const CTransaction another_tx{*another_mtx};
        if (fuzzed_data_provider.ConsumeBool() && !mtx->vin.empty()) {
            mtx->vin[0].prevout = COutPoint{another_tx.GetHash(), 0};
        }
        LOCK2(cs_main, pool.cs);
        if (!pool.GetIter(another_tx.GetHash())) {
            AddToRelayPool(pool, ConsumeTxRelayPoolEntry(fuzzed_data_provider, another_tx));
        }
    }
    const CTransaction tx{*mtx};
    if (fuzzed_data_provider.ConsumeBool()) {
        LOCK2(cs_main, pool.cs);
        if (!pool.GetIter(tx.GetHash())) {
            AddToRelayPool(pool, ConsumeTxRelayPoolEntry(fuzzed_data_provider, tx));
        }
    }
    {
        LOCK(pool.cs);
    }
}


namespace {
//! A surplus below 2^200, so surplus * vsize (vsize < 2^32) fits in 256 bits and
//! the oracle below can compare rates by exact cross-multiplication.
arith_uint256 ConsumeSurplus(FuzzedDataProvider& fdp)
{
    // Nine bytes, not 32, so the input lasts for a large pool.
    arith_uint256 v{fdp.ConsumeIntegral<uint64_t>()};
    v <<= fdp.ConsumeIntegralInRange<unsigned int>(0, 136);
    return v;
}
} // namespace

// Calls the replacement policy (policy/replacement.h) on a deliberately built
// relay pool and checks each function's contract against an independent
// computation over the transaction graph this target built itself.
FUZZ_TARGET(replacement, .init = initialize_replacement)
{
    SeedRandomStateForTest(SeedRand::ZEROS);
    FuzzedDataProvider fdp(buffer.data(), buffer.size());
    SetMockTime(ConsumeTime(fdp));

    bilingual_str error;
    CTxRelayPool pool{RelayPoolOptionsForTest(g_setup->m_node), error};
    Assert(error.empty());

    // The graph: each pool transaction spends confirmed outpoints (hashes that
    // are never a pool txid) or outputs of earlier pool transactions.
    struct Node {
        CTransactionRef tx;
        arith_uint256 surplus;
    };
    std::vector<Node> nodes;
    std::vector<COutPoint> unspent_pool_outputs;
    std::vector<COutPoint> spent_outpoints;
    uint32_t confirmed_counter{0};
    const auto confirmed_outpoint = [&] {
        // Txids of the form 0xff..ff<counter> never collide with a real txid here.
        const uint256 hash{*Assert(uint256::FromHex(strprintf("ff%062x", ++confirmed_counter)))};
        return COutPoint{Txid::FromUint256(hash), 0};
    };
    // Up to 150 entries, drawn directly (not a ConsumeBool loop) so pools large
    // enough for rule #5's 100-candidate bound are common.
    const size_t n_nodes{fdp.ConsumeIntegralInRange<size_t>(2, 150)};
    while (nodes.size() < n_nodes) {
        CMutableTransaction mtx;
        mtx.nLockTime = static_cast<uint32_t>(nodes.size());
        const int n_in{fdp.ConsumeIntegralInRange<int>(1, 3)};
        for (int i = 0; i < n_in; ++i) {
            // Mostly spend pool outputs, so descendant chains are deep. An
            // exhausted input reads 0 here and keeps chaining.
            if (!unspent_pool_outputs.empty() && fdp.ConsumeIntegralInRange<int>(0, 3) != 3) {
                const size_t pick{fdp.ConsumeIntegralInRange<size_t>(0, unspent_pool_outputs.size() - 1)};
                mtx.vin.emplace_back(unspent_pool_outputs[pick]);
                unspent_pool_outputs.erase(unspent_pool_outputs.begin() + pick);
            } else {
                mtx.vin.emplace_back(confirmed_outpoint());
            }
        }
        const int n_out{fdp.ConsumeIntegralInRange<int>(1, 4)};
        for (int i = 0; i < n_out; ++i) mtx.vout.emplace_back(1, CScript{} << OP_TRUE);
        const CTransactionRef tx{MakeTransactionRef(mtx)};
        for (const auto& in : tx->vin) spent_outpoints.push_back(in.prevout);
        for (uint32_t i = 0; i < tx->vout.size(); ++i) unspent_pool_outputs.emplace_back(tx->GetHash(), i);
        const arith_uint256 surplus{ConsumeSurplus(fdp)};
        AddToRelayPool(pool, CTxRelayPoolEntry{tx, 0, 1, 0, false, 0, {}, surplus});
        nodes.push_back({tx, surplus});
    }

    std::map<Txid, size_t> index;
    for (size_t i = 0; i < nodes.size(); ++i) index.emplace(nodes[i].tx->GetHash(), i);
    // children[i]: pool transactions that spend an output of nodes[i].
    std::vector<std::set<size_t>> children(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        for (const auto& in : nodes[i].tx->vin) {
            if (const auto it{index.find(in.prevout.hash)}; it != index.end()) children[it->second].insert(i);
        }
    }
    const auto descendants_of = [&](size_t root) {
        std::set<size_t> out{root};
        std::vector<size_t> todo{root};
        while (!todo.empty()) {
            const size_t n{todo.back()};
            todo.pop_back();
            for (size_t c : children[n]) {
                if (out.insert(c).second) todo.push_back(c);
            }
        }
        return out;
    };

    // The replacement: double-spends some pool inputs, and may add confirmed
    // inputs or (rule #2) unconfirmed pool outputs.
    CMutableTransaction rmtx;
    rmtx.nLockTime = 0xffffffff;
    LIMITED_WHILE(fdp.ConsumeBool() || rmtx.vin.empty(), 8)
    {
        switch (fdp.ConsumeIntegralInRange<int>(0, 2)) {
        case 0:
            rmtx.vin.emplace_back(spent_outpoints[fdp.ConsumeIntegralInRange<size_t>(0, spent_outpoints.size() - 1)]);
            break;
        case 1:
            rmtx.vin.emplace_back(confirmed_outpoint());
            break;
        default:
            if (!unspent_pool_outputs.empty()) {
                rmtx.vin.emplace_back(unspent_pool_outputs[fdp.ConsumeIntegralInRange<size_t>(0, unspent_pool_outputs.size() - 1)]);
            } else {
                rmtx.vin.emplace_back(confirmed_outpoint());
            }
        }
    }
    rmtx.vout.emplace_back(1, CScript{} << OP_TRUE);
    const CTransaction rtx{rmtx};

    std::set<COutPoint> r_prevouts;
    for (const auto& in : rtx.vin) r_prevouts.insert(in.prevout);
    std::set<size_t> conflicts;
    for (size_t i = 0; i < nodes.size(); ++i) {
        for (const auto& in : nodes[i].tx->vin) {
            if (r_prevouts.count(in.prevout)) conflicts.insert(i);
        }
    }

    LOCK2(cs_main, pool.cs);
    const auto iter_of = [&](size_t i) EXCLUSIVE_LOCKS_REQUIRED(pool.cs) {
        return *Assert(pool.GetIter(nodes[i].tx->GetHash()));
    };
    CTxRelayPool::setEntries iters_conflicting;
    for (size_t i : conflicts) iters_conflicting.insert(iter_of(i));

    // GetEntriesForConflicts: rule #5 counts each conflict with its descendants
    // (shared descendants counted again); otherwise all_conflicts is the union of
    // the descendant closures.
    {
        uint64_t count{0};
        bool over{false};
        std::set<size_t> closure;
        for (size_t i : conflicts) {
            const auto d{descendants_of(i)};
            count += d.size();
            if (count > MAX_REPLACEMENT_CANDIDATES) over = true;
            closure.insert(d.begin(), d.end());
        }
        CTxRelayPool::setEntries all_conflicts;
        const auto err{GetEntriesForConflicts(rtx, pool, iters_conflicting, all_conflicts)};
        Assert(err.has_value() == over);
        if (!over) {
            Assert(all_conflicts.size() == closure.size());
            for (size_t i : closure) Assert(all_conflicts.count(iter_of(i)) == 1);
        }
    }

    // HasNoNewUnconfirmed: an input that is not a parent of a conflict must not
    // spend a pool transaction.
    {
        std::set<Txid> parents;
        for (size_t i : conflicts) {
            for (const auto& in : nodes[i].tx->vin) parents.insert(in.prevout.hash);
        }
        bool adds_unconfirmed{false};
        for (const auto& in : rtx.vin) {
            if (!parents.count(in.prevout.hash) && index.count(in.prevout.hash)) adds_unconfirmed = true;
        }
        Assert(HasNoNewUnconfirmed(rtx, pool, iters_conflicting).has_value() == adds_unconfirmed);
    }

    // EntriesAndTxidsDisjoint: an error exactly when the two sets intersect.
    {
        CTxRelayPool::setEntries ancestors;
        std::set<Txid> ancestor_ids;
        std::set<Txid> direct;
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (fdp.ConsumeBool()) {
                ancestors.insert(iter_of(i));
                ancestor_ids.insert(nodes[i].tx->GetHash());
            }
            if (fdp.ConsumeBool()) direct.insert(nodes[i].tx->GetHash());
        }
        if (fdp.ConsumeBool()) direct.insert(rtx.GetHash());
        std::vector<Txid> both;
        std::set_intersection(ancestor_ids.begin(), ancestor_ids.end(), direct.begin(), direct.end(), std::back_inserter(both));
        Assert(EntriesAndTxidsDisjoint(ancestors, direct, rtx.GetHash()).has_value() == !both.empty());
    }

    // PaysMoreWorkThanConflicts / PaysMoreWorkForReplacement against exact
    // arith_uint256 arithmetic. An option copies a conflict's exact rate or the
    // exact surplus sum, so the boundary (equal is a rejection) is reached.
    if (!conflicts.empty()) {
        arith_uint256 sum{0};
        for (size_t i : conflicts) sum += nodes[i].surplus;
        arith_uint256 r_surplus{ConsumeSurplus(fdp)};
        int64_t r_vsize{fdp.ConsumeIntegralInRange<int64_t>(1, std::numeric_limits<uint32_t>::max())};
        switch (fdp.ConsumeIntegralInRange<int>(0, 3)) {
        case 0: { // equal rate to one conflict
            const size_t i{*std::next(conflicts.begin(), fdp.ConsumeIntegralInRange<size_t>(0, conflicts.size() - 1))};
            r_surplus = nodes[i].surplus;
            r_vsize = iter_of(i)->GetTxSize();
            break;
        }
        case 1: r_surplus = sum; break;
        case 2: r_surplus = sum + 1; break;
        default: break;
        }
        bool rate_rejected{false};
        for (size_t i : conflicts) {
            const arith_uint256 lhs{nodes[i].surplus * arith_uint256{static_cast<uint64_t>(r_vsize)}};
            const arith_uint256 rhs{r_surplus * arith_uint256{static_cast<uint64_t>(iter_of(i)->GetTxSize())}};
            if (lhs >= rhs) rate_rejected = true;
        }
        Assert(PaysMoreWorkThanConflicts(iters_conflicting, r_surplus, r_vsize, rtx.GetHash()).has_value() == rate_rejected);
        Assert(PaysMoreWorkForReplacement(sum, r_surplus, rtx.GetHash()).has_value() == (r_surplus <= sum));
    }
}
