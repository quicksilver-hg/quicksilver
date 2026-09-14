// Copyright (c) 2026 The Quicksilver developers / Distributed under the MIT software license.
#include <addresstype.h>
#include <chainparams.h>
#include <consensus/amount.h>
#include <key.h>
#include <node/miner.h>
#include <script/script.h>
#include <pow.h>
#include <arith_uint256.h>
#include <crypto/cuckatoo/cuckatoo.h>
#include <rpc/mining.h>
#include <univalue.h>
#include <test/util/setup_common.h>
#include <test/util/txrelaypool.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

using node::BlockAssembler;

namespace {
struct MiningAnchorSetup : public TestChain100Setup {
    MiningAnchorSetup()
    {
        // Trivial-but-real per-tx PoW: cycle skipped, target + anchor still enforced.
        const_cast<Consensus::Params&>(Params().GetConsensus()).fTxPowNoCycle = true;
    }

    // Build a feeless (in == out) tx spending input_tx:0, anchored to `anchor_height`
    // (its block hash resolved from the active chain) with a cheap proof that meets the
    // per-tx target. Returns the signed, proven tx. Does NOT submit.
    CTransactionRef MakeProvenTxAtAnchor(const CTransactionRef& input_tx, const CKey& key,
                                         const CScript& spk, int anchor_height)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        CTxOut out{input_tx->vout[0].nValue, spk}; // in == out
        CMutableTransaction mtx = CreateValidTransaction(
            {input_tx}, {COutPoint(input_tx->GetHash(), 0)}, /*input_height=*/0, {key},
            {out});
        const CBlockIndex* anchor{m_node.chainman->ActiveChain()[anchor_height]};
        assert(anchor);
        mtx.nAnchorHeight = anchor_height;
        const uint256 target{GetTxPowTarget(Params().GetConsensus(), anchor, CTransaction(mtx))};
        for (uint32_t i = 0; i < mtx.nCycle.size(); ++i) mtx.nCycle[i] = i + 1;
        while (UintToArith256(cuckatoo::CuckatooProofHash(mtx.nCycle)) > UintToArith256(target)) {
            mtx.nCycle[0] += static_cast<uint32_t>(mtx.nCycle.size());
        }
        return MakeTransactionRef(mtx);
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(mining_anchor_tests, MiningAnchorSetup)

// A relaypool tx whose anchor has gone stale is NOT selected into a new block, while a
// fresh-anchor sibling IS.
BOOST_AUTO_TEST_CASE(stale_anchor_excluded_from_block)
{
    LOCK(cs_main);
    const int W{Params().GetConsensus().nMaxAnchorAge};
    const int tip_height{m_node.chainman->ActiveChain().Height()}; // 100
    CKey key; key.MakeNewKey(true);
    CScript spk{GetScriptForDestination(WitnessV0KeyHash(key.GetPubKey()))};

    // Fresh: anchored at the tip (valid for this candidate height).
    CTransactionRef fresh{MakeProvenTxAtAnchor(m_coinbase_txns[0], coinbaseKey, spk, tip_height)};
    // Stale: anchored W+1 blocks back (already outside the window at the candidate height).
    CTransactionRef stale{MakeProvenTxAtAnchor(m_coinbase_txns[1], coinbaseKey, spk, tip_height - (W + 1))};

    TestRelayPoolEntryHelper entry;
    {
        LOCK(m_node.relaypool->cs);
        AddToRelayPool(*m_node.relaypool, entry.FromTx(fresh));
        AddToRelayPool(*m_node.relaypool, entry.FromTx(stale));
    }

    auto tmpl = BlockAssembler{m_node.chainman->ActiveChainstate(), m_node.relaypool.get(), {}}.CreateNewBlock();
    BOOST_REQUIRE(tmpl);
    std::set<uint256> in_block;
    for (const auto& tx : tmpl->block.vtx) in_block.insert(tx->GetHash());

    BOOST_CHECK(in_block.count(fresh->GetHash()));    // fresh included
    BOOST_CHECK(!in_block.count(stale->GetHash()));   // stale skipped
}

// The coinbase mint equals subsidy + (nTxPowMint per anchor-valid included tx); a stale
// tx contributes neither a block entry nor a mint.
BOOST_AUTO_TEST_CASE(mint_claim_matches_anchor_valid_set)
{
    LOCK(cs_main);
    const Consensus::Params& params{Params().GetConsensus()};
    const int W{params.nMaxAnchorAge};
    const int tip_height{m_node.chainman->ActiveChain().Height()};
    CKey key; key.MakeNewKey(true);
    CScript spk{GetScriptForDestination(WitnessV0KeyHash(key.GetPubKey()))};

    CTransactionRef fresh{MakeProvenTxAtAnchor(m_coinbase_txns[0], coinbaseKey, spk, tip_height)};
    CTransactionRef stale{MakeProvenTxAtAnchor(m_coinbase_txns[1], coinbaseKey, spk, tip_height - (W + 1))};
    {
        LOCK(m_node.relaypool->cs);
        TestRelayPoolEntryHelper entry;
        AddToRelayPool(*m_node.relaypool, entry.FromTx(fresh));
        AddToRelayPool(*m_node.relaypool, entry.FromTx(stale));
    }

    auto tmpl = BlockAssembler{m_node.chainman->ActiveChainstate(), m_node.relaypool.get(), {}}.CreateNewBlock();
    BOOST_REQUIRE(tmpl);
    // Exactly one relaypool tx (fresh) is included ⇒ mint = subsidy + 1*nTxPowMint (feeless ⇒ nFees=0).
    const CAmount expected{GetBlockSubsidy(tip_height + 1, params) + params.nTxPowMint};
    BOOST_CHECK_EQUAL(tmpl->block.vtx[0]->vout[0].nValue, expected);
}

// removeStaleAnchors evicts a stale-anchor tx AND its in-relaypool descendants, leaving
// fresh txs in place.
BOOST_AUTO_TEST_CASE(stale_anchor_relaypool_eviction_cascades)
{
    LOCK(cs_main);
    LOCK(m_node.relaypool->cs);
    const Consensus::Params& params{Params().GetConsensus()};
    const int W{params.nMaxAnchorAge};
    const int tip_height{m_node.chainman->ActiveChain().Height()};
    CKey key; key.MakeNewKey(true);
    CScript spk{GetScriptForDestination(WitnessV0KeyHash(key.GetPubKey()))};

    CTransactionRef stale_parent{MakeProvenTxAtAnchor(m_coinbase_txns[0], coinbaseKey, spk, tip_height - (W + 1))};
    CTransactionRef child{MakeProvenTxAtAnchor(stale_parent, key, spk, tip_height)};       // fresh, but spends stale_parent
    CTransactionRef fresh{MakeProvenTxAtAnchor(m_coinbase_txns[1], coinbaseKey, spk, tip_height)};
    TestRelayPoolEntryHelper entry;
    AddToRelayPool(*m_node.relaypool, entry.FromTx(stale_parent));
    AddToRelayPool(*m_node.relaypool, entry.FromTx(child));
    AddToRelayPool(*m_node.relaypool, entry.FromTx(fresh));
    BOOST_REQUIRE_EQUAL(m_node.relaypool->size(), 3U);

    m_node.relaypool->removeStaleAnchors(tip_height, params);

    BOOST_CHECK(!m_node.relaypool->exists(GenTxid::Txid(stale_parent->GetHash()))); // evicted
    BOOST_CHECK(!m_node.relaypool->exists(GenTxid::Txid(child->GetHash())));        // descendant cascade
    BOOST_CHECK(m_node.relaypool->exists(GenTxid::Txid(fresh->GetHash())));         // fresh kept
    BOOST_CHECK_EQUAL(m_node.relaypool->size(), 1U);
}

BOOST_AUTO_TEST_CASE(block_pow_descriptor_fields)
{
    // sandbox-style: trivial cycle, edgebits 19
    Consensus::Params p = Params().GetConsensus();
    arith_uint256 target = arith_uint256().SetCompact(0x207fffff);
    const UniValue d = BlockPowDescriptor(p, target);
    BOOST_CHECK_EQUAL(d["algorithm"].get_str(), "cuckatoo");
    BOOST_CHECK_EQUAL(d["edgebits"].getInt<int>(), (int)p.nEdgeBits);  // 19 on sandbox
    BOOST_CHECK_EQUAL(d["proofsize"].getInt<int>(), 42);
    BOOST_CHECK_EQUAL(d["proofhash"].get_str(), "blake2b");
    // 84 = the pre-pow through nNonce: 4 + 32 + 32 + 4 (nTime) + 4 (nBits)
    //      + 4 (nCongestion) + 4 (nNonce). Grew from 80 when nCongestion entered the
    //      header (#5c-1 Phase 2); it is a fact about the format, not a tolerance.
    BOOST_CHECK_EQUAL(d["prepowsize"].getInt<int>(), 84);
    BOOST_CHECK_EQUAL(d["prepowsize"].getInt<int>(), (int)CBlockHeader::PREPOW_SIZE);
    BOOST_CHECK_EQUAL(d["proofhashtarget"].get_str(), target.GetHex());
    BOOST_CHECK(!d["cycleencoding"].get_str().empty());
    // A miner that mutates the template's transaction set must recompute nCongestion,
    // so the recurrence inputs have to be published alongside it.
    const UniValue& cp = d["congestionparams"];
    BOOST_CHECK_EQUAL(cp["one"].getInt<int64_t>(), (int64_t)CONGESTION_ONE);
    BOOST_CHECK_EQUAL(cp["targetpermille"].getInt<int>(), p.nCongestionTargetPermille);
    BOOST_CHECK_EQUAL(cp["stepdenom"].getInt<int>(), p.nCongestionStepDenom);
    BOOST_CHECK_EQUAL(cp["maxmultiplier"].getInt<int>(), p.nCongestionMaxMultiplier);
    BOOST_CHECK_EQUAL(d["trivialcycle"].get_bool(), true);  // sandbox fBlockPowNoCycle

    // prod-style: real cycle, edgebits 29 (unified block/tx graph size) -> trivialcycle omitted
    Consensus::Params prod = p;
    prod.nEdgeBits = 29;
    prod.fBlockPowNoCycle = false;
    const UniValue dp = BlockPowDescriptor(prod, target);
    BOOST_CHECK_EQUAL(dp["edgebits"].getInt<int>(), 29);
    BOOST_CHECK(dp["trivialcycle"].isNull());  // absent on real networks
}

BOOST_AUTO_TEST_SUITE_END()
