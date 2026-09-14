// Copyright (c) 2024-present The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_TEST_FUZZ_UTIL_VAULT_H
#define QUICKSILVER_TEST_FUZZ_UTIL_VAULT_H

#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <policy/policy.h>
#include <vault/coincontrol.h>
#include <vault/spend.h>
#include <vault/test/util.h>
#include <vault/vault.h>

namespace vault {

/**
 * Wraps a vault for fuzzing.
 */
struct FuzzedVault {
    std::shared_ptr<CVault> vault;
    FuzzedVault(interfaces::Chain& chain, const std::string& name, const std::string& seed_insecure)
    {
        vault = std::make_shared<CVault>(&chain, name, CreateMockableVaultDatabase());
        {
            LOCK(vault->cs_vault);
            vault->SetVaultFlag(VAULT_FLAG_DESCRIPTORS);
            auto height{*Assert(chain.getHeight())};
            vault->SetLastBlockProcessed(height, chain.getBlockHash(height));
        }
        vault->m_keypool_size = 1; // Avoid timeout in TopUp()
        assert(vault->IsVaultFlagSet(VAULT_FLAG_DESCRIPTORS));
        ImportDescriptors(seed_insecure);
    }
    void ImportDescriptors(const std::string& seed_insecure)
    {
        const std::vector<std::string> DESCS{
            "pkh(%s/%s/*)",
            "tr(%s/%s/*)",
            "wpkh(%s/%s/*)",
        };

        for (const std::string& desc_fmt : DESCS) {
            for (bool internal : {true, false}) {
                const auto descriptor{strprintf(tfm::RuntimeFormat{desc_fmt}, "[5aa9973a/66h/4h/2h]" + seed_insecure, int{internal})};

                FlatSigningProvider keys;
                std::string error;
                auto parsed_desc = std::move(Parse(descriptor, keys, error, /*require_checksum=*/false).at(0));
                assert(parsed_desc);
                assert(error.empty());
                assert(parsed_desc->IsRange());
                assert(parsed_desc->IsSingleType());
                assert(!keys.keys.empty());
                VaultDescriptor w_desc{std::move(parsed_desc), /*creation_time=*/0, /*range_start=*/0, /*range_end=*/1, /*next_index=*/0};
                assert(!vault->GetDescriptorScriptPubKeyMan(w_desc));
                LOCK(vault->cs_vault);
                auto spk_manager{vault->AddVaultDescriptor(w_desc, keys, /*label=*/"", internal)};
                assert(spk_manager);
                vault->AddActiveScriptPubKeyMan(spk_manager->GetID(), *Assert(w_desc.descriptor->GetOutputType()), internal);
            }
        }
    }
    CTxDestination GetDestination(FuzzedDataProvider& fuzzed_data_provider)
    {
        auto type{fuzzed_data_provider.PickValueInArray(OUTPUT_TYPES)};
        if (fuzzed_data_provider.ConsumeBool()) {
            return *Assert(vault->GetNewDestination(type, ""));
        } else {
            return *Assert(vault->GetNewChangeDestination(type));
        }
    }
    CScript GetScriptPubKey(FuzzedDataProvider& fuzzed_data_provider) { return GetScriptForDestination(GetDestination(fuzzed_data_provider)); }
    void FundTx(FuzzedDataProvider& fuzzed_data_provider, CMutableTransaction tx)
    {
        std::vector<CRecipient> recipients;
        for (size_t idx = 0; idx < tx.vout.size(); idx++) {
            const CTxOut& tx_out = tx.vout[idx];
            CTxDestination dest;
            ExtractDestination(tx_out.scriptPubKey, dest);
            CRecipient recipient = {dest, tx_out.nValue};
            recipients.push_back(recipient);
        }
        CCoinControl coin_control;
        coin_control.m_allow_other_inputs = fuzzed_data_provider.ConsumeBool();
        CallOneOf(
            fuzzed_data_provider, [&] { coin_control.destChange = GetDestination(fuzzed_data_provider); },
            [&] { coin_control.m_change_type.emplace(fuzzed_data_provider.PickValueInArray(OUTPUT_TYPES)); },
            [&] { /* no op (leave uninitialized) */ });
        coin_control.m_include_unsafe_inputs = fuzzed_data_provider.ConsumeBool();
        // Add solving data (m_external_provider and SelectExternal)?

        int change_position{fuzzed_data_provider.ConsumeIntegralInRange<int>(-1, tx.vout.size() - 1)};
        bilingual_str error;
        // Clear tx.vout since it is not meant to be used now that we are passing outputs directly.
        // This sets us up for a future PR to completely remove tx from the function signature in favor of passing inputs directly
        tx.vout.clear();
        (void)FundTransaction(*vault, tx, recipients, change_position, /*lockUnspents=*/false, coin_control);
    }
};
}

#endif // QUICKSILVER_TEST_FUZZ_UTIL_VAULT_H
