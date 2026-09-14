// Copyright (c) 2017-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_VAULTINITINTERFACE_H
#define QUICKSILVER_VAULTINITINTERFACE_H

class ArgsManager;

namespace node {
struct NodeContext;
} // namespace node

class VaultInitInterface {
public:
    /** Is the vault component enabled */
    virtual bool HasVaultSupport() const = 0;
    /** Get vault help string */
    virtual void AddVaultOptions(ArgsManager& argsman) const = 0;
    /** Check vault parameter interaction */
    virtual bool ParameterInteraction() const = 0;
    /** Add vaults that should be opened to list of chain clients. */
    virtual void Construct(node::NodeContext& node) const = 0;

    virtual ~VaultInitInterface() = default;
};

extern const VaultInitInterface& g_vault_init_interface;

#endif // QUICKSILVER_VAULTINITINTERFACE_H
