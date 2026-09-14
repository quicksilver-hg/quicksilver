// Copyright (c) 2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <vault/transaction.h>

#include <interfaces/chain.h>

using interfaces::FoundBlock;

namespace vault {
bool CVaultTx::IsEquivalentTo(const CVaultTx& _tx) const
{
        CMutableTransaction tx1 {*this->tx};
        CMutableTransaction tx2 {*_tx.tx};
        for (auto& txin : tx1.vin) txin.scriptSig = CScript();
        for (auto& txin : tx2.vin) txin.scriptSig = CScript();
        return CTransaction(tx1) == CTransaction(tx2);
}

bool CVaultTx::InRelayPool() const
{
    return state<TxStateInRelayPool>();
}

int64_t CVaultTx::GetTxTime() const
{
    int64_t n = nTimeSmart;
    return n ? n : nTimeReceived;
}

void CVaultTx::updateState(interfaces::Chain& chain)
{
    bool active;
    auto lookup_block = [&](const uint256& hash, int& height, TxState& state) {
        // If tx block (or conflicting block) was reorged out of chain
        // while the vault was shutdown, change tx status to UNCONFIRMED
        // and reset block height, hash, and index. ABANDONED tx don't have
        // associated blocks and don't need to be updated. The case where a
        // transaction was reorged out while online and then reconfirmed
        // while offline is covered by the rescan logic.
        if (!chain.findBlock(hash, FoundBlock().inActiveChain(active).height(height)) || !active) {
            state = TxStateInactive{};
        }
    };
    if (auto* conf = state<TxStateConfirmed>()) {
        lookup_block(conf->confirmed_block_hash, conf->confirmed_block_height, m_state);
    } else if (auto* conf = state<TxStateBlockConflicted>()) {
        lookup_block(conf->conflicting_block_hash, conf->conflicting_block_height, m_state);
    } else if (auto* unresolved = state<TxStateBlockUnresolved>()) {
        const uint256 block_hash{unresolved->block_hash};
        const int index{unresolved->index};
        int height{-1};
        if (!chain.findBlock(block_hash, FoundBlock().inActiveChain(active).height(height)) || !active) {
            m_state = TxStateInactive{};
        } else if (index >= 0) {
            m_state = TxStateConfirmed{block_hash, height, index};
        } else {
            m_state = TxStateBlockConflicted{block_hash, height};
        }
    }
}
} // namespace vault
