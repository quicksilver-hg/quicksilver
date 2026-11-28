// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <agent/headerstore.h>

#include <random.h>
#include <streams.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/fs_helpers.h>

#include <cassert>
#include <cstdint>
#include <exception>
#include <system_error>
#include <memory>
#include <vector>

namespace agent {
namespace {

constexpr uint32_t HEADER_STORE_MAGIC{0x47415351}; // "QSAG" in little-endian streams.
constexpr uint32_t HEADER_STORE_VERSION{1};
constexpr uint64_t MAX_HEADER_STORE_HEADERS{10'000'000};

HeaderAcceptResult NoInvalidHeader(const CBlockHeader& genesis)
{
    return {HeaderAcceptCode::DUPLICATE, genesis.GetHash(), 0};
}

HeaderStoreLoadResult LoadResult(HeaderStoreResult status, std::unique_ptr<HeaderChain> chain, HeaderAcceptResult invalid_header)
{
    return {status, std::move(chain), invalid_header};
}

//! A temp name of its own per save, so two writers never share one and a stale
//! file from a crashed run cannot block the next save.
fs::path UniqueTempPath(const fs::path& path)
{
    return path + strprintf(".new-%016x", FastRandomContext().rand64()).c_str();
}

HeaderStoreResult WriteHeaderStoreFile(const HeaderChain& chain,
                                       const std::vector<CBlockHeader>& headers,
                                       const fs::path& temp_path,
                                       const fs::path& path)
{
    AutoFile file{fsbridge::fopen(temp_path, "wb")};
    if (file.IsNull()) return HeaderStoreResult::FILE_OPEN_FAILED;

    try {
        file << HEADER_STORE_MAGIC;
        file << HEADER_STORE_VERSION;
        file << chain.Genesis().GetBlockHash();
        file << static_cast<uint64_t>(headers.size());
        for (const CBlockHeader& header : headers) {
            file << header;
        }

        if (!file.Commit()) return HeaderStoreResult::FILE_COMMIT_FAILED;
        if (file.fclose() != 0) return HeaderStoreResult::FILE_COMMIT_FAILED;
        if (!RenameOver(temp_path, path)) return HeaderStoreResult::FILE_RENAME_FAILED;
    } catch (const std::exception&) {
        return HeaderStoreResult::FILE_WRITE_FAILED;
    }
    if (path.has_parent_path()) DirectoryCommit(path.parent_path());
    return HeaderStoreResult::OK;
}

} // namespace

const char* HeaderStoreResultString(HeaderStoreResult status)
{
    switch (status) {
    case HeaderStoreResult::OK:
        return "ok";
    case HeaderStoreResult::FILE_NOT_FOUND:
        return "file-not-found";
    case HeaderStoreResult::FILE_OPEN_FAILED:
        return "file-open-failed";
    case HeaderStoreResult::FILE_WRITE_FAILED:
        return "file-write-failed";
    case HeaderStoreResult::FILE_COMMIT_FAILED:
        return "file-commit-failed";
    case HeaderStoreResult::FILE_RENAME_FAILED:
        return "file-rename-failed";
    case HeaderStoreResult::DESERIALIZE_FAILED:
        return "deserialize-failed";
    case HeaderStoreResult::BAD_MAGIC:
        return "bad-magic";
    case HeaderStoreResult::BAD_VERSION:
        return "bad-version";
    case HeaderStoreResult::BAD_GENESIS:
        return "bad-genesis";
    case HeaderStoreResult::EMPTY_STORE:
        return "empty-store";
    case HeaderStoreResult::TOO_MANY_HEADERS:
        return "too-many-headers";
    case HeaderStoreResult::INVALID_HEADER:
        return "invalid-header";
    } // no default case, so the compiler can warn on missing enum values
    assert(false);
    return "unknown";
}

HeaderStoreResult SaveHeaderChain(const HeaderChain& chain, const fs::path& path)
{
    const std::vector<CBlockHeader> headers{chain.Headers()};
    // The loader refuses more, so refuse to write more: a store that saves must load.
    if (headers.size() > MAX_HEADER_STORE_HEADERS) return HeaderStoreResult::TOO_MANY_HEADERS;

    const fs::path temp_path{UniqueTempPath(path)};
    const HeaderStoreResult result{WriteHeaderStoreFile(chain, headers, temp_path, path)};
    if (result != HeaderStoreResult::OK) {
        std::error_code ignored;
        fs::remove(temp_path, ignored);
    }
    return result;
}

HeaderStoreLoadResult LoadHeaderChain(const Consensus::Params& params, const CBlockHeader& genesis, const fs::path& path)
{
    auto chain{std::make_unique<HeaderChain>(params, genesis)};
    const HeaderAcceptResult no_invalid{NoInvalidHeader(genesis)};

    if (path.empty()) return LoadResult(HeaderStoreResult::FILE_NOT_FOUND, std::move(chain), no_invalid);
    // A permission or path error is a result here, not an exception.
    bool exists{false};
    try {
        exists = fs::exists(path);
    } catch (const fs::filesystem_error&) {
        return LoadResult(HeaderStoreResult::FILE_OPEN_FAILED, std::move(chain), no_invalid);
    }
    if (!exists) return LoadResult(HeaderStoreResult::FILE_NOT_FOUND, std::move(chain), no_invalid);

    AutoFile file{fsbridge::fopen(path, "rb")};
    if (file.IsNull()) return LoadResult(HeaderStoreResult::FILE_OPEN_FAILED, std::move(chain), no_invalid);

    try {
        uint32_t magic{0};
        uint32_t version{0};
        uint256 stored_genesis;
        uint64_t header_count{0};

        file >> magic;
        file >> version;
        file >> stored_genesis;
        file >> header_count;

        if (magic != HEADER_STORE_MAGIC) return LoadResult(HeaderStoreResult::BAD_MAGIC, std::move(chain), no_invalid);
        if (version != HEADER_STORE_VERSION) return LoadResult(HeaderStoreResult::BAD_VERSION, std::move(chain), no_invalid);
        if (stored_genesis != params.hashGenesisBlock) return LoadResult(HeaderStoreResult::BAD_GENESIS, std::move(chain), no_invalid);
        if (header_count == 0) return LoadResult(HeaderStoreResult::EMPTY_STORE, std::move(chain), no_invalid);
        if (header_count > MAX_HEADER_STORE_HEADERS) return LoadResult(HeaderStoreResult::TOO_MANY_HEADERS, std::move(chain), no_invalid);

        CBlockHeader stored_genesis_header;
        file >> stored_genesis_header;
        if (stored_genesis_header.GetHash() != genesis.GetHash()) {
            return LoadResult(HeaderStoreResult::BAD_GENESIS, std::move(chain), no_invalid);
        }

        for (uint64_t i{1}; i < header_count; ++i) {
            CBlockHeader header;
            file >> header;
            HeaderAcceptResult accepted{chain->AcceptHeader(header)};
            if (!accepted.accepted()) {
                return LoadResult(HeaderStoreResult::INVALID_HEADER, std::move(chain), accepted);
            }
        }
    } catch (const std::exception&) {
        return LoadResult(HeaderStoreResult::DESERIALIZE_FAILED, std::move(chain), no_invalid);
    }

    return LoadResult(HeaderStoreResult::OK, std::move(chain), no_invalid);
}

} // namespace agent
