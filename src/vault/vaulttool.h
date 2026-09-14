// Copyright (c) 2016-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_VAULT_VAULTTOOL_H
#define QUICKSILVER_VAULT_VAULTTOOL_H

#include <string>

class ArgsManager;

namespace vault {
namespace VaultTool {

bool ExecuteVaultToolFunc(const ArgsManager& args, const std::string& command);

} // namespace VaultTool
} // namespace vault

#endif // QUICKSILVER_VAULT_VAULTTOOL_H
