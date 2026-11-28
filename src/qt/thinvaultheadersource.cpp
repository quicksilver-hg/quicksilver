// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <qt/thinvaultheadersource.h>

#include <agent/headerstore.h>
#include <chainparams.h>
#include <common/args.h>
#include <interfaces/vault.h>
#include <tinyformat.h>
#include <util/fs_helpers.h>

#include <system_error>
#include <utility>

ThinVaultHeaderSource::ThinVaultHeaderSource(fs::path path) : m_path{std::move(path)} {}

bool ThinVaultHeaderSource::refresh(interfaces::VaultLoader& loader, std::string& error)
{
    // fs::exists() only wraps the throwing overload, so probe the store through the
    // metadata this needs anyway and read "not found" as a store that is not written yet.
    std::error_code file_error;
    std::optional<fs::file_time_type> write_time;
    std::optional<uintmax_t> size;
    const fs::file_time_type observed_write_time{fs::last_write_time(m_path, file_error)};
    if (!file_error) {
        size = fs::file_size(m_path, file_error);
        if (file_error) {
            error = strprintf("Could not inspect thin header store %s: %s", fs::PathToString(m_path), file_error.message());
            return false;
        }
        write_time = observed_write_time;
        if (write_time == m_last_write_time && size == m_last_size) return true;
        // A store that failed to load, or whose tip was refused, fails the same way
        // until it changes, so do not revalidate it on every tick (S0245-C-02).
        if (m_failed_error && write_time == m_failed_write_time && size == m_failed_size) {
            error = *m_failed_error;
            return false;
        }
    } else if (file_error != std::errc::no_such_file_or_directory) {
        error = strprintf("Could not inspect thin header store %s: %s", fs::PathToString(m_path), file_error.message());
        return false;
    } else if (m_loaded_missing || m_last_write_time) {
        // Not written yet, or removed after a tip was applied: either way there is
        // no new information. A removed store would read as genesis only, which the
        // loader refuses as older on every tick (S0245-C-01).
        return true;
    }
    const bool exists{write_time.has_value()};

    const CChainParams& params{Params()};
    agent::HeaderStoreLoadResult loaded{
        agent::LoadHeaderChain(params.GetConsensus(), params.GenesisBlock(), m_path)};
    if (loaded.status != agent::HeaderStoreResult::OK &&
        loaded.status != agent::HeaderStoreResult::FILE_NOT_FOUND) {
        error = strprintf("Could not load thin header store %s: %s",
                          fs::PathToString(m_path),
                          agent::HeaderStoreResultString(loaded.status));
        rememberFailure(write_time, size, error);
        return false;
    }

    const CBlockIndex& tip{loaded.chain->Tip()};
    if (!loader.setHeaderTip(tip.nHeight, tip.GetBlockHash(), tip.GetBlockTime())) {
        error = strprintf("Thin header store %s tried to replace the current tip with an older or conflicting tip.",
                          fs::PathToString(m_path));
        rememberFailure(write_time, size, error);
        return false;
    }

    m_failed_error.reset();
    m_loaded_missing = !exists;
    m_last_write_time = write_time;
    m_last_size = size;
    error.clear();
    return true;
}

void ThinVaultHeaderSource::rememberFailure(std::optional<fs::file_time_type> write_time, std::optional<uintmax_t> size, const std::string& error)
{
    // Only a store that exists has a write time and size to compare against.
    if (!write_time) {
        m_failed_error.reset();
        return;
    }
    m_failed_write_time = write_time;
    m_failed_size = size;
    m_failed_error = error;
}

fs::path DefaultThinVaultHeaderStorePath()
{
    const fs::path configured_path{gArgs.GetPathArg("-headerstore", fs::path{"agent"} / "headers.dat")};
    return fsbridge::AbsPathJoin(gArgs.GetDataDirNet(), configured_path);
}
