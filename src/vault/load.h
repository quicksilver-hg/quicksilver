// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_VAULT_LOAD_H
#define QUICKSILVER_VAULT_LOAD_H

#include <string>
#include <vector>

class ArgsManager;
class CScheduler;

namespace interfaces {
class Chain;
} // namespace interfaces

namespace vault {
struct VaultContext;

//! Responsible for reading and validating the -vault arguments and verifying the vault database.
bool VerifyVaults(VaultContext& context);

//! Load vault databases.
bool LoadVaults(VaultContext& context);

//! Complete startup of vaults.
void StartVaults(VaultContext& context);

//! Flush all vaults in preparation for shutdown.
void FlushVaults(VaultContext& context);

//! Stop all vaults. Vaults will be flushed first.
void StopVaults(VaultContext& context);

//! Close all vaults.
void UnloadVaults(VaultContext& context);
} // namespace vault

#endif // QUICKSILVER_VAULT_LOAD_H
