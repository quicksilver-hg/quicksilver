// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_PRIMITIVES_BLOCK_H
#define QUICKSILVER_PRIMITIVES_BLOCK_H

#include <primitives/transaction.h>
#include <serialize.h>
#include <uint256.h>
#include <util/time.h>

#include <array>
#include <cstdint>

/** Nodes collect new transactions into a block, hash them into a hash tree,
 * and scan through nonce values to make the block's hash satisfy proof-of-work
 * requirements.  When they solve the proof-of-work, they broadcast the block
 * to everyone and the block is added to the block chain.  The first transaction
 * in the block is a special one that creates a new coin owned by the creator
 * of the block.
 */
class CBlockHeader
{
public:
    // header
    int32_t nVersion;
    uint256 hashPrevBlock;
    uint256 hashMerkleRoot;
    uint32_t nTime;
    uint32_t nBits;
    //! Quicksilver (#5c-1 Phase 2): the EIP-1559 congestion multiplier m for THIS block,
    //! fixed point with CONGESTION_ONE (65536) == 1.0. Consensus-checked at ConnectBlock
    //! against NextCongestionMultiplier(pprev->m_congestion, GetBlockWeight(block)).
    //! It lives in the header so a header-only client can derive a per-transaction PoW
    //! target, which needs m and cannot otherwise reach it (block weight is not in the
    //! header). MUST be serialized BEFORE nNonce: every solver grinds the last 4 bytes
    //! of the pre-pow, so nNonce has to stay at the tail.
    uint32_t nCongestion;
    uint32_t nNonce;
    //! Quicksilver: the Cuckatoo 42-cycle block proof (ascending edge indices).
    //! Part of the serialized header, so GetHash() binds the proof to identity.
    std::array<uint32_t, 42> nCycle;

    CBlockHeader()
    {
        SetNull();
    }

    SERIALIZE_METHODS(CBlockHeader, obj)
    {
        READWRITE(obj.nVersion, obj.hashPrevBlock, obj.hashMerkleRoot, obj.nTime, obj.nBits, obj.nCongestion, obj.nNonce);
        // Serialize the 42 edges as little-endian uint32, in order (the same
        // consensus encoding hashed by CuckatooProofHash).
        for (auto& edge : obj.nCycle) READWRITE(edge);
    }

    void SetNull()
    {
        nVersion = 0;
        hashPrevBlock.SetNull();
        hashMerkleRoot.SetNull();
        nTime = 0;
        nBits = 0;
        nCongestion = 0;
        nNonce = 0;
        nCycle.fill(0);
    }

    //! The 84-byte pre-pow prefix (through nNonce) — input to the siphash keys.
    //! nNonce is deliberately the trailing 4 bytes: every solver mutates buf[len-4..len-1].
    static constexpr size_t PREPOW_SIZE{84};
    std::array<unsigned char, PREPOW_SIZE> PrePowBytes() const;

    bool IsNull() const
    {
        return (nBits == 0);
    }

    uint256 GetHash() const;

    NodeSeconds Time() const
    {
        return NodeSeconds{std::chrono::seconds{nTime}};
    }

    int64_t GetBlockTime() const
    {
        return (int64_t)nTime;
    }
};


class CBlock : public CBlockHeader
{
public:
    // network and disk
    std::vector<CTransactionRef> vtx;

    // Memory-only flags for caching expensive checks
    mutable bool fChecked;                            // CheckBlock()
    mutable bool m_checked_witness_commitment{false}; // CheckWitnessCommitment()
    mutable bool m_checked_merkle_root{false};        // CheckMerkleRoot()

    CBlock()
    {
        SetNull();
    }

    CBlock(const CBlockHeader &header)
    {
        SetNull();
        *(static_cast<CBlockHeader*>(this)) = header;
    }

    SERIALIZE_METHODS(CBlock, obj)
    {
        READWRITE(AsBase<CBlockHeader>(obj), obj.vtx);
    }

    void SetNull()
    {
        CBlockHeader::SetNull();
        vtx.clear();
        fChecked = false;
        m_checked_witness_commitment = false;
        m_checked_merkle_root = false;
    }

    CBlockHeader GetBlockHeader() const
    {
        CBlockHeader block;
        block.nVersion       = nVersion;
        block.hashPrevBlock  = hashPrevBlock;
        block.hashMerkleRoot = hashMerkleRoot;
        block.nTime          = nTime;
        block.nBits          = nBits;
        block.nCongestion    = nCongestion;
        block.nNonce         = nNonce;
        block.nCycle         = nCycle;
        return block;
    }

    std::string ToString() const;
};

/** Describes a place in the block chain to another node such that if the
 * other node doesn't have the same branch, it can find a recent common trunk.
 * The further back it is, the further before the fork it may be.
 */
struct CBlockLocator
{
    std::vector<uint256> vHave;

    CBlockLocator() = default;

    explicit CBlockLocator(std::vector<uint256>&& have) : vHave(std::move(have)) {}

    SERIALIZE_METHODS(CBlockLocator, obj)
    {
        READWRITE(obj.vHave);
    }

    void SetNull()
    {
        vHave.clear();
    }

    bool IsNull() const
    {
        return vHave.empty();
    }
};

#endif // QUICKSILVER_PRIMITIVES_BLOCK_H
