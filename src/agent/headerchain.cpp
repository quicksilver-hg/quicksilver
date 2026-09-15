// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <agent/headerchain.h>

#include <consensus/consensus.h>
#include <pow.h>
#include <util/check.h>
#include <util/time.h>

#include <algorithm>
#include <cassert>
#include <chrono>

namespace agent {

const char* HeaderAcceptCodeString(HeaderAcceptCode code)
{
    switch (code) {
    case HeaderAcceptCode::ACCEPTED:
        return "accepted";
    case HeaderAcceptCode::DUPLICATE:
        return "duplicate";
    case HeaderAcceptCode::PREV_NOT_FOUND:
        return "prev-not-found";
    case HeaderAcceptCode::BAD_POW:
        return "bad-pow";
    case HeaderAcceptCode::BAD_DIFFICULTY:
        return "bad-difficulty";
    case HeaderAcceptCode::TIME_TOO_OLD:
        return "time-too-old";
    case HeaderAcceptCode::TIME_TOO_NEW:
        return "time-too-new";
    } // no default case, so the compiler can warn on missing enum values
    assert(false);
    return "unknown";
}

HeaderChain::HeaderEntry::HeaderEntry(const CBlockHeader& header)
    : hash{header.GetHash()},
      index{std::make_unique<CBlockIndex>(header)}
{
    index->phashBlock = &hash;
}

HeaderChain::HeaderChain(const Consensus::Params& params, const CBlockHeader& genesis)
    : m_params{params}
{
    Assume(genesis.GetHash() == m_params.hashGenesisBlock);
    HeaderEntry& entry{Append(genesis, nullptr)};
    m_tip = entry.index.get();
    // m_congestion is no longer seeded here: it is a header field, so CBlockIndex's
    // header constructor carries it for genesis and for every Append()ed header alike.
}

HeaderChain::HeaderEntry& HeaderChain::Append(const CBlockHeader& header, CBlockIndex* prev)
{
    auto entry{std::make_unique<HeaderEntry>(header)};
    CBlockIndex& index{*entry->index};
    index.pprev = prev;
    index.nHeight = prev ? prev->nHeight + 1 : 0;
    index.nChainWork = (prev ? prev->nChainWork : arith_uint256{}) + GetBlockProof(index);
    index.nTimeMax = prev ? std::max<unsigned int>(prev->nTimeMax, index.nTime) : index.nTime;
    // nStatus is deliberately left at its default. It is GUARDED_BY(::cs_main) because it
    // describes an entry in the node's global block index; these CBlockIndex objects are
    // private to this HeaderChain, never published there, and the agent never takes
    // cs_main. Nothing reads the field on this path -- headerstore serializes plain
    // CBlockHeaders, not CDiskBlockIndex -- so writing it was a dead store that only
    // clang's thread-safety analysis had any opinion about. Do not reinstate it: a write
    // here is either a lie about a lock we do not hold, or a sign this chain has started
    // to need real block-index state and should say so explicitly.
    index.BuildSkip();

    HeaderEntry& stored{*entry};
    m_by_hash.emplace(stored.hash, &stored);
    m_entries.emplace_back(std::move(entry));
    return stored;
}

const CBlockIndex* HeaderChain::Lookup(const uint256& hash) const
{
    const auto it{m_by_hash.find(hash)};
    if (it == m_by_hash.end()) return nullptr;
    return it->second->index.get();
}

CBlockLocator HeaderChain::GetLocator() const
{
    return ::GetLocator(&Tip());
}

std::vector<CBlockHeader> HeaderChain::Headers() const
{
    std::vector<CBlockHeader> headers;
    headers.reserve(m_entries.size());
    for (const auto& entry : m_entries) {
        headers.push_back(entry->index->GetBlockHeader());
    }
    return headers;
}

HeaderAcceptResult HeaderChain::AcceptHeader(const CBlockHeader& header)
{
    const uint256 hash{header.GetHash()};
    const CBlockIndex* existing{Lookup(hash)};
    if (existing != nullptr) {
        return {HeaderAcceptCode::DUPLICATE, hash, existing->nHeight};
    }

    const auto prev_it{m_by_hash.find(header.hashPrevBlock)};
    if (prev_it == m_by_hash.end()) {
        return {HeaderAcceptCode::PREV_NOT_FOUND, hash, -1};
    }
    CBlockIndex* prev{prev_it->second->index.get()};

    if (!CheckProofOfWork(header, m_params)) {
        return {HeaderAcceptCode::BAD_POW, hash, prev->nHeight + 1};
    }

    if (header.nBits != GetNextWorkRequired(prev, &header, m_params)) {
        return {HeaderAcceptCode::BAD_DIFFICULTY, hash, prev->nHeight + 1};
    }

    if (header.GetBlockTime() <= prev->GetMedianTimePast()) {
        return {HeaderAcceptCode::TIME_TOO_OLD, hash, prev->nHeight + 1};
    }

    if (header.Time() > NodeClock::now() + std::chrono::seconds{MAX_FUTURE_BLOCK_TIME}) {
        return {HeaderAcceptCode::TIME_TOO_NEW, hash, prev->nHeight + 1};
    }

    HeaderEntry& entry{Append(header, prev)};
    // First-seen wins on equal work: only a strictly heavier chain becomes tip.
    // That is the same rule a full node uses, and it is what lets getheaders
    // from a common ancestor land a two-block side chain without wedging.
    if (entry.index->nChainWork > m_tip->nChainWork) {
        m_tip = entry.index.get();
    }
    return {HeaderAcceptCode::ACCEPTED, entry.hash, entry.index->nHeight};
}

} // namespace agent
