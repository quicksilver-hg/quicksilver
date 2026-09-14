// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kernel/chainparams.h>

#include <chainparamsseeds.h>
#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <hash.h>
#include <kernel/messagestartchars.h>
#include <logging.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <uint256.h>
#include <util/chaintype.h>
#include <util/strencodings.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <type_traits>

using namespace util::hex_literals;

// Workaround MSVC bug triggering C7595 when calling consteval constructors in
// initializer lists.
// A fix may be on the way:
// https://developercommunity.visualstudio.com/t/consteval-conversion-function-fails/1579014
#if defined(_MSC_VER)
auto consteval_ctor(auto&& input) { return input; }
#else
#define consteval_ctor(input) (input)
#endif

static CBlock CreateGenesisBlock(const char* pszTimestamp, const CScript& genesisOutputScript, uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    CMutableTransaction txNew;
    txNew.version = 1;
    txNew.vin.resize(1);
    txNew.vout.resize(1);
    // Quicksilver genesis coinbase.
    txNew.vin[0].scriptSig = CScript() << std::vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
    txNew.vout[0].nValue = genesisReward;
    txNew.vout[0].scriptPubKey = genesisOutputScript;

    CBlock genesis;
    genesis.nTime    = nTime;
    genesis.nBits    = nBits;
    // Quicksilver (#5c-1 Phase 2): genesis sits at the congestion floor. It never passes
    // through ConnectBlock, so this header field IS the seed for the whole recurrence —
    // every descendant's multiplier is derived from it.
    genesis.nCongestion = static_cast<uint32_t>(CONGESTION_ONE);
    genesis.nNonce   = nNonce;
    genesis.nVersion = nVersion;
    genesis.vtx.push_back(MakeTransactionRef(std::move(txNew)));
    genesis.hashPrevBlock.SetNull();
    genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
    return genesis;
}

/**
 * Main network on which people trade goods and services.
 */
class CMainParams : public CChainParams {
public:
    CMainParams() {
        m_chain_type = ChainType::MAIN;
        // SLIP-44 coin type. Not type 0, and deliberately not 9555: that value
        // is already registered to Rincoin (RIN) in slip-0044.md. 9556 is free;
        // registration for Quicksilver is pending upstream.
        m_slip44_coin_type = 9556;
        // Quicksilver mainnet opens with every buried deployment already active:
        // pow_tests/mainnet_has_no_inherited_activation_heights.
        consensus.BIP34Height = 1;
        consensus.BIP34Hash = uint256{};
        consensus.BIP65Height = 1;
        consensus.BIP66Height = 1;
        consensus.CSVHeight = 1;
        consensus.SegwitHeight = 1;
        consensus.MinBIP9WarningHeight = 0;
        // Quicksilver mainnet difficulty FLOOR = 4 Cuckatoo cycles per block.
        // This is a cycle-PoW target, not a hash target: cycles-per-block
        // = 2^256 / target. At 0.015934 cycles/s on the final-slot quiet
        // P104-100 (F-174), 4 cycles is 251.0 s per block
        // against a 300 s target. The conservative slowest-card case is 809.4 s
        // after an explicit +25% slowdown and lower-95% cycle yield. The floor is
        // stated in real work per block, so
        // it moved 2 -> 4 when the graph size moved 29 -> 28: each cycle became
        // 2.05x cheaper (M7, same card at both sizes), and holding nBits would
        // have halved the work in the cheapest possible block.
        // Derivation and the rejected alternatives:
        // doc/audit/mainnet-difficulty-floor-model.md, tools/calibration/e28-floor.md.
        consensus.powLimit = uint256{"3fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
        consensus.nEdgeBits = 28; // Quicksilver block PoW (Cuckatoo); unified with the per-tx PoW graph size so one E28 solver serves both. Block difficulty comes from the target (cycles-per-block), not a larger graph.
        consensus.nTxEdgeBits = 28; // Quicksilver per-tx PoW (Cuckatoo); MUST equal nEdgeBits — r = 1 depends on it
        consensus.txPowLimit = uint256{"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"}; // permissive; tightened in #5/calibration
        consensus.nMaxAnchorAge = 100; // #5c-1 W: ~8h @ 5-min blocks
        consensus.nCongestionTargetPermille = 500;   // #5c-1 Phase 3: T=0.5
        consensus.nCongestionStepDenom      = 4;      // #5c-1 Phase 3: s=0.25 (faster response @ 5-min blocks)
        consensus.nCongestionMaxMultiplier  = 64;     // #5c-1 Phase 3: m_cap
        consensus.nBaseWorkMAWindow         = 144;    // #5c-1 Phase 3: aligned to the difficulty-retarget interval, and follows it down to 144 (12h)
        consensus.nTxWorkCouplingK          = 106;    // run2 (2026-06-28): K=round(alpha*r*S_tail/C), measured lean r=0.243 restores 4x mint-safety margin. E29 unification (r=1) raised the derived ceiling to 437; 106 held, now 4.1x inside it — see doc/audit/mainnet-difficulty-floor-model.md
        consensus.nTxPowMint = 57143;    // #5c-1 Phase 3: C=2*COIN/N_tx,max (mints = 2x tail subsidy at full block)
        // Quicksilver Stage 2 flag day (2026-08-01). See
        // doc/design/chain-growth.md and tools/calibration/stage2-byte-pricing.md.
        consensus.nTxWorkRefBytes = 4739;     // R_b: R/4, serialized bytes not weight
        consensus.nTxUtxoRefCount = 50;       // U: inside the measured 28-109 window
        consensus.nMaxBlockMint   = 2 * COIN; // criterion 4: size-independent issuance ceiling
        // Tail emission — FINALIZED magnitudes (2026-07-09-tail-emission-calibration-design.md).
        consensus.nInitialSubsidy  = 50 * COIN;   // S0: top of the bootstrap ramp
        consensus.nTailSubsidy     = 1 * COIN;    // perpetual ~1-coin tail
        consensus.nBootstrapBlocks = 1'051'920;   // 10 yr @ 5-min (288 blk/day * 365.25 * 10)
        // Quicksilver mainnet: SHORT retarget window (2026-07-20 launch roadmap).
        // A 2016-block window leaves a low-hashrate chain mis-targeted for months
        // in both directions: ~87 days to recover from a 10x hashrate loss, versus
        // ~6 days at 144. That is the difference between a chain a newcomer can
        // restart after the founder leaves and one they cannot.
        // See doc/audit/mainnet-difficulty-floor-model.md.
        consensus.nPowTargetTimespan = 144 * 5 * 60; // retarget interval 144 blocks (12h)
        consensus.nPowTargetSpacing = 5 * 60; // Quicksilver: 5-minute block target
        consensus.fPowAllowMinDifficultyBlocks = false;
        consensus.fPowNoRetargeting = false;
        consensus.nRuleChangeActivationThreshold = 1815; // 90% of 2016
        consensus.nMinerConfirmationWindow = 2016; // BIP9 signalling window; deliberately NOT the retarget interval (144)
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay

        // Taproot (BIPs 340-342): active from the first block, like every other
        // deployment on a chain with no pre-Taproot history.
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nStartTime = Consensus::BIP9Deployment::ALWAYS_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].min_activation_height = 0; // No activation delay

        consensus.nMinimumChainWork = uint256{}; // Quicksilver: fresh chain
        consensus.defaultAssumeValid = uint256{};

        /**
         * The message start string is designed to be unlikely to occur in normal data.
         * The characters are rarely used upper ASCII, not valid as UTF-8, and produce
         * a large 32-bit integer with any alignment.
         */
        pchMessageStart[0] = 0x48; // 'H'
        pchMessageStart[1] = 0x47; // 'G'
        pchMessageStart[2] = 0x51; // 'Q'
        pchMessageStart[3] = 0x53; // Quicksilver mainnet
        nDefaultPort = 9555;
        m_onion_service_port = 9556;  // RPC is 9554, so P2P+1 is free here
        nPruneAfterHeight = 100000;
        // Quicksilver: these are FIRST-YEAR growth figures, not a snapshot of a chain
        // that already exists — this chain starts at height 0, so a snapshot would be
        // zero and would tell a prospective operator nothing about the commitment they
        // are taking on. Measured at the honest 2-output traffic shape in M5
        // (tools/calibration/m5-disk-cost): 1,217,399 blocks+undo bytes and 244,224
        // chainstate bytes per block, at 288 blocks/day. See doc/design/chain-storage.md.
        m_assumed_blockchain_size = 128;
        m_assumed_chain_state_size = 26;

        // Quicksilver mainnet genesis. Genesis is the hardcoded trust anchor:
        // CheckProofOfWorkImpl exempts exactly consensus.hashGenesisBlock, and
        // normal Cuckatoo block PoW begins at height 1.
        const char* pszTimestamp = "Quicksilver mainnet genesis 2026-09-05 - zero-cycle trust anchor";
        // Quicksilver mark: the genesis coinbase output is a provably-unspendable
        // OP_RETURN, not a payment — no key anyone holds.
        const std::string genesisMark{"Quicksilver Genesis - for the agents, raised by human, Claude, Codex, and Grok"};
        const CScript genesisOutputScript = CScript() << OP_RETURN << std::vector<unsigned char>(genesisMark.begin(), genesisMark.end());
        genesis = CreateGenesisBlock(pszTimestamp, genesisOutputScript, 1788566400, 0,
                0x203fffff, // == UintToArith256(powLimit).GetCompact(); see pow_tests/genesis_nbits_equals_chain_powlimit
                1, 50 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"c9144acae20212e57e9e59f34a681bd25f55d81438001c424ebb95cd4869bcf3"});
        assert(genesis.hashMerkleRoot == uint256{"da9499c1476c92b69b7214ddb1a365548fff51159e523ca01c77686e022de966"});

        // Quicksilver publishes no DNS seeds: there is no domain to own, and a DNS
        // seed is a renewal liability that must survive the founder's exit.
        vSeeds.clear();

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,58);   // Quicksilver 'Q...' (version byte 58 yields leading 'Q')
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,50);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,166);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x03, 0xF7, 0x28, 0x12}; // Quicksilver "qpub"
        base58Prefixes[EXT_SECRET_KEY] = {0x03, 0xF7, 0x23, 0xD8}; // Quicksilver "qprv"

        bech32_hrp = "hg";

        // Peer discovery of last resort. The onion seed runs on maintainer hardware;
        // doc/bootstrapping.md documents -addnode for anyone who prefers not to rely
        // on it. A node with neither cannot find the network at all.
        vFixedSeeds = std::vector<uint8_t>(std::begin(chainparams_seed_main),
                                           std::end(chainparams_seed_main));

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        chainTxData = ChainTxData{
            // Quicksilver: fresh chain.
            .nTime    = 0,
            .tx_count = 0,
            .dTxRate  = 0,
        };
    }
};

/**
 * Quicksilver public test network.
 */
class CPublicTestParams : public CChainParams {
public:
    CPublicTestParams() {
        m_chain_type = ChainType::PUBLIC_TEST;
        m_slip44_coin_type = 1; // SLIP-44 testnet (all coins)
        consensus.BIP34Height = 1;
        consensus.BIP34Hash = uint256{};
        consensus.BIP65Height = 1;
        consensus.BIP66Height = 1;
        consensus.CSVHeight = 1;
        consensus.SegwitHeight = 1;
        consensus.MinBIP9WarningHeight = 0;
        // publictest rehearses mainnet, so it runs mainnet's consensus surface:
        // same 2^254 one-GPU floor, same 144-block retarget window, real
        // retargeting, no min-difficulty exception. This is a Cuckatoo cycle-PoW
        // floor, not a hash target — cycles-per-block = 2^256 / target, so 2^254
        // is 4 cycles per block. It moved from 2^255 (2 cycles) with the E28 flag
        // day, because a cycle at E28 is 2.06x cheaper and the floor is stated in
        // real work per block, not in nBits.
        //
        // A 4x retarget step off a target this large has no 256-bit
        // representation; ScaleTarget (pow.cpp, P0a) saturates rather than
        // wrapping, and both callers clamp to powLimit, so the floor is safe with
        // retargeting live. The former "powLimit must not overflow 4*timespan"
        // ceiling is retired — see sanity_check_chainparams and
        // mainnet_retarget_step_off_powlimit_is_safe_and_agreed in pow_tests.
        consensus.powLimit = uint256{"3fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
        consensus.nEdgeBits = 28; // Quicksilver block PoW (Cuckatoo); unified with the per-tx PoW graph size so one E28 solver serves both. Block difficulty comes from the target (cycles-per-block), not a larger graph.
        consensus.nTxEdgeBits = 28; // Quicksilver per-tx PoW (Cuckatoo); MUST equal nEdgeBits — r = 1 depends on it
        consensus.txPowLimit = uint256{"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"}; // permissive; tightened in #5/calibration
        consensus.nMaxAnchorAge = 100; // #5c-1 W: ~8h @ 5-min blocks
        consensus.nCongestionTargetPermille = 500;   // #5c-1 Phase 3: T=0.5
        consensus.nCongestionStepDenom      = 4;      // #5c-1 Phase 3: s=0.25 (faster response @ 5-min blocks)
        consensus.nCongestionMaxMultiplier  = 64;     // #5c-1 Phase 3: m_cap
        consensus.nBaseWorkMAWindow         = 144;    // = mainnet; follows the retarget interval
        consensus.nTxWorkCouplingK          = 106;    // run2 (2026-06-28): K=round(alpha*r*S_tail/C), measured lean r=0.243 restores 4x mint-safety margin
        consensus.nTxPowMint = 57143;    // #5c-1 Phase 3: C=2*COIN/N_tx,max (mints = 2x tail subsidy at full block)
        // Quicksilver Stage 2 flag day (2026-08-01). See
        // doc/design/chain-growth.md and tools/calibration/stage2-byte-pricing.md.
        consensus.nTxWorkRefBytes = 4739;     // R_b: R/4, serialized bytes not weight
        consensus.nTxUtxoRefCount = 50;       // U: inside the measured 28-109 window
        consensus.nMaxBlockMint   = 2 * COIN; // criterion 4: size-independent issuance ceiling
        // Tail emission — FINALIZED magnitudes (2026-07-09-tail-emission-calibration-design.md).
        consensus.nInitialSubsidy  = 50 * COIN;   // S0: top of the bootstrap ramp
        consensus.nTailSubsidy     = 1 * COIN;    // perpetual ~1-coin tail
        consensus.nBootstrapBlocks = 1'051'920;   // 10 yr @ 5-min (288 blk/day * 365.25 * 10)
        consensus.nPowTargetTimespan = 144 * 5 * 60; // = mainnet: retarget interval 144 blocks (12h)
        consensus.nPowTargetSpacing = 5 * 60; // Quicksilver: 5-minute block target
        consensus.fPowAllowMinDifficultyBlocks = false; // = mainnet. The min-difficulty walk-back is what masked the genesis nBits defect; a rehearsal must not carry a valve mainnet lacks.
        consensus.fPowNoRetargeting = false; // = mainnet: the retarget is the code P1 exists to exercise.
        consensus.nRuleChangeActivationThreshold = 1815; // = mainnet (90% of 2016); inert while every deployment is ALWAYS/NEVER_ACTIVE, but held equal so the parity guard is a plain equality
        consensus.nMinerConfirmationWindow = 2016; // BIP9 signalling window; deliberately NOT the retarget interval (144)
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay

        // Deployment of Taproot (BIPs 340-342)
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nStartTime = Consensus::BIP9Deployment::ALWAYS_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].min_activation_height = 0; // No activation delay

        consensus.nMinimumChainWork = uint256{}; // Quicksilver: fresh chain
        consensus.defaultAssumeValid = uint256{}; // Quicksilver: fresh chain

        pchMessageStart[0] = 0x48; // 'H'
        pchMessageStart[1] = 0x47; // 'G'
        pchMessageStart[2] = 0x51; // 'Q'
        pchMessageStart[3] = 0x56; // Quicksilver publictest (bumped at the F-147 re-mint; 0x55 was the pre-F-147 chain)
        nDefaultPort = 19557;
        m_onion_service_port = 19559;  // NOT P2P+1: 19558 is this network's RPC port
        nPruneAfterHeight = 1000;
        // Same consensus parameters as main, so the same first-year growth applies.
        m_assumed_blockchain_size = 128;
        m_assumed_chain_state_size = 26;

        const char* publictest_genesis_msg = "Quicksilver publictest genesis 2026-09-05 - zero-cycle trust anchor";
        // Quicksilver mark: provably-unspendable OP_RETURN genesis coinbase output.
        const std::string genesisMark{"Quicksilver Genesis - for the agents, raised by human, Claude, Codex, and Grok"};
        const CScript publictest_genesis_script = CScript() << OP_RETURN << std::vector<unsigned char>(genesisMark.begin(), genesisMark.end());
        genesis = CreateGenesisBlock(publictest_genesis_msg,
                publictest_genesis_script,
                1788566400,   // 2026-09-05 00:00 UTC — must be in the PAST relative to the soak, or block 1 would precede genesis and break median-time-past
                0,
                0x203fffff, // == UintToArith256(powLimit).GetCompact(); see pow_tests/genesis_nbits_equals_chain_powlimit
                1,
                50 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"917dde1f04c7470969bbdc344d39559e1af32b3b0e6caaed89db54637d6e46da"});
        assert(genesis.hashMerkleRoot == uint256{"bdf0a510a4f1094ae464987ef01a0fe8409d7c61fee2ef4102d4d84159e78ad6"});

        // No DNS seeds, for the same reason as main.
        vSeeds.clear();

        // The same onion service as main, on the publictest P2P port — one hidden
        // service, two ports, one key to keep.
        vFixedSeeds = std::vector<uint8_t>(std::begin(chainparams_seed_publictest),
                                           std::end(chainparams_seed_publictest));

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,55);   // Quicksilver publictest 'P...'
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,117);  // Quicksilver publictest 'p...'
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,149);  // Quicksilver publictest WIF 'P...'
        base58Prefixes[EXT_PUBLIC_KEY] = {0x03, 0xE2, 0xB9, 0x43}; // Quicksilver publictest "pqub"
        base58Prefixes[EXT_SECRET_KEY] = {0x03, 0xE2, 0xB5, 0x09}; // Quicksilver publictest "pqrv"

        bech32_hrp = "phg";

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        chainTxData = ChainTxData{
            // Quicksilver: fresh chain.
            .nTime    = 0,
            .tx_count = 0,
            .dTxRate  = 0,
        };
    }
};

/**
 * Quicksilver sandbox: intended for private local networks only. Has minimal
 * difficulty to ensure that blocks can be found instantly.
 */
class CSandboxParams : public CChainParams
{
public:
    explicit CSandboxParams(const SandboxOptions& opts)
    {
        m_chain_type = ChainType::SANDBOX;
        m_slip44_coin_type = 1; // SLIP-44 testnet (all coins)
        consensus.BIP34Height = 1; // Always active unless overridden
        consensus.BIP34Hash = uint256();
        consensus.BIP65Height = 1;  // Always active unless overridden
        consensus.BIP66Height = 1;  // Always active unless overridden
        consensus.CSVHeight = 1;    // Always active unless overridden
        consensus.SegwitHeight = 0; // Always active unless overridden
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
        consensus.nEdgeBits = 19; // Quicksilver sandbox block PoW (Cuckatoo, tiny graph for instant tests)
        // Restore upstream's trivial-but-real sandbox PoW: block PoW checks the target but
        // does NOT require a real 42-cycle (cycle-finding cost is target-independent, which
        // made the 100-block maturity fixtures take ~6 min). Sandbox ONLY. Per-tx PoW stays
        // real. See consensus/params.h fBlockPowNoCycle.
        consensus.fBlockPowNoCycle = true;
        consensus.nTxEdgeBits = 19; // Quicksilver sandbox per-tx PoW (tiny graph for fast tests)
        consensus.fTxPowNoCycle = opts.tx_pow_no_cycle;
        consensus.txPowLimit = uint256{"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"}; // permissive
        consensus.nMaxAnchorAge = 20;  // sandbox: small window for fast tests
        // sandbox: low target fullness so a single small (~10 KB OP_RETURN) tx tips a block
        // over target and exercises the congestion multiplier without 500 KB of block stuffing.
        // The recurrence SHAPE is identical to the other nets (T just sets where it pivots).
        consensus.nCongestionTargetPermille = 5;      // sandbox: 0.5% target (fast m-rise tests)
        consensus.nCongestionStepDenom      = 8;
        consensus.nCongestionMaxMultiplier  = 64;
        consensus.nBaseWorkMAWindow         = 10;     // small window for fast tests
        consensus.nTxWorkCouplingK          = 0xFFFFFFFFu; // huge K => base_coupled ~ 0 => permissive
        consensus.nTxPowMint = 1 * COIN; // sandbox fixture: permissive whole-coin mint used by fast tests
        // Quicksilver Stage 2 flag day (2026-08-01). See
        // doc/design/chain-growth.md and tools/calibration/stage2-byte-pricing.md.
        consensus.nTxWorkRefBytes = 4739;     // R_b: R/4, serialized bytes not weight
        consensus.nTxUtxoRefCount = 50;       // U: inside the measured 28-109 window
        consensus.nMaxBlockMint   = 2 * COIN; // criterion 4: size-independent issuance ceiling
        // Tail emission — FINALIZED; sandbox N=150 is a deliberate test convenience (fast tail-crossing).
        consensus.nInitialSubsidy  = 50 * COIN;
        consensus.nTailSubsidy     = 1 * COIN;
        consensus.nBootstrapBlocks = 150;
        consensus.nPowTargetTimespan = 24 * 60 * 60; // one day
        consensus.nPowTargetSpacing = 5 * 60; // Quicksilver: 5-minute block target
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.fPowNoRetargeting = true;
        consensus.nRuleChangeActivationThreshold = 108; // 75% for testchains
        consensus.nMinerConfirmationWindow = 144; // Faster than normal for sandbox (144 instead of 2016)

        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay

        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nStartTime = Consensus::BIP9Deployment::ALWAYS_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].min_activation_height = 0; // No activation delay

        consensus.nMinimumChainWork = uint256{};
        consensus.defaultAssumeValid = uint256{};

        pchMessageStart[0] = 0x48; // 'H'
        pchMessageStart[1] = 0x47; // 'G'
        pchMessageStart[2] = 0x51; // 'Q'
        pchMessageStart[3] = 0x52; // Quicksilver sandbox variant
        nDefaultPort = 19556;
        m_onion_service_port = 19555;  // NOT P2P+1: 19557 is this network's RPC port
        nPruneAfterHeight = opts.fastprune ? 100 : 1000;
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;

        for (const auto& [dep, height] : opts.activation_heights) {
            switch (dep) {
            case Consensus::BuriedDeployment::DEPLOYMENT_SEGWIT:
                consensus.SegwitHeight = int{height};
                break;
            case Consensus::BuriedDeployment::DEPLOYMENT_HEIGHTINCB:
                consensus.BIP34Height = int{height};
                break;
            case Consensus::BuriedDeployment::DEPLOYMENT_DERSIG:
                consensus.BIP66Height = int{height};
                break;
            case Consensus::BuriedDeployment::DEPLOYMENT_CLTV:
                consensus.BIP65Height = int{height};
                break;
            case Consensus::BuriedDeployment::DEPLOYMENT_CSV:
                consensus.CSVHeight = int{height};
                break;
            }
        }

        for (const auto& [deployment_pos, version_bits_params] : opts.version_bits_parameters) {
            consensus.vDeployments[deployment_pos].nStartTime = version_bits_params.start_time;
            consensus.vDeployments[deployment_pos].nTimeout = version_bits_params.timeout;
            consensus.vDeployments[deployment_pos].min_activation_height = version_bits_params.min_activation_height;
        }

        const char* pszTimestamp = "Quicksilver sandbox genesis 2026-08-24 - feeless per-tx PoW";
        // Quicksilver mark: provably-unspendable OP_RETURN genesis coinbase output.
        const std::string genesisMark{"Quicksilver Genesis - for the agents, raised by human, Claude, Codex, and Grok"};
        const CScript genesisOutputScript = CScript() << OP_RETURN << std::vector<unsigned char>(genesisMark.begin(), genesisMark.end());
        genesis = CreateGenesisBlock(pszTimestamp, genesisOutputScript, 1750000000, 4, 0x207fffff, 1, 50 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        // Quicksilver: zero-cycle trust-anchor genesis. The Cuckatoo design settled genesis
        // as PoW-exempt; the first real Cuckatoo block PoW starts at height 1.
        assert(consensus.hashGenesisBlock == uint256{"bd806e80eec28f4b16ab48db377ad6af369fa3b1197cb6d9d77d07db6d5e91e7"});
        assert(genesis.hashMerkleRoot == uint256{"feacef8e2fca6169ea9726bbf4db05c5efa7c006c1f0d1f98c2cb056d7a06469"});

        // Sandbox is local by construction: there is nothing to discover.
        // feature_config_args.py enables -fixedseeds only on sandbox, precisely
        // because both of these stay empty here.
        vFixedSeeds.clear();
        vSeeds.clear();

        fDefaultConsistencyChecks = true;
        m_is_mockable_chain = true;

        chainTxData = ChainTxData{
            0,
            0,
            0
        };

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,63);   // Quicksilver sandbox 'S...'
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,125);  // Quicksilver sandbox 's...'
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,170);  // Quicksilver sandbox WIF 'S...'
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x21, 0x18, 0xFF}; // Quicksilver sandbox "squb"
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x21, 0x14, 0xC4}; // Quicksilver sandbox "sqrv"

        bech32_hrp = "shg";
    }
};

std::unique_ptr<const CChainParams> CChainParams::Sandbox(const SandboxOptions& options)
{
    return std::make_unique<const CSandboxParams>(options);
}

std::unique_ptr<const CChainParams> CChainParams::Main()
{
    return std::make_unique<const CMainParams>();
}

std::unique_ptr<const CChainParams> CChainParams::PublicTest()
{
    return std::make_unique<const CPublicTestParams>();
}
