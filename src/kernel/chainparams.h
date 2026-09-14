// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_KERNEL_CHAINPARAMS_H
#define QUICKSILVER_KERNEL_CHAINPARAMS_H

#include <consensus/params.h>
#include <kernel/messagestartchars.h>
#include <primitives/block.h>
#include <uint256.h>
#include <util/chaintype.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

/**
 * Holds various statistics on transactions within a chain. Used to estimate
 * verification progress during chain sync.
 *
 * See also: CChainParams::TxData, GuessVerificationProgress.
 */
struct ChainTxData {
    int64_t nTime;    //!< UNIX timestamp of last known number of transactions
    uint64_t tx_count; //!< total number of transactions between genesis and that timestamp
    double dTxRate;   //!< estimated number of transactions per second after that timestamp
};

/**
 * CChainParams defines various tweakable parameters of a given instance of the
 * Quicksilver system.
 */
class CChainParams
{
public:
    enum Base58Type {
        PUBKEY_ADDRESS,
        SCRIPT_ADDRESS,
        SECRET_KEY,
        EXT_PUBLIC_KEY,
        EXT_SECRET_KEY,

        MAX_BASE58_TYPES
    };

    const Consensus::Params& GetConsensus() const { return consensus; }
    const MessageStartChars& MessageStart() const { return pchMessageStart; }
    uint16_t GetDefaultPort() const { return nDefaultPort; }
    /**
     * Loopback port the automatically created Tor onion service forwards to.
     *
     * Upstream derives this as P2P+1, which silently assumes that port is free.
     * Quicksilver's publictest and sandbox put their RPC port at P2P+1, so the
     * derived target collided with the node's own RPC listener and bare
     * `quicksilverd -sandbox` could not start. Naming it per network makes the
     * constraint checkable; net_tests/default_ports_do_not_collide asserts it stays
     * distinct from both the P2P and the RPC port.
     */
    uint16_t OnionServicePort() const { return m_onion_service_port; }

    const CBlock& GenesisBlock() const { return genesis; }
    /** Default value for -checkrelaypool and -checkblockindex argument */
    bool DefaultConsistencyChecks() const { return fDefaultConsistencyChecks; }
    /** If this chain is exclusively used for testing */
    bool IsTestChain() const { return m_chain_type != ChainType::MAIN; }
    /** SLIP-44 coin type used in default BIP44/84/86 descriptor paths. */
    uint32_t Slip44CoinType() const { return m_slip44_coin_type; }
    /** If this chain allows time to be mocked */
    bool IsMockableChain() const { return m_is_mockable_chain; }
    uint64_t PruneAfterHeight() const { return nPruneAfterHeight; }
    /** Minimum free space (in GB) needed for data directory */
    uint64_t AssumedBlockchainSize() const { return m_assumed_blockchain_size; }
    /** Minimum free space (in GB) needed for data directory when pruned; Does not include prune target*/
    uint64_t AssumedChainStateSize() const { return m_assumed_chain_state_size; }
    /** Whether it is possible to mine blocks on demand (no retargeting) */
    bool MineBlocksOnDemand() const { return consensus.fPowNoRetargeting; }
    /** Return the chain type string */
    std::string GetChainTypeString() const { return ChainTypeToString(m_chain_type); }
    /** Return the chain type */
    ChainType GetChainType() const { return m_chain_type; }
    /** Return the list of hostnames to look up for DNS seeds */
    const std::vector<std::string>& DNSSeeds() const { return vSeeds; }
    const std::vector<unsigned char>& Base58Prefix(Base58Type type) const { return base58Prefixes[type]; }
    const std::string& Bech32HRP() const { return bech32_hrp; }
    const std::vector<uint8_t>& FixedSeeds() const { return vFixedSeeds; }

    const ChainTxData& TxData() const { return chainTxData; }

    /**
     * VersionBitsParameters holds activation parameters
     */
    struct VersionBitsParameters {
        int64_t start_time;
        int64_t timeout;
        int min_activation_height;
    };

    /**
     * SandboxOptions holds configurations for creating a sandbox CChainParams.
     */
    struct SandboxOptions {
        std::unordered_map<Consensus::DeploymentPos, VersionBitsParameters> version_bits_parameters{};
        std::unordered_map<Consensus::BuriedDeployment, int> activation_heights{};
        bool fastprune{false};
        bool tx_pow_no_cycle{false};
    };

    static std::unique_ptr<const CChainParams> Sandbox(const SandboxOptions& options);
    static std::unique_ptr<const CChainParams> Main();
    static std::unique_ptr<const CChainParams> PublicTest();

protected:
    CChainParams() = default;

    Consensus::Params consensus;
    MessageStartChars pchMessageStart;
    uint16_t nDefaultPort;
    uint16_t m_onion_service_port;
    uint64_t nPruneAfterHeight;
    uint64_t m_assumed_blockchain_size;
    uint64_t m_assumed_chain_state_size;
    std::vector<std::string> vSeeds;
    std::vector<unsigned char> base58Prefixes[MAX_BASE58_TYPES];
    std::string bech32_hrp;
    ChainType m_chain_type;
    uint32_t m_slip44_coin_type;
    CBlock genesis;
    std::vector<uint8_t> vFixedSeeds;
    bool fDefaultConsistencyChecks;
    bool m_is_mockable_chain;
    ChainTxData chainTxData;
};

#endif // QUICKSILVER_KERNEL_CHAINPARAMS_H
