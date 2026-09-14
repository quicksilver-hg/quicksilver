// Copyright (c) 2020-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_VAULT_DUMP_H
#define QUICKSILVER_VAULT_DUMP_H

#include <util/fs.h>

#include <string>
#include <vector>

struct bilingual_str;
class ArgsManager;

namespace vault {
class VaultDatabase;

bool DumpVault(const ArgsManager& args, VaultDatabase& db, bilingual_str& error);
bool CreateFromDump(const ArgsManager& args, const std::string& name, const fs::path& vault_path, bilingual_str& error, std::vector<bilingual_str>& warnings);
} // namespace vault

#endif // QUICKSILVER_VAULT_DUMP_H
