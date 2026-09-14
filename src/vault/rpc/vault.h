// Copyright (c) 2016-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_VAULT_RPC_VAULT_H
#define QUICKSILVER_VAULT_RPC_VAULT_H

#include <span.h>

class CRPCCommand;

namespace vault {
Span<const CRPCCommand> GetVaultRPCCommands();
} // namespace vault

#endif // QUICKSILVER_VAULT_RPC_VAULT_H
