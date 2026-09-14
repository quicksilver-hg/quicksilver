// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <addresstype.h>
#include <arith_uint256.h>
#include <chainparams.h>
#include <coins.h>
#include <common/system.h>
#include <crypto/cuckatoo/cuckatoo.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <consensus/tx_verify.h>
#include <interfaces/mining.h>
#include <node/block_solve.h>
#include <node/miner.h>
#include <policy/policy.h>
#include <consensus/validation.h>
#include <pow.h>
#include <test/util/mining.h>
#include <test/util/random.h>
#include <test/util/transaction_utils.h>
#include <test/util/txrelaypool.h>
#include <txrelaypool.h>
#include <uint256.h>
#include <util/check.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <util/translation.h>
#include <validation.h>
#include <versionbits.h>

#include <test/util/setup_common.h>

#include <memory>
#include <vector>

#include <boost/test/unit_test.hpp>

using namespace util::hex_literals;
using interfaces::BlockTemplate;
using interfaces::Mining;
using node::BlockAssembler;

namespace miner_tests {
// Quicksilver: run on sandbox, not the TestingSetup MAIN default. Mainnet block
// PoW is real Cuckatoo E28 (unmineable in-process); sandbox's fBlockPowNoCycle
// makes block PoW trivial so the import loop can SolveBlockPoW each block. This
// matches the other mining fixtures (TestChain100Setup, validation_block_tests).
struct MinerTestingSetup : public SandboxingSetup {
    MinerTestingSetup()
    {
        // Opt in (this assembly-focused suite only) to skipping the expensive per-tx
        // Cuckatoo cycle verification, so blocks carrying many injected txs validate
        // without an infeasible per-tx grind. The per-tx target + anchor checks still
        // apply (see ProveTxPow); real CuckatooVerify stays on in txpow/mint/frame_b.
        Consensus::Params& params{const_cast<Consensus::Params&>(Params().GetConsensus())};
        params.fTxPowNoCycle = true;

        // The sequence-lock sub-cases in TestBasicMining were written for a chain BELOW
        // CSV/BIP68 activation: upstream runs miner_tests on the mainnet TestingSetup
        // (CSVHeight=419328, far above the ~110-block test chain), so BIP68 is never
        // enforced and the relative-locked txs "still generate a valid template until
        // BIP68 soft fork" (the test's own words). Sandbox buries CSV at height 1, which
        // would enforce BIP68 and reject those txs as non-final. Push CSV activation above
        // every height this suite reaches (the subsidy ramp climbs to nBootstrapBlocks-1 =
        // 149) to restore the precondition the assertions were authored against — this
        // preserves the original 3U/5U expectations rather than weakening them.
        params.CSVHeight = 100000;
    }

    // Give a tx a valid per-tx PoW under fTxPowNoCycle: anchor it to the current tip
    // and grind the cheap proof-hash to meet the per-tx target (no cycle solve). Must
    // be called before the tx's hash is taken (nAnchorHeight + nCycle bind the txid).
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

    void TestNoPackageBoost(const CScript& scriptPubKey, const std::vector<CTransactionRef>& txFirst) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    void TestBasicMining(const CScript& scriptPubKey, const std::vector<CTransactionRef>& txFirst, int baseheight) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    void TestSurplusWorkMining(const CScript& scriptPubKey, const std::vector<CTransactionRef>& txFirst) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    bool TestSequenceLocks(const CTransaction& tx, CTxRelayPool& tx_relaypool) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        CCoinsViewRelayPool view_relaypool{&m_node.chainman->ActiveChainstate().CoinsTip(), tx_relaypool};
        CBlockIndex* tip{m_node.chainman->ActiveChain().Tip()};
        const std::optional<LockPoints> lock_points{CalculateLockPointsAtTip(tip, view_relaypool, tx)};
        return lock_points.has_value() && CheckSequenceLocksAtTip(tip, *lock_points);
    }
    CTxRelayPool& MakeRelayPool()
    {
        // Delete the previous relaypool to ensure with valgrind that the old
        // pointer is not accessed, when the new one should be accessed
        // instead.
        m_node.relaypool.reset();
        bilingual_str error;
        m_node.relaypool = std::make_unique<CTxRelayPool>(RelayPoolOptionsForTest(m_node), error);
        Assert(error.empty());
        return *m_node.relaypool;
    }
    std::unique_ptr<Mining> MakeMining()
    {
        return interfaces::MakeMining(m_node);
    }
};
} // namespace miner_tests

BOOST_FIXTURE_TEST_SUITE(miner_tests, MinerTestingSetup)

constexpr static struct {
    unsigned char extranonce;
    unsigned int nonce;
} BLOCKINFO[]{{8, 582909131},  {0, 971462344},  {2, 1169481553}, {6, 66147495},  {7, 427785981},  {8, 80538907},
              {8, 207348013},  {2, 1951240923}, {4, 215054351},  {1, 491520534}, {8, 1282281282}, {4, 639565734},
              {3, 248274685},  {8, 1160085976}, {6, 396349768},  {5, 393780549}, {5, 1096899528}, {4, 965381630},
              {0, 728758712},  {5, 318638310},  {3, 164591898},  {2, 274234550}, {2, 254411237},  {7, 561761812},
              {2, 268342573},  {0, 402816691},  {1, 221006382},  {6, 538872455}, {7, 393315655},  {4, 814555937},
              {7, 504879194},  {6, 467769648},  {3, 925972193},  {2, 200581872}, {3, 168915404},  {8, 430446262},
              {5, 773507406},  {3, 1195366164}, {0, 433361157},  {3, 297051771}, {0, 558856551},  {2, 501614039},
              {3, 528488272},  {2, 473587734},  {8, 230125274},  {2, 494084400}, {4, 357314010},  {8, 60361686},
              {7, 640624687},  {3, 480441695},  {8, 1424447925}, {4, 752745419}, {1, 288532283},  {6, 669170574},
              {5, 1900907591}, {3, 555326037},  {3, 1121014051}, {0, 545835650}, {8, 189196651},  {5, 252371575},
              {0, 199163095},  {6, 558895874},  {6, 1656839784}, {6, 815175452}, {6, 718677851},  {5, 544000334},
              {0, 340113484},  {6, 850744437},  {4, 496721063},  {8, 524715182}, {6, 574361898},  {6, 1642305743},
              {6, 355110149},  {5, 1647379658}, {8, 1103005356}, {7, 556460625}, {3, 1139533992}, {5, 304736030},
              {2, 361539446},  {2, 143720360},  {6, 201939025},  {7, 423141476}, {4, 574633709},  {3, 1412254823},
              {4, 873254135},  {0, 341817335},  {6, 53501687},   {3, 179755410}, {5, 172209688},  {8, 516810279},
              {4, 1228391489}, {8, 325372589},  {6, 550367589},  {0, 876291812}, {7, 412454120},  {7, 717202854},
              {2, 222677843},  {6, 251778867},  {7, 842004420},  {7, 194762829}, {4, 96668841},   {1, 925485796},
              {0, 792342903},  {6, 678455063},  {6, 773251385},  {5, 186617471}, {6, 883189502},  {7, 396077336},
              {8, 254702874},  {0, 455592851}};

static std::unique_ptr<CBlockIndex> CreateBlockIndex(int nHeight, CBlockIndex* active_chain_tip) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    auto index{std::make_unique<CBlockIndex>()};
    index->nHeight = nHeight;
    index->pprev = active_chain_tip;
    return index;
}

// Test suite for surplus-work (#5c-2) inclusion ranking inside the miner.
// Quicksilver ranks txs by per-tx surplus-work rate (surplus/vsize), individually
// — there is no package boost (Approach B). Implemented as an additional function,
// rather than a separate test case, to reuse the real blockchain (real coinbase
// inputs + real anchored per-tx PoW) created in CreateNewBlock_validity. The
// unit-level ranking/eviction proofs live in txwork_ranking_tests; this is the
// miner integration angle: real proofs passing test_block_validity WHILE surplus
// order governs inclusion.
//
// Each entry carries a REAL anchored proof (ProveTxPow → passes per-tx PoW validation) and
// an explicit surplus override (TxWorkSurplus → drives relaypool/miner ordering). All txs are
// structurally identical (1 input spending an anyone-can-spend output, 1 output), so vsizes
// match and rate is monotonic in the surplus value.
void MinerTestingSetup::TestNoPackageBoost(const CScript& scriptPubKey, const std::vector<CTransactionRef>& txFirst)
{
    CTxRelayPool& tx_relaypool{MakeRelayPool()};
    auto mining{MakeMining()};
    BlockAssembler::Options options;
    options.coinbase_output_script = scriptPubKey;

    LOCK(tx_relaypool.cs);
    TestRelayPoolEntryHelper entry;

    // Build a tx spending `prevhash:prevn`, forwarding `in_value` unchanged (feeless),
    // with a real anchored proof and the given surplus override. Returns its txid.
    auto add_tx = [&](const Txid& prevhash, uint32_t prevn, CAmount in_value,
                      uint64_t surplus, bool spends_coinbase) -> Txid {
        CMutableTransaction tx;
        tx.vin.resize(1);
        tx.vin[0].scriptSig = CScript() << OP_1;
        tx.vin[0].prevout.hash = prevhash;
        tx.vin[0].prevout.n = prevn;
        tx.vout.resize(1);
        tx.vout[0].nValue = in_value; // feeless: in == out
        tx.vout[0].scriptPubKey = CScript() << OP_1; // anyone-can-spend, so a child can spend it
        ProveTxPow(tx);
        const Txid hash = tx.GetHash();
        AddToRelayPool(tx_relaypool, entry.TxWorkSurplus(arith_uint256(surplus))
                                      .Time(Now<NodeSeconds>()).SpendsCoinbase(spends_coinbase).FromTx(tx));
        return hash;
    };

    // Independent coinbase-spends with well-separated surpluses, plus a low-surplus parent
    // whose higher-surplus child would, if packages were ranked, drag the parent ahead of `mid`.
    //   parent  surplus 200 (spends txFirst[0])
    //   child   surplus 400 (spends parent)        -- child_individual(400) < mid(500)
    //   mid     surplus 500 (spends txFirst[1])     -- but parent+child combined(600) > mid(500)
    //   hi      surplus 900 (spends txFirst[2])
    const Txid hashParent = add_tx(txFirst[0]->GetHash(), 0, txFirst[0]->vout[0].nValue, 200, /*spends_coinbase=*/true);
    const Txid hashChild  = add_tx(hashParent,            0, txFirst[0]->vout[0].nValue, 400, /*spends_coinbase=*/false);
    const Txid hashMid    = add_tx(txFirst[1]->GetHash(), 0, txFirst[1]->vout[0].nValue, 500, /*spends_coinbase=*/true);
    const Txid hashHi     = add_tx(txFirst[2]->GetHash(), 0, txFirst[2]->vout[0].nValue, 900, /*spends_coinbase=*/true);

    std::unique_ptr<BlockTemplate> block_template = mining->createNewBlock(options);
    BOOST_REQUIRE(block_template);
    CBlock block{block_template->getBlock()};

    // coinbase + 4 relaypool txs.
    BOOST_REQUIRE_EQUAL(block.vtx.size(), 5U);

    // Approach-B (per-tx surplus rate, highest first; a selected tx pulls in its
    // unconfirmed ancestors only for topology, not a package rate boost):
    //   hi(900) -> mid(500) -> child(400) pulls parent(200) -> [parent, child]
    // => block order: hi, mid, parent, child.
    BOOST_CHECK(block.vtx[1]->GetHash() == hashHi);
    BOOST_CHECK(block.vtx[2]->GetHash() == hashMid);
    BOOST_CHECK(block.vtx[3]->GetHash() == hashParent);
    BOOST_CHECK(block.vtx[4]->GetHash() == hashChild);

    // The no-package-ranking guarantee, stated directly: `mid` (individual surplus 500)
    // is mined BEFORE `parent` (individual surplus 200), even though parent+child
    // combined (600) exceeds mid. A package ranking would have placed parent/child first.
    auto pos = [&](const Txid& h) -> size_t {
        for (size_t i = 0; i < block.vtx.size(); ++i)
            if (block.vtx[i]->GetHash() == h) return i;
        return block.vtx.size();
    };
    BOOST_CHECK(pos(hashMid) < pos(hashParent));
    // Topology is still respected: a parent always precedes its child.
    BOOST_CHECK(pos(hashParent) < pos(hashChild));
}

void MinerTestingSetup::TestBasicMining(const CScript& scriptPubKey, const std::vector<CTransactionRef>& txFirst, int baseheight)
{
    Txid hash;
    CMutableTransaction tx;
    TestRelayPoolEntryHelper entry;
    entry.nHeight = 11;
    // Feeless: entries carry no fee (the miner would otherwise add phantom fees to
    // the coinbase that ConnectBlock, which sees in == out, rejects as bad-cb-amount).

    auto mining{MakeMining()};
    BOOST_REQUIRE(mining);

    BlockAssembler::Options options;
    options.coinbase_output_script = scriptPubKey;

    {
        CTxRelayPool& tx_relaypool{MakeRelayPool()};
        LOCK(tx_relaypool.cs);

        // Just to make sure we can still make simple blocks
        auto block_template{mining->createNewBlock(options)};
        BOOST_REQUIRE(block_template);
        CBlock block{block_template->getBlock()};

        // block sigops > limit: 1000 CHECKMULTISIG + 1
        tx.vin.resize(1);
        // NOTE: OP_NOP is used to force 20 SigOps for the CHECKMULTISIG
        tx.vin[0].scriptSig = CScript() << OP_0 << OP_0 << OP_0 << OP_NOP << OP_CHECKMULTISIG << OP_1;
        tx.vin[0].prevout.hash = txFirst[0]->GetHash();
        tx.vin[0].prevout.n = 0;
        tx.vout.resize(1);
        // Feeless (in == out): each tx forwards its full input value; the chain
        // spends txFirst[0] then each prior tx, all carrying the same value.
        tx.vout[0].nValue = txFirst[0]->vout[0].nValue;
        for (unsigned int i = 0; i < 1001; ++i) {
            ProveTxPow(tx); // anchor + cheap per-tx proof before the txid is fixed
            hash = tx.GetHash();
            bool spendsCoinbase = i == 0; // only first tx spends coinbase
            // If we don't set the # of sig ops in the CTxRelayPoolEntry, template creation fails
            AddToRelayPool(tx_relaypool, entry.Time(Now<NodeSeconds>()).SpendsCoinbase(spendsCoinbase).FromTx(tx));
            tx.vin[0].prevout.hash = hash;
        }

        BOOST_CHECK_EXCEPTION(mining->createNewBlock(options), std::runtime_error, HasReason("bad-blk-sigops"));
    }

    {
        CTxRelayPool& tx_relaypool{MakeRelayPool()};
        LOCK(tx_relaypool.cs);

        tx.vin[0].prevout.hash = txFirst[0]->GetHash();
        tx.vout[0].nValue = txFirst[0]->vout[0].nValue; // feeless: in == out
        for (unsigned int i = 0; i < 1001; ++i) {
            ProveTxPow(tx);
            hash = tx.GetHash();
            bool spendsCoinbase = i == 0; // only first tx spends coinbase
            // If we do set the # of sig ops in the CTxRelayPoolEntry, template creation passes
            AddToRelayPool(tx_relaypool, entry.Time(Now<NodeSeconds>()).SpendsCoinbase(spendsCoinbase).SigOpsCost(80).FromTx(tx));
            tx.vin[0].prevout.hash = hash;
        }
        BOOST_REQUIRE(mining->createNewBlock(options));
    }

    {
        CTxRelayPool& tx_relaypool{MakeRelayPool()};
        LOCK(tx_relaypool.cs);

        // block size > limit
        tx.vin[0].scriptSig = CScript();
        // 18 * (520char + DROP) + OP_1 = 9433 bytes
        std::vector<unsigned char> vchData(520);
        for (unsigned int i = 0; i < 18; ++i) {
            tx.vin[0].scriptSig << vchData << OP_DROP;
        }
        tx.vin[0].scriptSig << OP_1;
        tx.vin[0].prevout.hash = txFirst[0]->GetHash();
        tx.vout[0].nValue = txFirst[0]->vout[0].nValue; // feeless: in == out
        for (unsigned int i = 0; i < 128; ++i) {
            ProveTxPow(tx);
            hash = tx.GetHash();
            bool spendsCoinbase = i == 0; // only first tx spends coinbase
            AddToRelayPool(tx_relaypool, entry.Time(Now<NodeSeconds>()).SpendsCoinbase(spendsCoinbase).FromTx(tx));
            tx.vin[0].prevout.hash = hash;
        }
        BOOST_REQUIRE(mining->createNewBlock(options));
    }

    {
        CTxRelayPool& tx_relaypool{MakeRelayPool()};
        LOCK(tx_relaypool.cs);

        // orphan in tx_relaypool, template creation fails
        ProveTxPow(tx); // pass per-tx PoW so the failure is the missing input, not tx-pow
        hash = tx.GetHash();
        AddToRelayPool(tx_relaypool, entry.Time(Now<NodeSeconds>()).FromTx(tx));
        BOOST_CHECK_EXCEPTION(mining->createNewBlock(options), std::runtime_error, HasReason("bad-txns-inputs-missingorspent"));
    }

    {
        CTxRelayPool& tx_relaypool{MakeRelayPool()};
        LOCK(tx_relaypool.cs);

        // parent + child package: both should be included (per-tx scoring; the old
        // "child higher rate than parent" package framing no longer applies). Feeless.
        tx.vin.resize(1);
        tx.vin[0].scriptSig = CScript() << OP_1;
        tx.vin[0].prevout.hash = txFirst[1]->GetHash();
        tx.vout[0].nValue = txFirst[1]->vout[0].nValue; // feeless: in == out
        ProveTxPow(tx);
        hash = tx.GetHash();
        AddToRelayPool(tx_relaypool, entry.Time(Now<NodeSeconds>()).SpendsCoinbase(true).FromTx(tx));
        const CAmount parent_out = tx.vout[0].nValue;
        tx.vin[0].prevout.hash = hash;
        tx.vin.resize(2);
        tx.vin[1].scriptSig = CScript() << OP_1;
        tx.vin[1].prevout.hash = txFirst[0]->GetHash();
        tx.vin[1].prevout.n = 0;
        tx.vout[0].nValue = parent_out + txFirst[0]->vout[0].nValue; // feeless: sum of both inputs
        ProveTxPow(tx);
        hash = tx.GetHash();
        AddToRelayPool(tx_relaypool, entry.Time(Now<NodeSeconds>()).SpendsCoinbase(true).FromTx(tx));
        BOOST_REQUIRE(mining->createNewBlock(options));
    }

    {
        CTxRelayPool& tx_relaypool{MakeRelayPool()};
        LOCK(tx_relaypool.cs);

        // coinbase in tx_relaypool, template creation fails
        tx.vin.resize(1);
        tx.vin[0].prevout.SetNull();
        tx.vin[0].scriptSig = CScript() << OP_0 << OP_1;
        tx.vout[0].nValue = 0;
        hash = tx.GetHash();
        // Null-prevout tx is a coinbase => per-tx-PoW exempt (no ProveTxPow needed).
        AddToRelayPool(tx_relaypool, entry.Time(Now<NodeSeconds>()).SpendsCoinbase(false).FromTx(tx));
        // Should throw bad-cb-multiple
        BOOST_CHECK_EXCEPTION(mining->createNewBlock(options), std::runtime_error, HasReason("bad-cb-multiple"));
    }

    {
        CTxRelayPool& tx_relaypool{MakeRelayPool()};
        LOCK(tx_relaypool.cs);

        // double spend txn pair in tx_relaypool, template creation fails. Feeless
        // (in == out): both spend txFirst[0] and forward its full value; they differ
        // only by output script, so they conflict on the same input.
        tx.vin[0].prevout.hash = txFirst[0]->GetHash();
        tx.vin[0].scriptSig = CScript() << OP_1;
        tx.vout[0].nValue = txFirst[0]->vout[0].nValue;
        tx.vout[0].scriptPubKey = CScript() << OP_1;
        ProveTxPow(tx);
        hash = tx.GetHash();
        AddToRelayPool(tx_relaypool, entry.Time(Now<NodeSeconds>()).SpendsCoinbase(true).FromTx(tx));
        tx.vout[0].scriptPubKey = CScript() << OP_2;
        ProveTxPow(tx);
        hash = tx.GetHash();
        AddToRelayPool(tx_relaypool, entry.Time(Now<NodeSeconds>()).SpendsCoinbase(true).FromTx(tx));
        BOOST_CHECK_EXCEPTION(mining->createNewBlock(options), std::runtime_error, HasReason("bad-txns-inputs-missingorspent"));
    }

    {
        CTxRelayPool& tx_relaypool{MakeRelayPool()};
        LOCK(tx_relaypool.cs);

        // Quicksilver subsidy ramp (#5a): there is no halving. The per-block subsidy
        // follows a linear ramp from nInitialSubsidy down to nTailSubsidy, reaching the
        // flat tail exactly at nBootstrapBlocks (N). We straddle N and assert the
        // assembler pays the exact ramp/tail value.
        const Consensus::Params& consensus = Params().GetConsensus();
        const int N = consensus.nBootstrapBlocks;
        int nHeight = m_node.chainman->ActiveChain().Height();
        // Grow a fake-index chain so the next block to mine sits at N-1 (still on the ramp).
        while (m_node.chainman->ActiveChain().Tip()->nHeight < N - 2) {
            CBlockIndex* prev = m_node.chainman->ActiveChain().Tip();
            CBlockIndex* next = new CBlockIndex();
            next->phashBlock = new uint256(m_rng.rand256());
            m_node.chainman->ActiveChainstate().CoinsTip().SetBestBlock(next->GetBlockHash());
            next->pprev = prev;
            next->nHeight = prev->nHeight + 1;
            next->BuildSkip();
            m_node.chainman->ActiveChain().SetTip(*next);
        }
        {
            auto bt{mining->createNewBlock(options)};
            BOOST_REQUIRE(bt);
            // Coinbase pays the ramp subsidy for the block being built (tip+1 = N-1).
            // RelayPool is empty here, so there is no Frame-B mint to add.
            BOOST_CHECK_EQUAL(bt->getBlock().vtx[0]->vout[0].nValue, GetBlockSubsidy(N - 1, consensus));
        }
        // Extend by one so the next block to mine sits at N (the flat tail).
        while (m_node.chainman->ActiveChain().Tip()->nHeight < N - 1) {
            CBlockIndex* prev = m_node.chainman->ActiveChain().Tip();
            CBlockIndex* next = new CBlockIndex();
            next->phashBlock = new uint256(m_rng.rand256());
            m_node.chainman->ActiveChainstate().CoinsTip().SetBestBlock(next->GetBlockHash());
            next->pprev = prev;
            next->nHeight = prev->nHeight + 1;
            next->BuildSkip();
            m_node.chainman->ActiveChain().SetTip(*next);
        }
        {
            auto bt{mining->createNewBlock(options)};
            BOOST_REQUIRE(bt);
            BOOST_CHECK_EQUAL(bt->getBlock().vtx[0]->vout[0].nValue, GetBlockSubsidy(N, consensus));
            BOOST_CHECK_EQUAL(GetBlockSubsidy(N, consensus), consensus.nTailSubsidy);
        }

        // invalid p2sh txn in tx_relaypool, template creation fails. Feeless (in == out):
        // the parent forwards txFirst[0]'s full value, the child forwards the parent's.
        tx.vin[0].prevout.hash = txFirst[0]->GetHash();
        tx.vin[0].prevout.n = 0;
        tx.vin[0].scriptSig = CScript() << OP_1;
        tx.vout[0].nValue = txFirst[0]->vout[0].nValue;
        CScript script = CScript() << OP_0;
        tx.vout[0].scriptPubKey = GetScriptForDestination(ScriptHash(script));
        ProveTxPow(tx);
        hash = tx.GetHash();
        AddToRelayPool(tx_relaypool, entry.Time(Now<NodeSeconds>()).SpendsCoinbase(true).FromTx(tx));
        tx.vin[0].prevout.hash = hash;
        tx.vin[0].scriptSig = CScript() << std::vector<unsigned char>(script.begin(), script.end());
        ProveTxPow(tx);
        hash = tx.GetHash();
        AddToRelayPool(tx_relaypool, entry.Time(Now<NodeSeconds>()).SpendsCoinbase(false).FromTx(tx));
        BOOST_CHECK_EXCEPTION(mining->createNewBlock(options), std::runtime_error, HasReason("mandatory-script-verify-flag-failed"));

        // Delete the dummy blocks again.
        while (m_node.chainman->ActiveChain().Tip()->nHeight > nHeight) {
            CBlockIndex* del = m_node.chainman->ActiveChain().Tip();
            m_node.chainman->ActiveChain().SetTip(*Assert(del->pprev));
            m_node.chainman->ActiveChainstate().CoinsTip().SetBestBlock(del->pprev->GetBlockHash());
            delete del->phashBlock;
            delete del;
        }
    }

    CTxRelayPool& tx_relaypool{MakeRelayPool()};
    LOCK(tx_relaypool.cs);

    // non-final txs in relaypool
    SetMockTime(m_node.chainman->ActiveChain().Tip()->GetMedianTimePast() + 1);
    const int flags{LOCKTIME_VERIFY_SEQUENCE};
    // height map
    std::vector<int> prevheights;

    // relative height locked
    tx.version = 2;
    tx.vin.resize(1);
    prevheights.resize(1);
    tx.vin[0].prevout.hash = txFirst[0]->GetHash(); // only 1 transaction
    tx.vin[0].prevout.n = 0;
    tx.vin[0].scriptSig = CScript() << OP_1;
    tx.vin[0].nSequence = m_node.chainman->ActiveChain().Tip()->nHeight + 1; // txFirst[0] is the 2nd block
    prevheights[0] = baseheight + 1;
    tx.vout.resize(1);
    tx.vout[0].nValue = txFirst[0]->vout[0].nValue; // feeless: in == out
    tx.vout[0].scriptPubKey = CScript() << OP_1;
    tx.nLockTime = 0;
    ProveTxPow(tx);
    hash = tx.GetHash();
    AddToRelayPool(tx_relaypool, entry.Time(Now<NodeSeconds>()).SpendsCoinbase(true).FromTx(tx));
    BOOST_CHECK(CheckFinalTxAtTip(*Assert(m_node.chainman->ActiveChain().Tip()), CTransaction{tx})); // Locktime passes
    BOOST_CHECK(!TestSequenceLocks(CTransaction{tx}, tx_relaypool)); // Sequence locks fail

    {
        CBlockIndex* active_chain_tip = m_node.chainman->ActiveChain().Tip();
        BOOST_CHECK(SequenceLocks(CTransaction(tx), flags, prevheights, *CreateBlockIndex(active_chain_tip->nHeight + 2, active_chain_tip))); // Sequence locks pass on 2nd block
    }

    // relative time locked
    tx.vin[0].prevout.hash = txFirst[1]->GetHash();
    tx.vout[0].nValue = txFirst[1]->vout[0].nValue; // feeless: in == out (ramp value differs per height)
    tx.vin[0].nSequence = CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG | (((m_node.chainman->ActiveChain().Tip()->GetMedianTimePast()+1-m_node.chainman->ActiveChain()[1]->GetMedianTimePast()) >> CTxIn::SEQUENCE_LOCKTIME_GRANULARITY) + 1); // txFirst[1] is the 3rd block
    prevheights[0] = baseheight + 2;
    ProveTxPow(tx);
    hash = tx.GetHash();
    AddToRelayPool(tx_relaypool, entry.Time(Now<NodeSeconds>()).FromTx(tx));
    BOOST_CHECK(CheckFinalTxAtTip(*Assert(m_node.chainman->ActiveChain().Tip()), CTransaction{tx})); // Locktime passes
    BOOST_CHECK(!TestSequenceLocks(CTransaction{tx}, tx_relaypool)); // Sequence locks fail

    const int SEQUENCE_LOCK_TIME = 512; // Sequence locks pass 512 seconds later
    for (int i = 0; i < CBlockIndex::nMedianTimeSpan; ++i)
        m_node.chainman->ActiveChain().Tip()->GetAncestor(m_node.chainman->ActiveChain().Tip()->nHeight - i)->nTime += SEQUENCE_LOCK_TIME; // Trick the MedianTimePast
    {
        CBlockIndex* active_chain_tip = m_node.chainman->ActiveChain().Tip();
        BOOST_CHECK(SequenceLocks(CTransaction(tx), flags, prevheights, *CreateBlockIndex(active_chain_tip->nHeight + 1, active_chain_tip)));
    }

    for (int i = 0; i < CBlockIndex::nMedianTimeSpan; ++i) {
        CBlockIndex* ancestor{Assert(m_node.chainman->ActiveChain().Tip()->GetAncestor(m_node.chainman->ActiveChain().Tip()->nHeight - i))};
        ancestor->nTime -= SEQUENCE_LOCK_TIME; // undo tricked MTP
    }

    // absolute height locked
    tx.vin[0].prevout.hash = txFirst[2]->GetHash();
    tx.vout[0].nValue = txFirst[2]->vout[0].nValue; // feeless: in == out (ramp value differs per height)
    tx.vin[0].nSequence = CTxIn::MAX_SEQUENCE_NONFINAL;
    prevheights[0] = baseheight + 3;
    tx.nLockTime = m_node.chainman->ActiveChain().Tip()->nHeight + 1;
    ProveTxPow(tx);
    hash = tx.GetHash();
    AddToRelayPool(tx_relaypool, entry.Time(Now<NodeSeconds>()).FromTx(tx));
    BOOST_CHECK(!CheckFinalTxAtTip(*Assert(m_node.chainman->ActiveChain().Tip()), CTransaction{tx})); // Locktime fails
    BOOST_CHECK(TestSequenceLocks(CTransaction{tx}, tx_relaypool)); // Sequence locks pass
    BOOST_CHECK(IsFinalTx(CTransaction(tx), m_node.chainman->ActiveChain().Tip()->nHeight + 2, m_node.chainman->ActiveChain().Tip()->GetMedianTimePast())); // Locktime passes on 2nd block

    // absolute time locked
    tx.vin[0].prevout.hash = txFirst[3]->GetHash();
    tx.vout[0].nValue = txFirst[3]->vout[0].nValue; // feeless: in == out (ramp value differs per height)
    tx.nLockTime = m_node.chainman->ActiveChain().Tip()->GetMedianTimePast();
    prevheights.resize(1);
    prevheights[0] = baseheight + 4;
    ProveTxPow(tx);
    hash = tx.GetHash();
    AddToRelayPool(tx_relaypool, entry.Time(Now<NodeSeconds>()).FromTx(tx));
    BOOST_CHECK(!CheckFinalTxAtTip(*Assert(m_node.chainman->ActiveChain().Tip()), CTransaction{tx})); // Locktime fails
    BOOST_CHECK(TestSequenceLocks(CTransaction{tx}, tx_relaypool)); // Sequence locks pass
    BOOST_CHECK(IsFinalTx(CTransaction(tx), m_node.chainman->ActiveChain().Tip()->nHeight + 2, m_node.chainman->ActiveChain().Tip()->GetMedianTimePast() + 1)); // Locktime passes 1 second later

    // relaypool-dependent transactions (not added)
    tx.vin[0].prevout.hash = hash;
    prevheights[0] = m_node.chainman->ActiveChain().Tip()->nHeight + 1;
    tx.nLockTime = 0;
    tx.vin[0].nSequence = 0;
    BOOST_CHECK(CheckFinalTxAtTip(*Assert(m_node.chainman->ActiveChain().Tip()), CTransaction{tx})); // Locktime passes
    BOOST_CHECK(TestSequenceLocks(CTransaction{tx}, tx_relaypool)); // Sequence locks pass
    tx.vin[0].nSequence = 1;
    BOOST_CHECK(!TestSequenceLocks(CTransaction{tx}, tx_relaypool)); // Sequence locks fail
    tx.vin[0].nSequence = CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG;
    BOOST_CHECK(TestSequenceLocks(CTransaction{tx}, tx_relaypool)); // Sequence locks pass
    tx.vin[0].nSequence = CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG | 1;
    BOOST_CHECK(!TestSequenceLocks(CTransaction{tx}, tx_relaypool)); // Sequence locks fail

    auto block_template = mining->createNewBlock(options);
    BOOST_REQUIRE(block_template);

    // None of the of the absolute height/time locked tx should have made
    // it into the template because we still check IsFinalTx in CreateNewBlock,
    // but relative locked txs will if inconsistently added to relaypool.
    // For now these will still generate a valid template until BIP68 soft fork
    CBlock block{block_template->getBlock()};
    BOOST_CHECK_EQUAL(block.vtx.size(), 3U);
    // However if we advance height by 1 and time by SEQUENCE_LOCK_TIME, all of them should be mined
    for (int i = 0; i < CBlockIndex::nMedianTimeSpan; ++i) {
        CBlockIndex* ancestor{Assert(m_node.chainman->ActiveChain().Tip()->GetAncestor(m_node.chainman->ActiveChain().Tip()->nHeight - i))};
        ancestor->nTime += SEQUENCE_LOCK_TIME; // Trick the MedianTimePast
    }
    // Advance the chain by one block so the absolute height/time-locked txs become
    // final and get mined. Append a well-formed index entry rather than bumping the
    // tip's nHeight in place: an in-place bump leaves a height gap (tip claims H+1 but
    // its pprev is H-1), which breaks GetAncestor — and the per-tx PoW anchor check
    // (CheckTxAnchor -> branch_tip->GetAncestor(nAnchorHeight)) needs a walkable height
    // chain to resolve each tx's anchor. nTime carries the tricked MTP forward.
    {
        CBlockIndex* prev = m_node.chainman->ActiveChain().Tip();
        CBlockIndex* next = new CBlockIndex();
        next->phashBlock = new uint256(m_rng.rand256());
        m_node.chainman->ActiveChainstate().CoinsTip().SetBestBlock(next->GetBlockHash());
        next->pprev = prev;
        next->nHeight = prev->nHeight + 1;
        next->nTime = prev->nTime;
        next->BuildSkip();
        m_node.chainman->ActiveChain().SetTip(*next);
    }
    SetMockTime(m_node.chainman->ActiveChain().Tip()->GetMedianTimePast() + 1);

    block_template = mining->createNewBlock(options);
    BOOST_REQUIRE(block_template);
    block = block_template->getBlock();
    BOOST_CHECK_EQUAL(block.vtx.size(), 5U);

    // Tear down the synthetic advance block and restore the real tip, leaving its
    // height bumped by one in place. This reproduces the original post-condition the
    // caller (CreateNewBlock_validity) relies on: it does a matching Tip()->nHeight--
    // before each subsequent Test* to step the subsidy height back down.
    {
        CBlockIndex* del = m_node.chainman->ActiveChain().Tip();
        m_node.chainman->ActiveChain().SetTip(*Assert(del->pprev));
        m_node.chainman->ActiveChainstate().CoinsTip().SetBestBlock(del->pprev->GetBlockHash());
        delete del->phashBlock;
        delete del;
    }
    m_node.chainman->ActiveChain().Tip()->nHeight++;
}

// Quicksilver is feeless and ranks relaypool txs for mining by per-tx surplus-work rate
// (#5c-2), not by fee. The inherited priority/fee-delta path is removed behavior.
// This test asserts the replacement invariant: explicit surplus overrides drive
// mining order. Real anchored proofs (ProveTxPow) keep the block self-validating.
void MinerTestingSetup::TestSurplusWorkMining(const CScript& scriptPubKey, const std::vector<CTransactionRef>& txFirst)
{
    auto mining{MakeMining()};
    BOOST_REQUIRE(mining);

    BlockAssembler::Options options;
    options.coinbase_output_script = scriptPubKey;

    CTxRelayPool& tx_relaypool{MakeRelayPool()};
    LOCK(tx_relaypool.cs);

    TestRelayPoolEntryHelper entry;

    // Independent feeless tx spending txFirst[idx], with a real proof and surplus override.
    auto add_tx = [&](size_t idx, uint64_t surplus) -> Txid {
        CMutableTransaction tx;
        tx.vin.resize(1);
        tx.vin[0].scriptSig = CScript() << OP_1;
        tx.vin[0].prevout.hash = txFirst[idx]->GetHash();
        tx.vin[0].prevout.n = 0;
        tx.vout.resize(1);
        tx.vout[0].nValue = txFirst[idx]->vout[0].nValue; // feeless: in == out
        tx.vout[0].scriptPubKey = CScript() << OP_1;
        ProveTxPow(tx);
        const Txid hash = tx.GetHash();
        AddToRelayPool(tx_relaypool, entry.TxWorkSurplus(arith_uint256(surplus))
                                      .Time(Now<NodeSeconds>()).SpendsCoinbase(true).FromTx(tx));
        return hash;
    };

    // Four independent txs, well-separated surpluses. Pure surplus order: D > C > B > A.
    const Txid hashA = add_tx(0, 100);
    const Txid hashB = add_tx(1, 300);
    const Txid hashC = add_tx(2, 500);
    const Txid hashD = add_tx(3, 700);

    auto block_template = mining->createNewBlock(options);
    BOOST_REQUIRE(block_template);
    CBlock block{block_template->getBlock()};

    // coinbase + 4 txs, in strict descending surplus order.
    BOOST_REQUIRE_EQUAL(block.vtx.size(), 5U);
    BOOST_CHECK(block.vtx[1]->GetHash() == hashD);
    BOOST_CHECK(block.vtx[2]->GetHash() == hashC);
    BOOST_CHECK(block.vtx[3]->GetHash() == hashB);
    BOOST_CHECK(block.vtx[4]->GetHash() == hashA);
}

// NOTE: These tests rely on CreateNewBlock doing its own self-validation!
BOOST_AUTO_TEST_CASE(CreateNewBlock_validity)
{
    auto mining{MakeMining()};
    BOOST_REQUIRE(mining);

    // Note that by default, these tests run with size accounting enabled.
    CScript scriptPubKey = CScript() << "04678afdb0fe5548271967f1a67130b7105cd6a828e03909a67962e0ea1f61deb649f6bc3f4cef38c4f35504e51ec112de5c384df7ba0b8d578a4c702b6bf11d5f"_hex << OP_CHECKSIG;
    BlockAssembler::Options options;
    options.coinbase_output_script = scriptPubKey;
    std::unique_ptr<BlockTemplate> block_template;

    // We can't make transactions until we have inputs
    // Therefore, load 110 blocks :)
    static_assert(std::size(BLOCKINFO) == 110, "Should have 110 blocks to import");
    int baseheight = 0;
    std::vector<CTransactionRef> txFirst;
    for (const auto& bi : BLOCKINFO) {
        const int current_height{mining->getTip()->height};

        // Simple block creation, nothing special yet:
        block_template = mining->createNewBlock(options);
        BOOST_REQUIRE(block_template);

        CBlock block{block_template->getBlock()};
        CMutableTransaction txCoinbase(*block.vtx[0]);
        {
            LOCK(cs_main);
            block.nVersion = VERSIONBITS_TOP_BITS;
            block.nTime = Assert(m_node.chainman)->ActiveChain().Tip()->GetMedianTimePast()+1;
            txCoinbase.version = 1;
            txCoinbase.vin[0].scriptSig = CScript{} << (current_height + 1) << bi.extranonce;
            txCoinbase.vout.resize(1); // Ignore the (optional) segwit commitment added by CreateNewBlock (as the hardcoded nonces don't account for this)
            txCoinbase.vout[0].scriptPubKey = CScript();
            block.vtx[0] = MakeTransactionRef(txCoinbase);
            if (txFirst.size() == 0)
                baseheight = current_height;
            if (txFirst.size() < 4)
                txFirst.push_back(block.vtx[0]);
            block.hashMerkleRoot = BlockMerkleRoot(block);
            // Quicksilver (#5c-1 Phase 2): the coinbase swap above changed the block
            // weight, and nCongestion folds that in — so the template's value is stale
            // and consensus would reject the block as "bad-congestion". Recompute it
            // exactly as ConnectBlock will, before grinding: it sits in the pre-pow.
            // (This block is far under the sandbox fullness target either way, so the
            // answer is the floor; deriving it rather than assuming that is the point.)
            block.nCongestion = static_cast<uint32_t>(NextCongestionMultiplier(
                Assert(m_node.chainman)->ActiveChain().Tip()->m_congestion,
                GetBlockWeight(block), Params().GetConsensus()));
            block.nNonce = bi.nonce;
            // Quicksilver: block PoW is Cuckatoo, so the hardcoded BLOCKINFO nonce is
            // not a solution. Grind the proof (sets nCycle) — under sandbox
            // fBlockPowNoCycle this is just blake2b(nCycle) <= target, so it is fast
            // and not bound to the block contents.
            SolveBlockPoW(block, Params().GetConsensus());
        }
        std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(block);
        // Alternate calls between Chainman's ProcessNewBlock and submitSolution
        // via the Mining interface. The former is used by net_processing as well
        // as the submitblock RPC.
        if (current_height % 2 == 0) {
            BOOST_REQUIRE(Assert(m_node.chainman)->ProcessNewBlock(shared_pblock, /*force_processing=*/true, /*min_pow_checked=*/true, nullptr));
        } else {
            BOOST_REQUIRE(block_template->submitSolution(block.nVersion, block.nTime, block.nCongestion, block.nNonce, block.nCycle, MakeTransactionRef(txCoinbase)));
        }
        {
            LOCK(cs_main);
            // The above calls don't guarantee the tip is actually updated, so
            // we explicitly check this.
            auto maybe_new_tip{Assert(m_node.chainman)->ActiveChain().Tip()};
            BOOST_REQUIRE_EQUAL(maybe_new_tip->GetBlockHash(), block.GetHash());
        }
        // This just adds coverage
        mining->waitTipChanged(block.hashPrevBlock);
    }

    LOCK(cs_main);

    TestBasicMining(scriptPubKey, txFirst, baseheight);

    m_node.chainman->ActiveChain().Tip()->nHeight--;
    SetMockTime(0);

    TestNoPackageBoost(scriptPubKey, txFirst);

    m_node.chainman->ActiveChain().Tip()->nHeight--;
    SetMockTime(0);

    TestSurplusWorkMining(scriptPubKey, txFirst);
}

BOOST_AUTO_TEST_CASE(cpu_block_mining_policy)
{
    BOOST_CHECK(node::CpuBlockMiningAllowed(19, false));    // sandbox: always feasible
    BOOST_CHECK(node::CpuBlockMiningAllowed(19, true));
    BOOST_CHECK(!node::CpuBlockMiningAllowed(31, false));   // real graph, no opt-in -> forbidden
    BOOST_CHECK(node::CpuBlockMiningAllowed(31, true));     // opt-in -> allowed
    BOOST_CHECK(!node::CpuBlockMiningAllowed(29, false));
}

BOOST_AUTO_TEST_SUITE_END()
