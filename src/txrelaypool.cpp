// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <txrelaypool.h>

#include <chain.h>
#include <coins.h>
#include <common/system.h>
#include <consensus/consensus.h>
#include <consensus/params.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <logging.h>
#include <policy/policy.h>
#include <policy/settings.h>
#include <random.h>
#include <tinyformat.h>
#include <util/check.h>
#include <util/moneystr.h>
#include <util/overflow.h>
#include <util/result.h>
#include <util/time.h>
#include <util/trace.h>
#include <util/translation.h>
#include <validationinterface.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>

TRACEPOINT_SEMAPHORE(relaypool, added);
TRACEPOINT_SEMAPHORE(relaypool, removed);

bool TestLockPointValidity(CChain& active_chain, const LockPoints& lp)
{
    AssertLockHeld(cs_main);
    // If there are relative lock times then the maxInputBlock will be set
    // If there are no relative lock times, the LockPoints don't depend on the chain
    if (lp.maxInputBlock) {
        // Check whether active_chain is an extension of the block at which the LockPoints
        // calculation was valid.  If not LockPoints are no longer valid
        if (!active_chain.Contains(lp.maxInputBlock)) {
            return false;
        }
    }

    // LockPoints still valid
    return true;
}

void CTxRelayPool::UpdateForDescendants(txiter updateIt, cacheMap& cachedDescendants,
                                      const std::set<uint256>& setExclude, std::set<uint256>& descendants_to_remove)
{
    CTxRelayPoolEntry::Children stageEntries, descendants;
    stageEntries = updateIt->GetRelayPoolChildrenConst();

    while (!stageEntries.empty()) {
        const CTxRelayPoolEntry& descendant = *stageEntries.begin();
        descendants.insert(descendant);
        stageEntries.erase(descendant);
        const CTxRelayPoolEntry::Children& children = descendant.GetRelayPoolChildrenConst();
        for (const CTxRelayPoolEntry& childEntry : children) {
            cacheMap::iterator cacheIt = cachedDescendants.find(mapTx.iterator_to(childEntry));
            if (cacheIt != cachedDescendants.end()) {
                // We've already calculated this one, just add the entries for this set
                // but don't traverse again.
                for (txiter cacheEntry : cacheIt->second) {
                    descendants.insert(*cacheEntry);
                }
            } else if (!descendants.count(childEntry)) {
                // Schedule for later processing
                stageEntries.insert(childEntry);
            }
        }
    }
    // descendants now contains all in-relaypool descendants of updateIt.
    // Update and add to cached descendant map
    int32_t modifySize = 0;
    int64_t modifyCount = 0;
    for (const CTxRelayPoolEntry& descendant : descendants) {
        if (!setExclude.count(descendant.GetTx().GetHash())) {
            modifySize += descendant.GetTxSize();
            modifyCount++;
            cachedDescendants[updateIt].insert(mapTx.iterator_to(descendant));
            // Update ancestor state for each descendant
            mapTx.modify(mapTx.iterator_to(descendant), [=](CTxRelayPoolEntry& e) {
              e.UpdateAncestorState(updateIt->GetTxSize(), 1, updateIt->GetSigOpCost());
            });
            // Don't directly remove the transaction here -- doing so would
            // invalidate iterators in cachedDescendants. Mark it for removal
            // by inserting into descendants_to_remove.
            if (descendant.GetCountWithAncestors() > uint64_t(m_opts.limits.ancestor_count) || descendant.GetSizeWithAncestors() > m_opts.limits.ancestor_size_vbytes) {
                descendants_to_remove.insert(descendant.GetTx().GetHash());
            }
        }
    }
    mapTx.modify(updateIt, [=](CTxRelayPoolEntry& e) { e.UpdateDescendantState(modifySize, modifyCount); });
}

void CTxRelayPool::UpdateTransactionsFromBlock(const std::vector<uint256>& vHashesToUpdate)
{
    AssertLockHeld(cs);
    // For each entry in vHashesToUpdate, store the set of in-relaypool, but not
    // in-vHashesToUpdate transactions, so that we don't have to recalculate
    // descendants when we come across a previously seen entry.
    cacheMap mapRelayPoolDescendantsToUpdate;

    // Use a set for lookups into vHashesToUpdate (these entries are already
    // accounted for in the state of their ancestors)
    std::set<uint256> setAlreadyIncluded(vHashesToUpdate.begin(), vHashesToUpdate.end());

    std::set<uint256> descendants_to_remove;

    // Iterate in reverse, so that whenever we are looking at a transaction
    // we are sure that all in-relaypool descendants have already been processed.
    // This maximizes the benefit of the descendant cache and guarantees that
    // CTxRelayPoolEntry::m_children will be updated, an assumption made in
    // UpdateForDescendants.
    for (const uint256& hash : vHashesToUpdate | std::views::reverse) {
        // calculate children from mapNextTx
        txiter it = mapTx.find(hash);
        if (it == mapTx.end()) {
            continue;
        }
        auto iter = mapNextTx.lower_bound(COutPoint(Txid::FromUint256(hash), 0));
        // First calculate the children, and update CTxRelayPoolEntry::m_children to
        // include them, and update their CTxRelayPoolEntry::m_parents to include this tx.
        // we cache the in-relaypool children to avoid duplicate updates
        {
            WITH_FRESH_EPOCH(m_epoch);
            for (; iter != mapNextTx.end() && iter->first->hash == hash; ++iter) {
                const uint256 &childHash = iter->second->GetHash();
                txiter childIter = mapTx.find(childHash);
                assert(childIter != mapTx.end());
                // We can skip updating entries we've encountered before or that
                // are in the block (which are already accounted for).
                if (!visited(childIter) && !setAlreadyIncluded.count(childHash)) {
                    UpdateChild(it, childIter, true);
                    UpdateParent(childIter, it, true);
                }
            }
        } // release epoch guard for UpdateForDescendants
        UpdateForDescendants(it, mapRelayPoolDescendantsToUpdate, setAlreadyIncluded, descendants_to_remove);
    }

    for (const auto& txid : descendants_to_remove) {
        // This txid may have been removed already in a prior call to removeRecursive.
        // Therefore we ensure it is not yet removed already.
        if (const std::optional<txiter> txiter = GetIter(txid)) {
            removeRecursive((*txiter)->GetTx(), RelayPoolRemovalReason::SIZELIMIT);
        }
    }
}

util::Result<CTxRelayPool::setEntries> CTxRelayPool::CalculateAncestorsAndCheckLimits(
    int64_t entry_size,
    size_t entry_count,
    CTxRelayPoolEntry::Parents& staged_ancestors,
    const Limits& limits) const
{
    int64_t totalSizeWithAncestors = entry_size;
    setEntries ancestors;

    while (!staged_ancestors.empty()) {
        const CTxRelayPoolEntry& stage = staged_ancestors.begin()->get();
        txiter stageit = mapTx.iterator_to(stage);

        ancestors.insert(stageit);
        staged_ancestors.erase(stage);
        totalSizeWithAncestors += stageit->GetTxSize();

        if (stageit->GetSizeWithDescendants() + entry_size > limits.descendant_size_vbytes) {
            return util::Error{Untranslated(strprintf("exceeds descendant size limit for tx %s [limit: %u]", stageit->GetTx().GetHash().ToString(), limits.descendant_size_vbytes))};
        } else if (stageit->GetCountWithDescendants() + entry_count > static_cast<uint64_t>(limits.descendant_count)) {
            return util::Error{Untranslated(strprintf("too many descendants for tx %s [limit: %u]", stageit->GetTx().GetHash().ToString(), limits.descendant_count))};
        } else if (totalSizeWithAncestors > limits.ancestor_size_vbytes) {
            return util::Error{Untranslated(strprintf("exceeds ancestor size limit [limit: %u]", limits.ancestor_size_vbytes))};
        }

        const CTxRelayPoolEntry::Parents& parents = stageit->GetRelayPoolParentsConst();
        for (const CTxRelayPoolEntry& parent : parents) {
            txiter parent_it = mapTx.iterator_to(parent);

            // If this is a new ancestor, add it.
            if (ancestors.count(parent_it) == 0) {
                staged_ancestors.insert(parent);
            }
            if (staged_ancestors.size() + ancestors.size() + entry_count > static_cast<uint64_t>(limits.ancestor_count)) {
                return util::Error{Untranslated(strprintf("too many unconfirmed ancestors [limit: %u]", limits.ancestor_count))};
            }
        }
    }

    return ancestors;
}

util::Result<void> CTxRelayPool::CheckPackageLimits(const Package& package,
                                                  const int64_t total_vsize) const
{
    size_t pack_count = package.size();

    // Package itself is busting relaypool limits; should be rejected even if no staged_ancestors exist
    if (pack_count > static_cast<uint64_t>(m_opts.limits.ancestor_count)) {
        return util::Error{Untranslated(strprintf("package count %u exceeds ancestor count limit [limit: %u]", pack_count, m_opts.limits.ancestor_count))};
    } else if (pack_count > static_cast<uint64_t>(m_opts.limits.descendant_count)) {
        return util::Error{Untranslated(strprintf("package count %u exceeds descendant count limit [limit: %u]", pack_count, m_opts.limits.descendant_count))};
    } else if (total_vsize > m_opts.limits.ancestor_size_vbytes) {
        return util::Error{Untranslated(strprintf("package size %u exceeds ancestor size limit [limit: %u]", total_vsize, m_opts.limits.ancestor_size_vbytes))};
    } else if (total_vsize > m_opts.limits.descendant_size_vbytes) {
        return util::Error{Untranslated(strprintf("package size %u exceeds descendant size limit [limit: %u]", total_vsize, m_opts.limits.descendant_size_vbytes))};
    }

    CTxRelayPoolEntry::Parents staged_ancestors;
    for (const auto& tx : package) {
        for (const auto& input : tx->vin) {
            std::optional<txiter> piter = GetIter(input.prevout.hash);
            if (piter) {
                staged_ancestors.insert(**piter);
                if (staged_ancestors.size() + package.size() > static_cast<uint64_t>(m_opts.limits.ancestor_count)) {
                    return util::Error{Untranslated(strprintf("too many unconfirmed parents [limit: %u]", m_opts.limits.ancestor_count))};
                }
            }
        }
    }
    // When multiple transactions are passed in, the ancestors and descendants of all transactions
    // considered together must be within limits even if they are not interdependent. This may be
    // stricter than the limits for each individual transaction.
    const auto ancestors{CalculateAncestorsAndCheckLimits(total_vsize, package.size(),
                                                          staged_ancestors, m_opts.limits)};
    // It's possible to overestimate the ancestor/descendant totals.
    if (!ancestors.has_value()) return util::Error{Untranslated("possibly " + util::ErrorString(ancestors).original)};
    return {};
}

util::Result<CTxRelayPool::setEntries> CTxRelayPool::CalculateRelayPoolAncestors(
    const CTxRelayPoolEntry &entry,
    const Limits& limits,
    bool fSearchForParents /* = true */) const
{
    CTxRelayPoolEntry::Parents staged_ancestors;
    const CTransaction &tx = entry.GetTx();

    if (fSearchForParents) {
        // Get parents of this transaction that are in the relaypool
        // GetRelayPoolParents() is only valid for entries in the relaypool, so we
        // iterate mapTx to find parents.
        for (unsigned int i = 0; i < tx.vin.size(); i++) {
            std::optional<txiter> piter = GetIter(tx.vin[i].prevout.hash);
            if (piter) {
                staged_ancestors.insert(**piter);
                if (staged_ancestors.size() + 1 > static_cast<uint64_t>(limits.ancestor_count)) {
                    return util::Error{Untranslated(strprintf("too many unconfirmed parents [limit: %u]", limits.ancestor_count))};
                }
            }
        }
    } else {
        // If we're not searching for parents, we require this to already be an
        // entry in the relaypool and use the entry's cached parents.
        txiter it = mapTx.iterator_to(entry);
        staged_ancestors = it->GetRelayPoolParentsConst();
    }

    return CalculateAncestorsAndCheckLimits(entry.GetTxSize(), /*entry_count=*/1, staged_ancestors,
                                            limits);
}

CTxRelayPool::setEntries CTxRelayPool::AssumeCalculateRelayPoolAncestors(
    std::string_view calling_fn_name,
    const CTxRelayPoolEntry &entry,
    const Limits& limits,
    bool fSearchForParents /* = true */) const
{
    auto result{CalculateRelayPoolAncestors(entry, limits, fSearchForParents)};
    if (!Assume(result)) {
        LogPrintLevel(HgLog::RELAYPOOL, HgLog::Level::Error, "%s: CalculateRelayPoolAncestors failed unexpectedly, continuing with empty ancestor set (%s)\n",
                      calling_fn_name, util::ErrorString(result).original);
    }
    return std::move(result).value_or(CTxRelayPool::setEntries{});
}

void CTxRelayPool::UpdateAncestorsOf(bool add, txiter it, setEntries &setAncestors)
{
    const CTxRelayPoolEntry::Parents& parents = it->GetRelayPoolParentsConst();
    // add or remove this tx as a child of each parent
    for (const CTxRelayPoolEntry& parent : parents) {
        UpdateChild(mapTx.iterator_to(parent), it, add);
    }
    const int32_t updateCount = (add ? 1 : -1);
    const int32_t updateSize{updateCount * it->GetTxSize()};
    for (txiter ancestorIt : setAncestors) {
        mapTx.modify(ancestorIt, [=](CTxRelayPoolEntry& e) { e.UpdateDescendantState(updateSize, updateCount); });
    }
}

void CTxRelayPool::UpdateEntryForAncestors(txiter it, const setEntries &setAncestors)
{
    int64_t updateCount = setAncestors.size();
    int64_t updateSize = 0;
    int64_t updateSigOpsCost = 0;
    for (txiter ancestorIt : setAncestors) {
        updateSize += ancestorIt->GetTxSize();
        updateSigOpsCost += ancestorIt->GetSigOpCost();
    }
    mapTx.modify(it, [=](CTxRelayPoolEntry& e){ e.UpdateAncestorState(updateSize, updateCount, updateSigOpsCost); });
}

void CTxRelayPool::UpdateChildrenForRemoval(txiter it)
{
    const CTxRelayPoolEntry::Children& children = it->GetRelayPoolChildrenConst();
    for (const CTxRelayPoolEntry& updateIt : children) {
        UpdateParent(mapTx.iterator_to(updateIt), it, false);
    }
}

void CTxRelayPool::UpdateForRemoveFromRelayPool(const setEntries &entriesToRemove, bool updateDescendants)
{
    // For each entry, walk back all ancestors and decrement size associated with this
    // transaction
    if (updateDescendants) {
        // updateDescendants should be true whenever we're not recursively
        // removing a tx and all its descendants, eg when a transaction is
        // confirmed in a block.
        // Here we only update statistics and not data in CTxRelayPool::Parents
        // and CTxRelayPoolEntry::Children (which we need to preserve until we're
        // finished with all operations that need to traverse the relaypool).
        for (txiter removeIt : entriesToRemove) {
            setEntries setDescendants;
            CalculateDescendants(removeIt, setDescendants);
            setDescendants.erase(removeIt); // don't update state for self
            int32_t modifySize = -removeIt->GetTxSize();
            int modifySigOps = -removeIt->GetSigOpCost();
            for (txiter dit : setDescendants) {
                mapTx.modify(dit, [=](CTxRelayPoolEntry& e){ e.UpdateAncestorState(modifySize, -1, modifySigOps); });
            }
        }
    }
    for (txiter removeIt : entriesToRemove) {
        const CTxRelayPoolEntry &entry = *removeIt;
        // Since this is a tx that is already in the relaypool, we can call CMPA
        // with fSearchForParents = false.  If the relaypool is in a consistent
        // state, then using true or false should both be correct, though false
        // should be a bit faster.
        // However, if we happen to be in the middle of processing a reorg, then
        // the relaypool can be in an inconsistent state.  In this case, the set
        // of ancestors reachable via GetRelayPoolParents()/GetRelayPoolChildren()
        // will be the same as the set of ancestors whose packages include this
        // transaction, because when we add a new transaction to the relaypool in
        // addNewTransaction(), we assume it has no children, and in the case of a
        // reorg where that assumption is false, the in-relaypool children aren't
        // linked to the in-block tx's until UpdateTransactionsFromBlock() is
        // called.
        // So if we're being called during a reorg, ie before
        // UpdateTransactionsFromBlock() has been called, then
        // GetRelayPoolParents()/GetRelayPoolChildren() will differ from the set of
        // relaypool parents we'd calculate by searching, and it's important that
        // we use the cached notion of ancestor transactions as the set of
        // things to update for removal.
        auto ancestors{AssumeCalculateRelayPoolAncestors(__func__, entry, Limits::NoLimits(), /*fSearchForParents=*/false)};
        // Note that UpdateAncestorsOf severs the child links that point to
        // removeIt in the entries for the parents of removeIt.
        UpdateAncestorsOf(false, removeIt, ancestors);
    }
    // After updating all the ancestor sizes, we can now sever the link between each
    // transaction being removed and any relaypool children (ie, update CTxRelayPoolEntry::m_parents
    // for each direct child of a transaction being removed).
    for (txiter removeIt : entriesToRemove) {
        UpdateChildrenForRemoval(removeIt);
    }
}

void CTxRelayPoolEntry::UpdateDescendantState(int32_t modifySize, int64_t modifyCount)
{
    nSizeWithDescendants += modifySize;
    assert(nSizeWithDescendants > 0);
    m_count_with_descendants += modifyCount;
    assert(m_count_with_descendants > 0);
}

void CTxRelayPoolEntry::UpdateAncestorState(int32_t modifySize, int64_t modifyCount, int64_t modifySigOps)
{
    nSizeWithAncestors += modifySize;
    assert(nSizeWithAncestors > 0);
    m_count_with_ancestors += modifyCount;
    assert(m_count_with_ancestors > 0);
    nSigOpCostWithAncestors += modifySigOps;
    assert(int(nSigOpCostWithAncestors) >= 0);
}

//! Clamp option values and populate the error if options are not valid.
static CTxRelayPool::Options&& Flatten(CTxRelayPool::Options&& opts, bilingual_str& error)
{
    opts.check_ratio = std::clamp<int>(opts.check_ratio, 0, 1'000'000);
    int64_t descendant_limit_bytes = opts.limits.descendant_size_vbytes * 40;
    if (opts.max_size_bytes < 0 || opts.max_size_bytes < descendant_limit_bytes) {
        error = strprintf(_("-maxrelaypool must be at least %d MB"), std::ceil(descendant_limit_bytes / 1'000'000.0));
    }
    return std::move(opts);
}

CTxRelayPool::CTxRelayPool(Options opts, bilingual_str& error)
    : m_opts{Flatten(std::move(opts), error)}
{
}

bool CTxRelayPool::isSpent(const COutPoint& outpoint) const
{
    LOCK(cs);
    return mapNextTx.count(outpoint);
}

unsigned int CTxRelayPool::GetTransactionsUpdated() const
{
    return nTransactionsUpdated;
}

void CTxRelayPool::AddTransactionsUpdated(unsigned int n)
{
    nTransactionsUpdated += n;
}

void CTxRelayPool::Apply(ChangeSet* changeset)
{
    AssertLockHeld(cs);
    RemoveStaged(changeset->m_to_remove, false, RelayPoolRemovalReason::REPLACED);

    for (size_t i=0; i<changeset->m_entry_vec.size(); ++i) {
        auto tx_entry = changeset->m_entry_vec[i];
        std::optional<CTxRelayPool::setEntries> ancestors;
        if (i == 0) {
            // Note: ChangeSet::CalculateRelayPoolAncestors() will return a
            // cached value if relaypool ancestors for this transaction were
            // previously calculated.
            // We can only use a cached ancestor calculation for the first
            // transaction in a package, because in-package parents won't be
            // present in the cached ancestor sets of in-package children.
            // We pass in Limits::NoLimits() to ensure that this function won't fail
            // (we're going to be applying this set of transactions whether or
            // not the relaypool policy limits are being respected).
            ancestors = *Assume(changeset->CalculateRelayPoolAncestors(tx_entry, Limits::NoLimits()));
        }
        // First splice this entry into mapTx.
        auto node_handle = changeset->m_to_add.extract(tx_entry);
        auto result = mapTx.insert(std::move(node_handle));

        Assume(result.inserted);
        txiter it = result.position;

        // Now update the entry for ancestors/descendants.
        if (ancestors.has_value()) {
            addNewTransaction(it, *ancestors);
        } else {
            addNewTransaction(it);
        }
    }
}

void CTxRelayPool::addNewTransaction(CTxRelayPool::txiter it)
{
    auto ancestors{AssumeCalculateRelayPoolAncestors(__func__, *it, Limits::NoLimits())};
    return addNewTransaction(it, ancestors);
}

void CTxRelayPool::addNewTransaction(CTxRelayPool::txiter newit, CTxRelayPool::setEntries& setAncestors)
{
    const CTxRelayPoolEntry& entry = *newit;

    // Update cachedInnerUsage to include contained transaction's usage.
    // (When we update the entry for in-relaypool parents, memory usage will be
    // further updated.)
    cachedInnerUsage += entry.DynamicMemoryUsage();

    const CTransaction& tx = newit->GetTx();
    std::set<Txid> setParentTransactions;
    for (unsigned int i = 0; i < tx.vin.size(); i++) {
        mapNextTx.insert(std::make_pair(&tx.vin[i].prevout, &tx));
        setParentTransactions.insert(tx.vin[i].prevout.hash);
    }
    // Don't bother worrying about child transactions of this one.
    // Normal case of a new transaction arriving is that there can't be any
    // children, because such children would be orphans.
    // An exception to that is if a transaction enters that used to be in a block.
    // In that case, our disconnect block logic will call UpdateTransactionsFromBlock
    // to clean up the mess we're leaving here.

    // Update ancestors with information about this tx
    for (const auto& pit : GetIterSet(setParentTransactions)) {
        UpdateParent(newit, pit, true);
    }
    UpdateAncestorsOf(true, newit, setAncestors);
    UpdateEntryForAncestors(newit, setAncestors);

    nTransactionsUpdated++;
    totalTxSize += entry.GetTxSize();
    txns_randomized.emplace_back(newit->GetSharedTx());
    newit->idx_randomized = txns_randomized.size() - 1;

    TRACEPOINT(relaypool, added,
        entry.GetTx().GetHash().data(),
        entry.GetTxSize()
    );
}

void CTxRelayPool::removeUnchecked(txiter it, RelayPoolRemovalReason reason)
{
    // We increment relaypool sequence value no matter removal reason
    // even if not directly reported below.
    uint64_t relaypool_sequence = GetAndIncrementSequence();

    if (reason != RelayPoolRemovalReason::BLOCK && m_opts.signals) {
        // Notify clients that a transaction has been removed from the relaypool
        // for any reason except being included in a block. Clients interested
        // in transactions included in blocks can subscribe to the BlockConnected
        // notification.
        m_opts.signals->TransactionRemovedFromRelayPool(it->GetSharedTx(), reason, relaypool_sequence);
    }
    TRACEPOINT(relaypool, removed,
        it->GetTx().GetHash().data(),
        RemovalReasonToString(reason).c_str(),
        it->GetTxSize(),
        std::chrono::duration_cast<std::chrono::duration<std::uint64_t>>(it->GetTime()).count()
    );

    for (const CTxIn& txin : it->GetTx().vin)
        mapNextTx.erase(txin.prevout);

    RemoveUnbroadcastTx(it->GetTx().GetHash(), true /* add logging because unchecked */);

    if (txns_randomized.size() > 1) {
        // Update idx_randomized of the to-be-moved entry.
        Assert(GetEntry(txns_randomized.back()->GetHash()))->idx_randomized = it->idx_randomized;
        // Remove entry from txns_randomized by replacing it with the back and deleting the back.
        txns_randomized[it->idx_randomized] = std::move(txns_randomized.back());
        txns_randomized.pop_back();
        if (txns_randomized.size() * 2 < txns_randomized.capacity())
            txns_randomized.shrink_to_fit();
    } else
        txns_randomized.clear();

    totalTxSize -= it->GetTxSize();
    cachedInnerUsage -= it->DynamicMemoryUsage();
    cachedInnerUsage -= memusage::DynamicUsage(it->GetRelayPoolParentsConst()) + memusage::DynamicUsage(it->GetRelayPoolChildrenConst());
    mapTx.erase(it);
    nTransactionsUpdated++;
}

// Calculates descendants of entry that are not already in setDescendants, and adds to
// setDescendants. Assumes entryit is already a tx in the relaypool and CTxRelayPoolEntry::m_children
// is correct for tx and all descendants.
// Also assumes that if an entry is in setDescendants already, then all
// in-relaypool descendants of it are already in setDescendants as well, so that we
// can save time by not iterating over those entries.
void CTxRelayPool::CalculateDescendants(txiter entryit, setEntries& setDescendants) const
{
    setEntries stage;
    if (setDescendants.count(entryit) == 0) {
        stage.insert(entryit);
    }
    // Traverse down the children of entry, only adding children that are not
    // accounted for in setDescendants already (because those children have either
    // already been walked, or will be walked in this iteration).
    while (!stage.empty()) {
        txiter it = *stage.begin();
        setDescendants.insert(it);
        stage.erase(it);

        const CTxRelayPoolEntry::Children& children = it->GetRelayPoolChildrenConst();
        for (const CTxRelayPoolEntry& child : children) {
            txiter childiter = mapTx.iterator_to(child);
            if (!setDescendants.count(childiter)) {
                stage.insert(childiter);
            }
        }
    }
}

void CTxRelayPool::removeRecursive(const CTransaction &origTx, RelayPoolRemovalReason reason)
{
    // Remove transaction from memory pool
    AssertLockHeld(cs);
    Assume(!m_have_changeset);
        setEntries txToRemove;
        txiter origit = mapTx.find(origTx.GetHash());
        if (origit != mapTx.end()) {
            txToRemove.insert(origit);
        } else {
            // When recursively removing but origTx isn't in the relaypool
            // be sure to remove any children that are in the pool. This can
            // happen during chain re-orgs if origTx isn't re-accepted into
            // the relaypool for any reason.
            for (unsigned int i = 0; i < origTx.vout.size(); i++) {
                auto it = mapNextTx.find(COutPoint(origTx.GetHash(), i));
                if (it == mapNextTx.end())
                    continue;
                txiter nextit = mapTx.find(it->second->GetHash());
                assert(nextit != mapTx.end());
                txToRemove.insert(nextit);
            }
        }
        setEntries setAllRemoves;
        for (txiter it : txToRemove) {
            CalculateDescendants(it, setAllRemoves);
        }

        RemoveStaged(setAllRemoves, false, reason);
}

void CTxRelayPool::removeForReorg(CChain& chain, std::function<bool(txiter)> check_final_and_mature)
{
    // Remove transactions spending a coinbase which are now immature and no-longer-final transactions
    AssertLockHeld(cs);
    AssertLockHeld(::cs_main);
    Assume(!m_have_changeset);

    setEntries txToRemove;
    for (indexed_transaction_set::const_iterator it = mapTx.begin(); it != mapTx.end(); it++) {
        if (check_final_and_mature(it)) txToRemove.insert(it);
    }
    setEntries setAllRemoves;
    for (txiter it : txToRemove) {
        CalculateDescendants(it, setAllRemoves);
    }
    RemoveStaged(setAllRemoves, false, RelayPoolRemovalReason::REORG);
    for (indexed_transaction_set::const_iterator it = mapTx.begin(); it != mapTx.end(); it++) {
        assert(TestLockPointValidity(chain, it->GetLockPoints()));
    }
}

void CTxRelayPool::removeConflicts(const CTransaction &tx)
{
    // Remove transactions which depend on inputs of tx, recursively
    AssertLockHeld(cs);
    for (const CTxIn &txin : tx.vin) {
        auto it = mapNextTx.find(txin.prevout);
        if (it != mapNextTx.end()) {
            const CTransaction &txConflict = *it->second;
            if (txConflict != tx)
            {
                removeRecursive(txConflict, RelayPoolRemovalReason::CONFLICT);
            }
        }
    }
}

/**
 * Called when a block is connected. Removes from relaypool.
 */
void CTxRelayPool::removeForBlock(const std::vector<CTransactionRef>& vtx)
{
    AssertLockHeld(cs);
    Assume(!m_have_changeset);
    for (const auto& tx : vtx)
    {
        txiter it = mapTx.find(tx->GetHash());
        if (it != mapTx.end()) {
            setEntries stage;
            stage.insert(it);
            RemoveStaged(stage, true, RelayPoolRemovalReason::BLOCK);
        }
        removeConflicts(*tx);
    }
}

void CTxRelayPool::check(const CCoinsViewCache& active_coins_tip, int64_t spendheight) const
{
    if (m_opts.check_ratio == 0) return;

    if (FastRandomContext().randrange(m_opts.check_ratio) >= 1) return;

    AssertLockHeld(::cs_main);
    LOCK(cs);
    LogDebug(HgLog::RELAYPOOL, "Checking relay pool with %u transactions and %u inputs\n", (unsigned int)mapTx.size(), (unsigned int)mapNextTx.size());

    uint64_t checkTotal = 0;
    uint64_t innerUsage = 0;
    uint64_t prev_ancestor_count{0};

    CCoinsViewCache relaypoolDuplicate(const_cast<CCoinsViewCache*>(&active_coins_tip));

    for (const auto& it : GetSortedDepthAndScore()) {
        checkTotal += it->GetTxSize();
        innerUsage += it->DynamicMemoryUsage();
        const CTransaction& tx = it->GetTx();
        innerUsage += memusage::DynamicUsage(it->GetRelayPoolParentsConst()) + memusage::DynamicUsage(it->GetRelayPoolChildrenConst());
        CTxRelayPoolEntry::Parents setParentCheck;
        for (const CTxIn &txin : tx.vin) {
            // Check that every relaypool transaction's inputs refer to available coins, or other relaypool tx's.
            indexed_transaction_set::const_iterator it2 = mapTx.find(txin.prevout.hash);
            if (it2 != mapTx.end()) {
                const CTransaction& tx2 = it2->GetTx();
                assert(tx2.vout.size() > txin.prevout.n && !tx2.vout[txin.prevout.n].IsNull());
                setParentCheck.insert(*it2);
            }
            // We are iterating through the relaypool entries sorted in order by ancestor count.
            // All parents must have been checked before their children and their coins added to
            // the relaypoolDuplicate coins cache.
            assert(relaypoolDuplicate.HaveCoin(txin.prevout));
            // Check whether its inputs are marked in mapNextTx.
            auto it3 = mapNextTx.find(txin.prevout);
            assert(it3 != mapNextTx.end());
            assert(it3->first == &txin.prevout);
            assert(it3->second == &tx);
        }
        auto comp = [](const CTxRelayPoolEntry& a, const CTxRelayPoolEntry& b) -> bool {
            return a.GetTx().GetHash() == b.GetTx().GetHash();
        };
        assert(setParentCheck.size() == it->GetRelayPoolParentsConst().size());
        assert(std::equal(setParentCheck.begin(), setParentCheck.end(), it->GetRelayPoolParentsConst().begin(), comp));
        // Verify ancestor state is correct.
        auto ancestors{AssumeCalculateRelayPoolAncestors(__func__, *it, Limits::NoLimits())};
        uint64_t nCountCheck = ancestors.size() + 1;
        int32_t nSizeCheck = it->GetTxSize();
        int64_t nSigOpCheck = it->GetSigOpCost();

        for (txiter ancestorIt : ancestors) {
            nSizeCheck += ancestorIt->GetTxSize();
            nSigOpCheck += ancestorIt->GetSigOpCost();
        }

        assert(it->GetCountWithAncestors() == nCountCheck);
        assert(it->GetSizeWithAncestors() == nSizeCheck);
        assert(it->GetSigOpCostWithAncestors() == nSigOpCheck);
        // Sanity check: we are walking in ascending ancestor count order.
        assert(prev_ancestor_count <= it->GetCountWithAncestors());
        prev_ancestor_count = it->GetCountWithAncestors();

        // Check children against mapNextTx
        CTxRelayPoolEntry::Children setChildrenCheck;
        auto iter = mapNextTx.lower_bound(COutPoint(it->GetTx().GetHash(), 0));
        int32_t child_sizes{0};
        for (; iter != mapNextTx.end() && iter->first->hash == it->GetTx().GetHash(); ++iter) {
            txiter childit = mapTx.find(iter->second->GetHash());
            assert(childit != mapTx.end()); // mapNextTx points to in-relaypool transactions
            if (setChildrenCheck.insert(*childit).second) {
                child_sizes += childit->GetTxSize();
            }
        }
        assert(setChildrenCheck.size() == it->GetRelayPoolChildrenConst().size());
        assert(std::equal(setChildrenCheck.begin(), setChildrenCheck.end(), it->GetRelayPoolChildrenConst().begin(), comp));
        // Also check to make sure size is greater than sum with immediate children.
        // just a sanity check, not definitive that this calc is correct...
        assert(it->GetSizeWithDescendants() >= child_sizes + it->GetTxSize());

        TxValidationState dummy_state; // Not used. CheckTxInputs() should always pass
        assert(!tx.IsCoinBase());
        assert(Consensus::CheckTxInputs(tx, dummy_state, relaypoolDuplicate, spendheight));
        for (const auto& input: tx.vin) relaypoolDuplicate.SpendCoin(input.prevout);
        AddCoins(relaypoolDuplicate, tx, std::numeric_limits<int>::max());
    }
    for (auto it = mapNextTx.cbegin(); it != mapNextTx.cend(); it++) {
        uint256 hash = it->second->GetHash();
        indexed_transaction_set::const_iterator it2 = mapTx.find(hash);
        const CTransaction& tx = it2->GetTx();
        assert(it2 != mapTx.end());
        assert(&tx == it->second);
    }

    assert(totalTxSize == checkTotal);
    assert(innerUsage == cachedInnerUsage);
}

bool CTxRelayPool::CompareDepthAndScore(const uint256& hasha, const uint256& hashb, bool wtxid)
{
    /* Return `true` if hasha should be considered sooner than hashb. Namely when:
     *   a is not in the relaypool, but b is
     *   both are in the relaypool and a has fewer ancestors than b
     *   both are in the relaypool and a has a higher score than b
     */
    LOCK(cs);
    indexed_transaction_set::const_iterator j = wtxid ? get_iter_from_wtxid(hashb) : mapTx.find(hashb);
    if (j == mapTx.end()) return false;
    indexed_transaction_set::const_iterator i = wtxid ? get_iter_from_wtxid(hasha) : mapTx.find(hasha);
    if (i == mapTx.end()) return true;
    uint64_t counta = i->GetCountWithAncestors();
    uint64_t countb = j->GetCountWithAncestors();
    if (counta == countb) {
        return CompareTxRelayPoolEntryByTxWorkRate()(*i, *j);
    }
    return counta < countb;
}

namespace {
class DepthAndScoreComparator
{
public:
    bool operator()(const CTxRelayPool::indexed_transaction_set::const_iterator& a, const CTxRelayPool::indexed_transaction_set::const_iterator& b)
    {
        uint64_t counta = a->GetCountWithAncestors();
        uint64_t countb = b->GetCountWithAncestors();
        if (counta == countb) {
            return CompareTxRelayPoolEntryByTxWorkRate()(*a, *b);
        }
        return counta < countb;
    }
};
} // namespace

std::vector<CTxRelayPool::indexed_transaction_set::const_iterator> CTxRelayPool::GetSortedDepthAndScore() const
{
    std::vector<indexed_transaction_set::const_iterator> iters;
    AssertLockHeld(cs);

    iters.reserve(mapTx.size());

    for (indexed_transaction_set::iterator mi = mapTx.begin(); mi != mapTx.end(); ++mi) {
        iters.push_back(mi);
    }
    std::sort(iters.begin(), iters.end(), DepthAndScoreComparator());
    return iters;
}

static TxRelayPoolInfo GetInfo(CTxRelayPool::indexed_transaction_set::const_iterator it) {
    return TxRelayPoolInfo{it->GetSharedTx(), it->GetTime(), it->GetTxSize()};
}

std::vector<CTxRelayPoolEntryRef> CTxRelayPool::entryAll() const
{
    AssertLockHeld(cs);

    std::vector<CTxRelayPoolEntryRef> ret;
    ret.reserve(mapTx.size());
    for (const auto& it : GetSortedDepthAndScore()) {
        ret.emplace_back(*it);
    }
    return ret;
}

std::vector<TxRelayPoolInfo> CTxRelayPool::infoAll() const
{
    LOCK(cs);
    auto iters = GetSortedDepthAndScore();

    std::vector<TxRelayPoolInfo> ret;
    ret.reserve(mapTx.size());
    for (auto it : iters) {
        ret.push_back(GetInfo(it));
    }

    return ret;
}

const CTxRelayPoolEntry* CTxRelayPool::GetEntry(const Txid& txid) const
{
    AssertLockHeld(cs);
    const auto i = mapTx.find(txid);
    return i == mapTx.end() ? nullptr : &(*i);
}

CTransactionRef CTxRelayPool::get(const uint256& hash) const
{
    LOCK(cs);
    indexed_transaction_set::const_iterator i = mapTx.find(hash);
    if (i == mapTx.end())
        return nullptr;
    return i->GetSharedTx();
}

TxRelayPoolInfo CTxRelayPool::info(const GenTxid& gtxid) const
{
    LOCK(cs);
    indexed_transaction_set::const_iterator i = (gtxid.IsWtxid() ? get_iter_from_wtxid(gtxid.GetHash()) : mapTx.find(gtxid.GetHash()));
    if (i == mapTx.end())
        return TxRelayPoolInfo();
    return GetInfo(i);
}

TxRelayPoolInfo CTxRelayPool::info_for_relay(const GenTxid& gtxid, uint64_t last_sequence) const
{
    LOCK(cs);
    indexed_transaction_set::const_iterator i = (gtxid.IsWtxid() ? get_iter_from_wtxid(gtxid.GetHash()) : mapTx.find(gtxid.GetHash()));
    if (i != mapTx.end() && i->GetSequence() < last_sequence) {
        return GetInfo(i);
    } else {
        return TxRelayPoolInfo();
    }
}

const CTransaction* CTxRelayPool::GetConflictTx(const COutPoint& prevout) const
{
    const auto it = mapNextTx.find(prevout);
    return it == mapNextTx.end() ? nullptr : it->second;
}

std::optional<CTxRelayPool::txiter> CTxRelayPool::GetIter(const uint256& txid) const
{
    auto it = mapTx.find(txid);
    if (it != mapTx.end()) return it;
    return std::nullopt;
}

CTxRelayPool::setEntries CTxRelayPool::GetIterSet(const std::set<Txid>& hashes) const
{
    CTxRelayPool::setEntries ret;
    for (const auto& h : hashes) {
        const auto mi = GetIter(h);
        if (mi) ret.insert(*mi);
    }
    return ret;
}

std::vector<CTxRelayPool::txiter> CTxRelayPool::GetIterVec(const std::vector<uint256>& txids) const
{
    AssertLockHeld(cs);
    std::vector<txiter> ret;
    ret.reserve(txids.size());
    for (const auto& txid : txids) {
        const auto it{GetIter(txid)};
        if (!it) return {};
        ret.push_back(*it);
    }
    return ret;
}

CCoinsViewRelayPool::CCoinsViewRelayPool(CCoinsView* baseIn, const CTxRelayPool& relaypoolIn) : CCoinsViewBacked(baseIn), relaypool(relaypoolIn) { }

std::optional<Coin> CCoinsViewRelayPool::GetCoin(const COutPoint& outpoint) const
{
    // Check to see if the inputs are made available by another tx in the package.
    // These Coins would not be available in the underlying CoinsView.
    if (auto it = m_temp_added.find(outpoint); it != m_temp_added.end()) {
        return it->second;
    }

    // If an entry in the relaypool exists, always return that one, as it's guaranteed to never
    // conflict with the underlying cache, and it cannot have pruned entries (as it contains full)
    // transactions. First checking the underlying cache risks returning a pruned entry instead.
    CTransactionRef ptx = relaypool.get(outpoint.hash);
    if (ptx) {
        if (outpoint.n < ptx->vout.size()) {
            Coin coin(ptx->vout[outpoint.n], RELAYPOOL_HEIGHT, false);
            m_non_base_coins.emplace(outpoint);
            return coin;
        }
        return std::nullopt;
    }
    return base->GetCoin(outpoint);
}

void CCoinsViewRelayPool::PackageAddTransaction(const CTransactionRef& tx)
{
    for (unsigned int n = 0; n < tx->vout.size(); ++n) {
        m_temp_added.emplace(COutPoint(tx->GetHash(), n), Coin(tx->vout[n], RELAYPOOL_HEIGHT, false));
        m_non_base_coins.emplace(tx->GetHash(), n);
    }
}
void CCoinsViewRelayPool::Reset()
{
    m_temp_added.clear();
    m_non_base_coins.clear();
}

size_t CTxRelayPool::DynamicMemoryUsage() const {
    LOCK(cs);
    // Estimate the overhead of mapTx to be 15 pointers + an allocation, as no exact formula for boost::multi_index_contained is implemented.
    return memusage::MallocUsage(sizeof(CTxRelayPoolEntry) + 15 * sizeof(void*)) * mapTx.size() + memusage::DynamicUsage(mapNextTx) + memusage::DynamicUsage(txns_randomized) + cachedInnerUsage;
}

void CTxRelayPool::RemoveUnbroadcastTx(const uint256& txid, const bool unchecked) {
    LOCK(cs);

    if (m_unbroadcast_txids.erase(txid))
    {
        LogDebug(HgLog::RELAYPOOL, "Removed %i from set of unbroadcast txns%s\n", txid.GetHex(), (unchecked ? " before confirmation that txn was sent out" : ""));
    }
}

void CTxRelayPool::RemoveStaged(setEntries &stage, bool updateDescendants, RelayPoolRemovalReason reason) {
    AssertLockHeld(cs);
    UpdateForRemoveFromRelayPool(stage, updateDescendants);
    for (txiter it : stage) {
        removeUnchecked(it, reason);
    }
}

int CTxRelayPool::Expire(std::chrono::seconds time)
{
    AssertLockHeld(cs);
    Assume(!m_have_changeset);
    indexed_transaction_set::index<entry_time>::type::iterator it = mapTx.get<entry_time>().begin();
    setEntries toremove;
    while (it != mapTx.get<entry_time>().end() && it->GetTime() < time) {
        toremove.insert(mapTx.project<0>(it));
        it++;
    }
    setEntries stage;
    for (txiter removeit : toremove) {
        CalculateDescendants(removeit, stage);
    }
    RemoveStaged(stage, false, RelayPoolRemovalReason::EXPIRY);
    return stage.size();
}

void CTxRelayPool::removeStaleAnchors(int tip_height, const Consensus::Params& params)
{
    AssertLockHeld(cs);
    Assume(!m_have_changeset);
    setEntries toremove;
    for (txiter it = mapTx.begin(); it != mapTx.end(); ++it) {
        // Coinbases never enter the relaypool; every entry has a real anchor height.
        const int64_t stale_after_height{
            int64_t{it->GetTx().nAnchorHeight} + params.nMaxAnchorAge};
        if (stale_after_height < int64_t{tip_height}) {
            toremove.insert(it);
        }
    }
    setEntries stage;
    for (txiter removeit : toremove) CalculateDescendants(removeit, stage);
    RemoveStaged(stage, false, RelayPoolRemovalReason::STALE_ANCHOR);
}

void CTxRelayPool::UpdateChild(txiter entry, txiter child, bool add)
{
    AssertLockHeld(cs);
    CTxRelayPoolEntry::Children s;
    if (add && entry->GetRelayPoolChildren().insert(*child).second) {
        cachedInnerUsage += memusage::IncrementalDynamicUsage(s);
    } else if (!add && entry->GetRelayPoolChildren().erase(*child)) {
        cachedInnerUsage -= memusage::IncrementalDynamicUsage(s);
    }
}

void CTxRelayPool::UpdateParent(txiter entry, txiter parent, bool add)
{
    AssertLockHeld(cs);
    CTxRelayPoolEntry::Parents s;
    if (add && entry->GetRelayPoolParents().insert(*parent).second) {
        cachedInnerUsage += memusage::IncrementalDynamicUsage(s);
    } else if (!add && entry->GetRelayPoolParents().erase(*parent)) {
        cachedInnerUsage -= memusage::IncrementalDynamicUsage(s);
    }
}


void CTxRelayPool::TrimToSize(size_t sizelimit, std::vector<COutPoint>* pvNoSpendsRemaining) {
    AssertLockHeld(cs);
    Assume(!m_have_changeset);

    unsigned nTxnRemoved = 0;
    while (!mapTx.empty() && DynamicMemoryUsage() > sizelimit) {
        // Evict the lowest surplus-work rate first. The by_txwork_rate index
        // is descending (begin() = highest rate), so the lowest-rate entry is the
        // last element. Per-tx individual scoring: a low-surplus parent still drags
        // its descendants out via CalculateDescendants (no orphaned children).
        //
        // Among entries at the SAME rate this takes the NEWEST, because the index
        // breaks ties oldest-first and we read it from the back. That is the decided
        // policy, not an accident of the comparator: on a feeless chain a full pool is
        // a pool full of floor-rate (surplus 0) txs, and refusing the newcomer is what
        // makes admission first-come-first-served. Note it is the exact reverse of the
        // mining tie-break, which takes the oldest of an equal-rate group first.
        // See CompareTxRelayPoolEntryByTxWorkRate.
        auto& rate_index = mapTx.get<txwork_rate>();
        txiter it = mapTx.project<0>(std::prev(rate_index.end()));

        setEntries stage;
        CalculateDescendants(it, stage);
        nTxnRemoved += stage.size();

        std::vector<CTransaction> txn;
        if (pvNoSpendsRemaining) {
            txn.reserve(stage.size());
            for (txiter iter : stage)
                txn.push_back(iter->GetTx());
        }
        RemoveStaged(stage, false, RelayPoolRemovalReason::SIZELIMIT);
        if (pvNoSpendsRemaining) {
            for (const CTransaction& tx : txn) {
                for (const CTxIn& txin : tx.vin) {
                    if (exists(GenTxid::Txid(txin.prevout.hash))) continue;
                    pvNoSpendsRemaining->push_back(txin.prevout);
                }
            }
        }
    }

    if (nTxnRemoved > 0) {
        LogDebug(HgLog::RELAYPOOL, "Removed %u txn by surplus-work eviction\n", nTxnRemoved);
    }
}

uint64_t CTxRelayPool::CalculateDescendantMaximum(txiter entry) const {
    // find parent with highest descendant count
    std::vector<txiter> candidates;
    setEntries counted;
    candidates.push_back(entry);
    uint64_t maximum = 0;
    while (candidates.size()) {
        txiter candidate = candidates.back();
        candidates.pop_back();
        if (!counted.insert(candidate).second) continue;
        const CTxRelayPoolEntry::Parents& parents = candidate->GetRelayPoolParentsConst();
        if (parents.size() == 0) {
            maximum = std::max(maximum, candidate->GetCountWithDescendants());
        } else {
            for (const CTxRelayPoolEntry& i : parents) {
                candidates.push_back(mapTx.iterator_to(i));
            }
        }
    }
    return maximum;
}

void CTxRelayPool::GetTransactionAncestry(const uint256& txid, size_t& ancestors, size_t& descendants, size_t* const ancestorsize) const {
    LOCK(cs);
    auto it = mapTx.find(txid);
    ancestors = descendants = 0;
    if (it != mapTx.end()) {
        ancestors = it->GetCountWithAncestors();
        if (ancestorsize) *ancestorsize = it->GetSizeWithAncestors();
        descendants = CalculateDescendantMaximum(it);
    }
}

bool CTxRelayPool::GetLoadTried() const
{
    LOCK(cs);
    return m_load_tried;
}

void CTxRelayPool::SetLoadTried(bool load_tried)
{
    LOCK(cs);
    m_load_tried = load_tried;
}

std::vector<CTxRelayPool::txiter> CTxRelayPool::GatherClusters(const std::vector<uint256>& txids) const
{
    AssertLockHeld(cs);
    std::vector<txiter> clustered_txs{GetIterVec(txids)};
    // Use epoch: visiting an entry means we have added it to the clustered_txs vector. It does not
    // necessarily mean the entry has been processed.
    WITH_FRESH_EPOCH(m_epoch);
    for (const auto& it : clustered_txs) {
        visited(it);
    }
    // i = index of where the list of entries to process starts
    for (size_t i{0}; i < clustered_txs.size(); ++i) {
        // DoS protection: if there are 500 or more entries to process, just quit.
        if (clustered_txs.size() > 500) return {};
        const txiter& tx_iter = clustered_txs.at(i);
        for (const auto& entries : {tx_iter->GetRelayPoolParentsConst(), tx_iter->GetRelayPoolChildrenConst()}) {
            for (const CTxRelayPoolEntry& entry : entries) {
                const auto entry_it = mapTx.iterator_to(entry);
                if (!visited(entry_it)) {
                    clustered_txs.push_back(entry_it);
                }
            }
        }
    }
    return clustered_txs;
}

CTxRelayPool::ChangeSet::TxHandle CTxRelayPool::ChangeSet::StageAddition(const CTransactionRef& tx, int64_t time, unsigned int entry_height, uint64_t entry_sequence, bool spends_coinbase, int64_t sigops_cost, LockPoints lp, arith_uint256 txwork_surplus)
{
    LOCK(m_pool->cs);
    Assume(m_to_add.find(tx->GetHash()) == m_to_add.end());
    auto newit = m_to_add.emplace(tx, time, entry_height, entry_sequence, spends_coinbase, sigops_cost, lp, txwork_surplus).first;

    m_entry_vec.push_back(newit);
    return newit;
}

void CTxRelayPool::ChangeSet::Apply()
{
    LOCK(m_pool->cs);
    m_pool->Apply(this);
    m_to_add.clear();
    m_to_remove.clear();
    m_entry_vec.clear();
    m_ancestors.clear();
}
