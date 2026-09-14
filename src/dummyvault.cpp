// Copyright (c) 2018-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/args.h>
#include <logging.h>
#include <vaultinitinterface.h>

class ArgsManager;

namespace interfaces {
class Chain;
class Handler;
class Vault;
class VaultLoader;
}

class DummyVaultInit : public VaultInitInterface {
public:

    bool HasVaultSupport() const override {return false;}
    void AddVaultOptions(ArgsManager& argsman) const override;
    bool ParameterInteraction() const override {return true;}
    void Construct(node::NodeContext& node) const override {LogInfo(HgLog::INIT, "No vault support compiled in!\n");}
};

void DummyVaultInit::AddVaultOptions(ArgsManager& argsman) const
{
    argsman.AddHiddenArgs({
        "-addresstype",
        "-avoidpartialspends",
        "-changetype",
        "-disablevault",
        "-keypool=<n>",
        "-signer=<cmd>",
        "-spendzeroconfchange",
        "-vault=<path>",
        "-vaultbroadcast",
        "-vaultdir=<dir>",
        "-vaultnotify=<cmd>",
        "-flushvault",
        "-headerstore=<file>",
        "-receiptstore=<file>",
        "-paymentreceiptdir=<dir>",
        "-vaultrejectlongchains",
        "-vaultcrosschain",
        "-unsafesqlitesync",
    });
}

const VaultInitInterface& g_vault_init_interface = DummyVaultInit();

namespace interfaces {

std::unique_ptr<VaultLoader> MakeVaultLoader(Chain& chain, ArgsManager& args)
{
    throw std::logic_error("Vault function called in non-vault build.");
}

} // namespace interfaces
