// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef QUICKSILVER_AGENT_HEADERSTORE_H
#define QUICKSILVER_AGENT_HEADERSTORE_H

#include <agent/headerchain.h>
#include <consensus/params.h>
#include <primitives/block.h>
#include <util/fs.h>

#include <memory>
#include <string>

namespace agent {

enum class HeaderStoreResult {
    OK,
    FILE_NOT_FOUND,
    FILE_OPEN_FAILED,
    FILE_WRITE_FAILED,
    FILE_COMMIT_FAILED,
    FILE_RENAME_FAILED,
    DESERIALIZE_FAILED,
    BAD_MAGIC,
    BAD_VERSION,
    BAD_GENESIS,
    EMPTY_STORE,
    TOO_MANY_HEADERS,
    INVALID_HEADER,
};

struct HeaderStoreLoadResult {
    HeaderStoreResult status;
    std::unique_ptr<HeaderChain> chain;
    HeaderAcceptResult invalid_header;

    bool ok() const { return status == HeaderStoreResult::OK; }
};

const char* HeaderStoreResultString(HeaderStoreResult status);

HeaderStoreResult SaveHeaderChain(const HeaderChain& chain, const fs::path& path);
HeaderStoreLoadResult LoadHeaderChain(const Consensus::Params& params, const CBlockHeader& genesis, const fs::path& path);

} // namespace agent

#endif // QUICKSILVER_AGENT_HEADERSTORE_H
