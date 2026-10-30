// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <common/args.h>
#include <logging.h>
#include <tinyformat.h>
#include <util/chaintype.h>
#include <util/strencodings.h>
#include <util/fs.h>
#include <vault/db.h>

#include <algorithm>
#include <exception>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace vault {
bool operator<(BytePrefix a, Span<const std::byte> b) { return std::ranges::lexicographical_compare(a.prefix, b.subspan(0, std::min(a.prefix.size(), b.size()))); }
bool operator<(Span<const std::byte> a, BytePrefix b) { return std::ranges::lexicographical_compare(a.subspan(0, std::min(a.size(), b.prefix.size())), b.prefix); }

std::vector<std::pair<fs::path, std::string>> ListDatabases(const fs::path& vault_dir)
{
    std::vector<std::pair<fs::path, std::string>> paths;
    std::error_code ec;

    for (auto it = fs::recursive_directory_iterator(vault_dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            if (fs::is_directory(*it)) {
                it.disable_recursion_pending();
                LogInfo(HgLog::VAULTDB, "%s: %s %s -- skipping.\n", __func__, ec.message(), fs::PathToString(it->path()));
            } else {
                LogInfo(HgLog::VAULTDB, "%s: %s %s\n", __func__, ec.message(), fs::PathToString(it->path()));
            }
            continue;
        }

        try {
            const fs::path path{it->path().lexically_relative(vault_dir)};

            if (it->status().type() == fs::file_type::directory) {
                if (IsSQLiteFile(SQLiteDataFile(it->path()))) {
                    // Found a directory which contains vault.dat sqlite file, add it as a vault with SQLITE format.
                    paths.emplace_back(path, "sqlite");
                }
            } else if (it.depth() == 0 && it->symlink_status().type() == fs::file_type::regular && it->path().extension() != ".bak") {
                if (it->path().filename() == "vault.dat") {
                    // Found top-level vault.dat file, add top level directory ""
                    // as a vault.
                    if (IsSQLiteFile(it->path())) {
                        paths.emplace_back(fs::path(), "sqlite");
                    }
                }
            }
        } catch (const std::exception& e) {
            LogInfo(HgLog::VAULTDB, "%s: Error scanning %s: %s\n", __func__, fs::PathToString(it->path()), e.what());
            it.disable_recursion_pending();
        }
    }

    return paths;
}

fs::path SQLiteDataFile(const fs::path& path)
{
    return path / "vault.dat";
}

std::optional<MessageStartChars> ReadSQLiteApplicationId(const fs::path& path)
{
    if (!fs::exists(path)) return std::nullopt;

    // A SQLite Database file is at least 512 bytes.
    std::error_code ec;
    auto size = fs::file_size(path, ec);
    if (ec) LogInfo(HgLog::VAULTDB, "%s: %s %s\n", __func__, ec.message(), fs::PathToString(path));
    if (size < 512) return std::nullopt;

    std::ifstream file{path, std::ios::binary};
    if (!file.is_open()) return std::nullopt;

    // Magic is at beginning and is 16 bytes long
    char magic[16];
    file.read(magic, 16);

    // Application id is at offset 68 and 4 bytes long
    file.seekg(68, std::ios::beg);
    MessageStartChars app_id;
    file.read(reinterpret_cast<char*>(app_id.data()), app_id.size());

    file.close();

    // Check the magic, see https://sqlite.org/fileformat.html
    std::string magic_str(magic, 16);
    if (magic_str != std::string{"SQLite format 3\000", 16}) {
        return std::nullopt;
    }
    return app_id;
}

bool IsSQLiteFile(const fs::path& path)
{
    // Check the application id matches our network magic
    const auto app_id{ReadSQLiteApplicationId(path)};
    return app_id && *app_id == Params().MessageStart();
}

std::string WrongNetworkVaultReason(const MessageStartChars& app_id)
{
    const CChainParams& running{Params()};
    const std::string running_name{ChainTypeToString(running.GetChainType())};
    for (const auto& params : {CChainParams::Main(), CChainParams::PublicTest(), CChainParams::Sandbox({})}) {
        if (params->MessageStart() != app_id) continue;
        const std::string name{ChainTypeToString(params->GetChainType())};
        return strprintf("This vault belongs to the %s network, but this node runs %s. The file is not damaged: open it with a node running %s.",
                         name, running_name, name);
    }
    return strprintf("This vault was made for a network this release does not know (id %s), and this node runs %s (id %s). The file is not damaged, but this release cannot open it.",
                     HexStr(app_id), running_name, HexStr(running.MessageStart()));
}

void ReadDatabaseArgs(const ArgsManager& args, DatabaseOptions& options)
{
    // Override current options with args values, if any were specified
    options.use_unsafe_sync = args.GetBoolArg("-unsafesqlitesync", options.use_unsafe_sync);
}

} // namespace vault
