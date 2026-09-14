// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <quicksilver-build-config.h> // IWYU pragma: keep

#include <common/args.h>
#include <init.h>
#include <interfaces/chain.h>
#include <interfaces/init.h>
#include <interfaces/vault.h>
#include <net.h>
#include <node/context.h>
#include <node/interface_ui.h>
#include <outputtype.h>
#include <univalue.h>
#include <util/check.h>
#include <util/moneystr.h>
#include <util/translation.h>
#include <vault/coincontrol.h>
#include <vault/vault.h>
#include <vaultinitinterface.h>

using node::NodeContext;

namespace vault {
class VaultInit : public VaultInitInterface
{
public:
    //! Was the vault component compiled in.
    bool HasVaultSupport() const override {return true;}

    //! Return the vaults help message.
    void AddVaultOptions(ArgsManager& argsman) const override;

    //! Vaults parameter interaction
    bool ParameterInteraction() const override;

    //! Add vaults that should be opened to list of chain clients.
    void Construct(NodeContext& node) const override;
};

void VaultInit::AddVaultOptions(ArgsManager& argsman) const
{
    argsman.AddArg("-addresstype", strprintf("What type of addresses to use (\"base58\", \"bech32\", or \"bech32m\", default: \"%s\")", FormatOutputType(DEFAULT_ADDRESS_TYPE)), ArgsManager::ALLOW_ANY, OptionsCategory::VAULT);
    argsman.AddArg("-avoidpartialspends", strprintf("Group outputs by address, selecting many (possibly all) or none, instead of selecting on a per-output basis. Privacy is improved as addresses are mostly swept with fewer transactions and outputs are aggregated in clean change addresses. It may result in a larger-than-necessary number of inputs being used. Always enabled for vaults with \"avoid_reuse\" enabled, otherwise default: %u.", DEFAULT_AVOIDPARTIALSPENDS), ArgsManager::ALLOW_ANY, OptionsCategory::VAULT);
    argsman.AddArg("-changetype",
                   "What type of change to use (\"base58\", \"bech32\", or \"bech32m\"). Default is \"base58\" when "
                   "-addresstype=base58, else it is an implementation detail.",
                   ArgsManager::ALLOW_ANY, OptionsCategory::VAULT);
    argsman.AddArg("-disablevault", "Do not load the vault and disable vault RPC calls", ArgsManager::ALLOW_ANY, OptionsCategory::VAULT);
    argsman.AddArg("-keypool=<n>", strprintf("Set key pool size to <n> (default: %u). Warning: Smaller sizes may increase the risk of losing funds when restoring from an old backup, if none of the addresses in the original keypool have been used.", DEFAULT_KEYPOOL_SIZE), ArgsManager::ALLOW_ANY, OptionsCategory::VAULT);
#ifdef ENABLE_EXTERNAL_SIGNER
    argsman.AddArg("-signer=<cmd>", "External signing tool, see doc/external-signer.md", ArgsManager::ALLOW_ANY, OptionsCategory::VAULT);
#endif
    argsman.AddArg("-spendzeroconfchange", strprintf("Spend unconfirmed change when sending transactions (default: %u)", DEFAULT_SPEND_ZEROCONF_CHANGE), ArgsManager::ALLOW_ANY, OptionsCategory::VAULT);
    argsman.AddArg("-vault=<path>", "Specify vault path to load at startup. Can be used multiple times to load multiple vaults. Path is to a directory containing vault data. If the path is not absolute, it is interpreted relative to <vaultdir>. This only loads existing vaults and does not create new ones.", ArgsManager::ALLOW_ANY | ArgsManager::NETWORK_ONLY, OptionsCategory::VAULT);
    argsman.AddArg("-vaultbroadcast",  strprintf("Make the vault broadcast transactions (default: %u)", DEFAULT_VAULTBROADCAST), ArgsManager::ALLOW_ANY, OptionsCategory::VAULT);
    argsman.AddArg("-vaultdir=<dir>", "Specify directory to hold vaults (default: <datadir>/vaults)", ArgsManager::ALLOW_ANY | ArgsManager::NETWORK_ONLY, OptionsCategory::VAULT);
#if HAVE_SYSTEM
    argsman.AddArg("-vaultnotify=<cmd>", "Execute command when a vault transaction changes. %s in cmd is replaced by TxID, %w is replaced by vault name, %b is replaced by the hash of the block including the transaction (set to 'unconfirmed' if the transaction is not included) and %h is replaced by the block height (-1 if not included). %w is not currently implemented on windows. On systems where %w is supported, it should NOT be quoted because this would break shell escaping used to invoke the command.", ArgsManager::ALLOW_ANY, OptionsCategory::VAULT);
#endif

    argsman.AddHiddenArgs({"-flushvault"});

    argsman.AddArg("-headerstore=<file>", "Read the validated thin header chain used by a node-free desktop vault. Relative paths are resolved under the network data directory. (default: agent/headers.dat)", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::VAULT);
    argsman.AddArg("-receiptstore=<file>", "Read and update the durable agent payment receipt store. Relative paths are resolved under the network data directory. (default: agent/payment-receipts.dat)", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::VAULT);
    argsman.AddArg("-paymentreceiptdir=<dir>", "Scan agent payment receipt JSON files for desktop UTXO refresh. Relative paths are resolved under the network data directory. (default: agent/payment-receipts.d)", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::VAULT);

#ifdef USE_SQLITE
    argsman.AddArg("-unsafesqlitesync", "Set SQLite synchronous=OFF to disable waiting for the database to sync to disk. This is unsafe and can cause data loss and corruption. This option is only used by tests to improve their performance (default: false)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::VAULT_DEBUG_TEST);
#else
    argsman.AddHiddenArgs({"-unsafesqlitesync"});
#endif

    argsman.AddArg("-vaultrejectlongchains", strprintf("Vault will not create transactions that violate relay pool chain limits (default: %u)", DEFAULT_VAULT_REJECT_LONG_CHAINS), ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::VAULT_DEBUG_TEST);
    argsman.AddArg("-vaultcrosschain", strprintf("Allow reusing vault files across chains (default: %u)", DEFAULT_VAULTCROSSCHAIN), ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::VAULT_DEBUG_TEST);
}

bool VaultInit::ParameterInteraction() const
{
    if (gArgs.GetBoolArg("-disablevault", DEFAULT_DISABLE_VAULT)) {
        for (const std::string& vault : gArgs.GetArgs("-vault")) {
            LogInfo(HgLog::VAULT, "%s: parameter interaction: -disablevault -> ignoring -vault=%s\n", __func__, vault);
        }

        return true;
    }

    if (gArgs.GetBoolArg("-blocksonly", DEFAULT_BLOCKSONLY) && gArgs.SoftSetBoolArg("-vaultbroadcast", false)) {
        LogInfo(HgLog::VAULT, "%s: parameter interaction: -blocksonly=1 -> setting -vaultbroadcast=0\n", __func__);
    }

    return true;
}

void VaultInit::Construct(NodeContext& node) const
{
    ArgsManager& args = *Assert(node.args);
    if (args.GetBoolArg("-disablevault", DEFAULT_DISABLE_VAULT)) {
        LogInfo(HgLog::VAULT, "Vault disabled!\n");
        return;
    }
    auto vault_loader = node.init->makeVaultLoader(*node.chain);
    node.vault_loader = vault_loader.get();
    node.chain_clients.emplace_back(std::move(vault_loader));
}
} // namespace vault

const VaultInitInterface& g_vault_init_interface = vault::VaultInit();
