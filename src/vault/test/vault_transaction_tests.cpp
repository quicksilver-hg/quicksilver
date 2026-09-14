// Copyright (c) 2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <vault/transaction.h>

#include <primitives/transaction.h>
#include <streams.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <vault/test/vault_test_fixture.h>

#include <boost/test/unit_test.hpp>

#include <string>

namespace vault {
BOOST_FIXTURE_TEST_SUITE(vault_transaction_tests, VaultTestingSetup)

BOOST_AUTO_TEST_CASE(roundtrip)
{
    for (uint8_t hash = 0; hash < 5; ++hash) {
        for (int index = -2; index < 3; ++index) {
            TxState state = TxStateInterpretSerialized(TxStateUnrecognized{uint256{hash}, index});
            BOOST_CHECK_EQUAL(TxStateSerializedBlockHash(state), uint256{hash});
            BOOST_CHECK_EQUAL(TxStateSerializedIndex(state), index);
        }
    }
}

//! Expected vault.dat bytes: tx, state hash, state index, mapValue, order form, timestamps, fFromMe.
//! No vMerkleBranch, vtxPrev, fSpent, or empty "fromaccount" pad.
static DataStream ExpectedVaultTxDisk(const CVaultTx& wtx)
{
    mapValue_t stored = wtx.mapValue;
    if (wtx.nOrderPos != -1) {
        stored["n"] = util::ToString(wtx.nOrderPos);
    }
    if (wtx.nTimeSmart) {
        stored["timesmart"] = strprintf("%u", wtx.nTimeSmart);
    }
    DataStream expected;
    expected << TX_WITH_WITNESS(wtx.tx)
             << TxStateSerializedBlockHash(wtx.m_state)
             << TxStateSerializedIndex(wtx.m_state)
             << stored
             << wtx.vOrderForm
             << wtx.nTimeReceived
             << wtx.fFromMe;
    return expected;
}

static void FillSampleMetadata(CVaultTx& wtx)
{
    wtx.mapValue["comment"] = "memo";
    wtx.mapValue["to"] = "alice";
    wtx.vOrderForm.emplace_back("Message", "hi");
    wtx.nTimeReceived = 1'700'000'000;
    wtx.nTimeSmart = 1'700'000'100;
    wtx.fFromMe = true;
    wtx.nOrderPos = 42;
}

static void CheckDiskRoundTrip(const CVaultTx& wtx)
{
    DataStream actual;
    actual << wtx;
    BOOST_CHECK_EQUAL(HexStr(actual), HexStr(ExpectedVaultTxDisk(wtx)));
    BOOST_CHECK_EQUAL(HexStr(actual).find(HexStr(std::string{"fromaccount"})), std::string::npos);

    CVaultTx loaded{MakeTransactionRef(CMutableTransaction{}), TxStateInactive{}};
    actual >> loaded;

    BOOST_CHECK(*loaded.tx == *wtx.tx);
    BOOST_CHECK_EQUAL(TxStateString(loaded.m_state), TxStateString(wtx.m_state));
    BOOST_CHECK(loaded.mapValue == wtx.mapValue);
    BOOST_CHECK(loaded.vOrderForm == wtx.vOrderForm);
    BOOST_CHECK_EQUAL(loaded.nTimeReceived, wtx.nTimeReceived);
    BOOST_CHECK_EQUAL(loaded.nTimeSmart, wtx.nTimeSmart);
    BOOST_CHECK_EQUAL(loaded.fFromMe, wtx.fFromMe);
    BOOST_CHECK_EQUAL(loaded.nOrderPos, wtx.nOrderPos);
    BOOST_CHECK(!loaded.mapValue.count("fromaccount"));
    BOOST_CHECK(!loaded.mapValue.count("spent"));
    BOOST_CHECK(!loaded.mapValue.count("n"));
    BOOST_CHECK(!loaded.mapValue.count("timesmart"));
}

BOOST_AUTO_TEST_CASE(disk_format_omits_merkle_compat_padding)
{
    CMutableTransaction mtx;
    mtx.version = 2;
    mtx.nLockTime = 777;

    CVaultTx inactive{MakeTransactionRef(mtx), TxStateInactive{}};
    FillSampleMetadata(inactive);
    CheckDiskRoundTrip(inactive);

    CVaultTx abandoned{MakeTransactionRef(mtx), TxStateInactive{/*abandoned=*/true}};
    FillSampleMetadata(abandoned);
    CheckDiskRoundTrip(abandoned);

    CVaultTx unresolved_confirmed{MakeTransactionRef(mtx), TxStateBlockUnresolved{uint256{3}, /*index=*/5}};
    FillSampleMetadata(unresolved_confirmed);
    CheckDiskRoundTrip(unresolved_confirmed);

    CVaultTx unresolved_conflicted{MakeTransactionRef(mtx), TxStateBlockUnresolved{uint256{4}, /*index=*/-1}};
    FillSampleMetadata(unresolved_conflicted);
    CheckDiskRoundTrip(unresolved_conflicted);

    CVaultTx defaults{MakeTransactionRef(mtx), TxStateInactive{}};
    CheckDiskRoundTrip(defaults);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace vault
