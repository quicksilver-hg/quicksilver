// Copyright (c) 2021-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <addresstype.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <key_io.h>
#include <script/solver.h>
#include <vault/receive.h>
#include <vault/transaction.h>
#include <vault/vault.h>

namespace vault {
isminetype InputIsMine(const CVault& vault, const CTxIn& txin)
{
    AssertLockHeld(vault.cs_vault);
    const CVaultTx* prev = vault.GetVaultTx(txin.prevout.hash);
    if (prev && txin.prevout.n < prev->tx->vout.size()) {
        return vault.IsMine(prev->tx->vout[txin.prevout.n]);
    }
    return ISMINE_NO;
}

CAmount OutputGetCredit(const CVault& vault, const CTxOut& txout, const isminefilter& filter)
{
    if (!MoneyRange(txout.nValue))
        throw std::runtime_error(std::string(__func__) + ": value out of range");
    LOCK(vault.cs_vault);
    return ((vault.IsMine(txout) & filter) ? txout.nValue : 0);
}

CAmount TxGetCredit(const CVault& vault, const CTransaction& tx, const isminefilter& filter)
{
    CAmount nCredit = 0;
    for (const CTxOut& txout : tx.vout)
    {
        nCredit += OutputGetCredit(vault, txout, filter);
        if (!MoneyRange(nCredit))
            throw std::runtime_error(std::string(__func__) + ": value out of range");
    }
    return nCredit;
}

bool ScriptIsChange(const CVault& vault, const CScript& script)
{
    // TODO: fix handling of 'change' outputs. The assumption is that any
    // payment to a script that is ours, but is not in the address book
    // is change. That assumption is likely to break when we implement multisignature
    // vaults that return change back into a multi-signature-protected address;
    // a better way of identifying which outputs are 'the send' and which are
    // 'the change' will need to be implemented (maybe extend CVaultTx to remember
    // which output, if any, was change).
    AssertLockHeld(vault.cs_vault);
    if (vault.IsMine(script))
    {
        CTxDestination address;
        if (!ExtractDestination(script, address))
            return true;
        if (!vault.FindAddressBookEntry(address)) {
            return true;
        }
    }
    return false;
}

bool OutputIsChange(const CVault& vault, const CTxOut& txout)
{
    return ScriptIsChange(vault, txout.scriptPubKey);
}

CAmount OutputGetChange(const CVault& vault, const CTxOut& txout)
{
    AssertLockHeld(vault.cs_vault);
    if (!MoneyRange(txout.nValue))
        throw std::runtime_error(std::string(__func__) + ": value out of range");
    return (OutputIsChange(vault, txout) ? txout.nValue : 0);
}

CAmount TxGetChange(const CVault& vault, const CTransaction& tx)
{
    LOCK(vault.cs_vault);
    CAmount nChange = 0;
    for (const CTxOut& txout : tx.vout)
    {
        nChange += OutputGetChange(vault, txout);
        if (!MoneyRange(nChange))
            throw std::runtime_error(std::string(__func__) + ": value out of range");
    }
    return nChange;
}

static CAmount GetCachableAmount(const CVault& vault, const CVaultTx& wtx, CVaultTx::AmountType type, const isminefilter& filter)
{
    auto& amount = wtx.m_amounts[type];
    if (!amount.m_cached[filter]) {
        amount.Set(filter, type == CVaultTx::DEBIT ? vault.GetDebit(*wtx.tx, filter) : TxGetCredit(vault, *wtx.tx, filter));
        wtx.m_is_cache_empty = false;
    }
    return amount.m_value[filter];
}

CAmount CachedTxGetCredit(const CVault& vault, const CVaultTx& wtx, const isminefilter& filter)
{
    AssertLockHeld(vault.cs_vault);

    // Must wait until coinbase is safely deep enough in the chain before valuing it
    if (vault.IsTxImmatureCoinBase(wtx))
        return 0;

    CAmount credit = 0;
    const isminefilter get_amount_filter{filter & ISMINE_SPENDABLE};
    if (get_amount_filter) {
        // GetBalance can assume transactions in mapVault won't change
        credit += GetCachableAmount(vault, wtx, CVaultTx::CREDIT, get_amount_filter);
    }
    return credit;
}

CAmount CachedTxGetDebit(const CVault& vault, const CVaultTx& wtx, const isminefilter& filter)
{
    if (wtx.tx->vin.empty())
        return 0;

    CAmount debit = 0;
    const isminefilter get_amount_filter{filter & ISMINE_SPENDABLE};
    if (get_amount_filter) {
        debit += GetCachableAmount(vault, wtx, CVaultTx::DEBIT, get_amount_filter);
    }
    return debit;
}

CAmount CachedTxGetChange(const CVault& vault, const CVaultTx& wtx)
{
    if (wtx.fChangeCached)
        return wtx.nChangeCached;
    wtx.nChangeCached = TxGetChange(vault, *wtx.tx);
    wtx.fChangeCached = true;
    return wtx.nChangeCached;
}

CAmount CachedTxGetImmatureCredit(const CVault& vault, const CVaultTx& wtx, const isminefilter& filter)
{
    AssertLockHeld(vault.cs_vault);

    if (vault.IsTxImmatureCoinBase(wtx) && wtx.isConfirmed()) {
        return GetCachableAmount(vault, wtx, CVaultTx::IMMATURE_CREDIT, filter);
    }

    return 0;
}

CAmount CachedTxGetAvailableCredit(const CVault& vault, const CVaultTx& wtx, const isminefilter& filter)
{
    AssertLockHeld(vault.cs_vault);

    // A filter that selects no mine type can only ever total zero, so there is nothing worth caching.
    bool allow_cache = (filter & ISMINE_SPENDABLE);

    // Must wait until coinbase is safely deep enough in the chain before valuing it
    if (vault.IsTxImmatureCoinBase(wtx))
        return 0;

    if (allow_cache && wtx.m_amounts[CVaultTx::AVAILABLE_CREDIT].m_cached[filter]) {
        return wtx.m_amounts[CVaultTx::AVAILABLE_CREDIT].m_value[filter];
    }

    bool allow_used_addresses = (filter & ISMINE_USED) || !vault.IsVaultFlagSet(VAULT_FLAG_AVOID_REUSE);
    CAmount nCredit = 0;
    Txid hashTx = wtx.GetHash();
    for (unsigned int i = 0; i < wtx.tx->vout.size(); i++) {
        const CTxOut& txout = wtx.tx->vout[i];
        if (!vault.IsSpent(COutPoint(hashTx, i)) && (allow_used_addresses || !vault.IsSpentKey(txout.scriptPubKey))) {
            nCredit += OutputGetCredit(vault, txout, filter);
            if (!MoneyRange(nCredit))
                throw std::runtime_error(std::string(__func__) + " : value out of range");
        }
    }

    if (allow_cache) {
        wtx.m_amounts[CVaultTx::AVAILABLE_CREDIT].Set(filter, nCredit);
        wtx.m_is_cache_empty = false;
    }

    return nCredit;
}

void CachedTxGetAmounts(const CVault& vault, const CVaultTx& wtx,
                  std::list<COutputEntry>& listReceived,
                  std::list<COutputEntry>& listSent, const isminefilter& filter,
                  bool include_change)
{
    listReceived.clear();
    listSent.clear();

    CAmount nDebit = CachedTxGetDebit(vault, wtx, filter);

    LOCK(vault.cs_vault);
    // Sent/received.
    for (unsigned int i = 0; i < wtx.tx->vout.size(); ++i)
    {
        const CTxOut& txout = wtx.tx->vout[i];
        isminetype fIsMine = vault.IsMine(txout);
        // Only need to handle txouts if AT LEAST one of these is true:
        //   1) they debit from us (sent)
        //   2) the output is to us (received)
        if (nDebit > 0)
        {
            if (!include_change && OutputIsChange(vault, txout))
                continue;
        }
        else if (!(fIsMine & filter))
            continue;

        // In either case, we need to get the destination address
        CTxDestination address;

        if (!ExtractDestination(txout.scriptPubKey, address) && !txout.scriptPubKey.IsUnspendable())
        {
            vault.VaultLogPrintf("CVaultTx::GetAmounts: Unknown transaction type found, txid %s\n",
                                    wtx.GetHash().ToString());
            address = CNoDestination();
        }

        COutputEntry output = {address, txout.nValue, (int)i};

        // If we are debited by the transaction, add the output as a "sent" entry
        if (nDebit > 0)
            listSent.push_back(output);

        // If we are receiving the output, add it as a "received" entry
        if (fIsMine & filter)
            listReceived.push_back(output);
    }

}

bool CachedTxIsFromMe(const CVault& vault, const CVaultTx& wtx, const isminefilter& filter)
{
    return (CachedTxGetDebit(vault, wtx, filter) > 0);
}

// NOLINTNEXTLINE(misc-no-recursion)
bool CachedTxIsTrusted(const CVault& vault, const CVaultTx& wtx, std::set<uint256>& trusted_parents)
{
    AssertLockHeld(vault.cs_vault);
    if (wtx.isConfirmed()) return true;
    if (wtx.isBlockConflicted()) return false;
    // using wtx's cached debit
    if (!vault.m_spend_zero_conf_change || !CachedTxIsFromMe(vault, wtx, ISMINE_SPENDABLE)) return false;

    // Don't trust unconfirmed transactions from us unless they are in the relaypool.
    if (!wtx.InRelayPool()) return false;

    // Trusted if all inputs are from us and are in the relaypool:
    for (const CTxIn& txin : wtx.tx->vin)
    {
        // Transactions not sent by us: not trusted
        const CVaultTx* parent = vault.GetVaultTx(txin.prevout.hash);
        if (parent == nullptr) return false;
        const CTxOut& parentOut = parent->tx->vout[txin.prevout.n];
        // Check that this specific input being spent is trusted
        if (vault.IsMine(parentOut) != ISMINE_SPENDABLE) return false;
        // If we've already trusted this parent, continue
        if (trusted_parents.count(parent->GetHash())) continue;
        // Recurse to check that the parent is also trusted
        if (!CachedTxIsTrusted(vault, *parent, trusted_parents)) return false;
        trusted_parents.insert(parent->GetHash());
    }
    return true;
}

bool CachedTxIsTrusted(const CVault& vault, const CVaultTx& wtx)
{
    std::set<uint256> trusted_parents;
    LOCK(vault.cs_vault);
    return CachedTxIsTrusted(vault, wtx, trusted_parents);
}

//! The scripts of every agent funding address this vault has reserved.
static std::set<CScript> AgentFundingScripts(const CVault& vault) EXCLUSIVE_LOCKS_REQUIRED(vault.cs_vault)
{
    AssertLockHeld(vault.cs_vault);
    std::set<CScript> scripts;
    for (const AgentAllotmentRecord& record : vault.ListAgentAllotmentRecords()) {
        const CTxDestination dest{DecodeDestination(record.funding_address)};
        if (IsValidDestination(dest)) scripts.insert(GetScriptForDestination(dest));
    }
    return scripts;
}

//! The part of a transaction's available credit that has been handed to an agent.
//!
//! Every condition here mirrors CachedTxGetAvailableCredit, because the result is
//! subtracted from it: an output counted as delegated but not as available would make the
//! spendable balance too small, and one counted as neither would vanish from both.
static CAmount TxGetDelegatedCredit(const CVault& vault, const CVaultTx& wtx, const std::set<CScript>& funding_scripts, const isminefilter& filter)
    EXCLUSIVE_LOCKS_REQUIRED(vault.cs_vault)
{
    AssertLockHeld(vault.cs_vault);
    if (vault.IsTxImmatureCoinBase(wtx)) return 0;

    const bool allow_used_addresses{(filter & ISMINE_USED) || !vault.IsVaultFlagSet(VAULT_FLAG_AVOID_REUSE)};
    CAmount delegated{0};
    const Txid hashTx{wtx.GetHash()};
    for (unsigned int i = 0; i < wtx.tx->vout.size(); ++i) {
        const CTxOut& txout = wtx.tx->vout[i];
        if (!funding_scripts.count(txout.scriptPubKey)) continue;
        const COutPoint outpoint{hashTx, i};
        // The export locks what it hands over, so the lock is what distinguishes an output
        // the agent controls from one that merely sits at its funding address.
        if (!vault.IsLockedCoin(outpoint)) continue;
        if (vault.IsSpent(outpoint)) continue;
        if (!allow_used_addresses && vault.IsSpentKey(txout.scriptPubKey)) continue;
        delegated += OutputGetCredit(vault, txout, filter);
    }
    return delegated;
}

Balance GetBalance(const CVault& vault, const int min_depth, bool avoid_reuse)
{
    Balance ret;
    isminefilter reuse_filter = avoid_reuse ? ISMINE_NO : ISMINE_USED;
    {
        LOCK(vault.cs_vault);
        // Reading the agent records touches the database, and an agent's outputs are locked
        // when its bundle is exported. Nothing locked therefore means nothing delegated,
        // which spares the read for every vault that has never set an agent up.
        std::vector<COutPoint> locked_outpoints;
        vault.ListLockedCoins(locked_outpoints);
        const std::set<CScript> funding_scripts{locked_outpoints.empty() ? std::set<CScript>{} : AgentFundingScripts(vault)};

        std::set<uint256> trusted_parents;
        for (const auto& entry : vault.mapVault)
        {
            const CVaultTx& wtx = entry.second;
            const bool is_trusted{CachedTxIsTrusted(vault, wtx, trusted_parents)};
            const int tx_depth{vault.GetTxDepthInMainChain(wtx)};
            const CAmount tx_credit_mine{CachedTxGetAvailableCredit(vault, wtx, ISMINE_SPENDABLE | reuse_filter)};
            // Coins handed to an agent still belong to this vault -- the parent key can
            // sweep them back -- but coin selection cannot reach them. Reporting them apart
            // from the spendable balance keeps the figure on screen equal to the figure a
            // transfer can actually spend. See doc/design/agent-client.md.
            const CAmount tx_delegated{funding_scripts.empty() ? 0 : TxGetDelegatedCredit(vault, wtx, funding_scripts, ISMINE_SPENDABLE | reuse_filter)};
            if (is_trusted && tx_depth >= min_depth) {
                ret.m_mine_trusted += tx_credit_mine - tx_delegated;
                ret.m_mine_delegated += tx_delegated;
            }
            if (!is_trusted && tx_depth == 0 && wtx.InRelayPool()) {
                ret.m_mine_untrusted_pending += tx_credit_mine;
            }
            ret.m_mine_immature += CachedTxGetImmatureCredit(vault, wtx, ISMINE_SPENDABLE);
        }
    }
    return ret;
}

std::map<CTxDestination, CAmount> GetAddressBalances(const CVault& vault)
{
    std::map<CTxDestination, CAmount> balances;

    {
        LOCK(vault.cs_vault);
        std::set<uint256> trusted_parents;
        for (const auto& vaultEntry : vault.mapVault)
        {
            const CVaultTx& wtx = vaultEntry.second;

            if (!CachedTxIsTrusted(vault, wtx, trusted_parents))
                continue;

            if (vault.IsTxImmatureCoinBase(wtx))
                continue;

            int nDepth = vault.GetTxDepthInMainChain(wtx);
            if (nDepth < (CachedTxIsFromMe(vault, wtx, ISMINE_SPENDABLE) ? 0 : 1))
                continue;

            for (unsigned int i = 0; i < wtx.tx->vout.size(); i++) {
                const auto& output = wtx.tx->vout[i];
                CTxDestination addr;
                if (!vault.IsMine(output))
                    continue;
                if(!ExtractDestination(output.scriptPubKey, addr))
                    continue;

                CAmount n = vault.IsSpent(COutPoint(Txid::FromUint256(vaultEntry.first), i)) ? 0 : output.nValue;
                balances[addr] += n;
            }
        }
    }

    return balances;
}

std::set< std::set<CTxDestination> > GetAddressGroupings(const CVault& vault)
{
    AssertLockHeld(vault.cs_vault);
    std::set< std::set<CTxDestination> > groupings;
    std::set<CTxDestination> grouping;

    for (const auto& vaultEntry : vault.mapVault)
    {
        const CVaultTx& wtx = vaultEntry.second;

        if (wtx.tx->vin.size() > 0)
        {
            bool any_mine = false;
            // group all input addresses with each other
            for (const CTxIn& txin : wtx.tx->vin)
            {
                CTxDestination address;
                if(!InputIsMine(vault, txin)) /* If this input isn't mine, ignore it */
                    continue;
                if(!ExtractDestination(vault.mapVault.at(txin.prevout.hash).tx->vout[txin.prevout.n].scriptPubKey, address))
                    continue;
                grouping.insert(address);
                any_mine = true;
            }

            // group change with input addresses
            if (any_mine)
            {
               for (const CTxOut& txout : wtx.tx->vout)
                   if (OutputIsChange(vault, txout))
                   {
                       CTxDestination txoutAddr;
                       if(!ExtractDestination(txout.scriptPubKey, txoutAddr))
                           continue;
                       grouping.insert(txoutAddr);
                   }
            }
            if (grouping.size() > 0)
            {
                groupings.insert(grouping);
                grouping.clear();
            }
        }

        // group lone addrs by themselves
        for (const auto& txout : wtx.tx->vout)
            if (vault.IsMine(txout))
            {
                CTxDestination address;
                if(!ExtractDestination(txout.scriptPubKey, address))
                    continue;
                grouping.insert(address);
                groupings.insert(grouping);
                grouping.clear();
            }
    }

    std::set< std::set<CTxDestination>* > uniqueGroupings; // a set of pointers to groups of addresses
    std::map< CTxDestination, std::set<CTxDestination>* > setmap;  // map addresses to the unique group containing it
    for (const std::set<CTxDestination>& _grouping : groupings)
    {
        // make a set of all the groups hit by this new group
        std::set< std::set<CTxDestination>* > hits;
        std::map< CTxDestination, std::set<CTxDestination>* >::iterator it;
        for (const CTxDestination& address : _grouping)
            if ((it = setmap.find(address)) != setmap.end())
                hits.insert((*it).second);

        // merge all hit groups into a new single group and delete old groups
        std::set<CTxDestination>* merged = new std::set<CTxDestination>(_grouping);
        for (std::set<CTxDestination>* hit : hits)
        {
            merged->insert(hit->begin(), hit->end());
            uniqueGroupings.erase(hit);
            delete hit;
        }
        uniqueGroupings.insert(merged);

        // update setmap
        for (const CTxDestination& element : *merged)
            setmap[element] = merged;
    }

    std::set< std::set<CTxDestination> > ret;
    for (const std::set<CTxDestination>* uniqueGrouping : uniqueGroupings)
    {
        ret.insert(*uniqueGrouping);
        delete uniqueGrouping;
    }

    return ret;
}
} // namespace vault
