// Copyright (c) 2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_TEST_UTIL_TXRELAYPOOL_H
#define QUICKSILVER_TEST_UTIL_TXRELAYPOOL_H

#include <policy/packages.h>
#include <txrelaypool.h>
#include <util/time.h>

namespace node {
struct NodeContext;
}
struct PackageRelayPoolAcceptResult;

CTxRelayPool::Options RelayPoolOptionsForTest(const node::NodeContext& node);

struct TestRelayPoolEntryHelper {
    // Default values
    NodeSeconds time{};
    unsigned int nHeight{1};
    uint64_t m_sequence{0};
    bool spendsCoinbase{false};
    unsigned int sigOpCost{4};
    LockPoints lp;
    arith_uint256 txwork_surplus{0};

    CTxRelayPoolEntry FromTx(const CMutableTransaction& tx) const;
    CTxRelayPoolEntry FromTx(const CTransactionRef& tx) const;

    // Change the default value
    TestRelayPoolEntryHelper& Time(NodeSeconds tp) { time = tp; return *this; }
    TestRelayPoolEntryHelper& Height(unsigned int _height) { nHeight = _height; return *this; }
    TestRelayPoolEntryHelper& Sequence(uint64_t _seq) { m_sequence = _seq; return *this; }
    TestRelayPoolEntryHelper& SpendsCoinbase(bool _flag) { spendsCoinbase = _flag; return *this; }
    TestRelayPoolEntryHelper& SigOpsCost(unsigned int _sigopsCost) { sigOpCost = _sigopsCost; return *this; }
    TestRelayPoolEntryHelper& TxWorkSurplus(arith_uint256 _surplus) { txwork_surplus = _surplus; return *this; }
};

/** Check expected properties for every PackageRelayPoolAcceptResult, regardless of value. Returns
 * a string if an error occurs with error populated, nullopt otherwise. If relaypool is provided,
 * checks that the expected transactions are in relaypool (this should be set to nullptr for a test_accept).
*/
std::optional<std::string>  CheckPackageRelayPoolAcceptResult(const Package& txns,
                                                            const PackageRelayPoolAcceptResult& result,
                                                            bool expect_valid,
                                                            const CTxRelayPool* relaypool);

/** For every transaction in tx_pool, check TRUC invariants:
 * - a TRUC tx's ancestor count must be within TRUC_ANCESTOR_LIMIT
 * - a TRUC tx's descendant count must be within TRUC_DESCENDANT_LIMIT
 * - if a TRUC tx has ancestors, its sigop-adjusted vsize must be within TRUC_CHILD_MAX_VSIZE
 * - any non-TRUC tx must only have non-TRUC parents
 * - any TRUC tx must only have TRUC parents
 *   */
void CheckRelayPoolTRUCInvariants(const CTxRelayPool& tx_pool);

/** One-line wrapper for creating a relaypool changeset with a single transaction
 *  and applying it. */
void AddToRelayPool(CTxRelayPool& tx_pool, const CTxRelayPoolEntry& entry);

#endif // QUICKSILVER_TEST_UTIL_TXRELAYPOOL_H
