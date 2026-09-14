// Copyright (c) 2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_TEST_FUZZ_UTIL_RELAYPOOL_H
#define QUICKSILVER_TEST_FUZZ_UTIL_RELAYPOOL_H

#include <kernel/relaypool_entry.h>
#include <validation.h>

class CTransaction;
class CTxRelayPool;
class FuzzedDataProvider;

class DummyChainState final : public Chainstate
{
public:
    void SetRelayPool(CTxRelayPool* relaypool)
    {
        m_relaypool = relaypool;
    }
};

[[nodiscard]] CTxRelayPoolEntry ConsumeTxRelayPoolEntry(FuzzedDataProvider& fuzzed_data_provider, const CTransaction& tx) noexcept;

#endif // QUICKSILVER_TEST_FUZZ_UTIL_RELAYPOOL_H
