// Copyright (c) 2021-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_VAULT_RECEIVE_H
#define QUICKSILVER_VAULT_RECEIVE_H

#include <consensus/amount.h>
#include <vault/transaction.h>
#include <vault/types.h>
#include <vault/vault.h>

namespace vault {
isminetype InputIsMine(const CVault& vault, const CTxIn& txin) EXCLUSIVE_LOCKS_REQUIRED(vault.cs_vault);

CAmount OutputGetCredit(const CVault& vault, const CTxOut& txout, const isminefilter& filter);
CAmount TxGetCredit(const CVault& vault, const CTransaction& tx, const isminefilter& filter);

bool ScriptIsChange(const CVault& vault, const CScript& script) EXCLUSIVE_LOCKS_REQUIRED(vault.cs_vault);
bool OutputIsChange(const CVault& vault, const CTxOut& txout) EXCLUSIVE_LOCKS_REQUIRED(vault.cs_vault);
CAmount OutputGetChange(const CVault& vault, const CTxOut& txout) EXCLUSIVE_LOCKS_REQUIRED(vault.cs_vault);
CAmount TxGetChange(const CVault& vault, const CTransaction& tx);

CAmount CachedTxGetCredit(const CVault& vault, const CVaultTx& wtx, const isminefilter& filter)
    EXCLUSIVE_LOCKS_REQUIRED(vault.cs_vault);
//! filter decides which addresses will count towards the debit
CAmount CachedTxGetDebit(const CVault& vault, const CVaultTx& wtx, const isminefilter& filter);
CAmount CachedTxGetChange(const CVault& vault, const CVaultTx& wtx);
CAmount CachedTxGetImmatureCredit(const CVault& vault, const CVaultTx& wtx, const isminefilter& filter)
    EXCLUSIVE_LOCKS_REQUIRED(vault.cs_vault);
CAmount CachedTxGetAvailableCredit(const CVault& vault, const CVaultTx& wtx, const isminefilter& filter = ISMINE_SPENDABLE)
    EXCLUSIVE_LOCKS_REQUIRED(vault.cs_vault);
struct COutputEntry
{
    CTxDestination destination;
    CAmount amount;
    int vout;
};
void CachedTxGetAmounts(const CVault& vault, const CVaultTx& wtx,
                        std::list<COutputEntry>& listReceived,
                        std::list<COutputEntry>& listSent,
                        const isminefilter& filter,
                        bool include_change);
bool CachedTxIsFromMe(const CVault& vault, const CVaultTx& wtx, const isminefilter& filter);
bool CachedTxIsTrusted(const CVault& vault, const CVaultTx& wtx, std::set<uint256>& trusted_parents) EXCLUSIVE_LOCKS_REQUIRED(vault.cs_vault);
bool CachedTxIsTrusted(const CVault& vault, const CVaultTx& wtx);

struct Balance {
    CAmount m_mine_trusted{0};           //!< Trusted and selectable, at depth=GetBalance.min_depth or more
    CAmount m_mine_untrusted_pending{0}; //!< Untrusted, but in relaypool (pending)
    CAmount m_mine_immature{0};          //!< Immature coinbases in the main chain
    CAmount m_mine_delegated{0};         //!< Trusted, but handed to an agent and therefore not selectable
};
Balance GetBalance(const CVault& vault, int min_depth = 0, bool avoid_reuse = true);

std::map<CTxDestination, CAmount> GetAddressBalances(const CVault& vault);
std::set<std::set<CTxDestination>> GetAddressGroupings(const CVault& vault) EXCLUSIVE_LOCKS_REQUIRED(vault.cs_vault);
} // namespace vault

#endif // QUICKSILVER_VAULT_RECEIVE_H
