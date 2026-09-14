// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_CONSENSUS_PARAMS_H
#define QUICKSILVER_CONSENSUS_PARAMS_H

#include <consensus/amount.h>
#include <uint256.h>

#include <chrono>
#include <limits>
#include <map>
#include <vector>

namespace Consensus {

/**
 * A buried deployment is one where the height of the activation has been hardcoded into
 * the client implementation long after the consensus change has activated. See BIP 90.
 */
enum BuriedDeployment : int16_t {
    // buried deployments get negative values to avoid overlap with DeploymentPos
    DEPLOYMENT_HEIGHTINCB = std::numeric_limits<int16_t>::min(),
    DEPLOYMENT_CLTV,
    DEPLOYMENT_DERSIG,
    DEPLOYMENT_CSV,
    DEPLOYMENT_SEGWIT,
};
constexpr bool ValidDeployment(BuriedDeployment dep) { return dep <= DEPLOYMENT_SEGWIT; }

enum DeploymentPos : uint16_t {
    DEPLOYMENT_TESTDUMMY,
    DEPLOYMENT_TAPROOT, // Deployment of Schnorr/Taproot (BIPs 340-342)
    // NOTE: Also add new deployments to VersionBitsDeploymentInfo in deploymentinfo.cpp
    MAX_VERSION_BITS_DEPLOYMENTS
};
constexpr bool ValidDeployment(DeploymentPos dep) { return dep < MAX_VERSION_BITS_DEPLOYMENTS; }

/**
 * Struct for each individual consensus rule change using BIP9.
 */
struct BIP9Deployment {
    /** Bit position to select the particular bit in nVersion. */
    int bit{28};
    /** Start MedianTime for version bits miner confirmation. Can be a date in the past */
    int64_t nStartTime{NEVER_ACTIVE};
    /** Timeout/expiry MedianTime for the deployment attempt. */
    int64_t nTimeout{NEVER_ACTIVE};
    /** If lock in occurs, delay activation until at least this block
     *  height.  Note that activation will only occur on a retarget
     *  boundary.
     */
    int min_activation_height{0};

    /** Constant for nTimeout very far in the future. */
    static constexpr int64_t NO_TIMEOUT = std::numeric_limits<int64_t>::max();

    /** Special value for nStartTime indicating that the deployment is always active.
     *  This is useful for testing, as it means tests don't need to deal with the activation
     *  process (which takes at least 3 BIP9 intervals). Only tests that specifically test the
     *  behaviour during activation cannot use this. */
    static constexpr int64_t ALWAYS_ACTIVE = -1;

    /** Special value for nStartTime indicating that the deployment is never active.
     *  This is useful for integrating the code changes for a new feature
     *  prior to deploying it on some or all networks. */
    static constexpr int64_t NEVER_ACTIVE = -2;
};

/**
 * Parameters that influence chain consensus.
 */
struct Params {
    uint256 hashGenesisBlock;
    /**
     * Hashes of blocks that
     * - are known to be consensus valid, and
     * - buried in the chain, and
     * - fail if the default script verify flags are applied.
     */
    std::map<uint256, uint32_t> script_flag_exceptions;
    /** Block height and hash at which BIP34 becomes active */
    int BIP34Height;
    uint256 BIP34Hash;
    /** Block height at which BIP65 becomes active */
    int BIP65Height;
    /** Block height at which BIP66 becomes active */
    int BIP66Height;
    /** Block height at which CSV (BIP68, BIP112 and BIP113) becomes active */
    int CSVHeight;
    /** Block height at which Segwit (BIP141, BIP143 and BIP147) becomes active.
     * Note that segwit v0 script rules are enforced on all blocks except the
     * BIP 16 exception blocks. */
    int SegwitHeight;
    /** Don't warn about unknown BIP 9 activations below this height.
     * This prevents us from warning about the CSV and segwit activations. */
    int MinBIP9WarningHeight;
    /**
     * Minimum blocks including miner confirmation of the total of 2016 blocks in a retargeting period,
     * (nPowTargetTimespan / nPowTargetSpacing) which is also used for BIP9 deployments.
     * Examples: 1916 for 95%, 1512 for testchains.
     */
    uint32_t nRuleChangeActivationThreshold;
    uint32_t nMinerConfirmationWindow;
    BIP9Deployment vDeployments[MAX_VERSION_BITS_DEPLOYMENTS];
    /** Proof of work parameters */
    uint256 powLimit;
    /** Quicksilver: Cuckatoo graph size for the BLOCK PoW. Consensus.
     *  {19 sandbox, 28 publictest/main}. Unified with the per-tx PoW graph size so a
     *  single E28 solver serves both; block difficulty comes from the target
     *  (cycles-per-block), not a larger graph. */
    uint8_t nEdgeBits;
    /** Quicksilver: Cuckatoo graph size for the PER-TX PoW. Consensus.
     *  {19 sandbox, 28 publictest/main}. Verify cost is EDGEBITS-invariant. */
    uint8_t nTxEdgeBits;
    /** Quicksilver: per-tx PoW target ceiling (the value thresholded against
     *  blake2b(cycle)). GetTxPowTarget applies the block-coupled congestion law
     *  and never returns a target easier than this ceiling. */
    uint256 txPowLimit;
    /** Quicksilver (#5c-1): max age (in blocks) of a tx's PoW anchor: the W grace
     *  window. A tx's nAnchorHeight must satisfy tip_height - nMaxAnchorAge <=
     *  nAnchorHeight <= tip_height. */
    int nMaxAnchorAge;
    /** Quicksilver (#5c-1): congestion-floor parameters. Public-network values
     *  are Phase 3 calibrated; sandbox keeps smaller windows/targets for fast tests. */
    int nCongestionTargetPermille; //!< target block fullness T, in permille (500 = 0.5)
    int nCongestionStepDenom;      //!< max per-block step s = 1/denom (8 = 0.125)
    int nCongestionMaxMultiplier;  //!< m_cap: ceiling on the congestion multiplier
    int nBaseWorkMAWindow;         //!< blocks averaged for base_coupled (difficulty smoothing)
    uint32_t nTxWorkCouplingK;     //!< K: divides MA(blockwork) to set the per-tx base floor
    /** Quicksilver Frame-B mint (#4): coins minted to the block miner per valid
     *  per-tx PoW (the cap C). Public-network values are Phase 3 calibrated;
     *  sandbox keeps a test-scale value. */
    CAmount nTxPowMint;
    /** Quicksilver Stage 2 flag day: serialized bytes that cost one unit of base
     *  work. RequiredTxWork charges bytes/nTxWorkRefBytes of base, so a transaction
     *  is priced by what it writes to everyone's disk rather than by existing.
     *  Denominated in WITH-WITNESS serialized bytes: weight is 4*total - 3*witness,
     *  so weight overstates an attacker's cost by up to 4x in their favour, and the
     *  witness discount is fee-market residue in a feeless chain. 4,739 = R/4 where
     *  R = 18,957 is the Stage 1 weight reference. Uniform across networks. */
    uint32_t nTxWorkRefBytes;
    /** Quicksilver Stage 2 flag day: net new UTXOs that cost one unit of base work.
     *  ADDITIVE with the byte term, not max() -- under max() it is inert, because
     *  the bloat shape is already byte-heavy. M5 measured chainstate at 75-83% of
     *  an operator's real on-disk cost, so UTXO creation, not transaction bytes, is
     *  what a storage bound must price. Uniform across networks. */
    uint32_t nTxUtxoRefCount;
    /** Quicksilver Stage 2 flag day: ceiling on the total per-tx mint one block may
     *  authorize (decision criterion 4). Without it, maximum issuance depends on
     *  how small transactions are -- M4 measured a 357x span. Applied inside
     *  GetBlockMintAllowance, so the assembler and the validator cannot disagree. */
    CAmount nMaxBlockMint;
    /** Quicksilver tail emission: genesis per-block base subsidy S0 — the top of the
     *  linear bootstrap ramp. FINALIZED (round-S0 anchor): 50 COIN, giving ≈26.8M COIN
     *  total bootstrap supply; legible headline subsidy, distinct from the 1-COIN tail.
     *  See doc/design/tail-emission.md. */
    CAmount nInitialSubsidy;
    /** Quicksilver tail emission: perpetual per-block tail subsidy, reached at the end of
     *  bootstrap and paid forever after (no hard cap; economics §7). FINALIZED: 1 COIN
     *  (economics §4.1). Coupled to C and K (calibration design §3) — not a free knob. */
    CAmount nTailSubsidy;
    /** Quicksilver tail emission: bootstrap length N in blocks. The ramp spans [0, N);
     *  at height >= N the subsidy is exactly nTailSubsidy. FINALIZED: main/publictest
     *  = 1'051'920 (10 yr @ 5-min: 288 blk/day × 365.25 × 10). sandbox uses a small N
     *  (150) as a deliberate test convenience so functional tests cross the tail fast. */
    int nBootstrapBlocks;
    bool fPowAllowMinDifficultyBlocks;
    bool fPowNoRetargeting;
    /** Quicksilver: when true, BLOCK PoW does not require a real Cuckatoo 42-cycle —
     *  only the (trivial) target threshold on CuckatooProofHash is checked. Restores
     *  upstream's "trivial-but-real" sandbox PoW invariant, which the Cuckatoo
     *  migration broke (real cycle-finding is target-independent in cost, making the
     *  100-block maturity fixtures take ~6 min). Set true ONLY on sandbox — a private,
     *  never-production network. Per-tx PoW is unaffected. Default false (fail-secure:
     *  a params block that forgets to set it still requires real block PoW). */
    bool fBlockPowNoCycle{false};
    /** Quicksilver: when true, per-tx PoW does not require solving or verifying a
     *  real Cuckatoo 42-cycle — only the target threshold on CuckatooProofHash is
     *  produced and checked (the anchor recency check is unaffected). Mirrors
     *  fBlockPowNoCycle for the PER-TX proof. Lets
     *  block-assembly tests that inject many txs (e.g. miner_tests' 1001-tx sigops
     *  case) avoid an infeasible per-tx grind. NOT set on any params by default — it
     *  is opt-in per test, so production and the audit-grade per-tx-PoW suites
     *  (txpow/mint/frame_b) always run the REAL CuckatooVerify. Default false. */
    bool fTxPowNoCycle{false};
    int64_t nPowTargetSpacing;
    int64_t nPowTargetTimespan;
    std::chrono::seconds PowTargetSpacing() const
    {
        return std::chrono::seconds{nPowTargetSpacing};
    }
    int64_t DifficultyAdjustmentInterval() const { return nPowTargetTimespan / nPowTargetSpacing; }
    /** The best chain should have at least this much work */
    uint256 nMinimumChainWork;
    /** By default assume that the signatures in ancestors of this block are valid */
    uint256 defaultAssumeValid;

    int DeploymentHeight(BuriedDeployment dep) const
    {
        switch (dep) {
        case DEPLOYMENT_HEIGHTINCB:
            return BIP34Height;
        case DEPLOYMENT_CLTV:
            return BIP65Height;
        case DEPLOYMENT_DERSIG:
            return BIP66Height;
        case DEPLOYMENT_CSV:
            return CSVHeight;
        case DEPLOYMENT_SEGWIT:
            return SegwitHeight;
        } // no default case, so the compiler can warn about missing cases
        return std::numeric_limits<int>::max();
    }
};

} // namespace Consensus

#endif // QUICKSILVER_CONSENSUS_PARAMS_H
