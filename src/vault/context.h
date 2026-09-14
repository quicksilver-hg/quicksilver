// Copyright (c) 2020-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_VAULT_CONTEXT_H
#define QUICKSILVER_VAULT_CONTEXT_H

#include <sync.h>
#include <uint256.h>

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <vector>

class ArgsManager;
class CScheduler;
namespace interfaces {
class Chain;
class Vault;
} // namespace interfaces

namespace vault {
class CVault;
using LoadVaultFn = std::function<void(std::unique_ptr<interfaces::Vault> vault)>;

struct HeaderTip {
    int height;
    uint256 hash;
    int64_t block_time;
};

//! VaultContext struct containing references to state shared between CVault
//! instances, like the reference to the chain interface, and the list of opened
//! vaults.
//!
//! Future shared state can be added here as an alternative to adding global
//! variables.
//!
//! The struct isn't intended to have any member functions. It should just be a
//! collection of state pointers that doesn't pull in dependencies or implement
//! behavior.
struct VaultContext {
    interfaces::Chain* chain{nullptr};
    CScheduler* scheduler{nullptr};
    ArgsManager* args{nullptr}; // Currently a raw pointer because the memory is not managed by this struct
    // It is unsafe to lock this after locking a CVault::cs_vault mutex because
    // this could introduce inconsistent lock ordering and cause deadlocks.
    Mutex vaults_mutex;
    std::vector<std::shared_ptr<CVault>> vaults GUARDED_BY(vaults_mutex);
    std::list<LoadVaultFn> vault_load_fns GUARDED_BY(vaults_mutex);
    std::optional<HeaderTip> header_tip GUARDED_BY(vaults_mutex);

    //! Declare default constructor and destructor that are not inline, so code
    //! instantiating the VaultContext struct doesn't need to #include class
    //! definitions for smart pointer and container members.
    VaultContext();
    ~VaultContext();
};
} // namespace vault

#endif // QUICKSILVER_VAULT_CONTEXT_H
