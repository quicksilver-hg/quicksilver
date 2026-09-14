// Copyright (c) 2021-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <crypto/cuckatoo/cuckatoo.h>
#include <key_io.h>
#include <policy/packages.h>
#include <pow.h>
#include <policy/policy.h>
#include <policy/replacement.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <serialize.h>
#include <streams.h>
#include <test/util/random.h>
#include <test/util/script.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>
#include <test/util/txrelaypool.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <string>
#include <vector>

using namespace util::hex_literals;

struct TxPackageTest : TestChain100Setup {
    explicit TxPackageTest(TestOpts opts = {})
        : TestChain100Setup{ChainType::SANDBOX, std::move(opts)}
    {
        // Quicksilver is feeless (#5b) and every spend carries a per-tx Cuckatoo proof
        // (#5c-1). These package tests submit many txs through ProcessNewPackage (the real
        // acceptance path), so opt into trivial-but-real per-tx PoW: the cycle solve is
        // skipped, but the per-tx target + anchor checks still apply (see ProveTxPow). This
        // matches the miner_tests / validation_block_tests mining fixtures.
        const_cast<Consensus::Params&>(Params().GetConsensus()).fTxPowNoCycle = true;
    }

    // Anchor `tx` to the active tip and grind a cheap Cuckatoo cycle that meets the per-tx
    // target, so the tx passes CheckTxProofOfWork. The proof tail is excluded from the
    // sighash, so this is safe to call AFTER signing without invalidating signatures.
    void ProveTxPow(CMutableTransaction& tx) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        const CBlockIndex* tip{m_node.chainman->ActiveChain().Tip()};
        tx.nAnchorHeight = tip->nHeight;
        const uint256 target{GetTxPowTarget(Params().GetConsensus(), tip, CTransaction(tx))};
        for (uint32_t i = 0; i < tx.nCycle.size(); ++i) tx.nCycle[i] = i + 1;
        while (UintToArith256(cuckatoo::CuckatooProofHash(tx.nCycle)) > UintToArith256(target)) {
            tx.nCycle[0] += static_cast<uint32_t>(tx.nCycle.size());
        }
    }

    // Build a feeless (in == out) tx spending `input_tx`:`vout` to a single output paying the
    // FULL input value to `out_spk`, signed by `signing_key`, with a valid anchored per-tx
    // proof. Feeless means there is no fee to assert on; the per-tx PoW replaces the fee as
    // the spam/ordering toll.
    CTransactionRef MakeFeelessProvenTx(const CTransactionRef& input_tx, uint32_t vout,
                                        const CKey& signing_key, const CScript& out_spk,
                                        int input_height = 0) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        const CTxOut out{input_tx->vout[vout].nValue, out_spk}; // in == out
        CMutableTransaction mtx = CreateValidTransaction(
            /*input_transactions=*/{input_tx}, /*inputs=*/{COutPoint(input_tx->GetHash(), vout)},
            /*input_height=*/input_height, /*input_signing_keys=*/{signing_key},
            /*outputs=*/{out});
        ProveTxPow(mtx);
        return MakeTransactionRef(mtx);
    }

    // Like MakeFeelessProvenTx, but grinds a strictly stronger proof (actual work >= 16x the
    // per-tx target), giving the tx a high surplus so it deterministically out-ranks a
    // bare-proof conflict under #6 replace-by-surplus-work. Used to pin the incumbent so a
    // lower-surplus replacement is reliably rejected.
    CTransactionRef MakeFeelessHighSurplusTx(const CTransactionRef& input_tx, uint32_t vout,
                                             const CKey& signing_key, const CScript& out_spk,
                                             int input_height = 0) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        const CTxOut out{input_tx->vout[vout].nValue, out_spk}; // in == out
        CMutableTransaction mtx = CreateValidTransaction(
            /*input_transactions=*/{input_tx}, /*inputs=*/{COutPoint(input_tx->GetHash(), vout)},
            /*input_height=*/input_height, /*input_signing_keys=*/{signing_key},
            /*outputs=*/{out});
        const CBlockIndex* tip{m_node.chainman->ActiveChain().Tip()};
        mtx.nAnchorHeight = tip->nHeight;
        // A proof hash below target/16 means actual work (2^256/(hash+1)) exceeds the
        // required work by >=16x, so the surplus is ~15x the required floor.
        const arith_uint256 strong{UintToArith256(GetTxPowTarget(Params().GetConsensus(), tip, CTransaction(mtx))) >> 4};
        for (uint32_t i = 0; i < mtx.nCycle.size(); ++i) mtx.nCycle[i] = i + 1;
        while (UintToArith256(cuckatoo::CuckatooProofHash(mtx.nCycle)) > strong) {
            mtx.nCycle[0] += static_cast<uint32_t>(mtx.nCycle.size());
        }
        return MakeTransactionRef(mtx);
    }

    // Build a feeless tx that FAILS its own per-tx PoW gate: a stale anchor (height 0, far
    // outside the recency window at a matured-chain tip) is rejected by CheckTxProofOfWork
    // with "bad-txns-pow-anchor", before the cycle is even examined. Used to show per-tx PoW
    // validity is non-aggregatable — a high-surplus child cannot rescue such a parent, and a
    // valid parent is unaffected by such a child (no package rescue in either direction).
    CTransactionRef MakeFeelessStaleAnchorTx(const CTransactionRef& input_tx, uint32_t vout,
                                             const CKey& signing_key, const CScript& out_spk,
                                             int input_height = 0) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        const CTxOut out{input_tx->vout[vout].nValue, out_spk}; // in == out
        CMutableTransaction mtx = CreateValidTransaction(
            /*input_transactions=*/{input_tx}, /*inputs=*/{COutPoint(input_tx->GetHash(), vout)},
            /*input_height=*/input_height, /*input_signing_keys=*/{signing_key},
            /*outputs=*/{out});
        mtx.nAnchorHeight = 0; // stale: outside the per-tx anchor recency window
        return MakeTransactionRef(mtx);
    }

// Create placeholder transactions that have no meaning.
inline CTransactionRef create_placeholder_tx(size_t num_inputs, size_t num_outputs)
{
    CMutableTransaction mtx = CMutableTransaction();
    mtx.vin.resize(num_inputs);
    mtx.vout.resize(num_outputs);
    auto random_script = CScript() << ToByteVector(m_rng.rand256()) << ToByteVector(m_rng.rand256());
    for (size_t i{0}; i < num_inputs; ++i) {
        mtx.vin[i].prevout.hash = Txid::FromUint256(m_rng.rand256());
        mtx.vin[i].prevout.n = 0;
        mtx.vin[i].scriptSig = random_script;
    }
    for (size_t o{0}; o < num_outputs; ++o) {
        mtx.vout[o].nValue = 1 * CENT;
        mtx.vout[o].scriptPubKey = random_script;
    }
    return MakeTransactionRef(mtx);
}
}; // struct TxPackageTest

BOOST_FIXTURE_TEST_SUITE(txpackage_tests, TxPackageTest)

BOOST_AUTO_TEST_CASE(package_hash_tests)
{
    // Random real segwit transaction
    DataStream stream_1{
        "02000000000101964b8aa63509579ca6086e6012eeaa4c2f4dd1e283da29b67c8eea38b3c6fd220000000000fdffffff0294c618000000000017a9145afbbb42f4e83312666d0697f9e66259912ecde38768fa2c0000000000160014897388a0889390fd0e153a22bb2cf9d8f019faf50247304402200547406380719f84d68cf4e96cc3e4a1688309ef475b150be2b471c70ea562aa02206d255f5acc40fd95981874d77201d2eb07883657ce1c796513f32b6079545cdf0121023ae77335cefcb5ab4c1dc1fb0d2acfece184e593727d7d5906c78e564c7c11d125cf0c000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"_hex,
    };
    CTransaction tx_1(deserialize, TX_WITH_WITNESS, stream_1);
    CTransactionRef ptx_1{MakeTransactionRef(tx_1)};

    // Random real nonsegwit transaction
    DataStream stream_2{
        "01000000010b26e9b7735eb6aabdf358bab62f9816a21ba9ebdb719d5299e88607d722c190000000008b4830450220070aca44506c5cef3a16ed519d7c3c39f8aab192c4e1c90d065f37b8a4af6141022100a8e160b856c2d43d27d8fba71e5aef6405b8643ac4cb7cb3c462aced7f14711a0141046d11fee51b0e60666d5049a9101a72741df480b96ee26488a4d3466b95c9a40ac5eeef87e10a5cd336c19a84565f80fa6c547957b7700ff4dfbdefe76036c339ffffffff021bff3d11000000001976a91404943fdd508053c75000106d3bc6e2754dbcff1988ac2f15de00000000001976a914a266436d2965547608b9e15d9032a7b9d64fa43188ac000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"_hex,
    };
    CTransaction tx_2(deserialize, TX_WITH_WITNESS, stream_2);
    CTransactionRef ptx_2{MakeTransactionRef(tx_2)};

    // Random real segwit transaction
    DataStream stream_3{
        "0200000000010177862801f77c2c068a70372b4c435ef8dd621291c36a64eb4dd491f02218f5324600000000fdffffff014a0100000000000022512035ea312034cfac01e956a269f3bf147f569c2fbb00180677421262da042290d803402be713325ff285e66b0380f53f2fae0d0fb4e16f378a440fed51ce835061437566729d4883bc917632f3cff474d6384bc8b989961a1d730d4a87ed38ad28bd337b20f1d658c6c138b1c312e072b4446f50f01ae0da03a42e6274f8788aae53416a7fac0063036f7264010118746578742f706c61696e3b636861727365743d7574662d3800357b2270223a226272632d3230222c226f70223a226d696e74222c227469636b223a224342414c222c22616d74223a2236393639227d6821c1f1d658c6c138b1c312e072b4446f50f01ae0da03a42e6274f8788aae53416a7f000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"_hex,
    };
    CTransaction tx_3(deserialize, TX_WITH_WITNESS, stream_3);
    CTransactionRef ptx_3{MakeTransactionRef(tx_3)};

    // tx_1 and tx_3 are segwit (wtxid != txid); tx_2 is non-segwit (wtxid == txid), so the
    // wtxid- and txid-based orderings are genuinely distinct — this lets us prove
    // GetPackageHash uses the wtxid and not the txid.
    BOOST_CHECK(tx_1.GetHash().ToUint256() != tx_1.GetWitnessHash().ToUint256());
    BOOST_CHECK(tx_2.GetHash().ToUint256() == tx_2.GetWitnessHash().ToUint256());
    BOOST_CHECK(tx_3.GetHash().ToUint256() != tx_3.GetWitnessHash().ToUint256());

    // Fold a set of ids in one of two candidate orders: ascending GetHex() (big-endian /
    // reversed-byte) order — the order GetPackageHash actually uses — or ascending raw uint256
    // (little-endian storage byte) order. Deriving the references this way keeps the test
    // robust to the wire-format change (the appended PoW tail alters every id) instead of
    // pinning brittle hardcoded hashes.
    auto fold_in_order = [](std::vector<uint256> ids, bool by_hex) {
        if (by_hex) std::sort(ids.begin(), ids.end(), [](const uint256& a, const uint256& b) { return a.GetHex() < b.GetHex(); });
        else        std::sort(ids.begin(), ids.end()); // uint256 operator< compares little-endian storage bytes
        HashWriter hw;
        for (const auto& id : ids) hw << id;
        return hw.GetSHA256();
    };

    const uint256 w1{tx_1.GetWitnessHash().ToUint256()}, w2{tx_2.GetWitnessHash().ToUint256()};
    const uint256 t1{tx_1.GetHash().ToUint256()}, t2{tx_2.GetHash().ToUint256()};

    // For the GetHex-vs-uint256 distinction to be meaningful, the three wtxids must sort
    // differently under the two orders (for some id sets the orders happen to coincide). Grind
    // tx_3's nPowNonce (part of the per-tx tail, not validated by this hashing test) until
    // they differ — deterministic, and not dependent on any one transaction's luck.
    CMutableTransaction mtx_3{tx_3};
    uint256 w3;
    for (uint32_t nonce = 0; ; ++nonce) {
        BOOST_REQUIRE(nonce < 100000); // a differing set is found within a handful of tries
        mtx_3.nPowNonce = nonce;
        ptx_3 = MakeTransactionRef(mtx_3);
        w3 = ptx_3->GetWitnessHash().ToUint256();
        if (fold_in_order({w1, w2, w3}, /*by_hex=*/true) != fold_in_order({w1, w2, w3}, /*by_hex=*/false)) break;
    }
    const uint256 t3{ptx_3->GetHash().ToUint256()};
    const uint256 expected_hash = fold_in_order({w1, w2, w3}, /*by_hex=*/true);

    // GetPackageHash equals the wtxid-GetHex-sorted fold and is invariant under input
    // permutation.
    std::vector<CTransactionRef> package_123{ptx_1, ptx_2, ptx_3};
    std::vector<CTransactionRef> package_132{ptx_1, ptx_3, ptx_2};
    std::vector<CTransactionRef> package_231{ptx_2, ptx_3, ptx_1};
    std::vector<CTransactionRef> package_213{ptx_2, ptx_1, ptx_3};
    std::vector<CTransactionRef> package_312{ptx_3, ptx_1, ptx_2};
    std::vector<CTransactionRef> package_321{ptx_3, ptx_2, ptx_1};
    BOOST_CHECK_EQUAL(expected_hash, GetPackageHash(package_123));
    BOOST_CHECK_EQUAL(expected_hash, GetPackageHash(package_132));
    BOOST_CHECK_EQUAL(expected_hash, GetPackageHash(package_231));
    BOOST_CHECK_EQUAL(expected_hash, GetPackageHash(package_213));
    BOOST_CHECK_EQUAL(expected_hash, GetPackageHash(package_312));
    BOOST_CHECK_EQUAL(expected_hash, GetPackageHash(package_321));

    // The negatives the original test guarded against: folding the TXIDS (not wtxids), or
    // folding the wtxids in raw uint256 (little-endian) order instead of GetHex order, both
    // yield a different digest.
    BOOST_CHECK(fold_in_order({t1, t2, t3}, /*by_hex=*/true) != expected_hash);  // uses wtxid, not txid
    BOOST_CHECK(fold_in_order({w1, w2, w3}, /*by_hex=*/false) != expected_hash); // uses GetHex order, not uint256 order
}

BOOST_AUTO_TEST_CASE(package_sanitization_tests)
{
    // Packages can't have more than 25 transactions.
    Package package_too_many;
    package_too_many.reserve(MAX_PACKAGE_COUNT + 1);
    for (size_t i{0}; i < MAX_PACKAGE_COUNT + 1; ++i) {
        package_too_many.emplace_back(create_placeholder_tx(1, 1));
    }
    PackageValidationState state_too_many;
    BOOST_CHECK(!IsWellFormedPackage(package_too_many, state_too_many, /*require_sorted=*/true));
    BOOST_CHECK_EQUAL(state_too_many.GetResult(), PackageValidationResult::PCKG_POLICY);
    BOOST_CHECK_EQUAL(state_too_many.GetRejectReason(), "package-too-many-transactions");

    // Packages can't have a total weight of more than 404'000WU.
    CTransactionRef large_ptx = create_placeholder_tx(150, 150);
    Package package_too_large;
    auto size_large = GetTransactionWeight(*large_ptx);
    size_t total_weight{0};
    while (total_weight <= MAX_PACKAGE_WEIGHT) {
        package_too_large.push_back(large_ptx);
        total_weight += size_large;
    }
    BOOST_CHECK(package_too_large.size() <= MAX_PACKAGE_COUNT);
    PackageValidationState state_too_large;
    BOOST_CHECK(!IsWellFormedPackage(package_too_large, state_too_large, /*require_sorted=*/true));
    BOOST_CHECK_EQUAL(state_too_large.GetResult(), PackageValidationResult::PCKG_POLICY);
    BOOST_CHECK_EQUAL(state_too_large.GetRejectReason(), "package-too-large");

    // Packages can't contain transactions with the same txid.
    Package package_duplicate_txids_empty;
    for (auto i{0}; i < 3; ++i) {
        CMutableTransaction empty_tx;
        package_duplicate_txids_empty.emplace_back(MakeTransactionRef(empty_tx));
    }
    PackageValidationState state_duplicates;
    BOOST_CHECK(!IsWellFormedPackage(package_duplicate_txids_empty, state_duplicates, /*require_sorted=*/true));
    BOOST_CHECK_EQUAL(state_duplicates.GetResult(), PackageValidationResult::PCKG_POLICY);
    BOOST_CHECK_EQUAL(state_duplicates.GetRejectReason(), "package-contains-duplicates");
    BOOST_CHECK(!IsConsistentPackage(package_duplicate_txids_empty));

    // Packages can't have transactions spending the same prevout
    CMutableTransaction tx_zero_1;
    CMutableTransaction tx_zero_2;
    COutPoint same_prevout{Txid::FromUint256(m_rng.rand256()), 0};
    tx_zero_1.vin.emplace_back(same_prevout);
    tx_zero_2.vin.emplace_back(same_prevout);
    // Different vouts (not the same tx)
    tx_zero_1.vout.emplace_back(CENT, P2WSH_OP_TRUE);
    tx_zero_2.vout.emplace_back(2 * CENT, P2WSH_OP_TRUE);
    Package package_conflicts{MakeTransactionRef(tx_zero_1), MakeTransactionRef(tx_zero_2)};
    BOOST_CHECK(!IsConsistentPackage(package_conflicts));
    // Transactions are considered sorted when they have no dependencies.
    BOOST_CHECK(IsTopoSortedPackage(package_conflicts));
    PackageValidationState state_conflicts;
    BOOST_CHECK(!IsWellFormedPackage(package_conflicts, state_conflicts, /*require_sorted=*/true));
    BOOST_CHECK_EQUAL(state_conflicts.GetResult(), PackageValidationResult::PCKG_POLICY);
    BOOST_CHECK_EQUAL(state_conflicts.GetRejectReason(), "conflict-in-package");

    // IsConsistentPackage only cares about conflicts between transactions, not about a transaction
    // conflicting with itself (i.e. duplicate prevouts in vin).
    CMutableTransaction dup_tx;
    const COutPoint rand_prevout{Txid::FromUint256(m_rng.rand256()), 0};
    dup_tx.vin.emplace_back(rand_prevout);
    dup_tx.vin.emplace_back(rand_prevout);
    Package package_with_dup_tx{MakeTransactionRef(dup_tx)};
    BOOST_CHECK(IsConsistentPackage(package_with_dup_tx));
    package_with_dup_tx.emplace_back(create_placeholder_tx(1, 1));
    BOOST_CHECK(IsConsistentPackage(package_with_dup_tx));
}

BOOST_AUTO_TEST_CASE(package_validation_tests)
{
    LOCK(cs_main);
    unsigned int initialPoolSize = m_node.relaypool->size();

    // Parent and Child Package — feeless (in == out) with anchored per-tx proofs.
    CKey parent_key = GenerateRandomKey();
    CScript parent_locking_script = GetScriptForDestination(PKHash(parent_key.GetPubKey()));
    CTransactionRef tx_parent = MakeFeelessProvenTx(/*input_tx=*/m_coinbase_txns[0], /*vout=*/0,
                                                    /*signing_key=*/coinbaseKey, /*out_spk=*/parent_locking_script);

    CKey child_key = GenerateRandomKey();
    CScript child_locking_script = GetScriptForDestination(PKHash(child_key.GetPubKey()));
    CTransactionRef tx_child = MakeFeelessProvenTx(/*input_tx=*/tx_parent, /*vout=*/0,
                                                   /*signing_key=*/parent_key, /*out_spk=*/child_locking_script,
                                                   /*input_height=*/101);
    Package package_parent_child{tx_parent, tx_child};
    const auto result_parent_child = ProcessNewPackage(m_node.chainman->ActiveChainstate(), *m_node.relaypool, package_parent_child, /*test_accept=*/true);
    if (auto err_parent_child{CheckPackageRelayPoolAcceptResult(package_parent_child, result_parent_child, /*expect_valid=*/true, nullptr)}) {
        BOOST_ERROR(err_parent_child.value());
    } else {
        auto it_parent = result_parent_child.m_tx_results.find(tx_parent->GetWitnessHash());
        auto it_child = result_parent_child.m_tx_results.find(tx_child->GetWitnessHash());

        // Feeless: each tx is its own singleton validation group (no package aggregation, #5c-2).
        BOOST_REQUIRE(it_parent != result_parent_child.m_tx_results.end());
        BOOST_REQUIRE(it_child != result_parent_child.m_tx_results.end());
        BOOST_CHECK(it_parent->second.m_result_type == RelayPoolAcceptResult::ResultType::VALID ||
                    it_parent->second.m_result_type == RelayPoolAcceptResult::ResultType::RELAYPOOL_ENTRY);
        BOOST_CHECK(it_parent->second.m_vsize.has_value());
        BOOST_CHECK_EQUAL(it_parent->second.m_vsize.value(), GetVirtualTransactionSize(*tx_parent));
        BOOST_CHECK_EQUAL(it_parent->second.m_package_wtxids.value().size(), 1);
        BOOST_CHECK_EQUAL(it_parent->second.m_package_wtxids.value().front(), tx_parent->GetWitnessHash());

        BOOST_CHECK(it_child->second.m_result_type == RelayPoolAcceptResult::ResultType::VALID ||
                    it_child->second.m_result_type == RelayPoolAcceptResult::ResultType::RELAYPOOL_ENTRY);
        BOOST_CHECK(it_child->second.m_vsize.has_value());
        BOOST_CHECK_EQUAL(it_child->second.m_vsize.value(), GetVirtualTransactionSize(*tx_child));
        BOOST_CHECK_EQUAL(it_child->second.m_package_wtxids.value().size(), 1);
        BOOST_CHECK_EQUAL(it_child->second.m_package_wtxids.value().front(), tx_child->GetWitnessHash());
    }
    // A single, giant transaction submitted through ProcessNewPackage fails on single tx policy.
    // Give it a valid anchored proof so it clears the per-tx PoW check and the rejection is
    // the intended tx-size policy failure, not a missing/stale anchor.
    CMutableTransaction giant_mtx{*create_placeholder_tx(999, 999)};
    ProveTxPow(giant_mtx);
    CTransactionRef giant_ptx = MakeTransactionRef(giant_mtx);
    BOOST_CHECK(GetVirtualTransactionSize(*giant_ptx) > DEFAULT_ANCESTOR_SIZE_LIMIT_KVB * 1000);
    Package package_single_giant{giant_ptx};
    auto result_single_large = ProcessNewPackage(m_node.chainman->ActiveChainstate(), *m_node.relaypool, package_single_giant, /*test_accept=*/true);
    if (auto err_single_large{CheckPackageRelayPoolAcceptResult(package_single_giant, result_single_large, /*expect_valid=*/false, nullptr)}) {
        BOOST_ERROR(err_single_large.value());
    } else {
        BOOST_CHECK_EQUAL(result_single_large.m_state.GetResult(), PackageValidationResult::PCKG_TX);
        BOOST_CHECK_EQUAL(result_single_large.m_state.GetRejectReason(), "transaction failed");
        auto it_giant_tx = result_single_large.m_tx_results.find(giant_ptx->GetWitnessHash());
        BOOST_CHECK_EQUAL(it_giant_tx->second.m_state.GetRejectReason(), "tx-size");
    }

    // Check that relaypool size hasn't changed.
    BOOST_CHECK_EQUAL(m_node.relaypool->size(), initialPoolSize);
}

BOOST_AUTO_TEST_CASE(noncontextual_package_tests)
{
    // The signatures won't be verified so we can just use a placeholder
    CKey placeholder_key = GenerateRandomKey();
    CScript spk = GetScriptForDestination(PKHash(placeholder_key.GetPubKey()));
    CKey placeholder_key_2 = GenerateRandomKey();
    CScript spk2 = GetScriptForDestination(PKHash(placeholder_key_2.GetPubKey()));

    // Parent and Child Package
    {
        auto mtx_parent = CreateValidRelayPoolTransaction(m_coinbase_txns[0], 0, 0, coinbaseKey, spk,
                                                        CAmount(49 * COIN), /*submit=*/false);
        CTransactionRef tx_parent = MakeTransactionRef(mtx_parent);

        auto mtx_child = CreateValidRelayPoolTransaction(tx_parent, 0, 101, placeholder_key, spk2,
                                                       CAmount(48 * COIN), /*submit=*/false);
        CTransactionRef tx_child = MakeTransactionRef(mtx_child);

        PackageValidationState state;
        BOOST_CHECK(IsWellFormedPackage({tx_parent, tx_child}, state, /*require_sorted=*/true));
        BOOST_CHECK(!IsWellFormedPackage({tx_child, tx_parent}, state, /*require_sorted=*/true));
        BOOST_CHECK_EQUAL(state.GetResult(), PackageValidationResult::PCKG_POLICY);
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "package-not-sorted");
        BOOST_CHECK(IsChildWithParents({tx_parent, tx_child}));
        BOOST_CHECK(IsChildWithParentsTree({tx_parent, tx_child}));
        BOOST_CHECK(GetPackageHash({tx_parent}) != GetPackageHash({tx_child}));
        BOOST_CHECK(GetPackageHash({tx_child, tx_child}) != GetPackageHash({tx_child}));
        BOOST_CHECK(GetPackageHash({tx_child, tx_parent}) != GetPackageHash({tx_child, tx_child}));
        BOOST_CHECK(!IsChildWithParents({}));
        BOOST_CHECK(!IsChildWithParentsTree({}));
    }

    // 24 Parents and 1 Child
    {
        Package package;
        CMutableTransaction child;
        for (int i{0}; i < 24; ++i) {
            auto parent = MakeTransactionRef(CreateValidRelayPoolTransaction(m_coinbase_txns[i + 1],
                                             0, 0, coinbaseKey, spk, CAmount(48 * COIN), false));
            package.emplace_back(parent);
            child.vin.emplace_back(COutPoint(parent->GetHash(), 0));
        }
        child.vout.emplace_back(47 * COIN, spk2);

        // The child must be in the package.
        BOOST_CHECK(!IsChildWithParents(package));

        // The parents can be in any order.
        FastRandomContext rng;
        std::shuffle(package.begin(), package.end(), rng);
        package.push_back(MakeTransactionRef(child));

        PackageValidationState state;
        BOOST_CHECK(IsWellFormedPackage(package, state, /*require_sorted=*/true));
        BOOST_CHECK(IsChildWithParents(package));
        BOOST_CHECK(IsChildWithParentsTree(package));

        package.erase(package.begin());
        BOOST_CHECK(IsChildWithParents(package));

        // The package cannot have unrelated transactions.
        package.insert(package.begin(), m_coinbase_txns[0]);
        BOOST_CHECK(!IsChildWithParents(package));
    }

    // 2 Parents and 1 Child where one parent depends on the other.
    {
        CMutableTransaction mtx_parent;
        mtx_parent.vin.emplace_back(COutPoint(m_coinbase_txns[0]->GetHash(), 0));
        mtx_parent.vout.emplace_back(20 * COIN, spk);
        mtx_parent.vout.emplace_back(20 * COIN, spk2);
        CTransactionRef tx_parent = MakeTransactionRef(mtx_parent);

        CMutableTransaction mtx_parent_also_child;
        mtx_parent_also_child.vin.emplace_back(COutPoint(tx_parent->GetHash(), 0));
        mtx_parent_also_child.vout.emplace_back(20 * COIN, spk);
        CTransactionRef tx_parent_also_child = MakeTransactionRef(mtx_parent_also_child);

        CMutableTransaction mtx_child;
        mtx_child.vin.emplace_back(COutPoint(tx_parent->GetHash(), 1));
        mtx_child.vin.emplace_back(COutPoint(tx_parent_also_child->GetHash(), 0));
        mtx_child.vout.emplace_back(39 * COIN, spk);
        CTransactionRef tx_child = MakeTransactionRef(mtx_child);

        PackageValidationState state;
        BOOST_CHECK(IsChildWithParents({tx_parent, tx_parent_also_child}));
        BOOST_CHECK(IsChildWithParents({tx_parent, tx_child}));
        BOOST_CHECK(IsChildWithParents({tx_parent, tx_parent_also_child, tx_child}));
        BOOST_CHECK(!IsChildWithParentsTree({tx_parent, tx_parent_also_child, tx_child}));
        // IsChildWithParents does not detect unsorted parents.
        BOOST_CHECK(IsChildWithParents({tx_parent_also_child, tx_parent, tx_child}));
        BOOST_CHECK(IsWellFormedPackage({tx_parent, tx_parent_also_child, tx_child}, state, /*require_sorted=*/true));
        BOOST_CHECK(!IsWellFormedPackage({tx_parent_also_child, tx_parent, tx_child}, state, /*require_sorted=*/true));
        BOOST_CHECK_EQUAL(state.GetResult(), PackageValidationResult::PCKG_POLICY);
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "package-not-sorted");
    }
}

BOOST_AUTO_TEST_CASE(package_submission_tests)
{
    LOCK(cs_main);
    unsigned int expected_pool_size = m_node.relaypool->size();
    CKey parent_key = GenerateRandomKey();
    CScript parent_locking_script = GetScriptForDestination(PKHash(parent_key.GetPubKey()));

    // Unrelated transactions are not allowed in package submission.
    Package package_unrelated;
    for (size_t i{0}; i < 10; ++i) {
        package_unrelated.emplace_back(MakeFeelessProvenTx(/*input_tx=*/m_coinbase_txns[i + 25], /*vout=*/0,
                                                           /*signing_key=*/coinbaseKey, /*out_spk=*/parent_locking_script));
    }
    auto result_unrelated_submit = ProcessNewPackage(m_node.chainman->ActiveChainstate(), *m_node.relaypool,
                                                     package_unrelated, /*test_accept=*/false);
    // We don't expect m_tx_results for each transaction when basic sanity checks haven't passed.
    BOOST_CHECK(result_unrelated_submit.m_state.IsInvalid());
    BOOST_CHECK_EQUAL(result_unrelated_submit.m_state.GetResult(), PackageValidationResult::PCKG_POLICY);
    BOOST_CHECK_EQUAL(result_unrelated_submit.m_state.GetRejectReason(), "package-not-child-with-parents");
    BOOST_CHECK_EQUAL(m_node.relaypool->size(), expected_pool_size);

    // Parent and Child (and Grandchild) Package — feeless (in == out), anchored proofs.
    Package package_parent_child;
    Package package_3gen;
    CTransactionRef tx_parent = MakeFeelessProvenTx(/*input_tx=*/m_coinbase_txns[0], /*vout=*/0,
                                                    /*signing_key=*/coinbaseKey, /*out_spk=*/parent_locking_script);
    package_parent_child.push_back(tx_parent);
    package_3gen.push_back(tx_parent);

    CKey child_key = GenerateRandomKey();
    CScript child_locking_script = GetScriptForDestination(PKHash(child_key.GetPubKey()));
    CTransactionRef tx_child = MakeFeelessProvenTx(/*input_tx=*/tx_parent, /*vout=*/0,
                                                   /*signing_key=*/parent_key, /*out_spk=*/child_locking_script,
                                                   /*input_height=*/101);
    package_parent_child.push_back(tx_child);
    package_3gen.push_back(tx_child);

    CKey grandchild_key = GenerateRandomKey();
    CScript grandchild_locking_script = GetScriptForDestination(PKHash(grandchild_key.GetPubKey()));
    CTransactionRef tx_grandchild = MakeFeelessProvenTx(/*input_tx=*/tx_child, /*vout=*/0,
                                                        /*signing_key=*/child_key, /*out_spk=*/grandchild_locking_script,
                                                        /*input_height=*/101);
    package_3gen.push_back(tx_grandchild);

    // 3 Generations is not allowed.
    {
        auto result_3gen_submit = ProcessNewPackage(m_node.chainman->ActiveChainstate(), *m_node.relaypool,
                                                    package_3gen, /*test_accept=*/false);
        BOOST_CHECK(result_3gen_submit.m_state.IsInvalid());
        BOOST_CHECK_EQUAL(result_3gen_submit.m_state.GetResult(), PackageValidationResult::PCKG_POLICY);
        BOOST_CHECK_EQUAL(result_3gen_submit.m_state.GetRejectReason(), "package-not-child-with-parents");
        BOOST_CHECK_EQUAL(m_node.relaypool->size(), expected_pool_size);
    }

    // Parent and child package where transactions are invalid for reasons other than fee and
    // missing inputs, so the package validation isn't expected to happen.
    {
        CScriptWitness bad_witness;
        bad_witness.stack.emplace_back(1);
        CMutableTransaction mtx_parent_invalid{*tx_parent};
        mtx_parent_invalid.vin[0].scriptWitness = bad_witness;
        CTransactionRef tx_parent_invalid = MakeTransactionRef(mtx_parent_invalid);
        Package package_invalid_parent{tx_parent_invalid, tx_child};
        auto result_quit_early = ProcessNewPackage(m_node.chainman->ActiveChainstate(), *m_node.relaypool,
                                                   package_invalid_parent, /*test_accept=*/ false);
        if (auto err_parent_invalid{CheckPackageRelayPoolAcceptResult(package_invalid_parent, result_quit_early, /*expect_valid=*/false, m_node.relaypool.get())}) {
            BOOST_ERROR(err_parent_invalid.value());
        } else {
            auto it_parent = result_quit_early.m_tx_results.find(tx_parent_invalid->GetWitnessHash());
            auto it_child = result_quit_early.m_tx_results.find(tx_child->GetWitnessHash());
            BOOST_CHECK_EQUAL(it_parent->second.m_state.GetResult(), TxValidationResult::TX_WITNESS_MUTATED);
            BOOST_CHECK_EQUAL(it_parent->second.m_state.GetRejectReason(), "bad-witness-nonstandard");
            BOOST_CHECK_EQUAL(it_child->second.m_state.GetResult(), TxValidationResult::TX_MISSING_INPUTS);
            BOOST_CHECK_EQUAL(it_child->second.m_state.GetRejectReason(), "bad-txns-inputs-missingorspent");
        }
        BOOST_CHECK_EQUAL(result_quit_early.m_state.GetResult(), PackageValidationResult::PCKG_TX);
    }

    // Child with missing parent. (Package-structure rejection fires before per-tx checks,
    // so the extra input invalidating the child's proof is irrelevant here.)
    CMutableTransaction mtx_child_missing{*tx_child};
    mtx_child_missing.vin.emplace_back(COutPoint(package_unrelated[0]->GetHash(), 0));
    Package package_missing_parent;
    package_missing_parent.push_back(tx_parent);
    package_missing_parent.push_back(MakeTransactionRef(mtx_child_missing));
    {
        const auto result_missing_parent = ProcessNewPackage(m_node.chainman->ActiveChainstate(), *m_node.relaypool,
                                                             package_missing_parent, /*test_accept=*/false);
        BOOST_CHECK(result_missing_parent.m_state.IsInvalid());
        BOOST_CHECK_EQUAL(result_missing_parent.m_state.GetResult(), PackageValidationResult::PCKG_POLICY);
        BOOST_CHECK_EQUAL(result_missing_parent.m_state.GetRejectReason(), "package-not-child-with-unconfirmed-parents");
        BOOST_CHECK_EQUAL(m_node.relaypool->size(), expected_pool_size);
    }

    // Submit package with parent + child.
    {
        const auto submit_parent_child = ProcessNewPackage(m_node.chainman->ActiveChainstate(), *m_node.relaypool,
                                                           package_parent_child, /*test_accept=*/false);
        expected_pool_size += 2;
        BOOST_CHECK_MESSAGE(submit_parent_child.m_state.IsValid(),
                            "Package validation unexpectedly failed: " << submit_parent_child.m_state.GetRejectReason());
        BOOST_CHECK_EQUAL(submit_parent_child.m_tx_results.size(), package_parent_child.size());
        auto it_parent = submit_parent_child.m_tx_results.find(tx_parent->GetWitnessHash());
        auto it_child = submit_parent_child.m_tx_results.find(tx_child->GetWitnessHash());
        BOOST_REQUIRE(it_parent != submit_parent_child.m_tx_results.end());
        BOOST_REQUIRE(it_child != submit_parent_child.m_tx_results.end());
        BOOST_CHECK(it_parent->second.m_state.IsValid());
        // Feeless: each tx is its own singleton validation group.
        BOOST_CHECK(it_parent->second.m_result_type == RelayPoolAcceptResult::ResultType::VALID ||
                    it_parent->second.m_result_type == RelayPoolAcceptResult::ResultType::RELAYPOOL_ENTRY);
        BOOST_CHECK(it_parent->second.m_vsize.has_value());
        BOOST_CHECK_EQUAL(it_parent->second.m_vsize.value(), GetVirtualTransactionSize(*tx_parent));
        BOOST_CHECK_EQUAL(it_parent->second.m_package_wtxids.value().size(), 1);
        BOOST_CHECK_EQUAL(it_parent->second.m_package_wtxids.value().front(), tx_parent->GetWitnessHash());
        BOOST_CHECK(it_child->second.m_result_type == RelayPoolAcceptResult::ResultType::VALID ||
                    it_child->second.m_result_type == RelayPoolAcceptResult::ResultType::RELAYPOOL_ENTRY);
        BOOST_CHECK(it_child->second.m_vsize.has_value());
        BOOST_CHECK_EQUAL(it_child->second.m_vsize.value(), GetVirtualTransactionSize(*tx_child));
        BOOST_CHECK_EQUAL(it_child->second.m_package_wtxids.value().size(), 1);
        BOOST_CHECK_EQUAL(it_child->second.m_package_wtxids.value().front(), tx_child->GetWitnessHash());

        BOOST_CHECK_EQUAL(m_node.relaypool->size(), expected_pool_size);
    }

    // Already-in-relaypool transactions should be detected and de-duplicated.
    {
        const auto submit_deduped = ProcessNewPackage(m_node.chainman->ActiveChainstate(), *m_node.relaypool,
                                                      package_parent_child, /*test_accept=*/false);
        if (auto err_deduped{CheckPackageRelayPoolAcceptResult(package_parent_child, submit_deduped, /*expect_valid=*/true, m_node.relaypool.get())}) {
            BOOST_ERROR(err_deduped.value());
        } else {
            auto it_parent_deduped = submit_deduped.m_tx_results.find(tx_parent->GetWitnessHash());
            auto it_child_deduped = submit_deduped.m_tx_results.find(tx_child->GetWitnessHash());
            BOOST_CHECK(it_parent_deduped->second.m_result_type == RelayPoolAcceptResult::ResultType::RELAYPOOL_ENTRY);
            BOOST_CHECK(it_child_deduped->second.m_result_type == RelayPoolAcceptResult::ResultType::RELAYPOOL_ENTRY);
        }

        BOOST_CHECK_EQUAL(m_node.relaypool->size(), expected_pool_size);
    }
}

// Tests for packages containing a single transaction
BOOST_AUTO_TEST_CASE(package_single_tx)
{
    // Mine blocks to mature coinbases.
    mineBlocks(3);
    LOCK(cs_main);
    auto expected_pool_size{m_node.relaypool->size()};

    // No unconfirmed parents. Pin this incumbent with a HIGH surplus so the lower-surplus
    // conflict submitted at the end of the test is deterministically rejected (#6).
    CKey single_key = GenerateRandomKey();
    CScript single_locking_script = GetScriptForDestination(PKHash(single_key.GetPubKey()));
    CTransactionRef tx_single = MakeFeelessHighSurplusTx(/*input_tx=*/m_coinbase_txns[0], /*vout=*/0,
                                                         /*signing_key=*/coinbaseKey, /*out_spk=*/single_locking_script);
    Package package_tx_single{tx_single};
    const auto result_single_tx = ProcessNewPackage(m_node.chainman->ActiveChainstate(), *m_node.relaypool,
                                                    package_tx_single, /*test_accept=*/false);
    expected_pool_size += 1;
    BOOST_CHECK_MESSAGE(result_single_tx.m_state.IsValid(),
                        "Package validation unexpectedly failed: " << result_single_tx.m_state.ToString());
    BOOST_CHECK_EQUAL(m_node.relaypool->size(), expected_pool_size);

    // Parent and Child. Both submitted by themselves through the ProcessNewPackage interface.
    CKey parent_key = GenerateRandomKey();
    CScript parent_locking_script = GetScriptForDestination(WitnessV0KeyHash(parent_key.GetPubKey()));
    CTransactionRef tx_parent = MakeFeelessProvenTx(/*input_tx=*/m_coinbase_txns[1], /*vout=*/0,
                                                    /*signing_key=*/coinbaseKey, /*out_spk=*/parent_locking_script);
    Package package_just_parent{tx_parent};
    const auto result_just_parent = ProcessNewPackage(m_node.chainman->ActiveChainstate(), *m_node.relaypool, package_just_parent, /*test_accept=*/false);
    if (auto err_parent_child{CheckPackageRelayPoolAcceptResult(package_just_parent, result_just_parent, /*expect_valid=*/true, nullptr)}) {
        BOOST_ERROR(err_parent_child.value());
    } else {
        auto it_parent = result_just_parent.m_tx_results.find(tx_parent->GetWitnessHash());
        BOOST_REQUIRE(it_parent != result_just_parent.m_tx_results.end());
        BOOST_CHECK_MESSAGE(it_parent->second.m_state.IsValid(), it_parent->second.m_state.ToString());
        // Feeless: no fee payload is returned.
        BOOST_CHECK(it_parent->second.m_result_type == RelayPoolAcceptResult::ResultType::VALID ||
                    it_parent->second.m_result_type == RelayPoolAcceptResult::ResultType::RELAYPOOL_ENTRY);
        BOOST_CHECK(it_parent->second.m_vsize.has_value());
        BOOST_CHECK_EQUAL(it_parent->second.m_vsize.value(), GetVirtualTransactionSize(*tx_parent));
        BOOST_CHECK_EQUAL(it_parent->second.m_package_wtxids.value().size(), 1);
        BOOST_CHECK_EQUAL(it_parent->second.m_package_wtxids.value().front(), tx_parent->GetWitnessHash());
    }
    expected_pool_size += 1;
    BOOST_CHECK_EQUAL(m_node.relaypool->size(), expected_pool_size);

    CKey child_key = GenerateRandomKey();
    CScript child_locking_script = GetScriptForDestination(WitnessV0KeyHash(child_key.GetPubKey()));
    CTransactionRef tx_child = MakeFeelessProvenTx(/*input_tx=*/tx_parent, /*vout=*/0,
                                                   /*signing_key=*/parent_key, /*out_spk=*/child_locking_script,
                                                   /*input_height=*/101);
    Package package_just_child{tx_child};
    const auto result_just_child = ProcessNewPackage(m_node.chainman->ActiveChainstate(), *m_node.relaypool, package_just_child, /*test_accept=*/false);
    if (auto err_parent_child{CheckPackageRelayPoolAcceptResult(package_just_child, result_just_child, /*expect_valid=*/true, nullptr)}) {
        BOOST_ERROR(err_parent_child.value());
    } else {
        auto it_child = result_just_child.m_tx_results.find(tx_child->GetWitnessHash());
        BOOST_REQUIRE(it_child != result_just_child.m_tx_results.end());
        BOOST_CHECK_MESSAGE(it_child->second.m_state.IsValid(), it_child->second.m_state.ToString());
        BOOST_CHECK(it_child->second.m_result_type == RelayPoolAcceptResult::ResultType::VALID ||
                    it_child->second.m_result_type == RelayPoolAcceptResult::ResultType::RELAYPOOL_ENTRY);
        BOOST_CHECK(it_child->second.m_vsize.has_value());
        BOOST_CHECK_EQUAL(it_child->second.m_vsize.value(), GetVirtualTransactionSize(*tx_child));
        BOOST_CHECK_EQUAL(it_child->second.m_package_wtxids.value().size(), 1);
        BOOST_CHECK_EQUAL(it_child->second.m_package_wtxids.value().front(), tx_child->GetWitnessHash());
    }
    expected_pool_size += 1;
    BOOST_CHECK_EQUAL(m_node.relaypool->size(), expected_pool_size);

    // A lower-surplus conflict of tx_single (a single tx submitted through ProcessNewPackage)
    // is rejected under #6 replace-by-surplus-work. tx_single carries a high surplus; this
    // conflict spends the same input (so it conflicts) to a distinct output (so it is a real
    // double-spend, not a duplicate) with only a bare proof, so its surplus rate is lower.
    CKey conflict_key = GenerateRandomKey();
    CScript conflict_locking_script = GetScriptForDestination(PKHash(conflict_key.GetPubKey()));
    CTransactionRef tx_single_low_surplus = MakeFeelessProvenTx(/*input_tx=*/m_coinbase_txns[0], /*vout=*/0,
                                                               /*signing_key=*/coinbaseKey, /*out_spk=*/conflict_locking_script);
    Package package_tx_single_low_surplus{tx_single_low_surplus};
    const auto result_single_tx_low_surplus = ProcessNewPackage(m_node.chainman->ActiveChainstate(), *m_node.relaypool,
                                                    package_tx_single_low_surplus, /*test_accept=*/false);

    BOOST_CHECK_EQUAL(m_node.relaypool->size(), expected_pool_size);

    BOOST_CHECK(!result_single_tx_low_surplus.m_state.IsValid());
    BOOST_CHECK_EQUAL(result_single_tx_low_surplus.m_state.GetResult(), PackageValidationResult::PCKG_TX);
    auto it_low_surplus = result_single_tx_low_surplus.m_tx_results.find(tx_single_low_surplus->GetWitnessHash());
    BOOST_CHECK_EQUAL(it_low_surplus->second.m_state.GetResult(), TxValidationResult::TX_RECONSIDERABLE);
    BOOST_CHECK_EQUAL(it_low_surplus->second.m_state.GetRejectReason(), "insufficient surplus work");
    if (auto err_single{CheckPackageRelayPoolAcceptResult(package_tx_single_low_surplus, result_single_tx_low_surplus, /*expect_valid=*/false, m_node.relaypool.get())}) {
        BOOST_ERROR(err_single.value());
    }
    BOOST_CHECK_EQUAL(m_node.relaypool->size(), expected_pool_size);
}

// Tests for packages containing transactions that have same-txid-different-witness equivalents in
// the relaypool.
BOOST_AUTO_TEST_CASE(package_witness_swap_tests)
{
    // Same-txid-different-witness handling is unchanged by the feeless model: a package tx
    // whose txid already names a relaypool entry (but with a different witness) is ignored and
    // the in-relaypool wtxid is returned. This guards a censorship vector (an attacker mutating
    // a witness) and is independent of fees. The transactions are made feeless (in == out)
    // with anchored per-tx proofs; the original's relay-floor scaffolding (MockRelayPoolMinFee,
    // low-feerate "invalid on its own" parent, package-feerate assertions) is dropped.
    //
    // The same-txid pairs are built by grinding the per-tx proof ONCE on the base variant and
    // then copying it and swapping only the witness: the witness is excluded from both the
    // proof pre-image and the txid, so the copy keeps the same txid (and valid proof) while
    // getting a different wtxid.
    mineBlocks(5);
    LOCK(cs_main);

    // Transactions with a same-txid-different-witness transaction in the relaypool should be ignored,
    // and the relaypool entry's wtxid returned.
    CScript witnessScript = CScript() << OP_DROP << OP_TRUE;
    CScript scriptPubKey = GetScriptForDestination(WitnessV0ScriptHash(witnessScript));
    CTransactionRef ptx_parent = MakeFeelessProvenTx(/*input_tx=*/m_coinbase_txns[0], /*vout=*/0,
                                                     /*signing_key=*/coinbaseKey, /*out_spk=*/scriptPubKey);

    // Make two children with the same txid but different witnesses.
    CScriptWitness witness1;
    witness1.stack.emplace_back(1);
    witness1.stack.emplace_back(witnessScript.begin(), witnessScript.end());

    CScriptWitness witness2(witness1);
    witness2.stack.emplace_back(2);
    witness2.stack.emplace_back(witnessScript.begin(), witnessScript.end());

    CKey child_key = GenerateRandomKey();
    CScript child_locking_script = GetScriptForDestination(WitnessV0KeyHash(child_key.GetPubKey()));
    CMutableTransaction mtx_child1;
    mtx_child1.version = 1;
    mtx_child1.vin.resize(1);
    mtx_child1.vin[0].prevout.hash = ptx_parent->GetHash();
    mtx_child1.vin[0].prevout.n = 0;
    mtx_child1.vin[0].scriptSig = CScript();
    mtx_child1.vin[0].scriptWitness = witness1;
    mtx_child1.vout.resize(1);
    mtx_child1.vout[0].nValue = ptx_parent->vout[0].nValue; // feeless: in == out
    mtx_child1.vout[0].scriptPubKey = child_locking_script;
    ProveTxPow(mtx_child1); // anchor + proof on the base variant only

    CMutableTransaction mtx_child2{mtx_child1};
    mtx_child2.vin[0].scriptWitness = witness2; // same txid + proof, different wtxid

    CTransactionRef ptx_child1 = MakeTransactionRef(mtx_child1);
    CTransactionRef ptx_child2 = MakeTransactionRef(mtx_child2);

    // child1 and child2 have the same txid
    BOOST_CHECK_EQUAL(ptx_child1->GetHash(), ptx_child2->GetHash());
    // child1 and child2 have different wtxids
    BOOST_CHECK(ptx_child1->GetWitnessHash() != ptx_child2->GetWitnessHash());
    // Check that they have different package hashes
    BOOST_CHECK(GetPackageHash({ptx_parent, ptx_child1}) != GetPackageHash({ptx_parent, ptx_child2}));

    // Try submitting Package1{parent, child1} and Package2{parent, child2} where the children are
    // same-txid-different-witness.
    {
        Package package_parent_child1{ptx_parent, ptx_child1};
        const auto submit_witness1 = ProcessNewPackage(m_node.chainman->ActiveChainstate(), *m_node.relaypool,
                                                       package_parent_child1, /*test_accept=*/false);
        if (auto err_witness1{CheckPackageRelayPoolAcceptResult(package_parent_child1, submit_witness1, /*expect_valid=*/true, m_node.relaypool.get())}) {
            BOOST_ERROR(err_witness1.value());
        }

        // Child2 would have been validated individually.
        Package package_parent_child2{ptx_parent, ptx_child2};
        const auto submit_witness2 = ProcessNewPackage(m_node.chainman->ActiveChainstate(), *m_node.relaypool,
                                                       package_parent_child2, /*test_accept=*/false);
        if (auto err_witness2{CheckPackageRelayPoolAcceptResult(package_parent_child2, submit_witness2, /*expect_valid=*/true, m_node.relaypool.get())}) {
            BOOST_ERROR(err_witness2.value());
        } else {
            auto it_parent2_deduped = submit_witness2.m_tx_results.find(ptx_parent->GetWitnessHash());
            auto it_child2 = submit_witness2.m_tx_results.find(ptx_child2->GetWitnessHash());
            BOOST_CHECK(it_parent2_deduped->second.m_result_type == RelayPoolAcceptResult::ResultType::RELAYPOOL_ENTRY);
            BOOST_CHECK(it_child2->second.m_result_type == RelayPoolAcceptResult::ResultType::DIFFERENT_WITNESS);
            BOOST_CHECK_EQUAL(ptx_child1->GetWitnessHash(), it_child2->second.m_other_wtxid.value());
        }

        // Deduplication should work when wtxid != txid. Submit package with the already-in-relaypool
        // transactions again, which should not fail.
        const auto submit_segwit_dedup = ProcessNewPackage(m_node.chainman->ActiveChainstate(), *m_node.relaypool,
                                                           package_parent_child1, /*test_accept=*/false);
        if (auto err_segwit_dedup{CheckPackageRelayPoolAcceptResult(package_parent_child1, submit_segwit_dedup, /*expect_valid=*/true, m_node.relaypool.get())}) {
            BOOST_ERROR(err_segwit_dedup.value());
        } else {
            auto it_parent_dup = submit_segwit_dedup.m_tx_results.find(ptx_parent->GetWitnessHash());
            auto it_child_dup = submit_segwit_dedup.m_tx_results.find(ptx_child1->GetWitnessHash());
            BOOST_CHECK(it_parent_dup->second.m_result_type == RelayPoolAcceptResult::ResultType::RELAYPOOL_ENTRY);
            BOOST_CHECK(it_child_dup->second.m_result_type == RelayPoolAcceptResult::ResultType::RELAYPOOL_ENTRY);
        }
    }

    // Try submitting Package1{child2, grandchild} where child2 is same-txid-different-witness as
    // the in-relaypool transaction, child1. Since child1 exists in the relaypool and its outputs are
    // available, child2 should be ignored and grandchild should be accepted.
    //
    // This tests a potential censorship vector in which an attacker broadcasts a competing package
    // where a parent's witness is mutated. The honest package should be accepted despite the fact
    // that we don't allow witness replacement.
    CKey grandchild_key = GenerateRandomKey();
    CScript grandchild_locking_script = GetScriptForDestination(WitnessV0KeyHash(grandchild_key.GetPubKey()));
    // Spends child2:0 — the same outpoint as child1:0, since child1 and child2 share a txid.
    CTransactionRef ptx_grandchild = MakeFeelessProvenTx(/*input_tx=*/ptx_child2, /*vout=*/0,
                                                         /*signing_key=*/child_key, /*out_spk=*/grandchild_locking_script);
    // Check that they have different package hashes
    BOOST_CHECK(GetPackageHash({ptx_child1, ptx_grandchild}) != GetPackageHash({ptx_child2, ptx_grandchild}));
    // We already submitted child1 above.
    {
        Package package_child2_grandchild{ptx_child2, ptx_grandchild};
        const auto submit_spend_ignored = ProcessNewPackage(m_node.chainman->ActiveChainstate(), *m_node.relaypool,
                                                            package_child2_grandchild, /*test_accept=*/false);
        if (auto err_spend_ignored{CheckPackageRelayPoolAcceptResult(package_child2_grandchild, submit_spend_ignored, /*expect_valid=*/true, m_node.relaypool.get())}) {
            BOOST_ERROR(err_spend_ignored.value());
        } else {
            auto it_child2_ignored = submit_spend_ignored.m_tx_results.find(ptx_child2->GetWitnessHash());
            auto it_grandchild = submit_spend_ignored.m_tx_results.find(ptx_grandchild->GetWitnessHash());
            BOOST_CHECK(it_child2_ignored->second.m_result_type == RelayPoolAcceptResult::ResultType::DIFFERENT_WITNESS);
            BOOST_CHECK(it_grandchild->second.m_result_type == RelayPoolAcceptResult::ResultType::VALID);
        }
    }

    // A package Package{parent1, parent2, parent3, child} where the parents are a mixture of
    // identical-tx-in-relaypool, same-txid-different-witness-in-relaypool, and new transactions.
    Package package_mixed;

    // Give all the parents anyone-can-spend scripts so we don't have to deal with signing the child.
    CScript acs_script = CScript() << OP_TRUE;
    CScript acs_spk = GetScriptForDestination(WitnessV0ScriptHash(acs_script));
    CScriptWitness acs_witness;
    acs_witness.stack.emplace_back(acs_script.begin(), acs_script.end());

    // parent1 will already be in the relaypool
    CTransactionRef ptx_parent1 = MakeFeelessProvenTx(/*input_tx=*/m_coinbase_txns[1], /*vout=*/0,
                                                      /*signing_key=*/coinbaseKey, /*out_spk=*/acs_spk);
    BOOST_CHECK(m_node.chainman->ProcessTransaction(ptx_parent1).m_result_type == RelayPoolAcceptResult::ResultType::VALID);
    package_mixed.push_back(ptx_parent1);

    // parent2 will have a same-txid-different-witness tx already in the relaypool
    CScript grandparent2_script = CScript() << OP_DROP << OP_TRUE;
    CScript grandparent2_spk = GetScriptForDestination(WitnessV0ScriptHash(grandparent2_script));
    CScriptWitness parent2_witness1;
    parent2_witness1.stack.emplace_back(1);
    parent2_witness1.stack.emplace_back(grandparent2_script.begin(), grandparent2_script.end());
    CScriptWitness parent2_witness2;
    parent2_witness2.stack.emplace_back(2);
    parent2_witness2.stack.emplace_back(grandparent2_script.begin(), grandparent2_script.end());

    // Create grandparent2 creating an output with multiple spending paths. Submit to relaypool.
    CTransactionRef ptx_grandparent2 = MakeFeelessProvenTx(/*input_tx=*/m_coinbase_txns[2], /*vout=*/0,
                                                           /*signing_key=*/coinbaseKey, /*out_spk=*/grandparent2_spk);
    BOOST_CHECK(m_node.chainman->ProcessTransaction(ptx_grandparent2).m_result_type == RelayPoolAcceptResult::ResultType::VALID);

    CMutableTransaction mtx_parent2_v1;
    mtx_parent2_v1.version = 1;
    mtx_parent2_v1.vin.resize(1);
    mtx_parent2_v1.vin[0].prevout.hash = ptx_grandparent2->GetHash();
    mtx_parent2_v1.vin[0].prevout.n = 0;
    mtx_parent2_v1.vin[0].scriptSig = CScript();
    mtx_parent2_v1.vin[0].scriptWitness = parent2_witness1;
    mtx_parent2_v1.vout.resize(1);
    mtx_parent2_v1.vout[0].nValue = ptx_grandparent2->vout[0].nValue; // feeless: in == out
    mtx_parent2_v1.vout[0].scriptPubKey = acs_spk;
    ProveTxPow(mtx_parent2_v1); // anchor + proof on the base variant only

    CMutableTransaction mtx_parent2_v2{mtx_parent2_v1};
    mtx_parent2_v2.vin[0].scriptWitness = parent2_witness2; // same txid + proof, different wtxid

    CTransactionRef ptx_parent2_v1 = MakeTransactionRef(mtx_parent2_v1);
    CTransactionRef ptx_parent2_v2 = MakeTransactionRef(mtx_parent2_v2);
    // Put parent2_v1 in the package, submit parent2_v2 to the relaypool.
    const RelayPoolAcceptResult parent2_v2_result = m_node.chainman->ProcessTransaction(ptx_parent2_v2);
    BOOST_CHECK(parent2_v2_result.m_result_type == RelayPoolAcceptResult::ResultType::VALID);
    package_mixed.push_back(ptx_parent2_v1);

    // parent3 will be a new transaction, valid on its own (feeless + anchored proof).
    CTransactionRef ptx_parent3 = MakeFeelessProvenTx(/*input_tx=*/m_coinbase_txns[3], /*vout=*/0,
                                                      /*signing_key=*/coinbaseKey, /*out_spk=*/acs_spk);
    package_mixed.push_back(ptx_parent3);

    // child spends parent1, parent2, and parent3
    CKey mixed_grandchild_key = GenerateRandomKey();
    CScript mixed_child_spk = GetScriptForDestination(WitnessV0KeyHash(mixed_grandchild_key.GetPubKey()));

    CMutableTransaction mtx_mixed_child;
    mtx_mixed_child.vin.emplace_back(COutPoint(ptx_parent1->GetHash(), 0));
    mtx_mixed_child.vin.emplace_back(COutPoint(ptx_parent2_v1->GetHash(), 0));
    mtx_mixed_child.vin.emplace_back(COutPoint(ptx_parent3->GetHash(), 0));
    mtx_mixed_child.vin[0].scriptWitness = acs_witness;
    mtx_mixed_child.vin[1].scriptWitness = acs_witness;
    mtx_mixed_child.vin[2].scriptWitness = acs_witness;
    // feeless: in == out (sum of the three parents' outputs).
    mtx_mixed_child.vout.emplace_back(ptx_parent1->vout[0].nValue + ptx_parent2_v1->vout[0].nValue + ptx_parent3->vout[0].nValue, mixed_child_spk);
    ProveTxPow(mtx_mixed_child);
    CTransactionRef ptx_mixed_child = MakeTransactionRef(mtx_mixed_child);
    package_mixed.push_back(ptx_mixed_child);

    // Submit package:
    // parent1 should be ignored
    // parent2_v1 should be ignored (and v2 wtxid returned)
    // parent3 should be accepted
    // child should be accepted
    {
        const auto mixed_result = ProcessNewPackage(m_node.chainman->ActiveChainstate(), *m_node.relaypool, package_mixed, false);
        if (auto err_mixed{CheckPackageRelayPoolAcceptResult(package_mixed, mixed_result, /*expect_valid=*/true, m_node.relaypool.get())}) {
            BOOST_ERROR(err_mixed.value());
        } else {
            auto it_parent1 = mixed_result.m_tx_results.find(ptx_parent1->GetWitnessHash());
            auto it_parent2 = mixed_result.m_tx_results.find(ptx_parent2_v1->GetWitnessHash());
            auto it_parent3 = mixed_result.m_tx_results.find(ptx_parent3->GetWitnessHash());
            auto it_child = mixed_result.m_tx_results.find(ptx_mixed_child->GetWitnessHash());

            BOOST_CHECK(it_parent1->second.m_result_type == RelayPoolAcceptResult::ResultType::RELAYPOOL_ENTRY);
            BOOST_CHECK(it_parent2->second.m_result_type == RelayPoolAcceptResult::ResultType::DIFFERENT_WITNESS);
            BOOST_CHECK(it_parent3->second.m_result_type == RelayPoolAcceptResult::ResultType::VALID);
            BOOST_CHECK(it_child->second.m_result_type == RelayPoolAcceptResult::ResultType::VALID);
            BOOST_CHECK_EQUAL(ptx_parent2_v2->GetWitnessHash(), it_parent2->second.m_other_wtxid.value());

            // Feeless: both newly validated txs have their own validation group.
            BOOST_CHECK(it_parent3->second.m_vsize.has_value());
            BOOST_CHECK_EQUAL(it_parent3->second.m_vsize.value(), GetVirtualTransactionSize(*ptx_parent3));
            BOOST_CHECK(it_child->second.m_vsize.has_value());
            BOOST_CHECK_EQUAL(it_child->second.m_vsize.value(), GetVirtualTransactionSize(*ptx_mixed_child));
            BOOST_CHECK(it_parent3->second.m_package_wtxids.value() == std::vector<Wtxid>{ptx_parent3->GetWitnessHash()});
            BOOST_CHECK(it_child->second.m_package_wtxids.value() == std::vector<Wtxid>{ptx_mixed_child->GetWitnessHash()});
        }
    }
}

BOOST_AUTO_TEST_CASE(package_no_work_subsidy_tests)
{
    // Quicksilver is feeless (#5b) — there is no relay floor to rescue a parent from — and per-tx
    // surplus work is non-aggregatable (5c-2 decision 1.1.1). RelayPool acceptance is therefore
    // purely per-tx: each transaction stands or falls on its own per-tx PoW, and neither a child
    // nor a parent can subsidize the other. (The mining/eviction side of non-aggregation — that a
    // high-surplus child does not lift its parent in the surplus-work order — is covered by
    // miner_tests.)
    mineBlocks(5);
    LOCK(::cs_main);
    LOCK(m_node.relaypool->cs);
    size_t expected_pool_size = m_node.relaypool->size();
    CKey child_key = GenerateRandomKey();
    CScript parent_spk = GetScriptForDestination(WitnessV0KeyHash(child_key.GetPubKey()));
    CKey grandchild_key = GenerateRandomKey();
    CScript child_spk = GetScriptForDestination(WitnessV0KeyHash(grandchild_key.GetPubKey()));

    // (A) Positive control: a valid parent + valid child package is accepted purely on each
    // transaction's own per-tx validity (both feeless, both with a sound anchored proof).
    {
        CTransactionRef tx_parent = MakeFeelessProvenTx(/*input_tx=*/m_coinbase_txns[0], /*vout=*/0,
                                                        /*signing_key=*/coinbaseKey, /*out_spk=*/parent_spk);
        CTransactionRef tx_child = MakeFeelessProvenTx(/*input_tx=*/tx_parent, /*vout=*/0,
                                                       /*signing_key=*/child_key, /*out_spk=*/child_spk, /*input_height=*/101);
        Package package_ok{tx_parent, tx_child};
        const auto submit_ok = ProcessNewPackage(m_node.chainman->ActiveChainstate(), *m_node.relaypool, package_ok, /*test_accept=*/false);
        if (auto err{CheckPackageRelayPoolAcceptResult(package_ok, submit_ok, /*expect_valid=*/true, m_node.relaypool.get())}) {
            BOOST_ERROR(err.value());
        }
        expected_pool_size += 2;
        BOOST_CHECK_EQUAL(m_node.relaypool->size(), expected_pool_size);
    }

    // (B) No forward package rescue: a parent that FAILS its own per-tx PoW is not rescued by a
    // high-surplus child. tx_bad_parent carries a stale anchor; tx_rich_child is an
    // otherwise-valid high-surplus spend of it. The package is rejected on the parent's
    // per-tx PoW failure and nothing enters the relaypool — the child's surplus cannot
    // substitute for the parent's missing per-tx work.
    {
        CTransactionRef tx_bad_parent = MakeFeelessStaleAnchorTx(/*input_tx=*/m_coinbase_txns[1], /*vout=*/0,
                                                                 /*signing_key=*/coinbaseKey, /*out_spk=*/parent_spk);
        CTransactionRef tx_rich_child = MakeFeelessHighSurplusTx(/*input_tx=*/tx_bad_parent, /*vout=*/0,
                                                                 /*signing_key=*/child_key, /*out_spk=*/child_spk, /*input_height=*/101);
        Package package_bad_parent{tx_bad_parent, tx_rich_child};
        const auto submit_bad_parent = ProcessNewPackage(m_node.chainman->ActiveChainstate(), *m_node.relaypool, package_bad_parent, /*test_accept=*/false);
        BOOST_CHECK(!submit_bad_parent.m_state.IsValid());
        BOOST_CHECK_EQUAL(submit_bad_parent.m_state.GetResult(), PackageValidationResult::PCKG_TX);
        auto it_bad_parent = submit_bad_parent.m_tx_results.find(tx_bad_parent->GetWitnessHash());
        // F-137: TX_PREMATURE_SPEND, not TX_CONSENSUS. This expectation was wrong when it
        // was written -- a stale anchor is tip-relative, so it can never be grounds for
        // discouraging the peer that relayed it. See stale_anchor_rejection_is_not_bannable.
        BOOST_CHECK_EQUAL(it_bad_parent->second.m_state.GetResult(), TxValidationResult::TX_PREMATURE_SPEND);
        BOOST_CHECK_EQUAL(it_bad_parent->second.m_state.GetRejectReason(), "bad-txns-pow-anchor");
        BOOST_CHECK(!m_node.relaypool->exists(GenTxid::Txid(tx_bad_parent->GetHash())));
        BOOST_CHECK(!m_node.relaypool->exists(GenTxid::Txid(tx_rich_child->GetHash())));
        BOOST_CHECK_EQUAL(m_node.relaypool->size(), expected_pool_size);
    }

    // (C) No reverse subsidy: a valid parent is accepted on its own per-tx validity even when
    // its child is invalid. The parent is neither held back by nor "paying for" the child.
    {
        CTransactionRef tx_good_parent = MakeFeelessProvenTx(/*input_tx=*/m_coinbase_txns[2], /*vout=*/0,
                                                             /*signing_key=*/coinbaseKey, /*out_spk=*/parent_spk);
        CTransactionRef tx_bad_child = MakeFeelessStaleAnchorTx(/*input_tx=*/tx_good_parent, /*vout=*/0,
                                                                /*signing_key=*/child_key, /*out_spk=*/child_spk, /*input_height=*/101);
        Package package_bad_child{tx_good_parent, tx_bad_child};
        const auto submit_bad_child = ProcessNewPackage(m_node.chainman->ActiveChainstate(), *m_node.relaypool, package_bad_child, /*test_accept=*/false);
        BOOST_CHECK(!submit_bad_child.m_state.IsValid());
        auto it_good_parent = submit_bad_child.m_tx_results.find(tx_good_parent->GetWitnessHash());
        auto it_bad_child = submit_bad_child.m_tx_results.find(tx_bad_child->GetWitnessHash());
        BOOST_CHECK(it_good_parent->second.m_result_type == RelayPoolAcceptResult::ResultType::VALID);
        BOOST_CHECK(it_bad_child->second.m_result_type == RelayPoolAcceptResult::ResultType::INVALID);
        BOOST_CHECK_EQUAL(it_bad_child->second.m_state.GetRejectReason(), "bad-txns-pow-anchor");
        BOOST_CHECK(m_node.relaypool->exists(GenTxid::Txid(tx_good_parent->GetHash())));  // parent accepted alone
        BOOST_CHECK(!m_node.relaypool->exists(GenTxid::Txid(tx_bad_child->GetHash())));
        expected_pool_size += 1;
        BOOST_CHECK_EQUAL(m_node.relaypool->size(), expected_pool_size);
    }
}

// F-137: a per-tx anchor miss is TIP-RELATIVE, and must not be punishable.
//
// CheckTxAnchor fails for three reasons, and not one of them means "these bytes are
// invalid forever": the anchor is ahead of OUR tip (we are behind and will catch up),
// the anchor has aged out of the recency window (it was valid until recently), or the
// anchor height resolves to a block on a branch we do not have. In all three the very
// same transaction is valid to a peer at a different height.
//
// Classifying that TX_CONSENSUS made MaybePunishNodeForTx discourage the relaying peer
// (see the TX_CONSENSUS arm in src/net_processing.cpp), so a node a few blocks behind
// the tip banned honest peers for relaying valid payments -- worst exactly when heights
// are most ragged, which is at launch. TX_PREMATURE_SPEND is the enum's own name for
// tip-relative validity and falls through that switch's no-punish arm.
//
// The consensus verdict is unchanged and lives where it belongs: ConnectBlock runs
// CheckTxAnchor against pindex->pprev and still rejects the BLOCK. Relay leniency here
// buys no leniency there.
BOOST_AUTO_TEST_CASE(stale_anchor_rejection_is_not_bannable)
{
    mineBlocks(3);
    LOCK(cs_main);

    CKey key = GenerateRandomKey();
    CScript spk = GetScriptForDestination(PKHash(key.GetPubKey()));
    CTransactionRef tx_stale = MakeFeelessStaleAnchorTx(/*input_tx=*/m_coinbase_txns[0], /*vout=*/0,
                                                        /*signing_key=*/coinbaseKey, /*out_spk=*/spk);

    const RelayPoolAcceptResult result = m_node.chainman->ProcessTransaction(tx_stale);
    BOOST_REQUIRE(result.m_result_type == RelayPoolAcceptResult::ResultType::INVALID);
    BOOST_CHECK_EQUAL(result.m_state.GetRejectReason(), "bad-txns-pow-anchor");
    // The whole point of the flag: this must NOT be a result that MaybePunishNodeForTx
    // discourages a peer for.
    BOOST_CHECK_EQUAL(result.m_state.GetResult(), TxValidationResult::TX_PREMATURE_SPEND);
    BOOST_CHECK(!m_node.relaypool->exists(GenTxid::Txid(tx_stale->GetHash())));
}

BOOST_AUTO_TEST_CASE(package_replacement_tests)
{
    // Feeless / per-tx-PoW: package-level replacement (package replacement, including a child
    // "sponsoring" its parent's replacement) is unsupported — 5c-2 decision 1.1.1 makes
    // per-tx surplus work non-aggregatable, so the only supported bump is a standalone
    // single-tx replace-by-surplus-work. This test pins an incumbent package, shows that a
    // conflicting replacement *package* is rejected with package-replacement-unsupported-feeless
    // (even when the replacing parent would win a standalone replacement on its own
    // surplus), and shows the same replacement succeeds when submitted as a standalone tx.
    mineBlocks(5);
    LOCK(::cs_main);
    LOCK(m_node.relaypool->cs);
    size_t expected_pool_size = m_node.relaypool->size();
    CKey child_key{GenerateRandomKey()};
    CScript parent_spk = GetScriptForDestination(WitnessV0KeyHash(child_key.GetPubKey()));
    CKey grandchild_key{GenerateRandomKey()};
    CScript child_spk = GetScriptForDestination(WitnessV0KeyHash(grandchild_key.GetPubKey()));

    // ---- Part 1: a genuine package-replacement attempt (child sponsors its parent) is rejected ----
    // Incumbent package on coinbase[0]: parentA carries a HIGH surplus so no bare-proof
    // parent can out-surplus it on its own.
    CTransactionRef tx_parentA = MakeFeelessHighSurplusTx(/*input_tx=*/m_coinbase_txns[0], /*vout=*/0,
                                                          /*signing_key=*/coinbaseKey, /*out_spk=*/parent_spk);
    CTransactionRef tx_childA = MakeFeelessProvenTx(/*input_tx=*/tx_parentA, /*vout=*/0,
                                                    /*signing_key=*/child_key, /*out_spk=*/child_spk, /*input_height=*/101);
    Package package_incumbent{tx_parentA, tx_childA};
    const auto submit_incumbent = ProcessNewPackage(m_node.chainman->ActiveChainstate(), *m_node.relaypool, package_incumbent, /*test_accept=*/false);
    if (auto err{CheckPackageRelayPoolAcceptResult(package_incumbent, submit_incumbent, /*expect_valid=*/true, m_node.relaypool.get())}) {
        BOOST_ERROR(err.value());
    }
    expected_pool_size += 2;
    BOOST_CHECK_EQUAL(m_node.relaypool->size(), expected_pool_size);

    // Replacement package {parentB, childB}: parentB conflicts parentA but has only a BARE
    // proof (cannot out-surplus parentA alone), while childB carries a HIGH surplus. The
    // only way this could be admitted is by aggregating childB's surplus onto parentB — the
    // package-level replacement that #6 forbids. It must be rejected and the incumbent must
    // survive untouched.
    CKey keyB{GenerateRandomKey()};
    CScript parent_spk_B = GetScriptForDestination(WitnessV0KeyHash(keyB.GetPubKey()));
    CTransactionRef tx_parentB = MakeFeelessProvenTx(/*input_tx=*/m_coinbase_txns[0], /*vout=*/0,
                                                     /*signing_key=*/coinbaseKey, /*out_spk=*/parent_spk_B);
    CTransactionRef tx_childB = MakeFeelessHighSurplusTx(/*input_tx=*/tx_parentB, /*vout=*/0,
                                                         /*signing_key=*/keyB, /*out_spk=*/child_spk, /*input_height=*/101);
    Package package_sponsor{tx_parentB, tx_childB};
    const auto submit_sponsor = ProcessNewPackage(m_node.chainman->ActiveChainstate(), *m_node.relaypool, package_sponsor, /*test_accept=*/false);
    BOOST_CHECK(!submit_sponsor.m_state.IsValid());
    BOOST_CHECK_EQUAL(submit_sponsor.m_state.GetResult(), PackageValidationResult::PCKG_POLICY);
    BOOST_CHECK_EQUAL(submit_sponsor.m_state.GetRejectReason(), "package-replacement-unsupported-feeless");
    // RelayPool unchanged; the incumbent package survives, the sponsor package is not admitted.
    BOOST_CHECK_EQUAL(m_node.relaypool->size(), expected_pool_size);
    BOOST_CHECK(m_node.relaypool->exists(GenTxid::Txid(tx_parentA->GetHash())));
    BOOST_CHECK(m_node.relaypool->exists(GenTxid::Txid(tx_childA->GetHash())));
    BOOST_CHECK(!m_node.relaypool->exists(GenTxid::Txid(tx_parentB->GetHash())));
    BOOST_CHECK(!m_node.relaypool->exists(GenTxid::Txid(tx_childB->GetHash())));

    // ---- Part 2: the supported bump — a STANDALONE single-tx replace-by-surplus-work ----
    // Incumbent single tx on coinbase[1] with a bare proof.
    CTransactionRef tx_incumbent_c = MakeFeelessProvenTx(/*input_tx=*/m_coinbase_txns[1], /*vout=*/0,
                                                         /*signing_key=*/coinbaseKey, /*out_spk=*/parent_spk);
    const auto submit_c = ProcessNewPackage(m_node.chainman->ActiveChainstate(), *m_node.relaypool, Package{tx_incumbent_c}, /*test_accept=*/false);
    if (auto err{CheckPackageRelayPoolAcceptResult(Package{tx_incumbent_c}, submit_c, /*expect_valid=*/true, m_node.relaypool.get())}) {
        BOOST_ERROR(err.value());
    }
    expected_pool_size += 1;
    BOOST_CHECK_EQUAL(m_node.relaypool->size(), expected_pool_size);

    // A higher-surplus standalone conflict replaces it (child-of-nothing, single tx).
    CKey keyD{GenerateRandomKey()};
    CScript parent_spk_D = GetScriptForDestination(WitnessV0KeyHash(keyD.GetPubKey()));
    CTransactionRef tx_replacement_d = MakeFeelessHighSurplusTx(/*input_tx=*/m_coinbase_txns[1], /*vout=*/0,
                                                                /*signing_key=*/coinbaseKey, /*out_spk=*/parent_spk_D);
    const auto submit_d = ProcessNewPackage(m_node.chainman->ActiveChainstate(), *m_node.relaypool, Package{tx_replacement_d}, /*test_accept=*/false);
    if (auto err{CheckPackageRelayPoolAcceptResult(Package{tx_replacement_d}, submit_d, /*expect_valid=*/true, m_node.relaypool.get())}) {
        BOOST_ERROR(err.value());
    }
    BOOST_CHECK(!m_node.relaypool->exists(GenTxid::Txid(tx_incumbent_c->GetHash()))); // replaced
    BOOST_CHECK(m_node.relaypool->exists(GenTxid::Txid(tx_replacement_d->GetHash())));
    BOOST_CHECK_EQUAL(m_node.relaypool->size(), expected_pool_size); // 1-for-1 replacement
}

struct TinyRelayPoolSetup : TxPackageTest {
    static constexpr int64_t TINY_POOL_BYTES{20'000};
    TinyRelayPoolSetup() : TxPackageTest{{.relaypool_max_size_bytes = TINY_POOL_BYTES}} {}
};

BOOST_FIXTURE_TEST_CASE(full_pool_rejection_is_relaypool_policy, TinyRelayPoolSetup)
{
    // F-155: AcceptToMemoryPool used to report TX_RECONSIDERABLE for a tx that
    // LimitRelayPoolSize trimmed. That schedules a 1p1c hunt, but a child cannot
    // supply work for its parent. The package-submission path already used
    // TX_RELAYPOOL_POLICY for the same "relaypool full" reason.
    mineBlocks(20);
    LOCK(::cs_main);
    CTxRelayPool& pool{*Assert(m_node.relaypool)};
    BOOST_REQUIRE_EQUAL(pool.m_opts.max_size_bytes, TINY_POOL_BYTES);

    CKey dest_key{GenerateRandomKey()};
    CScript dest{GetScriptForDestination(WitnessV0KeyHash(dest_key.GetPubKey()))};

    std::vector<CTransactionRef> incumbents;
    CTransactionRef probe_input;
    for (const auto& coinbase : m_coinbase_txns) {
        CTransactionRef tx{MakeFeelessHighSurplusTx(/*input_tx=*/coinbase, /*vout=*/0,
                                                    /*signing_key=*/coinbaseKey, /*out_spk=*/dest)};
        const RelayPoolAcceptResult res{m_node.chainman->ProcessTransaction(tx)};
        if (res.m_result_type == RelayPoolAcceptResult::ResultType::VALID) {
            incumbents.push_back(std::move(tx));
            continue;
        }
        const std::string reason{res.m_state.GetRejectReason()};
        if (reason == "bad-txns-premature-spend-of-coinbase") continue;
        BOOST_REQUIRE_EQUAL(reason, "relaypool full");
        probe_input = coinbase;
        break;
    }
    BOOST_REQUIRE_MESSAGE(probe_input, "tiny pool never filled; max_size_bytes plumbing did not take effect");
    BOOST_REQUIRE(!incumbents.empty());

    CTransactionRef probe{MakeFeelessProvenTx(/*input_tx=*/probe_input, /*vout=*/0,
                                             /*signing_key=*/coinbaseKey, /*out_spk=*/dest)};
    const RelayPoolAcceptResult probe_res{m_node.chainman->ProcessTransaction(probe)};
    BOOST_CHECK_EQUAL(probe_res.m_state.GetResult(), TxValidationResult::TX_RELAYPOOL_POLICY);
    BOOST_CHECK_EQUAL(probe_res.m_state.GetRejectReason(), "relaypool full");
    BOOST_CHECK(!pool.exists(GenTxid::Txid(probe->GetHash())));
    BOOST_CHECK(pool.exists(GenTxid::Txid(incumbents.front()->GetHash())));
}
BOOST_AUTO_TEST_SUITE_END()
