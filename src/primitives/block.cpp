// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <primitives/block.h>

#include <hash.h>
#include <streams.h>
#include <tinyformat.h>

#include <cassert>
#include <cstring>

uint256 CBlockHeader::GetHash() const
{
    return (HashWriter{} << *this).GetHash();
}

std::array<unsigned char, CBlockHeader::PREPOW_SIZE> CBlockHeader::PrePowBytes() const
{
    // The pre-pow is the header prefix through nNonce (excludes nCycle). These
    // bytes seed the Cuckatoo siphash keys; the cycle must not feed back in.
    //
    // nCongestion is placed BEFORE nNonce so that nNonce remains the trailing 4
    // bytes. Every solver entry point grinds the tail of this buffer in place
    // (solve_19/solve_28 via mutate_nonce, Solve28Bytes, dispatch.cpp's key
    // reconstruction, the bench KeyedPrepow helpers, and the external CUDA solver
    // in gpu/qsgpusolve.cu). Moving nNonce off the tail would silently make every
    // one of them grind the congestion field instead.
    DataStream ds;
    ds << nVersion << hashPrevBlock << hashMerkleRoot << nTime << nBits << nCongestion << nNonce;
    assert(ds.size() == PREPOW_SIZE);
    std::array<unsigned char, PREPOW_SIZE> out;
    std::memcpy(out.data(), ds.data(), out.size());
    return out;
}

std::string CBlock::ToString() const
{
    std::stringstream s;
    s << strprintf("CBlock(hash=%s, ver=0x%08x, hashPrevBlock=%s, hashMerkleRoot=%s, nTime=%u, nBits=%08x, nCongestion=%u, nNonce=%u, vtx=%u)\n",
        GetHash().ToString(),
        nVersion,
        hashPrevBlock.ToString(),
        hashMerkleRoot.ToString(),
        nTime, nBits, nCongestion, nNonce,
        vtx.size());
    for (const auto& tx : vtx) {
        s << "  " << tx->ToString() << "\n";
    }
    return s.str();
}
