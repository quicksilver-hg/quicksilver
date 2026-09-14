// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef QUICKSILVER_QT_THINVAULTHEADERSOURCE_H
#define QUICKSILVER_QT_THINVAULTHEADERSOURCE_H

#include <util/fs.h>

#include <cstdint>
#include <optional>
#include <string>

namespace interfaces {
class VaultLoader;
}

/** Validated, persistent header-tip input for a desktop vault without a node. */
class ThinVaultHeaderSource
{
public:
    explicit ThinVaultHeaderSource(fs::path path);

    /** Reload a changed header store and apply its verified tip to the vault. */
    bool refresh(interfaces::VaultLoader& loader, std::string& error);

private:
    fs::path m_path;
    std::optional<fs::file_time_type> m_last_write_time;
    std::optional<uintmax_t> m_last_size;
    bool m_loaded_missing{false};
};

fs::path DefaultThinVaultHeaderStorePath();

#endif // QUICKSILVER_QT_THINVAULTHEADERSOURCE_H
