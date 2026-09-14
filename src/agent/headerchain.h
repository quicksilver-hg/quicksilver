// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef QUICKSILVER_AGENT_HEADERCHAIN_H
#define QUICKSILVER_AGENT_HEADERCHAIN_H

#include <arith_uint256.h>
#include <chain.h>
#include <consensus/params.h>
#include <primitives/block.h>
#include <uint256.h>

#include <map>
#include <memory>
#include <vector>

namespace agent {

enum class HeaderAcceptCode {
    ACCEPTED,
    DUPLICATE,
    PREV_NOT_FOUND,
    BAD_POW,
    BAD_DIFFICULTY,
    TIME_TOO_OLD,
    TIME_TOO_NEW,
};

struct HeaderAcceptResult {
    HeaderAcceptCode code;
    uint256 hash;
    int height;

    bool accepted() const { return code == HeaderAcceptCode::ACCEPTED; }
};

const char* HeaderAcceptCodeString(HeaderAcceptCode code);

class HeaderChain
{
public:
    HeaderChain(const Consensus::Params& params, const CBlockHeader& genesis);
    HeaderChain(const HeaderChain&) = delete;
    HeaderChain& operator=(const HeaderChain&) = delete;
    HeaderChain(HeaderChain&&) = delete;
    HeaderChain& operator=(HeaderChain&&) = delete;

    HeaderAcceptResult AcceptHeader(const CBlockHeader& header);

    int Height() const { return Tip().nHeight; }
    const CBlockIndex& Genesis() const { return *m_entries.front()->index; }
    //! Most-work header. First-seen wins when chain work is equal.
    const CBlockIndex& Tip() const { return *m_tip; }
    arith_uint256 ChainWork() const { return Tip().nChainWork; }
    CBlockLocator GetLocator() const;
    std::vector<CBlockHeader> Headers() const;

    const CBlockIndex* Lookup(const uint256& hash) const;

private:
    struct HeaderEntry {
        explicit HeaderEntry(const CBlockHeader& header);

        uint256 hash;
        std::unique_ptr<CBlockIndex> index;
    };

    HeaderEntry& Append(const CBlockHeader& header, CBlockIndex* prev);

    Consensus::Params m_params;
    std::vector<std::unique_ptr<HeaderEntry>> m_entries;
    std::map<uint256, HeaderEntry*> m_by_hash;
    CBlockIndex* m_tip{nullptr};
};

} // namespace agent

#endif // QUICKSILVER_AGENT_HEADERCHAIN_H
